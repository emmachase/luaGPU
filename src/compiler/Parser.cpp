#include "Parser.h"
#include <cassert>

// ── helpers ───────────────────────────────────────────────────────────────────

static int tk_int(TK k) { return static_cast<int>(k); }

ShaderFunc Parser::parse(std::string_view source, std::string src_name) {
    Parser p(source, src_name);

    // Accept either of two top-level forms:
    //
    //   Form A (bare, used in tests):
    //     function(u_time, u_resolution)
    //         ...
    //     end
    //
    //   Form B (full Lua file, used in example .lua files):
    //     local <name> = shader(function(u_time, u_resolution)
    //         ...
    //     end)
    //
    // Detect Form B by peeking for 'local'.
    bool wrapped = false;
    if (p.check(TK::Local)) {
        p.consume();                                      // 'local'
        p.expect(TK::Name, "shader variable name");       // <name>
        p.expect(TK{(int)'='}, "'='");
        p.expect(TK::Name, "'shader'");                   // 'shader'
        p.expect(TK{(int)'('}, "'('");
        wrapped = true;
    }

    p.expect(TK::Function, "'function'");

    // outer param list
    p.expect(TK{(int)'('}, "'('");
    std::vector<std::string> params;
    if (!p.check(TK{(int)')'})) {
        do {
            Token t = p.expect(TK::Name, "parameter name");
            params.push_back(t.str_val);
        } while (p.match(TK{(int)','}));
    }
    p.expect(TK{(int)')'}, "')'");

    Block body = p.parse_block();
    p.expect(TK::End, "'end'");

    if (wrapped)
        p.expect(TK{(int)')'}, "')'");  // closing ')' of shader(...)

    // When wrapped (Form B), trailing Lua (e.g. "return myShader") is fine.
    if (!wrapped && !p.check(TK::Eof))
        p.error("unexpected token after outer function");

    ShaderFunc sf;
    sf.src_name       = src_name;
    sf.uniform_params = std::move(params);
    sf.body           = std::move(body);
    return sf;
}

Parser::Parser(std::string_view source, std::string src_name)
    : lex_(source, src_name), src_name_(std::move(src_name))
{}

// ── lexer helpers ─────────────────────────────────────────────────────────────

const Token &Parser::peek() const { return lex_.peek(); }

Token Parser::consume() { return lex_.next(); }

Token Parser::expect(TK kind, const char *what) {
    if (peek().kind != kind)
        error(std::string("expected ") + what + ", got '" +
              (peek().kind == TK::Eof ? "<eof>" :
               peek().kind == TK::Name ? peek().str_val : "?") + "'");
    return consume();
}

bool Parser::check(TK kind) const {
    return lex_.peek().kind == kind;
}

bool Parser::match(TK kind) {
    if (check(kind)) { consume(); return true; }
    return false;
}

SrcLoc Parser::loc() const {
    return SrcLoc{src_name_, lex_.peek().line};
}

void Parser::error(const std::string &msg) {
    error_at(lex_.peek().line, msg);
}

void Parser::error_at(int line, const std::string &msg) {
    throw ParseError(line, src_name_ + ":" + std::to_string(line) + ": " + msg);
}

bool Parser::is_block_end() const {
    TK k = peek().kind;
    return k == TK::End    || k == TK::Else   || k == TK::Elseif ||
           k == TK::Until  || k == TK::Eof;
}

// ── block ──────────────────────────────────────────────────────────────────────

Block Parser::parse_block(bool /*is_top*/) {
    Block stmts;
    while (!is_block_end()) {
        // skip stray semicolons
        if (check(TK{(int)';'})) { consume(); continue; }
        stmts.push_back(parse_stmt());
        // after a return, stop parsing statements in this block
        if (stmts.back() && std::holds_alternative<ReturnStmt>(stmts.back()->kind))
            break;
    }
    return stmts;
}

// ── statements ────────────────────────────────────────────────────────────────

StmtPtr Parser::parse_stmt() {
    SrcLoc l = loc();
    TK k = peek().kind;

    if (k == TK::Local)  { auto s = parse_local();           s->loc = l; return s; }
    if (k == TK::If)     { auto s = parse_if();              s->loc = l; return s; }
    if (k == TK::While)  { auto s = parse_while();           s->loc = l; return s; }
    if (k == TK::For)    { auto s = parse_for();             s->loc = l; return s; }
    if (k == TK::Return) { auto s = parse_return();          s->loc = l; return s; }
    if (k == TK::Break)  { consume(); auto s = std::make_unique<Stmt>(); s->kind = BreakStmt{}; s->loc = l; return s; }

    return parse_expr_or_assign();
}

StmtPtr Parser::parse_local() {
    consume(); // 'local'
    SrcLoc l = loc();

    if (check(TK::Function)) {
        consume(); // 'function'
        Token name = expect(TK::Name, "function name");
        expect(TK{(int)'('}, "'('");
        std::vector<std::string> params;
        if (!check(TK{(int)')'})) {
            do {
                Token p = expect(TK::Name, "parameter name");
                params.push_back(p.str_val);
            } while (match(TK{(int)','}));
        }
        expect(TK{(int)')'}, "')'");
        Block body = parse_block();
        expect(TK::End, "'end'");

        auto s = std::make_unique<Stmt>();
        s->kind = LocalFuncStmt{name.str_val, std::move(params), std::move(body)};
        s->loc  = l;
        return s;
    }

    Token name = expect(TK::Name, "variable name");
    std::optional<ExprPtr> init;
    if (match(TK{(int)'='}))
        init = parse_expr();

    auto s = std::make_unique<Stmt>();
    s->kind = LocalStmt{name.str_val, std::move(init)};
    s->loc  = l;
    return s;
}

StmtPtr Parser::parse_if() {
    consume(); // 'if'
    IfStmt stmt;

    // if branch
    ExprPtr cond = parse_expr();
    expect(TK::Then, "'then'");
    Block body = parse_block();
    stmt.branches.push_back(IfBranch{std::move(cond), std::move(body)});

    // elseif branches
    while (check(TK::Elseif)) {
        consume();
        ExprPtr ec = parse_expr();
        expect(TK::Then, "'then'");
        Block   eb = parse_block();
        stmt.branches.push_back(IfBranch{std::move(ec), std::move(eb)});
    }

    // else branch
    if (match(TK::Else)) {
        Block eb = parse_block();
        stmt.branches.push_back(IfBranch{nullptr, std::move(eb)});
    }

    expect(TK::End, "'end'");

    auto s = std::make_unique<Stmt>();
    s->kind = std::move(stmt);
    return s;
}

StmtPtr Parser::parse_while() {
    consume(); // 'while'
    ExprPtr cond = parse_expr();
    expect(TK::Do, "'do'");
    Block body = parse_block();
    expect(TK::End, "'end'");

    auto s = std::make_unique<Stmt>();
    s->kind = WhileStmt{std::move(cond), std::move(body)};
    return s;
}

StmtPtr Parser::parse_for() {
    consume(); // 'for'
    Token var = expect(TK::Name, "loop variable");
    expect(TK{(int)'='}, "'='");
    ExprPtr start = parse_expr();
    expect(TK{(int)','}, "','");
    ExprPtr stop = parse_expr();
    std::optional<ExprPtr> step;
    if (match(TK{(int)','}))
        step = parse_expr();
    expect(TK::Do, "'do'");
    Block body = parse_block();
    expect(TK::End, "'end'");

    auto s = std::make_unique<Stmt>();
    s->kind = ForStmt{var.str_val, std::move(start), std::move(stop), std::move(step), std::move(body)};
    return s;
}

StmtPtr Parser::parse_return() {
    consume(); // 'return'
    std::optional<ExprPtr> val;
    if (!is_block_end() && !check(TK{(int)';'}))
        val = parse_expr();
    match(TK{(int)';'});

    auto s = std::make_unique<Stmt>();
    s->kind = ReturnStmt{std::move(val)};
    return s;
}

StmtPtr Parser::parse_expr_or_assign() {
    SrcLoc l = loc();
    ExprPtr e = parse_expr();

    // assignment?  (only simple name = expr in shader body)
    if (check(TK{(int)'='})) {
        consume();
        if (!std::holds_alternative<NameExpr>(e->kind))
            error_at(l.line, "invalid assignment target");
        std::string name = std::get<NameExpr>(e->kind).name;
        ExprPtr val = parse_expr();
        auto s = std::make_unique<Stmt>();
        s->kind = AssignStmt{std::move(name), std::move(val)};
        s->loc  = l;
        return s;
    }

    auto s = std::make_unique<Stmt>();
    s->kind = ExprStmt{std::move(e)};
    s->loc  = l;
    return s;
}

// ── expressions (precedence climbing) ────────────────────────────────────────
//
// Precedence (low → high):
//   or
//   and
//   < > <= >= == ~=
//   ..
//   + -
//   * / %
//   unary: not  -
//   postfix: () .field
//   primary

ExprPtr Parser::parse_expr() { return parse_or_expr(); }

ExprPtr Parser::parse_or_expr() {
    ExprPtr left = parse_and_expr();
    while (check(TK::Or)) {
        SrcLoc l = loc();
        consume();
        ExprPtr right = parse_and_expr();
        auto e = std::make_unique<Expr>();
        e->kind = BinaryExpr{tk_int(TK::Or), std::move(left), std::move(right)};
        e->loc  = l;
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_and_expr() {
    ExprPtr left = parse_comparison();
    while (check(TK::And)) {
        SrcLoc l = loc();
        consume();
        ExprPtr right = parse_comparison();
        auto e = std::make_unique<Expr>();
        e->kind = BinaryExpr{tk_int(TK::And), std::move(left), std::move(right)};
        e->loc  = l;
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_comparison() {
    ExprPtr left = parse_concat();
    for (;;) {
        TK k = peek().kind;
        if (k != TK::Eq && k != TK::Ne &&
            k != TK{(int)'<'} && k != TK{(int)'>'} &&
            k != TK::Le && k != TK::Ge)
            break;
        SrcLoc l = loc();
        consume();
        ExprPtr right = parse_concat();
        auto e = std::make_unique<Expr>();
        e->kind = BinaryExpr{tk_int(k), std::move(left), std::move(right)};
        e->loc  = l;
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_concat() {
    ExprPtr left = parse_additive();
    if (check(TK::Concat)) {
        SrcLoc l = loc();
        consume();
        ExprPtr right = parse_concat(); // right-associative
        auto e = std::make_unique<Expr>();
        e->kind = BinaryExpr{tk_int(TK::Concat), std::move(left), std::move(right)};
        e->loc  = l;
        return e; // semantic pass will reject this
    }
    return left;
}

ExprPtr Parser::parse_additive() {
    ExprPtr left = parse_multiplicative();
    for (;;) {
        TK k = peek().kind;
        if (k != TK{(int)'+'} && k != TK{(int)'-'}) break;
        SrcLoc l = loc();
        consume();
        ExprPtr right = parse_multiplicative();
        auto e = std::make_unique<Expr>();
        e->kind = BinaryExpr{tk_int(k), std::move(left), std::move(right)};
        e->loc  = l;
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_multiplicative() {
    ExprPtr left = parse_unary();
    for (;;) {
        TK k = peek().kind;
        if (k != TK{(int)'*'} && k != TK{(int)'/'} && k != TK{(int)'%'}) break;
        SrcLoc l = loc();
        consume();
        ExprPtr right = parse_unary();
        auto e = std::make_unique<Expr>();
        e->kind = BinaryExpr{tk_int(k), std::move(left), std::move(right)};
        e->loc  = l;
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    SrcLoc l = loc();
    if (check(TK::Not)) {
        consume();
        ExprPtr operand = parse_unary();
        auto e = std::make_unique<Expr>();
        e->kind = UnaryExpr{tk_int(TK::Not), std::move(operand)};
        e->loc  = l;
        return e;
    }
    if (check(TK{(int)'-'})) {
        consume();
        ExprPtr operand = parse_unary();
        auto e = std::make_unique<Expr>();
        e->kind = UnaryExpr{(int)'-', std::move(operand)};
        e->loc  = l;
        return e;
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    ExprPtr base = parse_primary();
    for (;;) {
        SrcLoc l = loc();
        if (check(TK{(int)'.'})) {
            consume();
            Token field = expect(TK::Name, "field name");
            auto e = std::make_unique<Expr>();
            e->kind = FieldExpr{std::move(base), field.str_val};
            e->loc  = l;
            base = std::move(e);
        } else if (check(TK{(int)'('})) {
            auto args = parse_args();
            auto e = std::make_unique<Expr>();
            e->kind = CallExpr{std::move(base), std::move(args)};
            e->loc  = l;
            base = std::move(e);
        } else {
            break;
        }
    }
    return base;
}

ExprPtr Parser::parse_primary() {
    SrcLoc l = loc();

    // grouped expression
    if (check(TK{(int)'('})) {
        consume();
        ExprPtr e = parse_expr();
        expect(TK{(int)')'}, "')'");
        return e;
    }

    // function expression
    if (check(TK::Function)) {
        return parse_func_expr();
    }

    // table constructor
    if (check(TK{(int)'{'})) {
        return parse_table_expr();
    }

    Token t = consume();
    auto e = std::make_unique<Expr>();
    e->loc = l;

    switch (t.kind) {
        case TK::Number: e->kind = NumberLit{t.num_val};   break;
        case TK::String: e->kind = StringLit{t.str_val};   break;
        case TK::True:   e->kind = BoolLit{true};           break;
        case TK::False:  e->kind = BoolLit{false};          break;
        case TK::Nil:    e->kind = NilLit{};                break;
        case TK::Name:   e->kind = NameExpr{t.str_val};     break;
        default:
            error_at(t.line, "unexpected token in expression");
    }
    return e;
}

ExprPtr Parser::parse_func_expr(std::string name_hint) {
    SrcLoc l = loc();
    consume(); // 'function'

    // optional name after 'function' keyword (e.g. return function main(uv))
    std::string fname = std::move(name_hint);
    if (check(TK::Name)) {
        fname = consume().str_val;
    }

    expect(TK{(int)'('}, "'('");
    std::vector<std::string> params;
    if (!check(TK{(int)')'})) {
        do {
            Token p = expect(TK::Name, "parameter name");
            params.push_back(p.str_val);
        } while (match(TK{(int)','}));
    }
    expect(TK{(int)')'}, "')'");
    Block body = parse_block();
    expect(TK::End, "'end'");

    auto e = std::make_unique<Expr>();
    e->kind = FuncExpr{std::move(fname), std::move(params), std::move(body)};
    e->loc  = l;
    return e;
}

ExprPtr Parser::parse_table_expr() {
    SrcLoc l = loc();
    consume(); // '{'
    TableExpr tbl;
    while (!check(TK{(int)'}'})) {
        if (check(TK::Eof)) error("unfinished table constructor");
        // only named fields: key = value
        Token key = expect(TK::Name, "field name");
        expect(TK{(int)'='}, "'='");
        ExprPtr val = parse_expr();
        tbl.fields.push_back(TableField{key.str_val, std::move(val)});
        if (!match(TK{(int)','}) && !match(TK{(int)';'})) break;
    }
    expect(TK{(int)'}'}, "'}'");
    auto e = std::make_unique<Expr>();
    e->kind = std::move(tbl);
    e->loc  = l;
    return e;
}

std::vector<ExprPtr> Parser::parse_args() {
    expect(TK{(int)'('}, "'('");
    std::vector<ExprPtr> args;
    if (!check(TK{(int)')'})) {
        do {
            args.push_back(parse_expr());
        } while (match(TK{(int)','}));
    }
    expect(TK{(int)')'}, "')'");
    return args;
}
