// luaGPU — main entry point
// Usage: luagpu <shader.lua> [width] [height]
//
// Opens a GLFW window, runs the given .lua file through LuaJIT which calls
// shader() to compile GLSL, then renders a fullscreen quad at 60 fps with
// u_time / u_delta / u_resolution / u_mouse uniforms updated each frame.
// Press R to reload the shader from disk. Press Escape or close the window to quit.

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
    // Minimal pass-through vertex shader — inputs: position at location 0.
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

// ── LuaJIT helpers ────────────────────────────────────────────────────────────

// Run the .lua file and extract the ShaderHandle that shader() pushed.
// Returns 0 on failure (error printed to stderr).
static ShaderHandle *load_shader_from_lua(lua_State *L, const std::string &lua_path) {
    // Reset globals that the script might have set previously.
    lua_pushnil(L); lua_setglobal(L, "__luagpu_shader");

    // Run the file.
    if (luaL_dofile(L, lua_path.c_str()) != 0) {
        fprintf(stderr, "[luaGPU] Lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return nullptr;
    }

    // The convention: after running the script the ShaderHandle is the last
    // return value (if the script ends with `return shader(fn)`), or the
    // script stored it in a global named `__luagpu_shader`.
    // Try the return value on the stack first.
    if (lua_type(L, -1) == LUA_TUSERDATA) {
        ShaderHandle *h = static_cast<ShaderHandle *>(
            luaL_testudata(L, -1, "luagpu.ShaderHandle"));
        if (h) return h;
        lua_pop(L, 1);
    }

    // Fallback: check global.
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

static bool g_reload_requested = false;
static double g_mouse_x = 0.0, g_mouse_y = 0.0;
static int    g_win_w = 0, g_win_h = 0;

static void key_cb(GLFWwindow *win, int key, int /*sc*/, int action, int /*mod*/) {
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(win, GLFW_TRUE);
        if (key == GLFW_KEY_R)
            g_reload_requested = true;
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

// ── main ───────────────────────────────────────────────────────────────────────

// Log to both stderr and a file so we can see output even when the console
// disappears or the process crashes before stdio is flushed.
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
    log("[luaGPU] glfwInit...\n");
    if (!glfwInit()) {
        log("[luaGPU] glfwInit failed\n");
        return 1;
    }
    log("[luaGPU] glfwInit ok\n");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    log("[luaGPU] glfwCreateWindow...\n");
    GLFWwindow *window = glfwCreateWindow(win_w, win_h, "luaGPU", nullptr, nullptr);
    if (!window) {
        log("[luaGPU] glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    log("[luaGPU] window ok\n");
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glfwSetKeyCallback(window, key_cb);
    glfwSetCursorPosCallback(window, cursor_cb);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_cb);

    glfwGetFramebufferSize(window, &g_win_w, &g_win_h);
    // NOTE: glViewport deferred until after gladLoadGLLoader

    // ── GLAD ───────────────────────────────────────────────────────────────────
    log("[luaGPU] gladLoadGLLoader...\n");
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        log("[luaGPU] gladLoadGLLoader failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    log("[luaGPU] glad ok\n");
    glViewport(0, 0, g_win_w, g_win_h);

    // ── LuaJIT state ──────────────────────────────────────────────────────────
    log("[luaGPU] luaL_newstate...\n");
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    register_shader_functions(L);
    log("[luaGPU] Lua state ok\n");

    // ── Quad geometry ──────────────────────────────────────────────────────────
    init_quad();
    log("[luaGPU] quad ok\n");

    // ── Load initial shader ────────────────────────────────────────────────────
    GLuint prog = 0;
    auto try_load = [&]() -> bool {
        log("[luaGPU] loading shader from %s\n", lua_path.c_str());
        ShaderHandle *h = load_shader_from_lua(L, lua_path);
        if (!h) return false;

        log("[luaGPU] linking GL program...\n");
        std::string gl_err;
        GLuint new_prog = link_program(h->glsl, gl_err);
        if (!new_prog) {
            log("[luaGPU] OpenGL link error:\n%s\n", gl_err.c_str());
            log("--- GLSL ---\n%s\n--- end ---\n", h->glsl.c_str());
            return false;
        }

        if (prog) glDeleteProgram(prog);
        prog = new_prog;
        log("[luaGPU] Shader loaded ok: %s\n", lua_path.c_str());
        return true;
    };

    if (!try_load()) {
        log("[luaGPU] Failed to load initial shader — exiting.\n");
        lua_close(L);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ── Render loop ────────────────────────────────────────────────────────────
    using Clock = std::chrono::steady_clock;
    auto t0      = Clock::now();
    auto t_prev  = t0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (g_reload_requested) {
            g_reload_requested = false;
            // Fresh Lua state on reload so stale globals don't bleed through.
            lua_close(L);
            L = luaL_newstate();
            luaL_openlibs(L);
            register_shader_functions(L);
            try_load();  // errors already printed; keep old program if it fails
        }

        auto t_now   = Clock::now();
        float u_time  = std::chrono::duration<float>(t_now - t0).count();
        float u_delta = std::chrono::duration<float>(t_now - t_prev).count();
        t_prev = t_now;

        glClear(GL_COLOR_BUFFER_BIT);

        if (prog) {
            glUseProgram(prog);

            // Built-in uniforms.
            GLint loc;
            loc = glGetUniformLocation(prog, "u_time");
            if (loc >= 0) glUniform1f(loc, u_time);

            loc = glGetUniformLocation(prog, "u_delta");
            if (loc >= 0) glUniform1f(loc, u_delta);

            loc = glGetUniformLocation(prog, "u_resolution");
            if (loc >= 0) glUniform2f(loc,
                static_cast<float>(g_win_w), static_cast<float>(g_win_h));

            // Mouse: normalize to [0,1] with y flipped to match UV convention.
            loc = glGetUniformLocation(prog, "u_mouse");
            if (loc >= 0) glUniform2f(loc,
                g_win_w > 0 ? static_cast<float>(g_mouse_x) / g_win_w : 0.f,
                g_win_h > 0 ? 1.f - static_cast<float>(g_mouse_y) / g_win_h : 0.f);

            glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
        }

        glfwSwapBuffers(window);
    }

    // ── Cleanup ────────────────────────────────────────────────────────────────
    if (prog) glDeleteProgram(prog);
    glDeleteBuffers(1, &g_vbo);
    glDeleteVertexArrays(1, &g_vao);
    lua_close(L);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
