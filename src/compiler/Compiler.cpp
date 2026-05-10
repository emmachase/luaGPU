#include "Compiler.h"
#include "Emitter.h"
#include "Lexer.h"
#include <sstream>
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <map>

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
    TypeCtx global_ctx{expr_types_, uf_, next_struct_id_, structs_, named_structs_};
    type_block(sf.body, base_sym, global_ctx);
    pass3_solve(global_ctx);

    // ── Inject named struct bindings into base_sym ────────────────────────
    // StructDeclStmt defines struct names into the scope inside type_block,
    // but base_sym is the root scope used for monomorphization. We need to
    // make named structs visible in all instantiated function bodies.
    for (auto &[sname, sid] : named_structs_) {
        Binding b;
        b.name      = sname;
        b.kind      = BindingKind::StructType;
        b.type      = TypeInfo::make_struct(sid);
        b.struct_id = sid;
        base_sym.define(std::move(b));
    }

    // ── Semantic validation ────────────────────────────────────────────────
    validate(sf.body, false);

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
        entry_inst_ = entry_ptr;

        // ── Pass 4: walk call graph from entry ────────────────────────────
        pass4_monomorphize(*entry_ptr, base_sym);

        // ── Pass 5: struct dedup & topo-sort ──────────────────────────────
        pass5_dedup_structs();

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

        } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
            // Resolve each field type name to a TypeInfo.
            // Supported: builtin GLSL scalar/vector/matrix names, or a previously
            // declared named struct name (declaration order required).
            auto resolve_type_name = [&](const std::string &tname,
                                         const SrcLoc &loc) -> TypeInfo {
                // Builtin primitive types
                static const std::unordered_map<std::string, GlslType> prim_map = {
                    {"float",     GlslType::Float},
                    {"int",       GlslType::Int},
                    {"bool",      GlslType::Bool},
                    {"vec2",      GlslType::Vec2},
                    {"vec3",      GlslType::Vec3},
                    {"vec4",      GlslType::Vec4},
                    {"ivec2",     GlslType::IVec2},
                    {"ivec3",     GlslType::IVec3},
                    {"ivec4",     GlslType::IVec4},
                    {"mat2",      GlslType::Mat2},
                    {"mat3",      GlslType::Mat3},
                    {"mat4",      GlslType::Mat4},
                    {"sampler2D", GlslType::Sampler2D},
                };
                auto pit = prim_map.find(tname);
                if (pit != prim_map.end()) return TypeInfo::make(pit->second);

                // Named struct type
                auto nit = ctx.named_structs.find(tname);
                if (nit != ctx.named_structs.end())
                    return TypeInfo::make_struct(nit->second);

                emit_error(loc, "unknown type '" + tname +
                           "' in struct field (named structs must be declared before use)");
                return TypeInfo::unknown();
            };

            int sid = ctx.next_struct_id++;
            StructDef sd;
            sd.id        = sid;
            sd.is_named  = true;
            sd.user_name = sk.name;
            sd.glsl_name = sk.name;  // named structs keep user name
            for (auto &[fname, tname] : sk.fields) {
                TypeInfo ft = resolve_type_name(tname, s.loc);
                sd.fields.push_back({fname, ft});
            }
            ctx.structs.push_back(std::move(sd));
            ctx.named_structs[sk.name] = sid;

            // Register in the symbol table so Ray{...} is recognised as a ctor call.
            Binding b;
            b.name      = sk.name;
            b.kind      = BindingKind::StructType;
            b.type      = TypeInfo::make_struct(sid);
            b.struct_id = sid;
            sym.define(std::move(b));
        }
    }, s.kind);
}

TypeInfo Compiler::type_expr(const Expr &e, SymbolTable &sym, TypeCtx &ctx) {
    TypeInfo ty = std::visit([&](auto &ek) -> TypeInfo {
        using T = std::decay_t<decltype(ek)>;

        if constexpr (std::is_same_v<T, NumberLit>) {
            // Numeric literals are float scalars.  We give them a tvar so they
            // can participate in operations like `vec2 * 2.0` (GLSL allows scalar
            // broadcast in arithmetic), but we constrain the tvar to Float
            // immediately so it cannot be widened to a vector type by unification.
            int id = ctx.uf.new_tvar();
            TypeInfo existing;
            ctx.uf.constrain(id, TypeInfo::make(GlslType::Float), existing);
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
            // If the base type is not yet resolved (result of a polymorphic
            // local call), defer — monomorphization will resolve it.
            if (base.is_unknown() || base.is_tvar())
                return TypeInfo::unknown();
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
            int And_v = (int)TK::And, Or_v = (int)TK::Or;

            // Ternary pattern: (cond and A) or B
            // The result type is the type of A (or B — they should unify).
            // Don't let type_binop return Bool for this case.
            if (ek.op == Or_v) {
                if (auto *inner = std::get_if<BinaryExpr>(&ek.left->kind)) {
                    if (inner->op == And_v) {
                        // Type-check all three sub-expressions.
                        type_expr(*inner->left,  sym, ctx);  // condition
                        TypeInfo ty_a = type_expr(*inner->right, sym, ctx);  // consequent
                        TypeInfo ty_b = type_expr(*ek.right,     sym, ctx);  // alternate
                        // Unify consequent and alternate.
                        if (!ty_a.is_unknown() && !ty_b.is_unknown()) {
                            if (ty_a.is_tvar()) {
                                TypeInfo ex; ctx.uf.constrain(ty_a.tvar_id, ty_b, ex);
                            } else if (ty_b.is_tvar()) {
                                TypeInfo ex; ctx.uf.constrain(ty_b.tvar_id, ty_a, ex);
                            }
                        }
                        TypeInfo result = ty_a.is_unknown() ? ty_b : ty_a;
                        ctx.expr_types[&e] = result;
                        return result;
                    }
                }
            }

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
        if (b->kind == BindingKind::StructType) {
            // Named struct constructor: Ray { origin = vec3(...), dir = vec3(...) }
            // The single argument must be a TableExpr; its fields are already
            // type-checked above (arg_types[0] typed the TableExpr anonymously).
            // We need to:
            //  1. Validate that the table fields match the declared struct.
            //  2. Return the named struct's TypeInfo (not the anonymous one).
            int sid = b->struct_id;
            if (call.args.size() != 1) {
                emit_error(loc, "struct constructor '" + ne->name +
                           "' requires exactly one table argument");
                return TypeInfo::unknown();
            }
            auto *te = std::get_if<TableExpr>(&call.args[0]->kind);
            if (!te) {
                emit_error(loc, "struct constructor '" + ne->name +
                           "' argument must be a table literal { field = value, ... }");
                return TypeInfo::unknown();
            }
            // Find the declared StructDef.
            StructDef *decl = nullptr;
            for (auto &sd : ctx.structs)
                if (sd.id == sid) { decl = &sd; break; }
            if (!decl) {
                emit_error(loc, "internal: struct def not found for '" + ne->name + "'");
                return TypeInfo::unknown();
            }
            // Validate: every field in the table literal must exist in the declaration,
            // and every declared field must be present.
            for (auto &tf : te->fields) {
                bool found = false;
                for (auto &df : decl->fields)
                    if (df.name == tf.key) { found = true; break; }
                if (!found)
                    emit_error(loc, "struct '" + ne->name +
                               "' has no field '" + tf.key + "'");
            }
            for (auto &df : decl->fields) {
                bool found = false;
                for (auto &tf : te->fields)
                    if (tf.key == df.name) { found = true; break; }
                if (!found)
                    emit_error(loc, "missing field '" + df.name +
                               "' in constructor for struct '" + ne->name + "'");
            }
            // Override the anonymous struct's type annotation with the named one.
            // The anonymous struct created by type_expr(TableExpr) gets remapped
            // to the named struct's sid in pass5, but we can do better: directly
            // annotate the call expression and the table expression with the named sid.
            TypeInfo named_ti = TypeInfo::make_struct(sid);
            ctx.expr_types[call.args[0].get()] = named_ti;
            return named_ti;
        }
        if (b->kind == BindingKind::Builtin) {
            if (b->type.tag != GlslType::Unknown) return b->type;
            return arg_types.empty() ? TypeInfo::unknown() : arg_types[0];
        }
        if (b->kind == BindingKind::Function) {
            // Polymorphic local — look up an already-resolved MonoInstance.
            // First try an exact key match.
            MonoKey probe;
            probe.func_name = ne->name;
            probe.arg_types = arg_types;
            auto it = mono_registry_.find(probe);
            if (it != mono_registry_.end() &&
                it->second.return_type.tag != GlslType::Unknown)
                return it->second.return_type;

            // Fallback: find any instance of this function whose param_types are
            // all concrete and whose return_type is known.  This handles the case
            // where the registry key still has Unknown slots (from the first scan)
            // but param_types have since been updated by the refinement pass.
            for (auto &[k, m] : mono_registry_) {
                if (k.func_name != ne->name) continue;
                if (m.return_type.tag == GlslType::Unknown) continue;
                bool all_conc = true;
                for (auto &pt : m.param_types)
                    if (pt.is_unknown() || pt.is_tvar()) { all_conc = false; break; }
                if (all_conc) return m.return_type;
            }
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
    if (lhs.is_unknown() || rhs.is_unknown())
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
    if (operand.is_unknown()) return TypeInfo::unknown();
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
            } else if constexpr (std::is_same_v<T, BreakStmt>) {
            } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
                // Nothing to validate — field types were resolved at parse time.
            }
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
// Pass 5 — struct deduplication & topo-sort
// ───────────────────────────────────────────────────────────────────────────────

// Rewrite a single TypeInfo: if it's a struct, apply remap.
static TypeInfo remap_ti(TypeInfo ti, const std::unordered_map<int,int> &remap) {
    if (ti.tag == GlslType::Struct) {
        auto it = remap.find(ti.struct_id);
        if (it != remap.end()) ti.struct_id = it->second;
    }
    return ti;
}

// Remap all TypeInfos in an ExprTypeMap.
static void remap_expr_map(ExprTypeMap &m, const std::unordered_map<int,int> &remap) {
    for (auto &kv : m) {
        auto &ti = const_cast<TypeInfo &>(kv.second);
        ti = remap_ti(ti, remap);
    }
}

void Compiler::rewrite_struct_id(int old_id, int new_id) {
    struct_remap_[old_id] = new_id;
}

void Compiler::pass5_dedup_structs() {
    // ── Step 1: resolve all struct field types through the global UF ──────────
    // Structs are created in type_expr using the global uf_ (or per-instance uf
    // stored in inst.uf). Field types may still be tvars.
    // We need to resolve field types through each struct's owning UF.
    // Since struct_id is globally unique and structs_ is shared, but each struct
    // was created in some TypeCtx (global or per-instance), we resolve through
    // all available UFs.
    //
    // Strategy: collect all UFs (global + per instance) and resolve each field
    // type through whichever UF can concretize it.

    auto try_resolve = [&](TypeInfo ti) -> TypeInfo {
        if (ti.is_tvar()) {
            TypeInfo r = uf_.resolve(ti);
            if (!r.is_tvar()) return r;
            // Try entry instance UF.
            if (entry_inst_) {
                r = entry_inst_->uf.resolve(ti);
                if (!r.is_tvar()) return r;
            }
            // Try per-helper-instance UFs.
            for (auto *inst : mono_order_) {
                r = inst->uf.resolve(ti);
                if (!r.is_tvar()) return r;
            }
        }
        return ti;
    };

    for (auto &sd : structs_) {
        for (auto &f : sd.fields)
            f.type = try_resolve(f.type);
    }

    // ── Step 2: build canonical signature for each struct ─────────────────────
    // Key: sorted list of (field_name, type_tag, struct_id_if_struct)
    // We need the field types to be concrete for this to work.
    // Use alphabetical field order (as per spec) for the key.

    // Returns a string key representing the struct shape.
    // Named structs include their user name in the key so they are never merged
    // with anonymous structs or other named structs (even if the layout matches).
    // Returns "" if any field is still unknown (cannot deduplicate yet).
    auto struct_key = [&](const StructDef &sd) -> std::string {
        // Named structs are unique by name — never merge with anything else.
        if (sd.is_named)
            return "__named__" + sd.user_name + "__";

        // Sort fields by name for structural comparison.
        std::vector<std::pair<std::string,TypeInfo>> sorted_fields;
        sorted_fields.reserve(sd.fields.size());
        for (auto &f : sd.fields)
            sorted_fields.push_back({f.name, f.type});
        std::sort(sorted_fields.begin(), sorted_fields.end(),
                  [](auto &a, auto &b){ return a.first < b.first; });

        std::string key;
        for (auto &[name, ti] : sorted_fields) {
            key += name + ":";
            if (ti.tag == GlslType::Struct)
                key += "S" + std::to_string(ti.struct_id);
            else if (ti.is_tvar() || ti.is_unknown())
                key += "?";
            else
                key += glsl_type_name(ti.tag);
            key += ";";
        }
        return key;
    };

    // ── Step 3: deduplicate — multiple passes until stable ───────────────────
    // Each pass: for each struct, compute its key; if a lower-id struct with the
    // same key exists, merge the higher-id one into it.
    // Repeat until no merges happen (handles chains through nested structs).
    // Bounded by structs_.size() passes (monotone: each pass reduces IDs).
    bool changed = true;
    int dedup_limit = static_cast<int>(structs_.size()) + 1;
    while (changed && dedup_limit-- > 0) {
        changed = false;
        std::unordered_map<int,int> pass_remap;  // old → canonical within this pass
        // key → minimum struct_id seen with that shape
        std::map<std::string, int> key_to_canonical;

        // Accumulate global remap from previous passes so we skip already-dead structs.
        for (auto &sd : structs_) {
            // Skip structs already remapped to something else.
            if (struct_remap_.count(sd.id)) continue;

            // Apply any remaps already found this pass to field types first.
            for (auto &f : sd.fields)
                f.type = remap_ti(f.type, pass_remap);

            std::string key = struct_key(sd);
            if (key.empty() || key.find('?') != std::string::npos) continue;

            auto it = key_to_canonical.find(key);
            if (it == key_to_canonical.end()) {
                key_to_canonical[key] = sd.id;
            } else {
                // Always keep the minimum id as canonical.
                int canonical = std::min(it->second, sd.id);
                int loser     = std::max(it->second, sd.id);
                it->second = canonical;
                if (loser != canonical) {
                    pass_remap[loser] = canonical;
                    changed = true;
                }
            }
        }

        if (!pass_remap.empty()) {
            // Apply pass_remap globally to struct fields.
            for (auto &sd : structs_)
                for (auto &f : sd.fields)
                    f.type = remap_ti(f.type, pass_remap);

            // Apply to entry instance.
            if (entry_inst_) {
                remap_expr_map(entry_inst_->expr_types, pass_remap);
                entry_inst_->return_type = remap_ti(entry_inst_->return_type, pass_remap);
                for (auto &pt : entry_inst_->param_types)
                    pt = remap_ti(pt, pass_remap);
            }

            // Apply to all helper MonoInstance data.
            for (auto *inst : mono_order_) {
                remap_expr_map(inst->expr_types, pass_remap);
                inst->return_type = remap_ti(inst->return_type, pass_remap);
                for (auto &pt : inst->param_types)
                    pt = remap_ti(pt, pass_remap);
            }

            // Apply to global expr_types.
            remap_expr_map(expr_types_, pass_remap);

            // Accumulate into struct_remap_.
            for (auto &[old_id, new_id] : pass_remap)
                struct_remap_[old_id] = new_id;
        }
    }

    // ── Step 4: collect surviving struct ids ──────────────────────────────────
    // A struct is dead if it was merged into another (in struct_remap_) OR if
    // it is never referenced in any expression, param, return type, or field.
    std::unordered_set<int> dead_ids;
    for (auto &[old_id, new_id] : struct_remap_)
        dead_ids.insert(old_id);

    // Collect all live struct IDs from expressions and instance signatures.
    std::unordered_set<int> live_ids;
    auto mark_live = [&](TypeInfo ti) {
        if (ti.tag == GlslType::Struct) live_ids.insert(ti.struct_id);
    };
    auto mark_map = [&](const ExprTypeMap &m) {
        for (auto &kv : m) mark_live(kv.second);
    };
    mark_map(expr_types_);
    if (entry_inst_) {
        mark_map(entry_inst_->expr_types);
        mark_live(entry_inst_->return_type);
        for (auto &pt : entry_inst_->param_types) mark_live(pt);
    }
    for (auto *inst : mono_order_) {
        mark_map(inst->expr_types);
        mark_live(inst->return_type);
        for (auto &pt : inst->param_types) mark_live(pt);
    }
    // Also mark struct IDs referenced in struct fields (for nested structs).
    // Expand transitively.
    bool liveness_changed = true;
    while (liveness_changed) {
        liveness_changed = false;
        for (auto &sd : structs_) {
            if (!live_ids.count(sd.id)) continue;
            for (auto &f : sd.fields) {
                if (f.type.tag == GlslType::Struct) {
                    if (!live_ids.count(f.type.struct_id)) {
                        live_ids.insert(f.type.struct_id);
                        liveness_changed = true;
                    }
                }
            }
        }
    }

    // Remove dead and unreferenced structs.
    structs_.erase(std::remove_if(structs_.begin(), structs_.end(),
        [&](const StructDef &sd){
            return dead_ids.count(sd.id) > 0 || !live_ids.count(sd.id);
        }),
        structs_.end());

    // ── Step 5: topo-sort surviving structs by dependency ────────────────────
    // Build adjacency: struct A depends on struct B if any field of A has type B.
    std::unordered_map<int, std::vector<int>> deps; // id → ids it depends on
    for (auto &sd : structs_) {
        for (auto &f : sd.fields) {
            if (f.type.tag == GlslType::Struct && f.type.struct_id != sd.id)
                deps[sd.id].push_back(f.type.struct_id);
        }
    }

    // Kahn's algorithm.
    // Topo-sort using remaining_deps set: emit a node when all its deps are done.
    std::unordered_map<int, std::unordered_set<int>> remaining_deps;
    for (auto &sd : structs_) {
        remaining_deps[sd.id] = {};
        for (auto &f : sd.fields)
            if (f.type.tag == GlslType::Struct && f.type.struct_id != sd.id)
                remaining_deps[sd.id].insert(f.type.struct_id);
    }

    std::vector<StructDef> sorted;
    std::unordered_set<int> emitted_ids;
    sorted.reserve(structs_.size());

    // Build a map for quick lookup.
    std::unordered_map<int, const StructDef *> id_to_def;
    for (auto &sd : structs_) id_to_def[sd.id] = &sd;

    bool progress = true;
    while (sorted.size() < structs_.size() && progress) {
        progress = false;
        for (auto &sd : structs_) {
            if (emitted_ids.count(sd.id)) continue;
            // Check all deps are emitted.
            bool ready = true;
            for (int dep : remaining_deps[sd.id])
                if (!emitted_ids.count(dep)) { ready = false; break; }
            if (ready) {
                sorted.push_back(sd);
                emitted_ids.insert(sd.id);
                progress = true;
            }
        }
    }
    // If there are cycles (shouldn't happen with anonymous structs), append remainder.
    for (auto &sd : structs_)
        if (!emitted_ids.count(sd.id)) sorted.push_back(sd);

    structs_ = std::move(sorted);

    // ── Step 6: rename surviving structs S0, S1, ... in topo order ───────────
    // Named structs keep their user_name as glsl_name; anonymous ones get S0, S1, ...
    int name_idx = 0;
    for (auto &sd : structs_) {
        if (sd.is_named)
            sd.glsl_name = sd.user_name;
        else
            sd.glsl_name = "S" + std::to_string(name_idx++);
    }
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
    TypeCtx inst_ctx{inst.expr_types, inst.uf, next_struct_id_, structs_, named_structs_};
    type_block(*inst.body, sym, inst_ctx);
    pass3_solve(inst_ctx);

    // Find return type from the return statement.
    TypeInfo ret = TypeInfo::make(GlslType::Void);
    for (auto &s : *inst.body) {
        if (auto *rs = std::get_if<ReturnStmt>(&s->kind)) {
            if (rs->value) {
                const Expr *key = rs->value->get();
                auto it = inst.expr_types.find(key);
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
    std::vector<MonoInstance *> worklist;
    // entry lives in mono_registry_ — look it up by key to get a mutable ptr.
    worklist.push_back(&mono_registry_.at(entry.key));

    // The entry is already type-checked; scan it for calls.
    while (!worklist.empty()) {
        MonoInstance *cur = worklist.back();
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
                        // Record call-site → emitted name in the caller's instance.
                        const std::string &emitted = mono_registry_.at(key).emitted_name;
                        cur->call_names[&ek] = emitted;
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

    // ── Refinement pass ───────────────────────────────────────────────────────
    // The first scan registered instances with arg types collected before callee
    // return types were known (so some params were Unknown).  Now that all
    // instances are in mono_registry_, type_call can look up return types.
    // We iterate to fixed-point: each round typecheck all instances (callee-
    // first), then rescan every body to push newly-concrete arg types into
    // callee param_types.  Convergence is guaranteed because we only ever move
    // Unknown → concrete, never the reverse.
    std::vector<MonoInstance *> all_insts = mono_order_;
    all_insts.push_back(&mono_registry_.at(entry.key));

    // Fixed-point iteration: typecheck → rescan → repeat until no param_type
    // changes.
    bool changed = true;
    while (changed) {
        changed = false;

        for (MonoInstance *inst : all_insts) {
            inst->expr_types.clear();
            inst->uf = UnionFind{};
            inst->return_type = typecheck_instance(*inst, base_sym);
        }

        // Rescan every instance and propagate concrete arg types into callees.
        // Record whether any param_type was updated.
        for (MonoInstance *inst : all_insts) {
            // Snapshot current param_types for all instances before rescan.
            // We detect changes by comparing before/after inside the rescan lambda.
            // Simpler: pass a flag by reference into rescan_expr.
            std::function<void(MonoInstance &, const Expr &)> rescan_expr_chk;
            std::function<void(MonoInstance &, const Block &)> rescan_block_chk;

            rescan_expr_chk = [&](MonoInstance &inst2, const Expr &e) {
                std::visit([&](auto &ek) {
                    using T = std::decay_t<decltype(ek)>;
                    if constexpr (std::is_same_v<T, CallExpr>) {
                        std::vector<TypeInfo> arg_types;
                        for (auto &a : ek.args) {
                            TypeInfo ty;
                            auto it = inst2.expr_types.find(a.get());
                            ty = (it != inst2.expr_types.end()) ? it->second
                                                                 : TypeInfo::unknown();
                            if (ty.is_tvar()) ty = inst2.uf.resolve(ty);
                            arg_types.push_back(ty);
                        }

                        MonoKey key;
                        const FuncSig *sig = nullptr;
                        if (auto *ne = std::get_if<NameExpr>(&ek.callee->kind)) {
                            const Binding *b = base_sym.lookup(ne->name);
                            if (b && b->kind == BindingKind::Function) {
                                key.func_name = ne->name;
                                sig = find_sig(ne->name, "");
                            }
                        }

                        if (sig && !key.func_name.empty()) {
                            for (auto &[k, m] : mono_registry_) {
                                if (k.func_name != key.func_name) continue;
                                if (k.arg_types.size() != arg_types.size()) continue;
                                for (size_t i = 0; i < arg_types.size(); ++i) {
                                    if (!arg_types[i].is_unknown() &&
                                        (m.param_types[i].is_unknown() ||
                                         m.param_types[i].is_tvar())) {
                                        const_cast<MonoInstance &>(m).param_types[i] =
                                            arg_types[i];
                                        changed = true;
                                    }
                                }
                            }
                        }
                        for (auto &a : ek.args) rescan_expr_chk(inst2, *a);
                    } else {
                        if constexpr (std::is_same_v<T, BinaryExpr>) {
                            rescan_expr_chk(inst2, *ek.left);
                            rescan_expr_chk(inst2, *ek.right);
                        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                            rescan_expr_chk(inst2, *ek.operand);
                        } else if constexpr (std::is_same_v<T, FieldExpr>) {
                            rescan_expr_chk(inst2, *ek.base);
                        } else if constexpr (std::is_same_v<T, TableExpr>) {
                            for (auto &f : ek.fields) rescan_expr_chk(inst2, *f.value);
                        }
                    }
                }, e.kind);
            };

            rescan_block_chk = [&](MonoInstance &inst2, const Block &bl) {
                for (auto &s : bl) {
                    std::visit([&](auto &sk) {
                        using T = std::decay_t<decltype(sk)>;
                        if constexpr (std::is_same_v<T, LocalStmt>)
                            { if (sk.init) rescan_expr_chk(inst2, **sk.init); }
                        else if constexpr (std::is_same_v<T, AssignStmt>)
                            rescan_expr_chk(inst2, *sk.value);
                        else if constexpr (std::is_same_v<T, ExprStmt>)
                            rescan_expr_chk(inst2, *sk.expr);
                        else if constexpr (std::is_same_v<T, ReturnStmt>)
                            { if (sk.value) rescan_expr_chk(inst2, **sk.value); }
                        else if constexpr (std::is_same_v<T, IfStmt>)
                            for (auto &br : sk.branches) {
                                if (br.cond) rescan_expr_chk(inst2, *br.cond);
                                rescan_block_chk(inst2, br.body);
                            }
                        else if constexpr (std::is_same_v<T, WhileStmt>)
                            { rescan_expr_chk(inst2, *sk.cond);
                              rescan_block_chk(inst2, sk.body); }
                        else if constexpr (std::is_same_v<T, ForStmt>) {
                            rescan_expr_chk(inst2, *sk.start);
                            rescan_expr_chk(inst2, *sk.stop);
                            if (sk.step) rescan_expr_chk(inst2, **sk.step);
                            rescan_block_chk(inst2, sk.body);
                        }
                    }, s->kind);
                }
            };

            rescan_block_chk(*inst, *inst->body);
        }
    }

    // One final typecheck so expr_types reflect the fully-resolved param_types.
    for (MonoInstance *inst : all_insts) {
        inst->expr_types.clear();
        inst->uf = UnionFind{};
        inst->return_type = typecheck_instance(*inst, base_sym);
    }

    // ── Deduplication ─────────────────────────────────────────────────────────
    // "scene_4"), and a later scan created a concrete instance of the same
    // function (e.g. "scene_vec3"), the Unknown instance is now redundant.
    // Redirect every call_names reference from the Unknown instance's emitted
    // name to the concrete instance's emitted name, then drop the Unknown
    // instance from mono_order_ so the emitter never sees it.
    std::vector<MonoInstance *> to_remove;
    for (MonoInstance *inst : mono_order_) {
        // Check if any param is still Unknown/tvar.
        bool has_unknown = false;
        for (auto &pt : inst->param_types)
            if (pt.is_unknown() || (pt.is_tvar() &&
                inst->uf.resolve(pt).tag == GlslType::Unknown))
                { has_unknown = true; break; }
        if (!has_unknown) continue;

        // Look for a concrete sibling: same func_name, all-concrete param_types.
        MonoInstance *concrete = nullptr;
        for (auto &[k, m] : mono_registry_) {
            if (k.func_name != inst->key.func_name) continue;
            if (k.arg_types.size() != inst->key.arg_types.size()) continue;
            if (&m == inst) continue;
            bool all_conc = true;
            for (auto &pt : m.param_types)
                if (pt.is_unknown() || pt.is_tvar()) { all_conc = false; break; }
            if (all_conc) { concrete = const_cast<MonoInstance *>(&m); break; }
        }
        if (!concrete) continue;

        // Rewrite call_names in every instance: old emitted name → new.
        const std::string &old_name = inst->emitted_name;
        const std::string &new_name = concrete->emitted_name;
        for (auto &[k, m] : mono_registry_)
            for (auto &[ce, nm] : const_cast<MonoInstance &>(m).call_names)
                if (nm == old_name) nm = new_name;
        // Also fix the entry's call_names.
        for (auto &[ce, nm] : const_cast<MonoInstance &>(mono_registry_.at(entry.key)).call_names)
            if (nm == old_name) nm = new_name;

        to_remove.push_back(inst);
    }
    for (MonoInstance *inst : to_remove)
        mono_order_.erase(std::remove(mono_order_.begin(), mono_order_.end(), inst),
                          mono_order_.end());
}
