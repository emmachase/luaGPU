#pragma once
#include "AST.h"
#include "Types.h"
#include "SymbolTable.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

// ── Diagnostic ────────────────────────────────────────────────────────────────
struct Diagnostic {
    SrcLoc      loc;
    std::string message;
};

// ── Struct definition (post constraint solving) ───────────────────────────────
struct StructField {
    std::string name;
    TypeInfo    type;
};
struct StructDef {
    int                      id;
    std::string              glsl_name; // S0, S1, ... (or user_name for named structs)
    std::vector<StructField> fields;    // declaration order
    bool                     is_named = false;
    std::string              user_name;  // e.g. "Ray" (empty for anonymous)
};

// ── Annotated expression type map ─────────────────────────────────────────────
// Maps Expr* → resolved TypeInfo. Each MonoInstance gets its own map because
// the same AST nodes are typed differently per instantiation.
using ExprTypeMap = std::unordered_map<const Expr *, TypeInfo>;

// Maps Stmt* → TypeInfo for uninitialised LocalStmt declarations whose type is
// inferred from the first AssignStmt that targets them.
using StmtTypeMap = std::unordered_map<const Stmt *, TypeInfo>;

// ── Uniform descriptor (for the host) ─────────────────────────────────────────
struct UniformDesc {
    std::string name;
    TypeInfo    type;
    int         upvalue_index; // 1-based, as returned by lua_getupvalue
};

// ── Shaderlib descriptor ───────────────────────────────────────────────────────
// Describes a shaderlib whose functions can be called from the shader.
// The compiler parses src_text, collects exported functions, and monomorphizes
// only those that are reachable from the entry point.
struct ShaderLibDesc {
    std::string name;      // upvalue name used in the shader (e.g. "noise")
    std::string src_text;  // source text of the shaderlib closure body
    std::string src_name;  // file/chunk name for diagnostics
};

// ── Monomorphization key ───────────────────────────────────────────────────────
struct MonoKey {
    std::string           func_name;
    std::string           lib_name;   // empty = shader-local
    std::vector<TypeInfo> arg_types;

    bool operator==(const MonoKey &o) const {
        return func_name == o.func_name &&
               lib_name  == o.lib_name  &&
               arg_types == o.arg_types;
    }
};

struct MonoKeyHash {
    size_t operator()(const MonoKey &k) const {
        size_t h = std::hash<std::string>{}(k.func_name) ^
                   (std::hash<std::string>{}(k.lib_name) << 1);
        for (auto &t : k.arg_types)
            h ^= std::hash<int>{}((int)t.tag) * 2654435761u ^
                 std::hash<int>{}(t.struct_id + 1);
        return h;
    }
};

// Maps a CallExpr* (within a MonoInstance's body) to the emitted function name
// to call. Populated by pass4 after monomorphization resolves names.
using CallNameMap = std::unordered_map<const void *, std::string>;

// ── Monomorphized function instance ───────────────────────────────────────────
// Fully resolved: param types, return type, and a per-instance ExprTypeMap
// built by re-running type inference on the body with concrete param bindings.
struct MonoInstance {
    MonoKey                  key;
    std::string              emitted_name;   // e.g. "fade_0", "fade_vec2"
    TypeInfo                 return_type;
    std::vector<std::string> param_names;
    std::vector<TypeInfo>    param_types;
    const Block             *body;           // pointer into shader AST (not owned)
    ExprTypeMap              expr_types;     // type annotation for THIS instantiation
    StmtTypeMap              stmt_types;     // type for uninitialised LocalStmt nodes
    UnionFind                uf;             // union-find for THIS instantiation
    CallNameMap              call_names;     // CallExpr* → emitted function name
};

// ── Compiler result ───────────────────────────────────────────────────────────
struct CompileResult {
    bool                     ok = false;
    std::string              glsl;
    std::vector<UniformDesc> uniforms;
    std::vector<Diagnostic>  errors;
    std::vector<Diagnostic>  warnings;
};

// ── Compiler ──────────────────────────────────────────────────────────────────
class Compiler {
public:
    CompileResult compile(const ShaderFunc               &sf,
                          const std::vector<UniformDesc>  &uniforms,
                          const std::vector<ShaderLibDesc> &shaderlibs = {});

private:
    // ── persistent compiler state ──────────────────────────────────────────
    const ShaderFunc        *sf_          = nullptr;
    std::vector<UniformDesc>  uniforms_;
    std::vector<ShaderLibDesc> shaderlibs_;
    bool                     had_errors_  = false;

    // Parsed shaderlib ASTs (owned; indexed in parallel with shaderlibs_).
    // Stored here so the Block* pointers in FuncSig remain valid.
    std::vector<ShaderFunc>  shaderlib_asts_;

    // Global (shader-level) type annotation and union-find.
    // Used for passes 1-3 over the outer function body.
    ExprTypeMap              expr_types_;
    UnionFind                uf_;

    // Struct definitions (shared; struct_ids are globally unique)
    std::vector<StructDef>   structs_;
    int                      next_struct_id_ = 0;

    // Named struct registry: user name → struct_id
    std::unordered_map<std::string, int> named_structs_;

    // Monomorphization registry: key → instance.
    // Stored by value; pointers into this map are stable after reservation.
    std::unordered_map<MonoKey, MonoInstance, MonoKeyHash> mono_registry_;

    // Emission order (pointers into mono_registry_ values).
    // Functions must appear before their callers — built by DFS in pass 4.
    std::vector<MonoInstance *> mono_order_;

    // Entry instance pointer (shader_main) — not in mono_order_ but needs
    // struct remapping in pass5.
    MonoInstance *entry_inst_ = nullptr;

    // Function signatures discovered in pass 1
    struct FuncSig {
        std::string              name;
        std::string              lib_name;   // empty = shader-local
        std::vector<std::string> params;
        const Block             *body;
        SrcLoc                   loc;
    };
    std::vector<FuncSig> func_sigs_;

    // Diagnostics
    std::vector<Diagnostic> errors_;
    std::vector<Diagnostic> warnings_;

    // ── helpers ────────────────────────────────────────────────────────────
    void emit_error(const SrcLoc &loc, const std::string &msg);
    void emit_warning(const SrcLoc &loc, const std::string &msg);
    const FuncSig *find_sig(const std::string &name, const std::string &lib) const;

    // ── Pass 1: signature collection ──────────────────────────────────────
    void pass1_collect_signatures(const Block &block, const std::string &lib = "");

    // ── Pass 2 / 3 type-checking context ──────────────────────────────────
    // These run on the outer shader body using the global expr_types_ / uf_.
    // They also run per MonoInstance (with the instance's own maps).
    struct TypeCtx {
        ExprTypeMap &expr_types;
        UnionFind   &uf;
        int         &next_struct_id;
        std::vector<StructDef> &structs;
        // Maps user-defined struct name → struct_id (populated by type_stmt on StructDeclStmt)
        std::unordered_map<std::string, int> &named_structs;
        // Optional: records inferred types for uninitialised LocalStmt nodes so the
        // emitter can emit the correct declaration type without re-running inference.
        StmtTypeMap *stmt_types = nullptr;
        // Tracks uninitialised LocalStmt nodes by variable name so AssignStmt can
        // back-fill their type in stmt_types.
        std::unordered_map<std::string, const Stmt *> uninit_local_stmts;
    };

    TypeInfo type_expr (const Expr &e, SymbolTable &sym, TypeCtx &ctx);
    void     type_block(const Block &b, SymbolTable &sym, TypeCtx &ctx);
    void     type_stmt (const Stmt  &s, SymbolTable &sym, TypeCtx &ctx);

    TypeInfo type_call   (const CallExpr &c, const SrcLoc &loc, SymbolTable &sym, TypeCtx &ctx);
    TypeInfo type_binop  (int op, TypeInfo l, TypeInfo r, const SrcLoc &loc, TypeCtx &ctx);
    TypeInfo type_unop   (int op, TypeInfo operand, const SrcLoc &loc);
    TypeInfo type_swizzle(TypeInfo base, const std::string &field, const SrcLoc &loc);

    // ── Pass 3: constraint solving ─────────────────────────────────────────
    // Solves uf and resolves expr_types / struct field types.
    void pass3_solve(TypeCtx &ctx);

    // ── Semantic validation ────────────────────────────────────────────────
    void validate      (const Block &block, bool inside_shader_body);
    void validate_expr (const Expr  &e,     bool inside_shader_body);

    // ── Pass 5: struct deduplication & topo-sort ──────────────────────────
    // Resolves struct field types, merges structurally identical structs,
    // rewrites all struct_id references, and topo-sorts for emission.
    void pass5_dedup_structs();

    // Rewrite all struct_id references: old_id → new_id.
    // Walks structs_, all MonoInstance expr_types / param_types / return_types.
    void rewrite_struct_id(int old_id, int new_id);

    // Resolve a TypeInfo's struct_id through the dedup map (after pass5).
    TypeInfo resolve_struct(TypeInfo ti) const;

    // Struct id remapping built by pass5: old_id → canonical_id.
    std::unordered_map<int, int> struct_remap_;

    // ── Pass 4: monomorphization ───────────────────────────────────────────
    // Walks the call graph from the entry point, type-checks each unique
    // instantiation, records it in mono_registry_, appends to mono_order_.
    void pass4_monomorphize(const MonoInstance &entry, SymbolTable &base_sym);

    // Instantiate a function (or retrieve existing). Returns a pointer into
    // mono_registry_ (stable once the map is reserved).
    MonoInstance *instantiate(const MonoKey    &key,
                               const SrcLoc    &call_loc,
                               SymbolTable     &base_sym);

    // Type-check a body with concrete param bindings; fills inst.expr_types + inst.uf.
    // Returns the resolved return type.
    TypeInfo typecheck_instance(MonoInstance &inst, SymbolTable &base_sym);
};
