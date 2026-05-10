#include "ShaderCompiler.h"
#include "../compiler/Lexer.h"
#include "../compiler/Parser.h"
#include "../compiler/Compiler.h"
#include <fstream>
#include <sstream>
#include <cstring>

// ── Metatable names for uniform type detection ────────────────────────────────
// These must match what the host engine registers via luaL_newmetatable.
static const char *MT_FLOAT     = "luagpu.float";
static const char *MT_INT       = "luagpu.int";
static const char *MT_BOOL      = "luagpu.bool";
static const char *MT_VEC2      = "luagpu.vec2";
static const char *MT_VEC3      = "luagpu.vec3";
static const char *MT_VEC4      = "luagpu.vec4";
static const char *MT_IVEC2     = "luagpu.ivec2";
static const char *MT_IVEC3     = "luagpu.ivec3";
static const char *MT_IVEC4     = "luagpu.ivec4";
static const char *MT_MAT2      = "luagpu.mat2";
static const char *MT_MAT3      = "luagpu.mat3";
static const char *MT_MAT4      = "luagpu.mat4";
static const char *MT_TEXTURE   = "engine.Texture";
static const char *MT_SHADER    = "luagpu.ShaderHandle";

// ── ShaderHandle userdata GC ──────────────────────────────────────────────────

static int shader_handle_gc(lua_State *L) {
    ShaderHandle *h = static_cast<ShaderHandle *>(lua_touserdata(L, 1));
    if (h) h->~ShaderHandle();
    return 0;
}

static void push_shader_handle(lua_State *L, ShaderHandle &&h) {
    void *ud = lua_newuserdata(L, sizeof(ShaderHandle));
    new (ud) ShaderHandle(std::move(h));
    if (luaL_newmetatable(L, MT_SHADER)) {
        lua_pushcfunction(L, shader_handle_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
}

// ── extract_source ────────────────────────────────────────────────────────────

bool extract_source(lua_State *L, int fn_idx,
                    std::string &src_text,
                    std::string &src_name,
                    int &line_start,
                    int &line_end,
                    std::string &err_msg) {
    lua_Debug ar;
    lua_pushvalue(L, fn_idx);
    if (!lua_getinfo(L, ">Sl", &ar)) {
        err_msg = "debug.getinfo failed";
        return false;
    }

    if (!ar.source || ar.source[0] == '=') {
        err_msg = "cannot extract source: function has no Lua source (C function or built-in)";
        return false;
    }

    line_start = ar.linedefined;
    line_end   = ar.lastlinedefined;

    if (ar.source[0] == '@') {
        // Source is a file path.
        std::string path(ar.source + 1);
        src_name = path;
        std::ifstream f(path);
        if (!f.is_open()) {
            err_msg = "cannot open source file: " + path;
            return false;
        }
        std::string line;
        int lnum = 0;
        std::ostringstream ss;
        while (std::getline(f, line)) {
            ++lnum;
            if (lnum >= line_start && lnum <= line_end) {
                ss << line << "\n";
            }
            if (lnum > line_end) break;
        }
        src_text = ss.str();
    } else {
        // Source is inline chunk text.
        src_name = ar.short_src;
        const std::string full(ar.source);
        std::istringstream iss(full);
        std::string line;
        int lnum = 0;
        std::ostringstream ss;
        while (std::getline(iss, line)) {
            ++lnum;
            if (lnum >= line_start && lnum <= line_end) {
                ss << line << "\n";
            }
            if (lnum > line_end) break;
        }
        src_text = ss.str();
    }

    return true;
}

// ── inspect_upvalues ──────────────────────────────────────────────────────────

std::vector<UniformDesc> inspect_upvalues(lua_State *L, int fn_idx) {
    std::vector<UniformDesc> uniforms;

    for (int i = 1; ; ++i) {
        const char *name = lua_getupvalue(L, fn_idx, i);
        if (!name) break;  // no more upvalues

        // Skip _ENV and other Lua internals.
        if (std::strcmp(name, "_ENV") == 0) {
            lua_pop(L, 1);
            continue;
        }

        int ltype = lua_type(L, -1);
        TypeInfo ty = TypeInfo::unknown();

        if (ltype == LUA_TUSERDATA) {
            // Detect type by metatable name.
            struct { const char *mt; GlslType gt; } table[] = {
                { MT_FLOAT,   GlslType::Float    },
                { MT_INT,     GlslType::Int      },
                { MT_BOOL,    GlslType::Bool     },
                { MT_VEC2,    GlslType::Vec2     },
                { MT_VEC3,    GlslType::Vec3     },
                { MT_VEC4,    GlslType::Vec4     },
                { MT_IVEC2,   GlslType::IVec2    },
                { MT_IVEC3,   GlslType::IVec3    },
                { MT_IVEC4,   GlslType::IVec4    },
                { MT_MAT2,    GlslType::Mat2     },
                { MT_MAT3,    GlslType::Mat3     },
                { MT_MAT4,    GlslType::Mat4     },
                { MT_TEXTURE, GlslType::Sampler2D},
                { nullptr,    GlslType::Unknown  },
            };
            for (int k = 0; table[k].mt; ++k) {
                if (luaL_testudata(L, -1, table[k].mt)) {
                    ty = TypeInfo::make(table[k].gt);
                    break;
                }
            }

        } else if (ltype == LUA_TNIL) {
            // Nil upvalue at compile time — will be caught by the compiler
            // as an undefined binding; skip silently here and let the
            // compiler emit the error with a proper source location.
            lua_pop(L, 1);
            continue;

        } else if (ltype == LUA_TTABLE) {
            // Could be a shaderlib — handled separately; skip as uniform.
            lua_pop(L, 1);
            continue;
        }

        if (ty.tag == GlslType::Unknown) {
            // Unrecognized upvalue type — skip.
            lua_pop(L, 1);
            continue;
        }

        UniformDesc ud;
        ud.name          = name;
        ud.type          = ty;
        ud.upvalue_index = i;
        uniforms.push_back(std::move(ud));

        lua_pop(L, 1);
    }

    return uniforms;
}

// ── lua_shader ────────────────────────────────────────────────────────────────

int lua_shader(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);

    // 1. Extract source text.
    std::string src_text, src_name, err_msg;
    int line_start = 0, line_end = 0;
    if (!extract_source(L, 1, src_text, src_name, line_start, line_end, err_msg)) {
        luaL_error(L, "shader(): %s", err_msg.c_str());
        return 0;
    }

    // 2. Inspect upvalues for uniforms (and shaderlib refs).
    std::vector<UniformDesc> uniforms = inspect_upvalues(L, 1);

    // 3. Parse (Parser takes the source text directly).
    ShaderFunc sf;
    try {
        sf = Parser::parse(src_text, src_name);
    } catch (const std::exception &ex) {
        luaL_error(L, "shader(): parse error: %s", ex.what());
        return 0;
    }

    // 4. Compile.
    Compiler compiler;
    CompileResult result = compiler.compile(sf, uniforms);

    if (!result.ok) {
        // Collect all errors into one string.
        std::ostringstream ss;
        ss << "shader() compilation failed:\n";
        for (auto &e : result.errors)
            ss << "  " << e.loc.file << ":" << e.loc.line << ": " << e.message << "\n";
        luaL_error(L, "%s", ss.str().c_str());
        return 0;
    }

    // 5. Push ShaderHandle userdata.
    ShaderHandle handle;
    handle.glsl     = std::move(result.glsl);
    handle.uniforms = std::move(result.uniforms);
    push_shader_handle(L, std::move(handle));
    return 1;
}

// ── lua_shaderlib ─────────────────────────────────────────────────────────────

int lua_shaderlib(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);

    // Extract source text (needed later when the shader that uses this lib is compiled).
    std::string src_text, src_name, err_msg;
    int line_start = 0, line_end = 0;
    if (!extract_source(L, 1, src_text, src_name, line_start, line_end, err_msg)) {
        luaL_error(L, "shaderlib(): %s", err_msg.c_str());
        return 0;
    }

    // Build a table that represents the shaderlib.
    // It carries:
    //   __is_shaderlib = true   (detection marker)
    //   __source       = src_text
    //   __src_name     = src_name
    //
    // The actual public API table returned by the fn is not needed here — the
    // compiler uses the source text directly and resolves exported names via
    // the function's return statement during pass 1.
    lua_newtable(L);

    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__is_shaderlib");

    lua_pushstring(L, src_text.c_str());
    lua_setfield(L, -2, "__source");

    lua_pushstring(L, src_name.c_str());
    lua_setfield(L, -2, "__src_name");

    // Store the original function so callers can call it at runtime if needed.
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "__fn");

    return 1;
}

// ── register ──────────────────────────────────────────────────────────────────

void register_shader_functions(lua_State *L) {
    lua_pushcfunction(L, lua_shader);
    lua_setglobal(L, "shader");

    lua_pushcfunction(L, lua_shaderlib);
    lua_setglobal(L, "shaderlib");
}
