#pragma once
#include "Types.h"
#include "AST.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <optional>

// ── Binding kinds ──────────────────────────────────────────────────────────────
enum class BindingKind {
    Local,       // local variable or function parameter
    Uniform,     // shader outer-fn parameter (upvalue treated as uniform)
    Function,    // local function declaration
    ShaderLib,   // shaderlib upvalue reference
    Builtin,     // built-in function (math.sin, mix, etc.)
    Constructor, // vec3(), float(), mat4() etc.
    StructType,  // named struct type (Ray, Hit, etc.) — callable as constructor
};

struct Binding {
    std::string  name;
    BindingKind  kind;
    TypeInfo     type;

    // For Function / ShaderLib: pointer to AST body (not owned)
    // For local functions: points into the shader AST
    const Block             *func_body   = nullptr;
    std::vector<std::string> func_params;

    // For ShaderLib: lib name
    std::string lib_name;

    // For Builtin / Constructor: GLSL name to emit
    std::string glsl_name;

    // For StructType: the struct_id in the compiler's structs_ list
    int struct_id = -1;
};

// ── Scope frame ────────────────────────────────────────────────────────────────
struct Scope {
    std::unordered_map<std::string, Binding> bindings;
    Scope *parent = nullptr;

    const Binding *lookup(const std::string &name) const {
        auto it = bindings.find(name);
        if (it != bindings.end()) return &it->second;
        if (parent) return parent->lookup(name);
        return nullptr;
    }

    void define(Binding b) {
        bindings[b.name] = std::move(b);
    }
};

// ── SymbolTable ────────────────────────────────────────────────────────────────
// A stack of scope frames. Push/pop as we enter/leave blocks.
class SymbolTable {
public:
    SymbolTable();  // initialises the global (builtin) scope

    // Deep-copy: clones all scopes and rewires parent pointers.
    // Used by typecheck_instance to get a fresh per-instance scope.
    SymbolTable(const SymbolTable &other);
    SymbolTable &operator=(const SymbolTable &other);

    void push_scope();
    void pop_scope();

    Scope *current() { return current_; }

    // Look up a name, walking parent chain
    const Binding *lookup(const std::string &name) const {
        return current_->lookup(name);
    }

    // Define in current scope
    void define(Binding b) { current_->define(std::move(b)); }

private:
    // Storage for all scopes
    std::vector<std::unique_ptr<Scope>> scopes_;
    Scope *current_ = nullptr;

    void install_builtins();
};
