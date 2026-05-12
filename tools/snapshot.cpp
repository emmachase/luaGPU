// luaGPU snapshot — headless multi-frame render + 16-colour quantization
//
// Usage: snapshot <shader.lua> [frames] [duration] [width] [height]
//
//   frames   — number of frames to sample (default: 16)
//   duration — time span in seconds to sample over (default: 4.0)
//   width    — render width  in pixels (default: 800)
//   height   — render height in pixels (default: 600)
//
// Opens a hidden GLFW/OpenGL 3.3 context, compiles the given Lua shader, then
// renders `frames` frames at evenly-spaced time values in [0, duration).
// All pixel data from every frame is pooled into one large sample set, which
// is then quantized to 16 colours using a recursive median-cut algorithm.
// The resulting palette is printed to stdout as "R G B" triples (one per
// line), prefixed by a comment header.

#include <glad/glad.h>
#include <GLFW/glfw3.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "ShaderCompiler.h"
#include "Compiler.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Shared helpers (quad, compile/link, shader load)
// ─────────────────────────────────────────────────────────────────────────────

static const float kQuadVerts[] = { -1.f,-1.f, 1.f,-1.f, -1.f,1.f, 1.f,1.f };
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

static ShaderHandle *load_shader_from_lua(lua_State *L, const std::string &lua_path) {
    lua_pushnil(L); lua_setglobal(L, "__luagpu_shader");
    if (luaL_dofile(L, lua_path.c_str()) != 0) {
        fprintf(stderr, "[snapshot] Lua error: %s\n", lua_tostring(L, -1));
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
        fprintf(stderr, "[snapshot] No ShaderHandle found.\n");
    }
    return h;
}

// ─────────────────────────────────────────────────────────────────────────────
// Median-cut colour quantizer
// ─────────────────────────────────────────────────────────────────────────────

struct RGB { unsigned char r, g, b; };

struct Bucket {
    std::vector<int> indices;
    unsigned char min_r, max_r;
    unsigned char min_g, max_g;
    unsigned char min_b, max_b;

    void compute_bounds(const std::vector<RGB> &pixels) {
        min_r = max_r = pixels[indices[0]].r;
        min_g = max_g = pixels[indices[0]].g;
        min_b = max_b = pixels[indices[0]].b;
        for (int i : indices) {
            min_r = std::min(min_r, pixels[i].r);
            max_r = std::max(max_r, pixels[i].r);
            min_g = std::min(min_g, pixels[i].g);
            max_g = std::max(max_g, pixels[i].g);
            min_b = std::min(min_b, pixels[i].b);
            max_b = std::max(max_b, pixels[i].b);
        }
    }

    int longest_axis() const {
        int dr = max_r - min_r;
        int dg = max_g - min_g;
        int db = max_b - min_b;
        if (dr >= dg && dr >= db) return 0;
        if (dg >= db)             return 1;
        return 2;
    }

    RGB average_colour(const std::vector<RGB> &pixels) const {
        long long sr = 0, sg = 0, sb = 0;
        for (int i : indices) { sr += pixels[i].r; sg += pixels[i].g; sb += pixels[i].b; }
        long long n = (long long)indices.size();
        return { (unsigned char)(sr/n), (unsigned char)(sg/n), (unsigned char)(sb/n) };
    }
};

static std::pair<Bucket, Bucket> split_bucket(Bucket &b, const std::vector<RGB> &pixels) {
    int axis = b.longest_axis();
    std::sort(b.indices.begin(), b.indices.end(),
        [&](int a, int bb) {
            if (axis == 0) return pixels[a].r < pixels[bb].r;
            if (axis == 1) return pixels[a].g < pixels[bb].g;
            return pixels[a].b < pixels[bb].b;
        });
    size_t mid = b.indices.size() / 2;
    Bucket lo, hi;
    lo.indices.assign(b.indices.begin(), b.indices.begin() + (int)mid);
    hi.indices.assign(b.indices.begin() + (int)mid, b.indices.end());
    lo.compute_bounds(pixels);
    hi.compute_bounds(pixels);
    return { std::move(lo), std::move(hi) };
}

static std::vector<RGB> median_cut(const std::vector<RGB> &pixels, int num_colours) {
    assert(num_colours >= 1);

    std::vector<Bucket> buckets(1);
    buckets[0].indices.resize(pixels.size());
    for (int i = 0; i < (int)pixels.size(); ++i) buckets[0].indices[i] = i;
    buckets[0].compute_bounds(pixels);

    while ((int)buckets.size() < num_colours) {
        int best = 0, best_vol = -1;
        for (int i = 0; i < (int)buckets.size(); ++i) {
            if (buckets[i].indices.size() <= 1) continue;
            int vol = (buckets[i].max_r - buckets[i].min_r + 1)
                    * (buckets[i].max_g - buckets[i].min_g + 1)
                    * (buckets[i].max_b - buckets[i].min_b + 1);
            if (vol > best_vol) { best_vol = vol; best = i; }
        }
        if (best_vol <= 0) break;

        auto [lo, hi] = split_bucket(buckets[best], pixels);
        buckets.erase(buckets.begin() + best);
        buckets.push_back(std::move(lo));
        buckets.push_back(std::move(hi));
    }

    std::vector<RGB> palette;
    palette.reserve(buckets.size());
    for (auto &bk : buckets)
        if (!bk.indices.empty())
            palette.push_back(bk.average_colour(pixels));
    while ((int)palette.size() < num_colours)
        palette.push_back({0,0,0});
    return palette;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: snapshot <shader.lua> [frames] [duration] [width] [height]\n");
        return 1;
    }

    const std::string lua_path = argv[1];
    const int         n_frames = argc > 2 ? std::atoi(argv[2])           : 16;
    const float       duration = argc > 3 ? (float)std::atof(argv[3])    : 4.f;
    const int         width    = argc > 4 ? std::atoi(argv[4])           : 800;
    const int         height   = argc > 5 ? std::atoi(argv[5])           : 600;
    const int         N_COLOURS = 16;

    if (n_frames < 1) { fprintf(stderr, "[snapshot] frames must be >= 1\n"); return 1; }
    if (duration <= 0.f) { fprintf(stderr, "[snapshot] duration must be > 0\n"); return 1; }

    fprintf(stderr, "[snapshot] %s — %d frame(s) over %.2fs at %dx%d\n",
            lua_path.c_str(), n_frames, (double)duration, width, height);

    // ── GLFW / OpenGL (hidden window) ─────────────────────────────────────────
    if (!glfwInit()) { fprintf(stderr, "[snapshot] glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow *window = glfwCreateWindow(width, height, "snapshot", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "[snapshot] glfwCreateWindow failed\n");
        glfwTerminate(); return 1;
    }
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        fprintf(stderr, "[snapshot] gladLoadGLLoader failed\n");
        glfwDestroyWindow(window); glfwTerminate(); return 1;
    }
    glViewport(0, 0, width, height);

    // ── LuaJIT + shader load ──────────────────────────────────────────────────
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    register_shader_functions(L);

    ShaderHandle *h = load_shader_from_lua(L, lua_path);
    if (!h) {
        lua_close(L);
        glfwDestroyWindow(window); glfwTerminate(); return 1;
    }

    // ── Build and link GLSL program ───────────────────────────────────────────
    std::string gl_err;
    std::string wrapped_glsl =
        "#version 330 core\n"
        "\n"
        "uniform vec2  u_resolution;\n"
        "uniform float u_time;\n"
        "uniform float u_delta;\n"
        "uniform vec2  u_mouse;\n"
        "\n"
        "out vec4 frag_out;\n"
        "\n"
        + h->glsl +
        "\n"
        "void main() {\n"
        "    vec2 uv = gl_FragCoord.xy / u_resolution;\n"
        "    frag_out = shader_main(uv);\n"
        "}\n";

    GLuint prog = link_program(wrapped_glsl, gl_err);
    if (!prog) {
        fprintf(stderr, "[snapshot] GLSL link error:\n%s\n", gl_err.c_str());
        lua_close(L);
        glfwDestroyWindow(window); glfwTerminate(); return 1;
    }

    init_quad();

    // Cache uniform locations.
    const GLint loc_res   = glGetUniformLocation(prog, "u_resolution");
    const GLint loc_time  = glGetUniformLocation(prog, "u_time");
    const GLint loc_delta = glGetUniformLocation(prog, "u_delta");
    const GLint loc_mouse = glGetUniformLocation(prog, "u_mouse");

    // ── Sample n_frames frames, quantizing each one independently ────────────
    const int              frame_pixels = width * height;
    std::vector<unsigned char> raw(frame_pixels * 3);
    std::vector<RGB>           pixels(frame_pixels);

    // Time step between samples; if only one frame, sample at t=0.
    const float t_step = n_frames > 1 ? duration / (float)(n_frames - 1) : 0.f;

    // ── Render, readback, and quantize all frames first ──────────────────────
    struct FrameResult { float time; std::vector<RGB> palette; };
    std::vector<FrameResult> results;
    results.reserve(n_frames);

    for (int f = 0; f < n_frames; ++f) {
        const float u_time = f * t_step;

        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        if (loc_res   >= 0) glUniform2f(loc_res,   (float)width, (float)height);
        if (loc_time  >= 0) glUniform1f(loc_time,  u_time);
        if (loc_delta >= 0) glUniform1f(loc_delta, t_step);
        if (loc_mouse >= 0) glUniform2f(loc_mouse, 0.f, 0.f);

        glBindVertexArray(g_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);

        glFlush();
        glFinish();

        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
        for (int i = 0; i < frame_pixels; ++i)
            pixels[i] = { raw[i*3], raw[i*3+1], raw[i*3+2] };

        fprintf(stderr, "[snapshot] frame %d/%d  t=%.3f — quantizing...\n",
                f + 1, n_frames, (double)u_time);

        results.push_back({ u_time, median_cut(pixels, N_COLOURS) });
    }

    fprintf(stderr, "[snapshot] done. emitting Lua table.\n");

    // ── Emit Lua table ────────────────────────────────────────────────────────
    printf("-- snapshot: %s  %d frame(s) over %.2fs  %dx%d  %d colours\n",
           lua_path.c_str(), n_frames, (double)duration, width, height, N_COLOURS);
    printf("return {\n");
    for (int f = 0; f < (int)results.size(); ++f) {
        printf("  { -- frame %d  t=%.6f\n", f, (double)results[f].time);
        for (const RGB &c : results[f].palette)
            printf("    { %3d, %3d, %3d },\n", (int)c.r, (int)c.g, (int)c.b);
        printf("  },\n");
    }
    printf("}\n");

    // ── Cleanup ───────────────────────────────────────────────────────────────
    glDeleteProgram(prog);
    glDeleteBuffers(1, &g_vbo);
    glDeleteVertexArrays(1, &g_vao);
    lua_close(L);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
