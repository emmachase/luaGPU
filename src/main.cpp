// luaGPU — main entry point
// Usage: luagpu <shader.lua> [width] [height]
//
// Opens a GLFW window, runs the given .lua file through LuaJIT which calls
// shader() to compile GLSL, then renders a fullscreen quad at 60 fps with
// u_time / u_delta / u_resolution / u_mouse uniforms updated each frame.
// The shader file is watched for changes and reloaded automatically.
// Compile errors are displayed as an overlay banner in the window.
// Press R to force-reload. Press Escape or close the window to quit.

#include <glad/glad.h>
#include <GLFW/glfw3.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "ShaderCompiler.h"
#include "Compiler.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ── Fullscreen quad ────────────────────────────────────────────────────────────

static const float kQuadVerts[] = {
    -1.f, -1.f,
     1.f, -1.f,
    -1.f,  1.f,
     1.f,  1.f,
};

static GLuint g_vao = 0, g_vbo = 0;

static void init_quad() {
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
}

// ── Shader compilation helpers ─────────────────────────────────────────────────

static GLuint compile_stage(GLenum type, const char *src, std::string &err) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[4096];
        glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
        err = buf;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_program(const std::string &frag_glsl, std::string &err) {
    static const char *vert_src =
        "#version 330 core\n"
        "layout(location = 0) in vec2 a_pos;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

    GLuint vs = compile_stage(GL_VERTEX_SHADER, vert_src, err);
    if (!vs) return 0;

    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, frag_glsl.c_str(), err);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[4096];
        glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        err = buf;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ── Error overlay ──────────────────────────────────────────────────────────────
// Renders a semi-transparent dark panel with white text using a baked 8×8
// bitmap font.  No external dependencies.

// 8×8 bitmap font covering ASCII 32–127 (96 glyphs).
// Each glyph is 8 bytes (one byte per row, MSB = leftmost pixel).
// Source: a minimal hand-picked subset of the classic IBM CP437 8×8 font.
static const unsigned char kFont8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00}, // 33 !
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, // 34 "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 35 #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 36 $  (unused but kept for index)
    {0x63,0x33,0x18,0x0C,0x06,0x33,0x31,0x00}, // 37 %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 38 &
    {0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, // 39 '
    {0x0C,0x06,0x03,0x03,0x03,0x06,0x0C,0x00}, // 40 (
    {0x18,0x30,0x60,0x60,0x60,0x30,0x18,0x00}, // 41 )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 43 +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x0C}, // 44 ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 45 -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46 .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 47 /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 48 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 49 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 50 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 51 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 52 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 53 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 54 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 55 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 56 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 57 9
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // 58 :
    {0x00,0x18,0x18,0x00,0x18,0x18,0x0C,0x00}, // 59 ;
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // 60 <
    {0x00,0x00,0x3F,0x00,0x3F,0x00,0x00,0x00}, // 61 =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 62 >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 63 ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 64 @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 65 A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 66 B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 67 C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 68 D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 69 E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 70 F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 71 G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 72 H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 73 I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 74 J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 75 K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 76 L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 77 M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 78 N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 79 O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 80 P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 81 Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 82 R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 83 S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 84 T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 85 U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 86 V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 87 W
    {0x63,0x63,0x36,0x1C,0x36,0x63,0x63,0x00}, // 88 X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 89 Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 90 Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // 91 [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 92 backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // 93 ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // 94 ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, // 95 _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // 96 `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 97 a
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 98 b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 99 c
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, // 100 d
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, // 101 e
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, // 102 f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 103 g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 104 h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 105 i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 106 j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 107 k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 108 l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 109 m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 110 n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 111 o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 112 p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 113 q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 114 r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 115 s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 116 t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 117 u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 118 v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 119 w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 120 x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 121 y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 122 z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // 123 {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 124 |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // 125 }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // 126 ~
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 127 DEL
};

struct ErrorOverlay {
    GLuint prog       = 0;
    GLuint font_tex   = 0;
    bool   ready      = false;

    void init() {
        // Fragment shader: draws a dark panel covering the top portion of the
        // screen, then renders each character of the error string by sampling
        // the baked font texture.
        static const char *vert =
            "#version 330 core\n"
            "layout(location=0) in vec2 a_pos;\n"
            "void main(){ gl_Position=vec4(a_pos,0,1); }\n";

        static const char *frag =
            "#version 330 core\n"
            "uniform sampler2D u_font;\n"      // 768×8 R8 atlas (96 glyphs × 8 px wide)
            "uniform vec2      u_res;\n"
            "uniform int       u_chars[512];\n" // codepoints, 0-terminated
            "uniform int       u_cols;\n"       // chars per line (for wrapping)
            "out vec4 frag_out;\n"
            "void main() {\n"
            "    vec2 fc = gl_FragCoord.xy;\n"
            // Flip y so row 0 is at the top.
            "    float y = u_res.y - fc.y;\n"
            "    float x = fc.x;\n"
            // Only draw inside the panel (top of screen).
            "    int GW = 8; int GH = 8;\n"     // glyph size in pixels
            "    int PAD = 6;\n"                 // padding in pixels
            "    int col = int(x - float(PAD)) / GW;\n"
            "    int row = int(y - float(PAD)) / GH;\n"
            "    int px  = int(x - float(PAD)) - col * GW;\n"
            "    int py  = int(y - float(PAD)) - row * GH;\n"
            // Determine which character index to look up.
            "    int ci = row * u_cols + col;\n"
            "    bool in_text = (col >= 0 && col < u_cols && row >= 0\n"
            "                    && px >= 0 && px < GW && py >= 0 && py < GH\n"
            "                    && ci < 512 && u_chars[ci] != 0);\n"
            // Panel height in pixels = enough rows of glyphs + padding.
            "    int num_rows = 0;\n"
            "    for(int i=0;i<512;i++){ if(u_chars[i]==0) break;\n"
            "        num_rows = (i / u_cols) + 1; }\n"
            "    float panel_h = float(num_rows * GH + PAD * 2);\n"
            "    if(y > panel_h){ discard; }\n"
            // Dark semi-transparent background.
            "    vec4 bg = vec4(0.05, 0.0, 0.0, 0.88);\n"
            "    if(!in_text){ frag_out = bg; return; }\n"
            // Sample font atlas: glyph index = codepoint - 32.
            "    int glyph = u_chars[ci] - 32;\n"
            "    if(glyph < 0 || glyph >= 96){ frag_out = bg; return; }\n"
            // Atlas is 768 wide (96*8), 8 tall.
            "    float u = (float(glyph * GW + px) + 0.5) / 768.0;\n"
            "    float v = (float(py) + 0.5) / 8.0;\n"
            "    float bit = texture(u_font, vec2(u, v)).r;\n"
            "    vec4 fg = vec4(1.0, 0.85, 0.3, 1.0);\n"   // amber text
            "    frag_out = mix(bg, fg, bit);\n"
            "}\n";

        std::string err;
        GLuint vs = compile_stage(GL_VERTEX_SHADER,   vert, err);
        GLuint fs = compile_stage(GL_FRAGMENT_SHADER, frag, err);
        if (!vs || !fs) { if(vs) glDeleteShader(vs); if(fs) glDeleteShader(fs); return; }
        prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs); glDeleteShader(fs);

        // Build font texture: 768×8, one bit per pixel expanded to R8.
        std::vector<unsigned char> atlas(768 * 8, 0);
        for (int g = 0; g < 96; ++g) {
            for (int row = 0; row < 8; ++row) {
                unsigned char byte = kFont8x8[g][row];
                for (int bit = 0; bit < 8; ++bit) {
                    int x = g * 8 + bit;
                    atlas[row * 768 + x] = (byte & (0x80 >> bit)) ? 255 : 0;
                }
            }
        }
        glGenTextures(1, &font_tex);
        glBindTexture(GL_TEXTURE_2D, font_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 768, 8, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        ready = true;
    }

    // Draw the error overlay.  msg must be non-empty.
    void draw(const std::string &msg, int win_w, int win_h) {
        if (!ready) return;

        // Word-wrap / hard-wrap the message into columns.
        const int COLS = (win_w - 12) / 8;  // chars per line (with padding)
        if (COLS <= 0) return;

        // Pack codepoints (with newline → wrap) into u_chars array.
        int char_data[512] = {};
        int ci = 0;
        int col_cur = 0;
        for (char c : msg) {
            if (ci >= 511) break;
            if (c == '\n') {
                // Pad to end of line.
                while (col_cur < COLS && ci < 511) { char_data[ci++] = ' '; col_cur++; }
                col_cur = 0;
                continue;
            }
            if (col_cur >= COLS) {
                // Hard-wrap.
                col_cur = 0;
            }
            char_data[ci++] = (unsigned char)c;
            col_cur++;
        }
        // Zero-terminate.
        if (ci < 512) char_data[ci] = 0;

        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "u_res"),
                    (float)win_w, (float)win_h);
        glUniform1i(glGetUniformLocation(prog, "u_cols"), COLS);
        glUniform1iv(glGetUniformLocation(prog, "u_chars"), 512, char_data);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, font_tex);
        glUniform1i(glGetUniformLocation(prog, "u_font"), 0);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(g_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }

    void destroy() {
        if (prog)     { glDeleteProgram(prog);    prog     = 0; }
        if (font_tex) { glDeleteTextures(1, &font_tex); font_tex = 0; }
        ready = false;
    }
};

// ── LuaJIT helpers ────────────────────────────────────────────────────────────

static ShaderHandle *load_shader_from_lua(lua_State *L, const std::string &lua_path) {
    lua_pushnil(L); lua_setglobal(L, "__luagpu_shader");
    if (luaL_dofile(L, lua_path.c_str()) != 0) {
        fprintf(stderr, "[luaGPU] Lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return nullptr;
    }
    if (lua_type(L, -1) == LUA_TUSERDATA) {
        ShaderHandle *h = static_cast<ShaderHandle *>(
            luaL_testudata(L, -1, "luagpu.ShaderHandle"));
        if (h) return h;
        lua_pop(L, 1);
    }
    lua_getglobal(L, "__luagpu_shader");
    ShaderHandle *h = static_cast<ShaderHandle *>(
        luaL_testudata(L, -1, "luagpu.ShaderHandle"));
    if (!h) {
        lua_pop(L, 1);
        fprintf(stderr,
            "[luaGPU] No ShaderHandle found. Make sure the script returns shader(fn) "
            "or assigns it to __luagpu_shader.\n");
    }
    return h;
}

// ── GLFW callbacks ─────────────────────────────────────────────────────────────

static std::atomic<bool> g_reload_requested { false };
static double g_mouse_x = 0.0, g_mouse_y = 0.0;
static int    g_win_w = 0, g_win_h = 0;

static void key_cb(GLFWwindow *win, int key, int /*sc*/, int action, int /*mod*/) {
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(win, GLFW_TRUE);
        if (key == GLFW_KEY_R)
            g_reload_requested.store(true, std::memory_order_relaxed);
    }
}

static void cursor_cb(GLFWwindow * /*win*/, double x, double y) {
    g_mouse_x = x;
    g_mouse_y = y;
}

static void framebuffer_size_cb(GLFWwindow * /*win*/, int w, int h) {
    g_win_w = w; g_win_h = h;
    glViewport(0, 0, w, h);
}

// ── File watcher ───────────────────────────────────────────────────────────────

static std::atomic<bool> g_watcher_stop { false };

static void watcher_thread(std::string watch_dir, std::string watch_file) {
    HANDLE hDir = CreateFileA(
        watch_dir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (hDir == INVALID_HANDLE_VALUE) return;

    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    alignas(DWORD) char buf[4096];

    while (!g_watcher_stop.load(std::memory_order_relaxed)) {
        ResetEvent(ov.hEvent);
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(
                hDir, buf, sizeof(buf), FALSE,
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                &bytes, &ov, nullptr))
            break;

        while (!g_watcher_stop.load(std::memory_order_relaxed)) {
            if (WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0) break;
        }
        if (g_watcher_stop.load(std::memory_order_relaxed)) break;

        if (!GetOverlappedResult(hDir, &ov, &bytes, FALSE) || bytes == 0) continue;

        FILE_NOTIFY_INFORMATION *info =
            reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buf);
        bool matched = false;
        for (;;) {
            if (info->Action == FILE_ACTION_MODIFIED ||
                info->Action == FILE_ACTION_ADDED    ||
                info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                char narrow[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0,
                    info->FileName,
                    static_cast<int>(info->FileNameLength / sizeof(WCHAR)),
                    narrow, MAX_PATH - 1, nullptr, nullptr);
                if (_stricmp(narrow, watch_file.c_str()) == 0) { matched = true; break; }
            }
            if (!info->NextEntryOffset) break;
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(
                reinterpret_cast<char *>(info) + info->NextEntryOffset);
        }
        if (matched)
            g_reload_requested.store(true, std::memory_order_relaxed);
    }

    CloseHandle(ov.hEvent);
    CloseHandle(hDir);
}

// ── Log ────────────────────────────────────────────────────────────────────────

static FILE *g_log = nullptr;
static void log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

// ── main ───────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    fopen_s(&g_log, "luagpu.log", "w");

    if (argc < 2) {
        log("Usage: luagpu <shader.lua> [width] [height]\n");
        return 1;
    }

    const std::string lua_path = argv[1];
    int win_w = argc > 2 ? std::atoi(argv[2]) : 800;
    int win_h = argc > 3 ? std::atoi(argv[3]) : 600;

    log("[luaGPU] starting: %s %dx%d\n", lua_path.c_str(), win_w, win_h);

    // ── GLFW init ──────────────────────────────────────────────────────────────
    if (!glfwInit()) { log("[luaGPU] glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow *window = glfwCreateWindow(win_w, win_h, "luaGPU", nullptr, nullptr);
    if (!window) { log("[luaGPU] glfwCreateWindow failed\n"); glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetKeyCallback(window, key_cb);
    glfwSetCursorPosCallback(window, cursor_cb);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_cb);
    glfwGetFramebufferSize(window, &g_win_w, &g_win_h);

    // ── GLAD ───────────────────────────────────────────────────────────────────
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        log("[luaGPU] gladLoadGLLoader failed\n");
        glfwDestroyWindow(window); glfwTerminate(); return 1;
    }
    glViewport(0, 0, g_win_w, g_win_h);

    // ── LuaJIT state ──────────────────────────────────────────────────────────
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    register_shader_functions(L);

    // ── Quad geometry ──────────────────────────────────────────────────────────
    init_quad();

    // ── Error overlay ──────────────────────────────────────────────────────────
    ErrorOverlay overlay;
    overlay.init();

    // ── Load initial shader ────────────────────────────────────────────────────
    GLuint prog = 0;
    std::string error_msg;

    auto try_load = [&]() -> bool {
        log("[luaGPU] loading shader from %s\n", lua_path.c_str());
        ShaderHandle *h = load_shader_from_lua(L, lua_path);
        if (!h) {
            // Extract error from Lua (already printed to stderr).
            // Re-run just to capture it into error_msg cleanly.
            lua_pushnil(L); lua_setglobal(L, "__luagpu_shader");
            if (luaL_dofile(L, lua_path.c_str()) != 0) {
                error_msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
                lua_pop(L, 1);
            } else {
                error_msg = "shader() was not called or returned no handle";
                lua_settop(L, 0);
            }
            log("[luaGPU] error: %s\n", error_msg.c_str());
            return false;
        }

        std::string gl_err;
        GLuint new_prog = link_program(h->glsl, gl_err);
        if (!new_prog) {
            error_msg = "GLSL link error:\n" + gl_err;
            log("[luaGPU] OpenGL link error:\n%s\n", gl_err.c_str());
            log("--- GLSL ---\n%s\n--- end ---\n", h->glsl.c_str());
            return false;
        }

        if (prog) glDeleteProgram(prog);
        prog = new_prog;
        error_msg.clear();
        log("[luaGPU] Shader loaded ok: %s\n", lua_path.c_str());
        return true;
    };

    // Initial load — stay open even on failure so the overlay shows the error.
    try_load();

    // ── File watcher thread ────────────────────────────────────────────────────
    std::string watch_dir, watch_file;
    {
        size_t slash = lua_path.find_last_of("/\\");
        if (slash == std::string::npos) {
            watch_dir  = ".";
            watch_file = lua_path;
        } else {
            watch_dir  = lua_path.substr(0, slash);
            watch_file = lua_path.substr(slash + 1);
        }
    }
    std::thread watcher(watcher_thread, watch_dir, watch_file);
    log("[luaGPU] watching %s for changes\n", lua_path.c_str());

    // ── Render loop ────────────────────────────────────────────────────────────
    using Clock = std::chrono::steady_clock;
    auto t0     = Clock::now();
    auto t_prev = t0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (g_reload_requested.load(std::memory_order_relaxed)) {
            g_reload_requested.store(false, std::memory_order_relaxed);
            lua_close(L);
            L = luaL_newstate();
            luaL_openlibs(L);
            register_shader_functions(L);
            try_load();
        }

        auto  t_now   = Clock::now();
        float u_time  = std::chrono::duration<float>(t_now - t0).count();
        float u_delta = std::chrono::duration<float>(t_now - t_prev).count();
        t_prev = t_now;

        glClear(GL_COLOR_BUFFER_BIT);

        if (prog) {
            glUseProgram(prog);

            GLint loc;
            loc = glGetUniformLocation(prog, "u_time");
            if (loc >= 0) glUniform1f(loc, u_time);

            loc = glGetUniformLocation(prog, "u_delta");
            if (loc >= 0) glUniform1f(loc, u_delta);

            loc = glGetUniformLocation(prog, "u_resolution");
            if (loc >= 0) glUniform2f(loc,
                static_cast<float>(g_win_w), static_cast<float>(g_win_h));

            loc = glGetUniformLocation(prog, "u_mouse");
            if (loc >= 0) glUniform2f(loc,
                g_win_w > 0 ? static_cast<float>(g_mouse_x) / g_win_w : 0.f,
                g_win_h > 0 ? 1.f - static_cast<float>(g_mouse_y) / g_win_h : 0.f);

            glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
        }

        // Draw error overlay on top if there's an active error.
        if (!error_msg.empty())
            overlay.draw(error_msg, g_win_w, g_win_h);

        glfwSwapBuffers(window);
    }

    // ── Cleanup ────────────────────────────────────────────────────────────────
    g_watcher_stop.store(true, std::memory_order_relaxed);
    watcher.join();
    overlay.destroy();
    if (prog) glDeleteProgram(prog);
    glDeleteBuffers(1, &g_vbo);
    glDeleteVertexArrays(1, &g_vao);
    lua_close(L);
    glfwDestroyWindow(window);
    glfwTerminate();
    if (g_log) fclose(g_log);
    return 0;
}

#include <glad/glad.h>
#include <GLFW/glfw3.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "ShaderCompiler.h"
#include "Compiler.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
