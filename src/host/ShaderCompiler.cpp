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
    if (h) {
        if (h->lua_closure_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, h->lua_closure_ref);
        h->~ShaderHandle();
    }
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

// ── Path resolver ─────────────────────────────────────────────────────────────

static PathResolverFn g_path_resolver = nullptr;

void set_path_resolver(PathResolverFn fn) {
    g_path_resolver = fn;
}

// ── Host-injected uniforms ────────────────────────────────────────────────────

static std::vector<InjectedUniform> g_injected_uniforms;

void set_injected_uniforms(const std::vector<InjectedUniform> &uniforms) {
    g_injected_uniforms = uniforms;
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
        // Source is a file path — may be a virtual engine path.
        std::string path(ar.source + 1);
        src_name = path;

        // Resolve through the host engine's VFS if a resolver is registered.
        if (g_path_resolver) {
            char resolved[4096];
            if (g_path_resolver(path.c_str(), resolved, sizeof(resolved)))
                path = resolved;
        }

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

// Attempt to classify a plain Lua table (at stack top) as a vecN or matN type.
// Returns GlslType::Unknown if it doesn't match.
// On bad content (non-numeric elements, non-square nested arrays) calls
// luaL_error(), which throws — caller must not pop the value after this.
static GlslType classify_table(lua_State *L, const char *upvalue_name) {
    // Check if it looks like a nested (matrix) table: first element is a table.
    lua_rawgeti(L, -1, 1);
    bool first_is_table = lua_istable(L, -1);
    lua_pop(L, 1);

    int outer_len = (int)lua_objlen(L, -1);

    if (first_is_table) {
        // Nested table — must be 2×2, 3×3, or 4×4 of numbers.
        if (outer_len < 2 || outer_len > 4)
            return GlslType::Unknown;  // not a recognised mat size

        for (int row = 1; row <= outer_len; ++row) {
            lua_rawgeti(L, -1, row);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                luaL_error(L, "upvalue '%s': expected a square table of tables "
                              "(mat2/mat3/mat4) but row %d is not a table",
                              upvalue_name, row);
            }
            int col_len = (int)lua_objlen(L, -1);
            if (col_len != outer_len) {
                lua_pop(L, 1);
                luaL_error(L, "upvalue '%s': matrix table is not square "
                              "(%dx%d)", upvalue_name, outer_len, col_len);
            }
            for (int col = 1; col <= col_len; ++col) {
                lua_rawgeti(L, -1, col);
                if (!lua_isnumber(L, -1)) {
                    lua_pop(L, 2);
                    luaL_error(L, "upvalue '%s': matrix element [%d][%d] "
                                  "is not a number", upvalue_name, row, col);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);  // pop row table
        }

        switch (outer_len) {
            case 2: return GlslType::Mat2;
            case 3: return GlslType::Mat3;
            case 4: return GlslType::Mat4;
        }
    } else {
        // Flat table — must be 2, 3, or 4 numbers → vecN.
        if (outer_len < 2 || outer_len > 4)
            return GlslType::Unknown;

        for (int j = 1; j <= outer_len; ++j) {
            lua_rawgeti(L, -1, j);
            if (!lua_isnumber(L, -1)) {
                lua_pop(L, 1);
                luaL_error(L, "upvalue '%s': vec table element [%d] "
                              "is not a number", upvalue_name, j);
            }
            lua_pop(L, 1);
        }

        switch (outer_len) {
            case 2: return GlslType::Vec2;
            case 3: return GlslType::Vec3;
            case 4: return GlslType::Vec4;
        }
    }
    return GlslType::Unknown;
}


void inspect_upvalues(lua_State *L, int fn_idx,
                      std::vector<UniformDesc>   &out_uniforms,
                      std::vector<ShaderLibDesc> &out_libs) {
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

        } else if (ltype == LUA_TNUMBER) {
            // Plain Lua numbers are treated as float uniforms.
            // (LuaJIT also has integer subtypes but lua_tointeger would truncate;
            // GLSL shaders overwhelmingly expect float for scalar upvalues.)
            ty = TypeInfo::make(GlslType::Float);

        } else if (ltype == LUA_TBOOLEAN) {
            ty = TypeInfo::make(GlslType::Bool);

        } else if (ltype == LUA_TNIL) {
            // Nil upvalue at compile time — will be caught by the compiler
            // as an undefined binding; skip silently here and let the
            // compiler emit the error with a proper source location.
            lua_pop(L, 1);
            continue;

        } else if (ltype == LUA_TTABLE) {
            // Check if this is a shaderlib table (__is_shaderlib == true).
            lua_getfield(L, -1, "__is_shaderlib");
            bool is_shaderlib = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);  // pop __is_shaderlib value

            if (is_shaderlib) {
                ShaderLibDesc desc;
                desc.name = name;

                lua_getfield(L, -1, "__source");
                const char *src = lua_tostring(L, -1);
                desc.src_text = src ? src : "";
                lua_pop(L, 1);

                lua_getfield(L, -1, "__src_name");
                const char *sname = lua_tostring(L, -1);
                desc.src_name = sname ? sname : name;
                lua_pop(L, 1);

                out_libs.push_back(std::move(desc));
                // Whether shaderlib or unrecognized table, skip as a uniform.
                lua_pop(L, 1);
                continue;
            }

            // Not a shaderlib — try to classify as vecN / matN.
            GlslType tgt = classify_table(L, name);  // may luaL_error
            if (tgt != GlslType::Unknown) {
                ty = TypeInfo::make(tgt);
                // Fall through to the UniformDesc push below.
            } else {
                // Unrecognised table — skip.
                lua_pop(L, 1);
                continue;
            }
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
        out_uniforms.push_back(std::move(ud));

        lua_pop(L, 1);
    }
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

    // 2. Inspect upvalues for uniforms and shaderlib references.
    std::vector<UniformDesc>   uniforms;
    std::vector<ShaderLibDesc> shaderlibs;
    inspect_upvalues(L, 1, uniforms, shaderlibs);

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
    CompileResult result = compiler.compile(sf, uniforms, shaderlibs, g_injected_uniforms);

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

    // Keep a registry reference to the outer closure so the render loop can
    // call lua_getupvalue on it each frame to read current uniform values.
    lua_pushvalue(L, 1);                       // push closure
    handle.lua_closure_ref = luaL_ref(L, LUA_REGISTRYINDEX);

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
