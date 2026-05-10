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

std::string Emitter::type_str(TypeInfo ti) const {
    if (ti.is_tvar()) ti = uf_->resolve(ti);
    if (ti.tag == GlslType::Struct)
        return "S" + std::to_string(ti.struct_id);
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

    // 5. Helper function instances (callee-before-caller)
    if (!mono_order.empty()) {
        line("// helper functions");
        for (auto *inst : mono_order)
            emit_instance(*inst, false);
        line();
    }

    // 6. Forward declaration of shader_main
    {
        std::string ret = type_str(entry.return_type);
        std::string sig = ret + " shader_main(";
        for (size_t i = 0; i < entry.param_names.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += type_str(entry.param_types[i]) + " " + entry.param_names[i];
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
    expr_types_ = &inst.expr_types;
    // UnionFind is per-instance but we only need resolve(); cast away const
    // because UnionFind::resolve is not const (path compression).
    uf_ = const_cast<UnionFind *>(&inst.uf);

    std::string name = is_entry ? "shader_main" : inst.emitted_name;
    std::string ret  = type_str(inst.return_type);

    std::string sig = ret + " " + name + "(";
    for (size_t i = 0; i < inst.param_names.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += type_str(inst.param_types[i]) + " " + inst.param_names[i];
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
                ty = resolved(**sk.init);
                if (ty.is_tvar()) ty = uf_->resolve(ty);
                if (ty.tag == GlslType::Unknown) ty = TypeInfo::make(GlslType::Float);
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
        }
    }, s.kind);
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

            // Generic call: callee(args)
            std::string s = expr_str(*ek.callee) + "(";
            for (size_t i = 0; i < ek.args.size(); ++i) {
                if (i > 0) s += ", ";
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
