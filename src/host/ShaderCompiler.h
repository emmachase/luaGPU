#pragma once
// ShaderCompiler — host-side bridge between the LuaJIT runtime and the
// Lua→GLSL compiler. Called from the C++ implementation of shader() and
// shaderlib().
//
// Dependencies: LuaJIT headers must be on the include path.
// Link against: the compiler static library + LuaJIT.

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "Compiler.h"
#include <string>
#include <vector>

// ── ShaderHandle ──────────────────────────────────────────────────────────────
// Returned to Lua as a userdata. Holds the compiled GLSL + uniform descriptors
// + the upvalue indices needed for per-frame sync.
struct ShaderHandle {
    std::string              glsl;
    std::vector<UniformDesc> uniforms;
    unsigned int             gl_program = 0;  // filled in by the OpenGL layer
};

// ── C API ─────────────────────────────────────────────────────────────────────

// lua_CFunction: shader(fn) → ShaderHandle userdata
// Stack on entry: [1] = the outer closure
int lua_shader(lua_State *L);

// lua_CFunction: shaderlib(fn) → table with __is_shaderlib marker
int lua_shaderlib(lua_State *L);

// Register both functions into the global Lua environment.
void register_shader_functions(lua_State *L);

// ── Source extraction ─────────────────────────────────────────────────────────
// Extract the source text of a Lua function using debug.getinfo.
// Returns false and sets err_msg on failure (e.g. source is a C function).
bool extract_source(lua_State *L, int fn_idx,
                    std::string &src_text,
                    std::string &src_name,
                    int &line_start,
                    int &line_end,
                    std::string &err_msg);

// ── Upvalue inspection ────────────────────────────────────────────────────────
// Walk the upvalues of the function at fn_idx, identify uniforms (via
// constructor metatable tags) and shaderlib references.
std::vector<UniformDesc> inspect_upvalues(lua_State *L, int fn_idx);
