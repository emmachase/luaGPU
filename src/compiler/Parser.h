#pragma once
#include "Lexer.h"
#include "AST.h"
#include <stdexcept>

struct ParseError : std::runtime_error {
    int line;
    ParseError(int ln, const std::string &msg)
        : std::runtime_error(msg), line(ln) {}
};

class Parser {
public:
    // Parses the outer shader function body from already-lexed source.
    // Returns a ShaderFunc representing the full outer closure.
    static ShaderFunc parse(std::string_view source, std::string src_name);

private:
    explicit Parser(std::string_view source, std::string src_name);

    // ── statement parsers ─────────────────────────────────────────────────
    StmtPtr parse_stmt();
    StmtPtr parse_local();        // local x = ... or local function ...
    StmtPtr parse_if();
    StmtPtr parse_while();
    StmtPtr parse_for();
    StmtPtr parse_return();
    StmtPtr parse_expr_or_assign(); // expression statement or assignment
    Block   parse_block(bool is_top = false); // until end/else/elseif/until/eof

    // ── expression parsers (precedence climbing) ──────────────────────────
    ExprPtr parse_expr();
    ExprPtr parse_or_expr();
    ExprPtr parse_and_expr();
    ExprPtr parse_comparison();
    ExprPtr parse_concat();     // .. (rejected at semantic level, parsed here)
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();    // calls, field access, method calls
    ExprPtr parse_primary();

    // function expression: function(params) body end
    ExprPtr parse_func_expr(std::string name_hint = "");

    // table constructor: { field = expr, ... }
    ExprPtr parse_table_expr();

    // argument list: (expr, expr, ...)
    std::vector<ExprPtr> parse_args();

    // ── lexer helpers ──────────────────────────────────────────────────────
    const Token &peek() const;
    Token        consume();
    Token        expect(TK kind, const char *what);
    bool         check(TK kind) const;
    bool         match(TK kind);  // consume if matches, return true
    SrcLoc       loc() const;

    [[noreturn]] void error(const std::string &msg);
    [[noreturn]] void error_at(int line, const std::string &msg);

    Lexer       lex_;
    std::string src_name_;

    // block-terminator helpers
    bool is_block_end() const;
};
