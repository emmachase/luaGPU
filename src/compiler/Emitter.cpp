#include "Emitter.h"
#include "Lexer.h"
#include <cassert>
#include <sstream>

// ── Formatting helpers ────────────────────────────────────────────────────────

void Emitter::line(const std::string &s) {
    if (s.empty()) out_ << "\n";
    else           out_ << indent_str() << s << "\n";
}

void Emitter::indent_push() { ++indent_; }
void Emitter::indent_pop()  { if (indent_ > 0) --indent_; }

TypeInfo Emitter::resolved(const Expr &e) const {
    auto it = expr_types_->find(&e);
    if (it != expr_types_->end()) return it->second;
    return TypeInfo::unknown();
}

// If the expression is a CallExpr with a known emitted name in call_names_,
// look up the corresponding MonoInstance in mono_order_ and return its return_type.
// Falls back to TypeInfo::unknown() if not found.
TypeInfo Emitter::resolved_call_return(const Expr &e) const {
    if (!call_names_ || !mono_order_) return TypeInfo::unknown();
    auto *ce = std::get_if<CallExpr>(&e.kind);
    if (!ce) return TypeInfo::unknown();
    auto cn_it = call_names_->find(ce);
    if (cn_it == call_names_->end()) return TypeInfo::unknown();
    const std::string &emitted = cn_it->second;
    for (auto *inst : *mono_order_)
        if (inst->emitted_name == emitted) return inst->return_type;
    return TypeInfo::unknown();
}

std::string Emitter::type_str(TypeInfo ti) const {
    if (ti.is_tvar()) ti = uf_->resolve(ti);
    if (ti.tag == GlslType::Struct) {
        // Look up glsl_name from the struct definitions (handles named structs).
        if (structs_) {
            for (auto &sd : *structs_)
                if (sd.id == ti.struct_id) return sd.glsl_name;
        }
        // Fallback (should not happen after pass5).
        return "S" + std::to_string(ti.struct_id);
    }
    return glsl_type_name(ti.tag);
}

// ── Main entry ────────────────────────────────────────────────────────────────

std::string Emitter::emit(
    const ShaderFunc                  &sf,
    const std::vector<UniformDesc>    &uniforms,
    const std::vector<StructDef>      &structs,
    const std::vector<MonoInstance *> &mono_order,
    const MonoInstance                &entry)
{
    (void)sf; // ShaderFunc not needed — all info comes from instances
    mono_order_ = &mono_order;
    structs_    = &structs;

    // 1. Version
    line("#version 330 core");
    line();

    // 2. Built-in uniforms (always emitted)
    line("// built-in uniforms");
    line("uniform vec2  u_resolution;");
    line("uniform float u_time;");
    line("uniform float u_delta;");
    line("uniform vec2  u_mouse;");
    line();

    // 3. User uniforms
    if (!uniforms.empty()) {
        line("// user uniforms");
        for (auto &u : uniforms)
            line("uniform " + type_str(u.type) + " " + u.name + ";");
        line();
    }

    // 4. Struct definitions (already in declaration order)
    if (!structs.empty()) {
        line("// struct definitions");
        for (auto &sd : structs) {
            line("struct " + sd.glsl_name + " {");
            indent_push();
            for (auto &f : sd.fields)
                line(type_str(f.type) + " " + f.name + ";");
            indent_pop();
            line("};");
        }
        line();
    }

    // 5. Forward declarations for all helper instances
    if (!mono_order.empty()) {
        line("// forward declarations");
        for (auto *inst : mono_order) {
            UnionFind *iuf = const_cast<UnionFind *>(&inst->uf);
            auto res = [&](TypeInfo ti) -> TypeInfo {
                return ti.is_tvar() ? iuf->resolve(ti) : ti;
            };
            std::string sig = type_str(res(inst->return_type)) + " " +
                              inst->emitted_name + "(";
            for (size_t i = 0; i < inst->param_names.size(); ++i) {
                if (i > 0) sig += ", ";
                sig += type_str(res(inst->param_types[i])) + " " +
                       inst->param_names[i];
            }
            sig += ");";
            line(sig);
        }
        line();
    }

    // 6. Helper function instances (callee-before-caller)
    if (!mono_order.empty()) {
        line("// helper functions");
        for (auto *inst : mono_order)
            emit_instance(*inst, false);
        line();
    }

    // 6. Forward declaration of shader_main
    {
        UnionFind *entry_uf = const_cast<UnionFind *>(&entry.uf);
        auto res = [&](TypeInfo ti) -> TypeInfo {
            return ti.is_tvar() ? entry_uf->resolve(ti) : ti;
        };
        std::string ret = type_str(res(entry.return_type));
        std::string sig = ret + " shader_main(";
        for (size_t i = 0; i < entry.param_names.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += type_str(res(entry.param_types[i])) + " " + entry.param_names[i];
        }
        sig += ");";
        line("// forward declaration");
        line(sig);
        line();
    }

    // 7. void main() wrapper
    line("out vec4 frag_out;");
    line();
    line("void main() {");
    indent_push();
    line("vec2 uv = gl_FragCoord.xy / u_resolution;");
    {
        std::string call = "frag_out = shader_main(";
        for (size_t i = 0; i < entry.param_names.size(); ++i) {
            if (i > 0) call += ", ";
            call += entry.param_names[i];
        }
        call += ");";
        line(call);
    }
    indent_pop();
    line("}");
    line();

    // 8. shader_main definition
    emit_instance(entry, true);

    return out_.str();
}

// ── Instance emitter ──────────────────────────────────────────────────────────

void Emitter::emit_instance(const MonoInstance &inst, bool is_entry) {
    // Switch the per-expression context to this instance.
    const ExprTypeMap *prev_et = expr_types_;
    UnionFind         *prev_uf = uf_;
    const CallNameMap *prev_cn = call_names_;
    expr_types_ = &inst.expr_types;
    // UnionFind is per-instance but we only need resolve(); cast away const
    // because UnionFind::resolve is not const (path compression).
    uf_ = const_cast<UnionFind *>(&inst.uf);
    call_names_ = &inst.call_names;

    std::string name = is_entry ? "shader_main" : inst.emitted_name;

    // Resolve return type and param types through the instance's union-find so
    // that any type variables unified during typecheck_instance become concrete.
    auto resolve_ti = [&](TypeInfo ti) -> TypeInfo {
        if (ti.is_tvar()) return uf_->resolve(ti);
        return ti;
    };

    std::string ret  = type_str(resolve_ti(inst.return_type));

    std::string sig = ret + " " + name + "(";
    for (size_t i = 0; i < inst.param_names.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += type_str(resolve_ti(inst.param_types[i])) + " " + inst.param_names[i];
    }
    sig += ") {";
    line(sig);
    indent_push();
    emit_block(*inst.body);
    indent_pop();
    line("}");
    line();

    expr_types_ = prev_et;
    uf_         = prev_uf;
    call_names_ = prev_cn;
}

// ── Block / statement emitter ─────────────────────────────────────────────────

void Emitter::emit_block(const Block &block) {
    for (auto &s : block) {
        // LocalFuncStmt: already emitted as a MonoInstance — skip the declaration.
        if (std::holds_alternative<LocalFuncStmt>(s->kind)) continue;
        emit_stmt(*s);
    }
}

void Emitter::emit_stmt(const Stmt &s) {
    std::visit([&](auto &sk) {
        using T = std::decay_t<decltype(sk)>;

        if constexpr (std::is_same_v<T, LocalStmt>) {
            TypeInfo ty = TypeInfo::make(GlslType::Float);
            if (sk.init) {
                // If the initialiser is a numeric or bool literal, its type is
                // authoritative — do not let downstream unification widen it to a
                // vector type (e.g. `local t = 0.0` must stay float even when t
                // is later used in `rd * t` which would otherwise unify t as vec3).
                const Expr &init_expr = **sk.init;
                bool is_literal = std::holds_alternative<NumberLit>(init_expr.kind) ||
                                  std::holds_alternative<BoolLit>(init_expr.kind);
                if (is_literal) {
                    // Numeric literals are polymorphic tvars in the type system
                    // (to allow `vec2 * 2.0`), but as a local variable declaration
                    // the declared type should always be the narrow scalar type,
                    // not whatever the tvar was widened to by downstream uses.
                    if (std::holds_alternative<BoolLit>(init_expr.kind))
                        ty = TypeInfo::make(GlslType::Bool);
                    else
                        ty = TypeInfo::make(GlslType::Float);
                } else {
                    ty = resolved(init_expr);
                    if (ty.is_tvar()) ty = uf_->resolve(ty);
                    if (ty.tag == GlslType::Unknown)
                        ty = resolved_call_return(init_expr);
                    if (ty.tag == GlslType::Unknown) ty = TypeInfo::make(GlslType::Float);
                }
            }
            std::string decl = type_str(ty) + " " + sk.name;
            if (sk.init) decl += " = " + expr_str(**sk.init);
            line(decl + ";");

        } else if constexpr (std::is_same_v<T, LocalFuncStmt>) {
            // Skip — emitted as a separate MonoInstance before this function.
            (void)sk;

        } else if constexpr (std::is_same_v<T, AssignStmt>) {
            line(sk.target + " = " + expr_str(*sk.value) + ";");

        } else if constexpr (std::is_same_v<T, ExprStmt>) {
            line(expr_str(*sk.expr) + ";");

        } else if constexpr (std::is_same_v<T, ReturnStmt>) {
            if (sk.value) line("return " + expr_str(**sk.value) + ";");
            else          line("return;");

        } else if constexpr (std::is_same_v<T, IfStmt>) {
            bool first = true;
            for (auto &br : sk.branches) {
                if (br.cond) {
                    std::string kw = first ? "if" : "else if";
                    line(kw + " (" + expr_str(*br.cond) + ") {");
                } else {
                    line("else {");
                }
                indent_push();
                emit_block(br.body);
                indent_pop();
                line("}");
                first = false;
            }

        } else if constexpr (std::is_same_v<T, WhileStmt>) {
            line("while (" + expr_str(*sk.cond) + ") {");
            indent_push();
            emit_block(sk.body);
            indent_pop();
            line("}");

        } else if constexpr (std::is_same_v<T, ForStmt>) {
            std::string step_s = sk.step ? expr_str(**sk.step) : "1";
            std::string init_s = "int " + sk.var + " = " + expr_str(*sk.start);
            std::string cond_s = sk.var + " <= " + expr_str(*sk.stop);
            std::string inc_s  = (step_s == "1") ? sk.var + "++"
                                                  : sk.var + " += " + step_s;
            line("for (" + init_s + "; " + cond_s + "; " + inc_s + ") {");
            indent_push();
            emit_block(sk.body);
            indent_pop();
            line("}");

        } else if constexpr (std::is_same_v<T, BreakStmt>) {
            line("break;");

        } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
            // Named struct declarations are emitted in the struct definitions
            // section before all functions — nothing to emit here.
            (void)sk;
        }
    }, s.kind);
}

// ── Broadcast helper ─────────────────────────────────────────────────────────
// Emit expr e, wrapping it in vecN(...) if e resolves to a float scalar but
// vec_type is a float vector.  Used for genType builtins (pow, clamp, mix…).
std::string Emitter::expr_str_broadcast(const Expr &e, GlslType vec_type) {
    std::string s = expr_str(e);
    if (!is_float_vec(vec_type)) return s;
    TypeInfo ty = resolved(e);
    if (ty.is_tvar()) ty = uf_->resolve(ty);
    // Only promote plain float scalars (not already a vector or unknown).
    if (ty.tag == GlslType::Float) {
        s = std::string(glsl_type_name(vec_type)) + "(" + s + ")";
    }
    return s;
}

// ── Expression emitter ────────────────────────────────────────────────────────

std::string Emitter::expr_str(const Expr &e) {
    return std::visit([&](auto &ek) -> std::string {
        using T = std::decay_t<decltype(ek)>;

        if constexpr (std::is_same_v<T, NumberLit>) {
            double v = ek.value;
            std::ostringstream ss;
            // Emit with decimal point so GLSL treats it as a float literal.
            if (v == (long long)v) ss << (long long)v << ".0";
            else                   ss << v;
            return ss.str();

        } else if constexpr (std::is_same_v<T, BoolLit>) {
            return ek.value ? "true" : "false";

        } else if constexpr (std::is_same_v<T, StringLit>) {
            return "/* string: rejected */";

        } else if constexpr (std::is_same_v<T, NilLit>) {
            return "/* nil: rejected */";

        } else if constexpr (std::is_same_v<T, NameExpr>) {
            return ek.name;

        } else if constexpr (std::is_same_v<T, FieldExpr>) {
            // math.fn → just fn (bare GLSL builtin)
            if (auto *ne = std::get_if<NameExpr>(&ek.base->kind))
                if (ne->name == "math") return ek.field;
            return expr_str(*ek.base) + "." + ek.field;

        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            int Not_v = static_cast<int>(TK::Not);
            if (ek.op == Not_v) return "!(" + expr_str(*ek.operand) + ")";
            if (ek.op == '-')   return "-("  + expr_str(*ek.operand) + ")";
            return expr_str(*ek.operand);

        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            int And_v = static_cast<int>(TK::And);
            int Or_v  = static_cast<int>(TK::Or);

            // (a and b) or c  →  (a) ? (b) : (c)
            if (ek.op == Or_v) {
                if (auto *inner = std::get_if<BinaryExpr>(&ek.left->kind)) {
                    if (inner->op == And_v) {
                        return "(" + expr_str(*inner->left) + ") ? (" +
                               expr_str(*inner->right) + ") : (" +
                               expr_str(*ek.right) + ")";
                    }
                }
            }

            // % → mod() for float, % for int
            if (ek.op == '%') {
                TypeInfo lt = resolved(*ek.left);
                if (lt.is_tvar()) lt = uf_->resolve(lt);
                if (lt.tag == GlslType::Int || is_int_vec(lt.tag))
                    return "(" + expr_str(*ek.left) + " % " + expr_str(*ek.right) + ")";
                return "mod(" + expr_str(*ek.left) + ", " + expr_str(*ek.right) + ")";
            }

            std::string op_str;
            switch (ek.op) {
                case static_cast<int>(TK::And): op_str = "&&"; break;
                case static_cast<int>(TK::Or):  op_str = "||"; break;
                case static_cast<int>(TK::Eq):  op_str = "=="; break;
                case static_cast<int>(TK::Ne):  op_str = "!="; break;
                case static_cast<int>(TK::Le):  op_str = "<="; break;
                case static_cast<int>(TK::Ge):  op_str = ">="; break;
                default: op_str = std::string(1, (char)ek.op); break;
            }
            return "(" + expr_str(*ek.left) + " " + op_str + " " + expr_str(*ek.right) + ")";

        } else if constexpr (std::is_same_v<T, CallExpr>) {
            // bit.* → GLSL bitwise operators
            if (auto *fe = std::get_if<FieldExpr>(&ek.callee->kind)) {
                if (auto *ne = std::get_if<NameExpr>(&fe->base->kind)) {
                    if (ne->name == "bit") {
                        std::string a = ek.args.size() > 0 ? expr_str(*ek.args[0]) : "";
                        std::string b = ek.args.size() > 1 ? expr_str(*ek.args[1]) : "";
                        if (fe->field == "band")   return "(" + a + " & "  + b + ")";
                        if (fe->field == "bor")    return "(" + a + " | "  + b + ")";
                        if (fe->field == "bxor")   return "(" + a + " ^ "  + b + ")";
                        if (fe->field == "bnot")   return "(~" + a + ")";
                        if (fe->field == "lshift")  return "(" + a + " << " + b + ")";
                        if (fe->field == "rshift")  return "(" + a + " >> " + b + ")";
                    }
                    // math.fn → fn(args) — flatten namespace
                    if (ne->name == "math") {
                        std::string s = fe->field + "(";
                        for (size_t i = 0; i < ek.args.size(); ++i) {
                            if (i > 0) s += ", ";
                            s += expr_str(*ek.args[i]);
                        }
                        return s + ")";
                    }
                    // shaderlib.fn → emitted_name(args) — resolved from mono_registry
                    // The callee name is already the emitted name if in mono_order;
                    // for simplicity emit as lib_fn(...) and let the linker resolve.
                    {
                        std::string s = ne->name + "_" + fe->field + "(";
                        for (size_t i = 0; i < ek.args.size(); ++i) {
                            if (i > 0) s += ", ";
                            s += expr_str(*ek.args[i]);
                        }
                        return s + ")";
                    }
                }
            }

            // shl / shr globals
            if (auto *ne = std::get_if<NameExpr>(&ek.callee->kind)) {
                if (ne->name == "shl" && ek.args.size() == 2)
                    return "(" + expr_str(*ek.args[0]) + " << " + expr_str(*ek.args[1]) + ")";
                if (ne->name == "shr" && ek.args.size() == 2)
                    return "(" + expr_str(*ek.args[0]) + " >> " + expr_str(*ek.args[1]) + ")";
            }

            // Generic call: resolve emitted name from call_names_ if available
            std::string callee_name;
            if (call_names_) {
                auto it = call_names_->find(&ek);
                if (it != call_names_->end()) callee_name = it->second;
            }
            if (callee_name.empty()) callee_name = expr_str(*ek.callee);

            // genType builtins that require all args to be the same vector type.
            // If any arg is a float vector and another is a plain float scalar,
            // promote the scalar to vecN(x).
            static const char *gentype_builtins[] = {
                "pow", "clamp", "mix", "smoothstep", "step", "min", "max",
                "mod", "fma", nullptr
            };
            bool is_gentype = false;
            for (int gi = 0; gentype_builtins[gi]; ++gi) {
                if (callee_name == gentype_builtins[gi]) { is_gentype = true; break; }
            }

            // Find the widest float-vector type among the args (if any).
            GlslType vec_type = GlslType::Unknown;
            if (is_gentype) {
                for (auto &a : ek.args) {
                    TypeInfo at = resolved(*a);
                    if (at.is_tvar()) at = uf_->resolve(at);
                    if (is_float_vec(at.tag)) {
                        if (vec_type == GlslType::Unknown ||
                            vec_dim(at.tag) > vec_dim(vec_type))
                            vec_type = at.tag;
                    }
                }
            }

            std::string s = callee_name + "(";
            for (size_t i = 0; i < ek.args.size(); ++i) {
                if (i > 0) s += ", ";
                if (is_gentype && is_float_vec(vec_type))
                    s += expr_str_broadcast(*ek.args[i], vec_type);
                else
                    s += expr_str(*ek.args[i]);
            }
            return s + ")";

        } else if constexpr (std::is_same_v<T, TableExpr>) {
            // Struct constructor: S0(f0, f1, ...)
            TypeInfo ty = resolved(e);
            if (ty.is_tvar()) ty = uf_->resolve(ty);
            std::string s = type_str(ty) + "(";
            for (size_t i = 0; i < ek.fields.size(); ++i) {
                if (i > 0) s += ", ";
                s += expr_str(*ek.fields[i].value);
            }
            return s + ")";

        } else if constexpr (std::is_same_v<T, FuncExpr>) {
            return std::string("/* func */");
        } else {
            return std::string("");
        }
    }, e.kind);
}
