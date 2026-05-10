#include "SymbolTable.h"
#include <stdexcept>

SymbolTable::SymbolTable() {
    // global (builtin) scope
    auto global = std::make_unique<Scope>();
    current_ = global.get();
    scopes_.push_back(std::move(global));
    install_builtins();
}

// ── copy constructor ──────────────────────────────────────────────────────────
// Deep-clones the scope chain. Parent pointers are rewritten to point into the
// new scope vector by matching original addresses.

SymbolTable::SymbolTable(const SymbolTable &other) {
    scopes_.reserve(other.scopes_.size());
    for (auto &s : other.scopes_)
        scopes_.push_back(std::make_unique<Scope>(*s));  // copies bindings map

    // Rewire parent pointers using the index of the original scope pointer.
    for (size_t i = 0; i < other.scopes_.size(); ++i) {
        Scope *orig_parent = other.scopes_[i]->parent;
        if (!orig_parent) {
            scopes_[i]->parent = nullptr;
        } else {
            for (size_t j = 0; j < other.scopes_.size(); ++j) {
                if (other.scopes_[j].get() == orig_parent) {
                    scopes_[i]->parent = scopes_[j].get();
                    break;
                }
            }
        }
    }

    // current_ maps to the same index as other.current_.
    for (size_t i = 0; i < other.scopes_.size(); ++i) {
        if (other.scopes_[i].get() == other.current_) {
            current_ = scopes_[i].get();
            break;
        }
    }
}

SymbolTable &SymbolTable::operator=(const SymbolTable &other) {
    if (this != &other) {
        SymbolTable tmp(other);
        std::swap(scopes_,  tmp.scopes_);
        std::swap(current_, tmp.current_);
    }
    return *this;
}

void SymbolTable::push_scope() {
    auto s = std::make_unique<Scope>();
    s->parent = current_;
    current_ = s.get();
    scopes_.push_back(std::move(s));
}

void SymbolTable::pop_scope() {
    if (!current_->parent)
        throw std::logic_error("SymbolTable: pop on global scope");
    current_ = current_->parent;
    // Don't erase from scopes_ — parent pointers remain valid
}

// ── builtin installation ──────────────────────────────────────────────────────

static void def_builtin(Scope *s, const char *name, const char *glsl,
                        GlslType ret = GlslType::Unknown) {
    Binding b;
    b.name      = name;
    b.kind      = BindingKind::Builtin;
    b.glsl_name = glsl;
    b.type      = TypeInfo::make(ret);
    s->define(std::move(b));
}

static void def_ctor(Scope *s, const char *name, GlslType ret) {
    Binding b;
    b.name      = name;
    b.kind      = BindingKind::Constructor;
    b.glsl_name = name;  // same as Lua name
    b.type      = TypeInfo::make(ret);
    s->define(std::move(b));
}

void SymbolTable::install_builtins() {
    Scope *g = current_;

    // ── constructors ──────────────────────────────────────────────────────
    def_ctor(g, "float",  GlslType::Float);
    def_ctor(g, "int",    GlslType::Int);
    def_ctor(g, "bool",   GlslType::Bool);
    def_ctor(g, "vec2",   GlslType::Vec2);
    def_ctor(g, "vec3",   GlslType::Vec3);
    def_ctor(g, "vec4",   GlslType::Vec4);
    def_ctor(g, "ivec2",  GlslType::IVec2);
    def_ctor(g, "ivec3",  GlslType::IVec3);
    def_ctor(g, "ivec4",  GlslType::IVec4);
    def_ctor(g, "mat2",   GlslType::Mat2);
    def_ctor(g, "mat3",   GlslType::Mat3);
    def_ctor(g, "mat4",   GlslType::Mat4);

    // ── math.* mapped as globals ──────────────────────────────────────────
    // (The compiler also handles "math" as a table namespace separately;
    //  these bare names are in scope as aliases.)
    def_builtin(g, "sin",        "sin");
    def_builtin(g, "cos",        "cos");
    def_builtin(g, "tan",        "tan");
    def_builtin(g, "asin",       "asin");
    def_builtin(g, "acos",       "acos");
    def_builtin(g, "atan",       "atan");
    def_builtin(g, "sqrt",       "sqrt");
    def_builtin(g, "abs",        "abs");
    def_builtin(g, "floor",      "floor");
    def_builtin(g, "ceil",       "ceil");
    def_builtin(g, "max",        "max");
    def_builtin(g, "min",        "min");
    def_builtin(g, "pow",        "pow");
    def_builtin(g, "exp",        "exp");
    def_builtin(g, "log",        "log");

    // ── GLSL-only builtins ────────────────────────────────────────────────
    def_builtin(g, "mix",        "mix");
    def_builtin(g, "clamp",      "clamp");
    def_builtin(g, "smoothstep", "smoothstep");
    def_builtin(g, "step",       "step");
    def_builtin(g, "fract",      "fract");
    def_builtin(g, "mod",        "mod");
    def_builtin(g, "sign",       "sign");
    def_builtin(g, "length",     "length",   GlslType::Float);
    def_builtin(g, "distance",   "distance", GlslType::Float);
    def_builtin(g, "dot",        "dot",      GlslType::Float);
    def_builtin(g, "cross",      "cross",    GlslType::Vec3);
    def_builtin(g, "normalize",  "normalize");
    def_builtin(g, "reflect",    "reflect");
    def_builtin(g, "refract",    "refract");

    // ── texture sampler ───────────────────────────────────────────────────
    def_builtin(g, "texture", "texture", GlslType::Vec4);

    // ── bitwise globals ───────────────────────────────────────────────────
    def_builtin(g, "shl", "<<");
    def_builtin(g, "shr", ">>");

    // ── built-in variables (injected into main scope by the compiler) ─────
    // uv, frag_coord, u_resolution, u_time, u_delta, u_mouse
    // These are not in the global scope but are injected at shader scope.
}
