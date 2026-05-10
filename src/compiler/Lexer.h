#pragma once
#include <string>
#include <string_view>
#include <stdexcept>

// Token kinds — single-char tokens use their ASCII value (< 256).
// Multi-char / keyword tokens start at 256.
enum class TK : int {
    // Keep ORDER RESERVED in sync with luaX_tokens
    And = 256, Break,
    Do, Else, Elseif, End, False, For, Function,
    If, In, Local, Nil, Not, Or, Repeat,
    Return, Then, True, Until, While,
    // compound symbols
    Concat,   // ..
    Dots,     // ...
    Eq,       // ==
    Ge,       // >=
    Le,       // <=
    Ne,       // ~=
    // literals
    Number,
    Name,
    String,
    Eof,
};

struct Token {
    TK       kind;
    int      line;
    // payload (valid depending on kind)
    double      num_val;   // TK::Number
    std::string str_val;   // TK::Name, TK::String
};

// Raised on lex errors.
struct LexError : std::runtime_error {
    int line;
    LexError(int ln, const std::string &msg)
        : std::runtime_error(msg), line(ln) {}
};

class Lexer {
public:
    // source   — the Lua source text
    // src_name — filename or chunk label for error messages
    Lexer(std::string_view source, std::string src_name);

    // Advance and return the next token.
    Token next();

    // Peek at the next token without consuming it.
    const Token &peek() const;

    int current_line() const { return line_; }
    const std::string &source_name() const { return src_name_; }

private:
    std::string      src_;
    std::string      src_name_;
    mutable size_t   pos_  = 0;
    mutable int      line_ = 1;

    // one-token lookahead cache (mutable: peek() is logically const)
    mutable bool  have_peek_ = false;
    mutable Token peek_tok_;

    Token read_token() const;
    Token read_string(char delim) const;
    Token read_long_string() const;
    Token read_number() const;
    Token read_name_or_kw() const;

    int  cur()  const;
    int  peek_char() const;
    void advance() const;
    void skip_whitespace_and_comments() const;
    bool skip_long_string_content(int level, std::string *out) const;

    void inc_line() const;
    [[noreturn]] void lex_error(const std::string &msg) const;
};
