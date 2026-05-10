#pragma once
#include "AST.h"
#include "Types.h"
#include "Compiler.h"
#include <string>
#include <sstream>

class Emitter {
public:
    // Emit the complete GLSL shader string.
    // mono_order: callee-before-caller order of helper function instances.
    // entry:      the shader_main instance (already in mono_registry, NOT in mono_order).
    std::string emit(
        const ShaderFunc                  &sf,
        const std::vector<UniformDesc>    &uniforms,
        const std::vector<StructDef>      &structs,
        const std::vector<MonoInstance *> &mono_order,
        const MonoInstance                &entry);

private:
    std::ostringstream out_;
    // Pointer to the current instance's type map + union-find, set per function.
    const ExprTypeMap *expr_types_ = nullptr;
    UnionFind         *uf_         = nullptr;
    const CallNameMap *call_names_ = nullptr;
    // All helper instances (for return-type lookup when local init type is Unknown).
    const std::vector<MonoInstance *> *mono_order_ = nullptr;
    // Struct definitions (for glsl_name lookup in type_str).
    const std::vector<StructDef> *structs_ = nullptr;
    int                indent_     = 0;

    void line(const std::string &s = "");
    void indent_push();
    void indent_pop();

    // Emit one monomorphized function (helpers + entry).
    void emit_instance(const MonoInstance &inst, bool is_entry);

    void emit_block(const Block &block);
    void emit_stmt (const Stmt  &s);

    std::string expr_str(const Expr &e);
    // Emit an expression, promoting float scalars to `vecN(x)` when vec_type
    // is a vector type.  Used for genType builtins like pow/clamp/mix.
    std::string expr_str_broadcast(const Expr &e, GlslType vec_type);
    TypeInfo    resolved(const Expr &e) const;
    TypeInfo    resolved_call_return(const Expr &e) const; // looks up mono_order_ by emitted name
    std::string type_str(TypeInfo ti)   const;

    std::string indent_str() const { return std::string(indent_ * 4, ' '); }
};
