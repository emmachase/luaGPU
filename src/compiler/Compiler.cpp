#include "Compiler.h"
#include "Emitter.h"
#include "Lexer.h"
#include <sstream>
#include <algorithm>
#include <cassert>
#include <stdexcept>

// ───────────────────────────────────────────────────────────────────────────────
// Helpers
// ───────────────────────────────────────────────────────────────────────────────

void Compiler::emit_error(const SrcLoc &loc, const std::string &msg) {
    errors_.push_back({loc, msg});
    had_errors_ = true;
}

void Compiler::emit_warning(const SrcLoc &loc, const std::string &msg) {
    warnings_.push_back({loc, msg});
}

const Compiler::FuncSig *Compiler::find_sig(const std::string &name,
                                              const std::string &lib) const {
    for (auto &fs : func_sigs_)
        if (fs.name == name && fs.lib_name == lib) return &fs;
    return nullptr;
}

// ───────────────────────────────────────────────────────────────────────────────
// Top-level entry
// ───────────────────────────────────────────────────────────────────────────────

CompileResult Compiler::compile(const ShaderFunc          &sf,
                                 const std::vector<UniformDesc> &uniforms) {
    sf_       = &sf;
    uniforms_ = uniforms;

    // ── Pass 1 ────────────────────────────────────────────────────────────
    pass1_collect_signatures(sf.body);

    // ── Build base symbol table ────────────────────────────────────────────
    SymbolTable base_sym;   // global scope with builtins already installed
    base_sym.push_scope();  // shader scope

    auto inject = [&](const char *name, GlslType t) {
        Binding b; b.name = name; b.kind = BindingKind::Local;
        b.type = TypeInfo::make(t);
        base_sym.define(std::move(b));
    };
    inject("uv",           GlslType::Vec2);
    inject("frag_coord",   GlslType::Vec4);
    inject("u_resolution", GlslType::Vec2);
    inject("u_time",       GlslType::Float);
    inject("u_delta",      GlslType::Float);
    inject("u_mouse",      GlslType::Vec2);

    for (auto &u : uniforms_) {
        Binding b; b.name = u.name; b.kind = BindingKind::Uniform;
        b.type = u.type;
        base_sym.define(std::move(b));
    }

    for (auto &fs : func_sigs_) {
        Binding b;
        b.name        = fs.name;
        b.lib_name    = fs.lib_name;
        b.kind        = fs.lib_name.empty() ? BindingKind::Function
                                             : BindingKind::ShaderLib;
        b.func_params = fs.params;
        b.func_body   = fs.body;
        b.type        = TypeInfo::unknown();   // polymorphic
        base_sym.define(std::move(b));
    }

    // ── Passes 2 + 3 on the outer body (for local var types) ──────────────
    TypeCtx global_ctx{expr_types_, uf_, next_struct_id_, structs_};
    type_block(sf.body, base_sym, global_ctx);
    pass3_solve(global_ctx);

    // ── Semantic validation ────────────────────────────────────────────────
    validate(sf.body, true);

    if (had_errors_) goto done;

    // ── Find entry-point function expression ───────────────────────────────
    {
        const FuncExpr *entry_fe = nullptr;
        for (auto &s : sf.body) {
            if (auto *rs = std::get_if<ReturnStmt>(&s->kind)) {
                if (rs->value) {
                    if (auto *fe = std::get_if<FuncExpr>(&(*rs->value)->kind))
                        entry_fe = fe;
                }
            }
        }
        if (!entry_fe) {
            emit_error({sf.src_name, 0},
                       "shader() function must return a function (the entry point)");
            goto done;
        }

        // Build the entry MonoInstance — it is always concrete (uv: vec2).
        MonoKey entry_key;
        entry_key.func_name = entry_fe->name.empty() ? "main" : entry_fe->name;
        for (auto &p : entry_fe->params) {
            const Binding *b = base_sym.lookup(p);
            entry_key.arg_types.push_back(b ? b->type : TypeInfo::make(GlslType::Vec2));
        }

        MonoInstance entry_inst;
        entry_inst.key          = entry_key;
        entry_inst.emitted_name = "shader_main";
        entry_inst.param_names  = entry_fe->params;
        for (auto &p : entry_fe->params) {
            const Binding *b = base_sym.lookup(p);
            entry_inst.param_types.push_back(b ? b->type : TypeInfo::make(GlslType::Vec2));
        }
        entry_inst.body = &entry_fe->body;

        // Type-check the entry body — this also discovers callee types for pass 4.
        entry_inst.return_type = typecheck_instance(entry_inst, base_sym);

        // Store in registry and emit order.
        auto [it, inserted] = mono_registry_.emplace(entry_key, std::move(entry_inst));
        MonoInstance *entry_ptr = &it->second;

        // ── Pass 4: walk call graph from entry ────────────────────────────
        pass4_monomorphize(*entry_ptr, base_sym);

        // Emit
        Emitter emitter;
        std::string glsl = emitter.emit(sf, uniforms_, structs_, mono_order_,
                                        *entry_ptr);
        CompileResult r;
        r.ok       = !had_errors_;
        r.glsl     = std::move(glsl);
        r.uniforms = uniforms_;
        r.errors   = errors_;
        r.warnings = warnings_;
        return r;
    }

done:
    CompileResult r;
    r.ok      = false;
    r.errors  = errors_;
    r.warnings= warnings_;
    return r;
}

// ───────────────────────────────────────────────────────────────────────────────
// Pass 1
// ───────────────────────────────────────────────────────────────────────────────

void Compiler::pass1_collect_signatures(const Block &block,
                                         const std::string &lib) {
    for (auto &s : block) {
        if (auto *lf = std::get_if<LocalFuncStmt>(&s->kind)) {
            FuncSig sig;
            sig.name     = lf->name;
            sig.lib_name = lib;
            sig.params   = lf->params;
            sig.body     = &lf->body;
            sig.loc      = s->loc;
            func_sigs_.push_back(std::move(sig));
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────────
// Pass 2 — type inference (shared between outer body and per-instance bodies)
// ───────────────────────────────────────────────────────────────────────────────

void Compiler::type_block(const Block &block, SymbolTable &sym, TypeCtx &ctx) {
    sym.push_scope();
    for (auto &s : block) type_stmt(*s, sym, ctx);
    sym.pop_scope();
}

void Compiler::type_stmt(const Stmt &s, SymbolTable &sym, TypeCtx &ctx) {
    std::visit([&](auto &sk) {
        using T = std::decay_t<decltype(sk)>;

        if constexpr (std::is_same_v<T, LocalStmt>) {
            TypeInfo ty = TypeInfo::unknown();
            if (sk.init) ty = type_expr(**sk.init, sym, ctx);
            Binding b; b.name = sk.name; b.kind = BindingKind::Local; b.type = ty;
            sym.define(std::move(b));

        } else if constexpr (std::is_same_v<T, LocalFuncStmt>) {
            // Body is type-checked on demand per instantiation in pass 4.
            // Register the name in scope so recursive checks can skip it.
            Binding b; b.name = sk.name; b.kind = BindingKind::Function;
            b.func_params = sk.params; b.func_body = &sk.body;
            b.type = TypeInfo::unknown();
            sym.define(std::move(b));

        } else if constexpr (std::is_same_v<T, AssignStmt>) {
            type_expr(*sk.value, sym, ctx);

        } else if constexpr (std::is_same_v<T, ExprStmt>) {
            type_expr(*sk.expr, sym, ctx);

        } else if constexpr (std::is_same_v<T, ReturnStmt>) {
            if (sk.value) type_expr(**sk.value, sym, ctx);

        } else if constexpr (std::is_same_v<T, IfStmt>) {
            for (auto &br : sk.branches) {
                if (br.cond) type_expr(*br.cond, sym, ctx);
                type_block(br.body, sym, ctx);
            }

        } else if constexpr (std::is_same_v<T, WhileStmt>) {
            type_expr(*sk.cond, sym, ctx);
            type_block(sk.body, sym, ctx);

        } else if constexpr (std::is_same_v<T, ForStmt>) {
            type_expr(*sk.start, sym, ctx);
            type_expr(*sk.stop,  sym, ctx);
            if (sk.step) type_expr(**sk.step, sym, ctx);
            sym.push_scope();
            Binding b; b.name = sk.var; b.kind = BindingKind::Local;
            b.type = TypeInfo::make(GlslType::Int);
            sym.define(std::move(b));
            for (auto &stmt : sk.body) type_stmt(*stmt, sym, ctx);
            sym.pop_scope();

        } else if constexpr (std::is_same_v<T, BreakStmt>) {
            // nothing
        }
    }, s.kind);
}

TypeInfo Compiler::type_expr(const Expr &e, SymbolTable &sym, TypeCtx &ctx) {
    TypeInfo ty = std::visit([&](auto &ek) -> TypeInfo {
        using T = std::decay_t<decltype(ek)>;

        if constexpr (std::is_same_v<T, NumberLit>) {
            int id = ctx.uf.new_tvar();
            return TypeInfo::make_tvar(id);

        } else if constexpr (std::is_same_v<T, BoolLit>) {
            return TypeInfo::make(GlslType::Bool);

        } else if constexpr (std::is_same_v<T, StringLit> ||
                              std::is_same_v<T, NilLit>) {
            return TypeInfo::unknown();

        } else if constexpr (std::is_same_v<T, NameExpr>) {
            const Binding *b = sym.lookup(ek.name);
            if (!b) {
                emit_error(e.loc, "undefined variable '" + ek.name + "'");
                return TypeInfo::unknown();
            }
            return b->type;

        } else if constexpr (std::is_same_v<T, FieldExpr>) {
            TypeInfo base = type_expr(*ek.base, sym, ctx);
            if (is_vec(base.tag))
                return type_swizzle(base, ek.field, e.loc);
            if (base.tag == GlslType::Struct) {
                int sid = base.struct_id;
                if (sid >= 0 && sid < (int)ctx.structs.size()) {
                    for (auto &f : ctx.structs[sid].fields)
                        if (f.name == ek.field) return f.type;
                }
                emit_error(e.loc, "struct has no field '" + ek.field + "'");
                return TypeInfo::unknown();
            }
            // namespace (math.sin) — resolve as bare builtin
            if (auto *ne = std::get_if<NameExpr>(&ek.base->kind)) {
                if (ne->name == "math" || ne->name == "bit") {
                    // type resolved at call site in type_call
                    return TypeInfo::unknown();
                }
            }
            emit_error(e.loc, "field access on non-vector, non-struct type");
            return TypeInfo::unknown();

        } else if constexpr (std::is_same_v<T, CallExpr>) {
            return type_call(ek, e.loc, sym, ctx);

        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            TypeInfo op_ty = type_expr(*ek.operand, sym, ctx);
            return type_unop(ek.op, op_ty, e.loc);

        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            TypeInfo lhs = type_expr(*ek.left,  sym, ctx);
            TypeInfo rhs = type_expr(*ek.right, sym, ctx);
            return type_binop(ek.op, lhs, rhs, e.loc, ctx);

        } else if constexpr (std::is_same_v<T, TableExpr>) {
            int sid = ctx.next_struct_id++;
            StructDef sd;
            sd.id        = sid;
            sd.glsl_name = "S" + std::to_string(sid);
            for (auto &f : ek.fields) {
                TypeInfo ft = type_expr(*f.value, sym, ctx);
                sd.fields.push_back({f.key, ft});
            }
            ctx.structs.push_back(std::move(sd));
            return TypeInfo::make_struct(sid);

        } else if constexpr (std::is_same_v<T, FuncExpr>) {
            return TypeInfo::make(GlslType::Void);
        } else {
            return TypeInfo::unknown();
        }
    }, e.kind);

    ctx.expr_types[&e] = ty;
    return ty;
}

// ── type_call ─────────────────────────────────────────────────────────────────

TypeInfo Compiler::type_call(const CallExpr &call, const SrcLoc &loc,
                              SymbolTable &sym, TypeCtx &ctx) {
    std::vector<TypeInfo> arg_types;
    for (auto &a : call.args)
        arg_types.push_back(type_expr(*a, sym, ctx));

    // Simple name call
    if (auto *ne = std::get_if<NameExpr>(&call.callee->kind)) {
        const Binding *b = sym.lookup(ne->name);
        if (!b) { emit_error(loc, "call to undefined '" + ne->name + "'"); return TypeInfo::unknown(); }
        if (b->kind == BindingKind::Constructor) return b->type;
        if (b->kind == BindingKind::Builtin) {
            if (b->type.tag != GlslType::Unknown) return b->type;
            return arg_types.empty() ? TypeInfo::unknown() : arg_types[0];
        }
        if (b->kind == BindingKind::Function) {
            // Polymorphic local — return Unknown; concrete type resolved per-instance in pass 4.
            return TypeInfo::unknown();
        }
    }

    // Field call: math.fn / bit.fn / lib.fn
    if (auto *fe = std::get_if<FieldExpr>(&call.callee->kind)) {
        if (auto *ne = std::get_if<NameExpr>(&fe->base->kind)) {
            const std::string &ns = ne->name;
            const std::string &fn = fe->field;

            if (ns == "math") {
                const Binding *b = sym.lookup(fn);
                if (b && b->kind == BindingKind::Builtin) {
                    if (b->type.tag != GlslType::Unknown) return b->type;
                    return arg_types.empty() ? TypeInfo::unknown() : arg_types[0];
                }
                emit_error(loc, "unknown math function 'math." + fn + "'");
                return TypeInfo::unknown();
            }
            if (ns == "bit") {
                // Result is same type as first int operand
                return arg_types.empty() ? TypeInfo::make(GlslType::Int) : arg_types[0];
            }
            // shaderlib call — unknown until instantiated
            const Binding *lib = sym.lookup(ns);
            if (lib && lib->kind == BindingKind::ShaderLib)
                return TypeInfo::unknown();
        }
    }

    if (auto *ne = std::get_if<NameExpr>(&call.callee->kind))
        if (ne->name == "texture") return TypeInfo::make(GlslType::Vec4);

    emit_error(loc, "cannot call this expression");
    return TypeInfo::unknown();
}

// ── type_binop ────────────────────────────────────────────────────────────────

TypeInfo Compiler::type_binop(int op, TypeInfo lhs, TypeInfo rhs,
                               const SrcLoc &loc, TypeCtx &ctx) {
    if (lhs.tag == GlslType::Unknown || rhs.tag == GlslType::Unknown)
        return TypeInfo::unknown();

    // Unify type variables with each other or with concrete types
    if (lhs.is_tvar() && rhs.is_tvar()) {
        TypeInfo ca, cb; ctx.uf.unify(lhs.tvar_id, rhs.tvar_id, ca, cb);
        return lhs;
    }
    if (lhs.is_tvar()) { TypeInfo ex; ctx.uf.constrain(lhs.tvar_id, rhs, ex); return rhs; }
    if (rhs.is_tvar()) { TypeInfo ex; ctx.uf.constrain(rhs.tvar_id, lhs, ex); return lhs; }

    lhs = ctx.uf.resolve(lhs);
    rhs = ctx.uf.resolve(rhs);

    int And_v = (int)TK::And, Or_v = (int)TK::Or;
    int Eq_v  = (int)TK::Eq,  Ne_v = (int)TK::Ne;
    int Le_v  = (int)TK::Le,  Ge_v = (int)TK::Ge;

    if (op == And_v || op == Or_v) return TypeInfo::make(GlslType::Bool);
    if (op == Eq_v  || op == Ne_v ||
        op == '<'   || op == '>'  || op == Le_v || op == Ge_v)
        return TypeInfo::make(GlslType::Bool);

    if (op == (int)TK::Concat) {
        emit_error(loc, "string concatenation (..) is not supported");
        return TypeInfo::unknown();
    }

    if (lhs == rhs) return lhs;
    if (is_scalar(lhs.tag) && is_vec(rhs.tag)) return rhs;
    if (is_vec(lhs.tag)    && is_scalar(rhs.tag)) return lhs;
    if (is_scalar(lhs.tag) && is_mat(rhs.tag)) return rhs;
    if (is_mat(lhs.tag)    && is_scalar(rhs.tag)) return lhs;
    if (op == '*') {
        if (lhs.tag == GlslType::Mat4 && rhs.tag == GlslType::Vec4) return rhs;
        if (lhs.tag == GlslType::Mat3 && rhs.tag == GlslType::Vec3) return rhs;
        if (lhs.tag == GlslType::Mat2 && rhs.tag == GlslType::Vec2) return rhs;
    }

    emit_error(loc, std::string("type error: cannot apply operator to ") +
                    glsl_type_name(lhs.tag) + " and " + glsl_type_name(rhs.tag));
    return TypeInfo::unknown();
}

TypeInfo Compiler::type_unop(int op, TypeInfo operand, const SrcLoc &loc) {
    if (operand.tag == GlslType::Unknown) return TypeInfo::unknown();
    if (op == (int)TK::Not) return TypeInfo::make(GlslType::Bool);
    if (op == '-')           return operand;
    emit_error(loc, "unknown unary operator");
    return TypeInfo::unknown();
}

TypeInfo Compiler::type_swizzle(TypeInfo base, const std::string &field,
                                 const SrcLoc &loc) {
    int dim = vec_dim(base.tag);
    if (dim == 0) { emit_error(loc, "swizzle on non-vector type"); return TypeInfo::unknown(); }

    bool xyzw = field.find_first_not_of("xyzw") == std::string::npos;
    bool rgba  = field.find_first_not_of("rgba")  == std::string::npos;
    bool stpq  = field.find_first_not_of("stpq")  == std::string::npos;
    if (!xyzw && !rgba && !stpq) {
        emit_error(loc, "mixed swizzle sets in '." + field + "'");
        return TypeInfo::unknown();
    }
    if (field.size() > 4) {
        emit_error(loc, "swizzle too long '." + field + "'");
        return TypeInfo::unknown();
    }
    const char *set = xyzw ? "xyzw" : (rgba ? "rgba" : "stpq");
    for (char c : field) {
        int idx = (int)(std::strchr(set, c) - set);
        if (idx >= dim) {
            emit_error(loc, std::string("swizzle '") + c + "' out of range for " +
                            glsl_type_name(base.tag));
            return TypeInfo::unknown();
        }
    }
    GlslType scalar = vec_scalar(base.tag);
    return field.size() == 1 ? TypeInfo::make(scalar)
                              : TypeInfo::make(make_vec(scalar, (int)field.size()));
}

// ───────────────────────────────────────────────────────────────────────────────
// Pass 3 — constraint solving
// ───────────────────────────────────────────────────────────────────────────────

void Compiler::pass3_solve(TypeCtx &ctx) {
    // Resolve all annotated expression types through the union-find.
    // Unresolved tvars default to float inside UnionFind::resolve().
    for (auto &[ptr, ti] : ctx.expr_types)
        if (ti.is_tvar()) ti = ctx.uf.resolve(ti);

    // Resolve struct field types.
    for (auto &sd : ctx.structs)
        for (auto &f : sd.fields)
            if (f.type.is_tvar()) f.type = ctx.uf.resolve(f.type);
}

// ───────────────────────────────────────────────────────────────────────────────
// Semantic validation
// ───────────────────────────────────────────────────────────────────────────────

void Compiler::validate(const Block &block, bool inside_shader_body) {
    for (auto &s : block) {
        std::visit([&](auto &sk) {
            using T = std::decay_t<decltype(sk)>;
            if constexpr (std::is_same_v<T, LocalStmt>) {
                if (sk.init) validate_expr(**sk.init, inside_shader_body);
            } else if constexpr (std::is_same_v<T, LocalFuncStmt>) {
                validate(sk.body, true);
            } else if constexpr (std::is_same_v<T, AssignStmt>) {
                validate_expr(*sk.value, inside_shader_body);
            } else if constexpr (std::is_same_v<T, ExprStmt>) {
                validate_expr(*sk.expr, inside_shader_body);
            } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                if (sk.value) validate_expr(**sk.value, inside_shader_body);
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                for (auto &br : sk.branches) {
                    if (br.cond) validate_expr(*br.cond, inside_shader_body);
                    validate(br.body, inside_shader_body);
                }
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                validate_expr(*sk.cond, inside_shader_body);
                validate(sk.body, inside_shader_body);
            } else if constexpr (std::is_same_v<T, ForStmt>) {
                validate_expr(*sk.start, inside_shader_body);
                validate_expr(*sk.stop,  inside_shader_body);
                if (sk.step) validate_expr(**sk.step, inside_shader_body);
                validate(sk.body, inside_shader_body);
            } else if constexpr (std::is_same_v<T, BreakStmt>) {}
        }, s->kind);
    }
}

void Compiler::validate_expr(const Expr &e, bool inside_shader_body) {
    std::visit([&](auto &ek) {
        using T = std::decay_t<decltype(ek)>;
        if constexpr (std::is_same_v<T, NilLit>)
            emit_error(e.loc, "nil is not supported in shaders");
        else if constexpr (std::is_same_v<T, StringLit>)
            emit_error(e.loc, "string values are not supported in shaders");
        else if constexpr (std::is_same_v<T, BinaryExpr>) {
            if (ek.op == (int)TK::Concat)
                emit_error(e.loc, "string concatenation (..) is not supported");
            // and/or ternary: check middle operand is not false literal
            if (ek.op == (int)TK::Or) {
                if (auto *inner = std::get_if<BinaryExpr>(&ek.left->kind)) {
                    if (inner->op == (int)TK::And) {
                        if (auto *bl = std::get_if<BoolLit>(&inner->right->kind))
                            if (!bl->value)
                                emit_error(e.loc,
                                    "'and/or' ternary: middle operand must not be literal false");
                    }
                }
            }
            validate_expr(*ek.left,  inside_shader_body);
            validate_expr(*ek.right, inside_shader_body);
        } else if constexpr (std::is_same_v<T, UnaryExpr>)
            validate_expr(*ek.operand, inside_shader_body);
        else if constexpr (std::is_same_v<T, CallExpr>) {
            for (auto &a : ek.args) validate_expr(*a, inside_shader_body);
        } else if constexpr (std::is_same_v<T, FieldExpr>)
            validate_expr(*ek.base, inside_shader_body);
        else if constexpr (std::is_same_v<T, FuncExpr>) {
            if (inside_shader_body)
                emit_error(e.loc, "anonymous functions are not allowed inside shader body");
            validate(ek.body, true);
        } else if constexpr (std::is_same_v<T, TableExpr>)
            for (auto &f : ek.fields) validate_expr(*f.value, inside_shader_body);
    }, e.kind);
}

// ───────────────────────────────────────────────────────────────────────────────
// Pass 4 — monomorphization
// ───────────────────────────────────────────────────────────────────────────────

TypeInfo Compiler::typecheck_instance(MonoInstance &inst, SymbolTable &base_sym) {
    // Build a fresh symbol table for this instance.
    // Inherit the base scope (builtins, uniforms, other function sigs),
    // then push a function scope with concrete parameter bindings.
    SymbolTable sym = base_sym;  // copies the scope stack
    sym.push_scope();
    for (size_t i = 0; i < inst.param_names.size(); ++i) {
        Binding b; b.name = inst.param_names[i]; b.kind = BindingKind::Local;
        b.type = inst.param_types[i];
        sym.define(std::move(b));
    }

    // Run type inference on the body using this instance's own ctx.
    TypeCtx inst_ctx{inst.expr_types, inst.uf, next_struct_id_, structs_};
    type_block(*inst.body, sym, inst_ctx);
    pass3_solve(inst_ctx);

    // Find return type from the return statement.
    TypeInfo ret = TypeInfo::make(GlslType::Void);
    for (auto &s : *inst.body) {
        if (auto *rs = std::get_if<ReturnStmt>(&s->kind)) {
            if (rs->value) {
                auto it = inst.expr_types.find(rs->value->get());
                if (it != inst.expr_types.end()) {
                    ret = it->second;
                    if (ret.is_tvar()) ret = inst.uf.resolve(ret);
                }
            }
            break;
        }
    }
    return ret;
}

void Compiler::pass4_monomorphize(const MonoInstance &entry, SymbolTable &base_sym) {
    // DFS over the call graph. Use a work queue of instances whose bodies
    // need to be scanned for callee invocations.
    std::vector<const MonoInstance *> worklist;
    worklist.push_back(&entry);

    // The entry is already type-checked; scan it for calls.
    while (!worklist.empty()) {
        const MonoInstance *cur = worklist.back();
        worklist.pop_back();

        // Walk the body looking for calls to local/shaderlib functions.
        std::function<void(const Expr &)> scan_expr = [&](const Expr &e) {
            std::visit([&](auto &ek) {
                using T = std::decay_t<decltype(ek)>;
                if constexpr (std::is_same_v<T, CallExpr>) {
                    // Collect arg types from this instance's expr_types map.
                    std::vector<TypeInfo> arg_types;
                    for (auto &a : ek.args) {
                        auto it = cur->expr_types.find(a.get());
                        arg_types.push_back(
                            it != cur->expr_types.end() ? it->second
                                                        : TypeInfo::unknown());
                    }

                    MonoKey key;
                    const FuncSig *sig = nullptr;

                    if (auto *ne = std::get_if<NameExpr>(&ek.callee->kind)) {
                        const Binding *b = base_sym.lookup(ne->name);
                        if (b && b->kind == BindingKind::Function) {
                            key.func_name = ne->name;
                            sig = find_sig(ne->name, "");
                        }
                    } else if (auto *fe = std::get_if<FieldExpr>(&ek.callee->kind)) {
                        if (auto *fe_base = std::get_if<NameExpr>(&fe->base->kind)) {
                            const Binding *lib = base_sym.lookup(fe_base->name);
                            if (lib && lib->kind == BindingKind::ShaderLib) {
                                key.func_name = fe->field;
                                key.lib_name  = fe_base->name;
                                sig = find_sig(fe->field, fe_base->name);
                            }
                        }
                    }

                    if (sig && !key.func_name.empty()) {
                        key.arg_types = arg_types;
                        if (mono_registry_.find(key) == mono_registry_.end()) {
                            // New instantiation needed.
                            MonoInstance inst;
                            inst.key         = key;
                            inst.param_names = sig->params;
                            inst.param_types = arg_types;
                            inst.body        = sig->body;

                            // Generate emitted name
                            int idx = (int)mono_registry_.size();
                            if (arg_types.size() == 1 && arg_types[0].is_concrete())
                                inst.emitted_name = key.func_name + "_" +
                                                    glsl_type_name(arg_types[0].tag);
                            else
                                inst.emitted_name = key.func_name + "_" + std::to_string(idx);

                            inst.return_type = typecheck_instance(inst, base_sym);

                            auto [it, _] = mono_registry_.emplace(key, std::move(inst));
                            mono_order_.push_back(&it->second);
                            worklist.push_back(&it->second);
                        }
                    }

                    // recurse into args
                    for (auto &a : ek.args) scan_expr(*a);
                } else {
                    // recurse into subexpressions
                    if constexpr (std::is_same_v<T, BinaryExpr>) {
                        scan_expr(*ek.left); scan_expr(*ek.right);
                    } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                        scan_expr(*ek.operand);
                    } else if constexpr (std::is_same_v<T, FieldExpr>) {
                        scan_expr(*ek.base);
                    } else if constexpr (std::is_same_v<T, TableExpr>) {
                        for (auto &f : ek.fields) scan_expr(*f.value);
                    }
                }
            }, e.kind);
        };

        std::function<void(const Block &)> scan_block = [&](const Block &bl) {
            for (auto &s : bl) {
                std::visit([&](auto &sk) {
                    using T = std::decay_t<decltype(sk)>;
                    if constexpr (std::is_same_v<T, LocalStmt>)
                        { if (sk.init) scan_expr(**sk.init); }
                    else if constexpr (std::is_same_v<T, LocalFuncStmt>)
                        scan_block(sk.body);
                    else if constexpr (std::is_same_v<T, AssignStmt>)
                        scan_expr(*sk.value);
                    else if constexpr (std::is_same_v<T, ExprStmt>)
                        scan_expr(*sk.expr);
                    else if constexpr (std::is_same_v<T, ReturnStmt>)
                        { if (sk.value) scan_expr(**sk.value); }
                    else if constexpr (std::is_same_v<T, IfStmt>)
                        for (auto &br : sk.branches) {
                            if (br.cond) scan_expr(*br.cond);
                            scan_block(br.body);
                        }
                    else if constexpr (std::is_same_v<T, WhileStmt>)
                        { scan_expr(*sk.cond); scan_block(sk.body); }
                    else if constexpr (std::is_same_v<T, ForStmt>) {
                        scan_expr(*sk.start); scan_expr(*sk.stop);
                        if (sk.step) scan_expr(**sk.step);
                        scan_block(sk.body);
                    }
                }, s->kind);
            }
        };

        scan_block(*cur->body);
    }
}
