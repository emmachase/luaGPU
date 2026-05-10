// test_compiler.cpp
// Minimal self-contained test runner for the Lua→GLSL compiler.
// Build with the 'compiler' static lib; no external test framework needed.

#include "../src/compiler/Parser.h"
#include "../src/compiler/Compiler.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <functional>
#include <vector>
#include <cstring>

// ── tiny test harness ─────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char *expr, const char *file, int line) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::cerr << "FAIL  " << file << ":" << line << "  " << expr << "\n";
    }
}
#define CHECK(cond) check(!!(cond), #cond, __FILE__, __LINE__)

// Check that GLSL output contains a substring.
#define CHECK_CONTAINS(glsl, needle) \
    check((glsl).find(needle) != std::string::npos, \
          "contains(\"" needle "\")", __FILE__, __LINE__)

// Check that GLSL does NOT contain a substring.
#define CHECK_NOT_CONTAINS(glsl, needle) \
    check((glsl).find(needle) == std::string::npos, \
          "not_contains(\"" needle "\")", __FILE__, __LINE__)

// ── helpers ───────────────────────────────────────────────────────────────────

// Read a whole file into a string. Returns empty string on failure.
static std::string read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Compile Lua source with no uniforms; return CompileResult.
static CompileResult compile_src(std::string_view src, const char *name = "test") {
    try {
        ShaderFunc sf = Parser::parse(src, name);
        Compiler   c;
        return c.compile(sf, {});
    } catch (const std::exception &ex) {
        CompileResult r;
        r.ok = false;
        r.errors.push_back({SrcLoc{name, 0}, std::string("exception: ") + ex.what()});
        return r;
    }
}

// Compile and assert it succeeds; return GLSL string.
static std::string compile_ok(std::string_view src, const char *name = "test") {
    auto r = compile_src(src, name);
    if (!r.ok) {
        for (auto &d : r.errors)
            std::cerr << "  error: " << d.message << "\n";
    }
    CHECK(r.ok);
    return r.glsl;
}

// Compile and assert it fails; return error messages joined.
static std::string compile_fail(std::string_view src, const char *name = "test") {
    auto r = compile_src(src, name);
    CHECK(!r.ok);
    std::string msg;
    for (auto &d : r.errors) { msg += d.message; msg += '\n'; }
    return msg;
}

// ── test cases ────────────────────────────────────────────────────────────────

static void test_minimal_shader() {
    // Simplest possible shader: no helpers, no uniforms.
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        return vec4(0.0, 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "minimal");

    CHECK_CONTAINS(glsl, "#version 330 core");
    CHECK_CONTAINS(glsl, "uniform float u_time;");
    CHECK_CONTAINS(glsl, "uniform vec2  u_resolution;");
    CHECK_CONTAINS(glsl, "uniform float u_delta;");
    CHECK_CONTAINS(glsl, "uniform vec2  u_mouse;");
    CHECK_CONTAINS(glsl, "out vec4 frag_out;");
    CHECK_CONTAINS(glsl, "void main()");
    CHECK_CONTAINS(glsl, "gl_FragCoord.xy / u_resolution");
    CHECK_CONTAINS(glsl, "vec4 shader_main(vec2 uv)");
    CHECK_CONTAINS(glsl, "vec4(0.0, 0.0, 0.0, 1.0)");
}

static void test_helper_function() {
    // Helper function should be emitted before shader_main.
    const char *src = R"(
function(u_time, u_resolution)
    local function fade(t)
        return t * t * (3.0 - 2.0 * t)
    end
    return function main(uv)
        return vec4(fade(uv.x), 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "helper");

    CHECK_CONTAINS(glsl, "float fade_float(float t)");
    CHECK_CONTAINS(glsl, "vec4 shader_main(vec2 uv)");

    // fade must appear before shader_main in the output
    auto pos_fade = glsl.find("float fade_float(");
    auto pos_main = glsl.find("vec4 shader_main(");
    CHECK(pos_fade != std::string::npos && pos_main != std::string::npos && pos_fade < pos_main);
}

static void test_if_statement() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local r = 0.0
        if uv.x > 0.5 then
            r = 1.0
        else
            r = 0.0
        end
        return vec4(r, 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "if_stmt");
    CHECK_CONTAINS(glsl, "if (");
    CHECK_CONTAINS(glsl, "else");
}

static void test_for_loop() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local s = 0.0
        for i = 1, 10, 1 do
            s = s + 1.0
        end
        return vec4(s, 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "for_loop");
    CHECK_CONTAINS(glsl, "for (");
}

static void test_while_loop() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local i = 0
        while i < 10 do
            i = i + 1
        end
        return vec4(0.0, 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "while_loop");
    CHECK_CONTAINS(glsl, "while (");
}

static void test_ternary_pattern() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local val = 0.6
        local x = (val > 0.5) and vec3(1.0, 0.0, 0.0) or vec3(0.0, 0.0, 0.0)
        return vec4(x, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "ternary");
    CHECK_CONTAINS(glsl, "?");
    CHECK_CONTAINS(glsl, ":");
}

static void test_swizzle() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local r = uv.x
        local g = uv.y
        return vec4(r, g, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "swizzle");
    CHECK_CONTAINS(glsl, ".x");
    CHECK_CONTAINS(glsl, ".y");
}

static void test_math_builtins() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local s = math.sin(u_time)
        local c = math.cos(u_time)
        return vec4(s, c, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "math");
    CHECK_CONTAINS(glsl, "sin(");
    CHECK_CONTAINS(glsl, "cos(");
}

static void test_glsl_builtins() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local v = normalize(uv)
        local m = mix(0.0, 1.0, v.x)
        return vec4(m, m, m, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "glsl_builtins");
    CHECK_CONTAINS(glsl, "normalize(");
    CHECK_CONTAINS(glsl, "mix(");
}

static void test_struct_from_table() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local p = { x = uv.x, y = uv.y }
        return vec4(p.x, p.y, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "struct");
    // Anonymous struct should be emitted
    CHECK_CONTAINS(glsl, "struct S");
    CHECK_CONTAINS(glsl, ".x");
    CHECK_CONTAINS(glsl, ".y");
}

static void test_not_operator() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local b = not (uv.x > 0.5)
        local r = b and 1.0 or 0.0
        return vec4(r, 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "not_op");
    CHECK_CONTAINS(glsl, "!");
}

static void test_mod_operator_float() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local r = uv.x % 0.5
        return vec4(r, 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "mod_float");
    CHECK_CONTAINS(glsl, "mod(");
}

static void test_ne_operator() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local b = uv.x ~= 0.5
        local r = b and 1.0 or 0.0
        return vec4(r, 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "ne_op");
    CHECK_CONTAINS(glsl, "!=");
}

// ── error cases ───────────────────────────────────────────────────────────────

static void test_error_type_mismatch() {
    // vec3 + mat4 should be a type error
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local v = vec3(1.0, 0.0, 0.0)
        local m = mat4(1.0)
        local bad = v + m
        return vec4(0.0)
    end
end
)";
    auto msg = compile_fail(src, "type_mismatch");
    CHECK(msg.find("type error") != std::string::npos || msg.find("cannot") != std::string::npos);
}

static void test_error_concat_rejected() {
    const char *src = R"(
function(u_time, u_resolution)
    return function main(uv)
        local bad = uv .. uv
        return vec4(0.0)
    end
end
)";
    // Should fail at parse or compile time — either a parse error or type error
    bool threw = false;
    try {
        auto r = compile_src(src, "concat");
        if (!r.ok) threw = true;
    } catch (...) {
        threw = true;
    }
    CHECK(threw);
}

// ── spec example: fade shader ──────────────────────────────────────────────────
static void test_spec_fade_shader() {
    const char *src = R"(
function(u_time, u_resolution)
    local function fade(t)
        return t * t * (3.0 - 2.0 * t)
    end
    return function main(uv)
        return vec4(fade(uv.x), 0.0, 0.0, 1.0)
    end
end
)";
    auto glsl = compile_ok(src, "spec_fade");

    // Structure checks
    CHECK_CONTAINS(glsl, "#version 330 core");
    CHECK_CONTAINS(glsl, "uniform float u_time;");
    CHECK_CONTAINS(glsl, "float fade_float(float t)");
    CHECK_CONTAINS(glsl, "vec4 shader_main(vec2 uv)");
    CHECK_CONTAINS(glsl, "frag_out = shader_main(uv)");

    // fade body contains the formula
    CHECK_CONTAINS(glsl, "3.0");

    // fade defined before shader_main
    auto p_fade = glsl.find("float fade_float(");
    auto p_main = glsl.find("vec4 shader_main(");
    CHECK(p_fade < p_main);
}

// ── example shader file tests ─────────────────────────────────────────────────
// These tests read the .lua files from the examples/ directory relative to the
// repo root (i.e. the working directory when the test binary is run via ctest
// or directly from the build dir with --test-dir set to the repo root).

static void test_example_plasma() {
    std::string src = read_file("examples/plasma.lua");
    CHECK(!src.empty());
    if (src.empty()) return;
    compile_ok(src, "plasma");
}

static void test_example_mandelbrot() {
    std::string src = read_file("examples/mandelbrot.lua");
    CHECK(!src.empty());
    if (src.empty()) return;
    compile_ok(src, "mandelbrot");
}

static void test_example_raymarcher() {
    std::string src = read_file("examples/raymarcher.lua");
    CHECK(!src.empty());
    if (src.empty()) return;
    compile_ok(src, "raymarcher");
}

static void test_example_voronoi() {
    std::string src = read_file("examples/voronoi.lua");
    CHECK(!src.empty());
    if (src.empty()) return;
    compile_ok(src, "voronoi");
}

static void test_example_fbm_landscape() {
    std::string src = read_file("examples/fbm_landscape.lua");
    CHECK(!src.empty());
    if (src.empty()) return;
    compile_ok(src, "fbm_landscape");
}

// ── entry point ───────────────────────────────────────────────────────────────
int main() {
    struct Test { const char *name; std::function<void()> fn; };
    std::vector<Test> tests = {
        {"minimal_shader",      test_minimal_shader},
        {"helper_function",     test_helper_function},
        {"if_statement",        test_if_statement},
        {"for_loop",            test_for_loop},
        {"while_loop",          test_while_loop},
        {"ternary_pattern",     test_ternary_pattern},
        {"swizzle",             test_swizzle},
        {"math_builtins",       test_math_builtins},
        {"glsl_builtins",       test_glsl_builtins},
        {"struct_from_table",   test_struct_from_table},
        {"not_operator",        test_not_operator},
        {"mod_operator_float",  test_mod_operator_float},
        {"ne_operator",         test_ne_operator},
        {"error_type_mismatch", test_error_type_mismatch},
        {"error_concat_rejected", test_error_concat_rejected},
        {"spec_fade_shader",      test_spec_fade_shader},
        {"example_plasma",        test_example_plasma},
        {"example_mandelbrot",    test_example_mandelbrot},
        {"example_raymarcher",    test_example_raymarcher},
        {"example_voronoi",       test_example_voronoi},
        {"example_fbm_landscape", test_example_fbm_landscape},
    };

    for (auto &t : tests) {
        std::cout << "[ RUN  ] " << t.name << "\n";
        t.fn();
        std::cout << (g_fail == 0 ? "[ OK   ] " : "[ FAIL ] ") << t.name << "\n";
        // Reset per-test fail tracking not needed — accumulate overall counts
    }

    std::cout << "\n" << (g_fail == 0 ? "ALL PASSED" : "FAILURES DETECTED")
              << "  pass=" << g_pass << "  fail=" << g_fail << "\n";
    return g_fail ? 1 : 0;
}
