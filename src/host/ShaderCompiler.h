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
    int                      lua_closure_ref = LUA_NOREF; // registry ref to outer closure
};

// ── C API ─────────────────────────────────────────────────────────────────────

// lua_CFunction: shader(fn) → ShaderHandle userdata
// Stack on entry: [1] = the outer closure
int lua_shader(lua_State *L);

// lua_CFunction: shaderlib(fn) → table with __is_shaderlib marker
int lua_shaderlib(lua_State *L);

// Register both functions into the global Lua environment.
void register_shader_functions(lua_State *L);

// ── Host-injected uniforms ────────────────────────────────────────────────────
// Uniforms that the host guarantees to supply in every GL program (e.g.
// engine-wide globals like u_rect, u_viewport_scale).  Set once at startup;
// every subsequent shader() call will make these names available as typed
// symbols inside Lua shaders and emit the corresponding `uniform <type> <name>;`
// declarations in the GLSL output.
//
//   set_injected_uniforms({{"u_rect", GlslType::Vec4},
//                          {"u_viewport_scale", GlslType::Vec2}});
//
// Pass an empty vector to clear previously registered uniforms.
void set_injected_uniforms(const std::vector<InjectedUniform> &uniforms);

// ── Path resolver hook ────────────────────────────────────────────────────────
// Optional callback invoked when extract_source needs to open a file whose
// path came from debug.getinfo (i.e. a virtual/engine path).  The callback
// receives the raw path string and should write the resolved host-OS path into
// out_path (max out_size bytes including NUL).  Return true on success, false
// to fall back to using the raw path unchanged.
//
// Register once at engine startup before any shader() calls are made.
// Pass nullptr to clear a previously registered resolver.
using PathResolverFn = bool(*)(const char *virtual_path,
                               char       *out_path,
                               size_t      out_size);
void set_path_resolver(PathResolverFn fn);

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
// constructor metatable tags) and shaderlib references (__is_shaderlib tables).
// Uniforms are appended to out_uniforms; shaderlibs to out_libs.
void inspect_upvalues(lua_State *L, int fn_idx,
                      std::vector<UniformDesc>    &out_uniforms,
                      std::vector<ShaderLibDesc>  &out_libs);
