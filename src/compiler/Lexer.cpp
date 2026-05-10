#include "Lexer.h"
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <unordered_map>

// ── keyword table ───────────────────────────────────────────────────────────
static const std::unordered_map<std::string, TK> kKeywords = {
    {"and",      TK::And},
    {"break",    TK::Break},
    {"do",       TK::Do},
    {"else",     TK::Else},
    {"elseif",   TK::Elseif},
    {"end",      TK::End},
    {"false",    TK::False},
    {"for",      TK::For},
    {"function", TK::Function},
    {"if",       TK::If},
    {"in",       TK::In},
    {"local",    TK::Local},
    {"nil",      TK::Nil},
    {"not",      TK::Not},
    {"or",       TK::Or},
    {"repeat",   TK::Repeat},
    {"return",   TK::Return},
    {"then",     TK::Then},
    {"true",     TK::True},
    {"until",    TK::Until},
    {"while",    TK::While},
};

// ── constructor ─────────────────────────────────────────────────────────────
Lexer::Lexer(std::string_view source, std::string src_name)
    : src_(source), src_name_(std::move(src_name))
{}

// ── public interface ─────────────────────────────────────────────────────────
Token Lexer::next() {
    if (have_peek_) {
        have_peek_ = false;
        return peek_tok_;
    }
    return read_token();
}

const Token &Lexer::peek() const {
    if (!have_peek_) {
        peek_tok_  = read_token();
        have_peek_ = true;
    }
    return peek_tok_;
}

// ── internal helpers ─────────────────────────────────────────────────────────
int Lexer::cur() const {
    if (pos_ >= src_.size()) return -1;
    return (unsigned char)src_[pos_];
}

int Lexer::peek_char() const {
    if (pos_ + 1 >= src_.size()) return -1;
    return (unsigned char)src_[pos_ + 1];
}

void Lexer::advance() const {
    if (pos_ < src_.size()) ++pos_;
}

void Lexer::inc_line() const {
    // current char should be \n or \r
    int old = cur();
    advance(); // skip \n or \r
    // handle \r\n and \n\r as a single newline
    if ((cur() == '\n' || cur() == '\r') && cur() != old)
        advance();
    ++line_;
}

void Lexer::lex_error(const std::string &msg) const {
    throw LexError(line_, src_name_ + ":" + std::to_string(line_) + ": " + msg);
}

// ── skip whitespace and comments ─────────────────────────────────────────────
void Lexer::skip_whitespace_and_comments() const {
    for (;;) {
        int c = cur();
        if (c == -1) return;

        if (c == '\n' || c == '\r') {
            inc_line();
            continue;
        }

        if (std::isspace(c)) {
            advance();
            continue;
        }

        // comment?
        if (c == '-' && peek_char() == '-') {
            advance(); advance(); // skip --
            // long comment?
            if (cur() == '[') {
                size_t save_pos = pos_;
                int    save_ln  = line_;
                advance(); // skip [
                int level = 0;
                while (cur() == '=') { advance(); ++level; }
                if (cur() == '[') {
                    advance(); // skip second [
                    // skip long comment content
                    skip_long_string_content(level, nullptr);
                    continue;
                } else {
                    // not a long comment — treat as short comment
                    pos_  = save_pos;
                    line_ = save_ln;
                    // fall through to short comment handling
                }
            }
            // short comment: skip to end of line
            while (cur() != -1 && cur() != '\n' && cur() != '\r')
                advance();
            continue;
        }

        return; // non-whitespace, non-comment
    }
}

// Skips content of a long string/comment of the given bracket level.
// If out != nullptr, appends the content to *out.
// Returns true on success, false if EOZ reached before closing bracket.
bool Lexer::skip_long_string_content(int level, std::string *out) const {
    // We enter right after the opening [[, [=[, etc.
    // Skip optional leading newline
    if (cur() == '\n' || cur() == '\r') inc_line();

    for (;;) {
        int c = cur();
        if (c == -1) return false; // EOZ

        if (c == '\n' || c == '\r') {
            if (out) out->push_back('\n');
            inc_line();
            continue;
        }

        if (c == ']') {
            advance();
            int cnt = 0;
            while (cur() == '=' && cnt < level) { advance(); ++cnt; }
            if (cnt == level && cur() == ']') {
                advance(); // consume closing ]
                return true;
            }
            // not a closing bracket — emit what we consumed
            if (out) {
                out->push_back(']');
                for (int i = 0; i < cnt; ++i) out->push_back('=');
            }
            continue;
        }

        if (out) out->push_back((char)c);
        advance();
    }
}

// ── token readers ─────────────────────────────────────────────────────────────
Token Lexer::read_long_string() const {
    // pos_ is right after the first [; count = signs
    int level = 0;
    while (cur() == '=') { advance(); ++level; }
    if (cur() != '[') lex_error("invalid long string delimiter");
    advance(); // skip second [

    std::string content;
    if (!skip_long_string_content(level, &content))
        lex_error("unfinished long string");

    return Token{TK::String, line_, 0.0, std::move(content)};
}

Token Lexer::read_string(char delim) const {
    advance(); // skip opening quote
    std::string s;
    for (;;) {
        int c = cur();
        if (c == -1 || c == '\n' || c == '\r')
            lex_error("unfinished string");
        if (c == delim) { advance(); break; }
        if (c != '\\') {
            s.push_back((char)c);
            advance();
            continue;
        }
        // escape
        advance();
        c = cur();
        switch (c) {
            case 'a': s.push_back('\a'); advance(); break;
            case 'b': s.push_back('\b'); advance(); break;
            case 'f': s.push_back('\f'); advance(); break;
            case 'n': s.push_back('\n'); advance(); break;
            case 'r': s.push_back('\r'); advance(); break;
            case 't': s.push_back('\t'); advance(); break;
            case 'v': s.push_back('\v'); advance(); break;
            case '\n': case '\r': s.push_back('\n'); inc_line(); break;
            case -1:  lex_error("unfinished string"); break;
            default:
                if (std::isdigit(c)) {
                    int val = 0;
                    for (int i = 0; i < 3 && std::isdigit(cur()); ++i) {
                        val = val * 10 + (cur() - '0');
                        advance();
                    }
                    if (val > 255) lex_error("escape sequence too large");
                    s.push_back((char)val);
                } else {
                    s.push_back((char)c);
                    advance();
                }
                break;
        }
    }
    return Token{TK::String, line_, 0.0, std::move(s)};
}

Token Lexer::read_number() const {
    int start_line = line_;
    std::string buf;

    // hex?
    if (cur() == '0' && (peek_char() == 'x' || peek_char() == 'X')) {
        buf.push_back((char)cur()); advance();
        buf.push_back((char)cur()); advance();
        while (std::isxdigit(cur()) || cur() == '_') {
            if (cur() != '_') buf.push_back((char)cur());
            advance();
        }
    } else {
        while (std::isdigit(cur())) { buf.push_back((char)cur()); advance(); }
        if (cur() == '.') { buf.push_back('.'); advance(); }
        while (std::isdigit(cur())) { buf.push_back((char)cur()); advance(); }
        if (cur() == 'e' || cur() == 'E') {
            buf.push_back((char)cur()); advance();
            if (cur() == '+' || cur() == '-') { buf.push_back((char)cur()); advance(); }
            while (std::isdigit(cur())) { buf.push_back((char)cur()); advance(); }
        }
    }

    char *end;
    double val = std::strtod(buf.c_str(), &end);
    if (end != buf.c_str() + buf.size())
        lex_error("malformed number '" + buf + "'");

    return Token{TK::Number, start_line, val, {}};
}

Token Lexer::read_name_or_kw() const {
    int start_line = line_;
    std::string name;
    while (cur() != -1 && (std::isalnum(cur()) || cur() == '_')) {
        name.push_back((char)cur());
        advance();
    }
    auto it = kKeywords.find(name);
    if (it != kKeywords.end())
        return Token{it->second, start_line, 0.0, {}};
    return Token{TK::Name, start_line, 0.0, std::move(name)};
}

// ── main dispatch ────────────────────────────────────────────────────────────
Token Lexer::read_token() const {
    skip_whitespace_and_comments();

    int start_line = line_;
    int c = cur();

    if (c == -1) return Token{TK::Eof, line_, 0.0, {}};

    // identifiers / keywords
    if (std::isalpha(c) || c == '_') return read_name_or_kw();

    // numbers
    if (std::isdigit(c)) return read_number();
    if (c == '.' && std::isdigit(peek_char())) return read_number();

    // strings
    if (c == '"' || c == '\'') return read_string((char)c);

    // compound / single-char tokens
    advance();
    switch (c) {
        case '.':
            if (cur() == '.') {
                advance();
                if (cur() == '.') { advance(); return Token{TK::Dots,   start_line}; }
                return Token{TK::Concat, start_line};
            }
            return Token{TK{(int)'.'},  start_line};
        case '=':
            if (cur() == '=') { advance(); return Token{TK::Eq, start_line}; }
            return Token{TK{(int)'='},  start_line};
        case '<':
            if (cur() == '=') { advance(); return Token{TK::Le, start_line}; }
            return Token{TK{(int)'<'},  start_line};
        case '>':
            if (cur() == '=') { advance(); return Token{TK::Ge, start_line}; }
            return Token{TK{(int)'>'},  start_line};
        case '~':
            if (cur() == '=') { advance(); return Token{TK::Ne, start_line}; }
            lex_error("unexpected character '~'");
        case '[': {
            // could be a long string
            if (cur() == '[' || cur() == '=') {
                size_t save_pos = pos_;
                int    save_ln  = line_;
                int level = 0;
                while (cur() == '=') { advance(); ++level; }
                if (cur() == '[') {
                    advance(); // skip second [
                    std::string content;
                    if (!skip_long_string_content(level, &content))
                        lex_error("unfinished long string");
                    return Token{TK::String, start_line, 0.0, std::move(content)};
                }
                // not a long string — restore and return bare [
                pos_  = save_pos;
                line_ = save_ln;
            }
            return Token{TK{(int)'['},  start_line};
        }
        default:
            return Token{TK{c}, start_line};
    }
}
