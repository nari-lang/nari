#pragma once

#include <string>
#include <vector>

// Token types shared between the lexer (parser/lexer.cpp) and the
// precedence-climbing parser (parser.cpp). Split out of parser.cpp.
namespace Parser {

enum class TokenKind {
    TK_EOF,
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    // `interpolated strings like this {some_var}!`
    TK_INTERP_STRING,
    // (
    TK_LPAREN,
    // )
    TK_RPAREN,
    // {
    TK_LBRACE,
    // }
    TK_RBRACE,
    // [
    TK_LBRACKET,
    // ]
    TK_RBRACKET,
    // ,
    TK_COMMA,
    // =
    TK_EQUAL,
    // ;
    TK_SEMICOLON,
    // :
    TK_COLON,
    // ::
    TK_COLONCOLON,
    // .
    TK_DOT,
    // ...
    TK_ELLIPSIS,
    // ?
    TK_QUESTION,
    // ?.
    TK_QUESTIONDOT,
    // @
    TK_AT,
    // +
    TK_PLUS,
    // -
    TK_MINUS,
    // *
    TK_STAR,
    // /
    TK_SLASH,
    // !
    TK_BANG,
    // <
    TK_LT,
    // >
    TK_GT,
    // <=
    TK_LE,
    // >=
    TK_GE,
    // ==
    TK_EQEQ,
    // !=
    TK_NEQ,
    // &&
    TK_ANDAND,
    // ||
    TK_OROR,
    // %
    TK_PERCENT,
    // ++
    TK_PLUSPLUS,
    // --
    TK_MINUSMINUS,
    // +=
    TK_PLUSEQ,
    // -=
    TK_MINUSEQ,
    // *=
    TK_STAREQ,
    // /=
    TK_SLASHEQ,
    // %=
    TK_PERCENTEQ,
    // **
    TK_EXPONENT,
    // ??
    TK_NULLISHCOALESCE,
    // ->
    TK_ARROW,
    // =>
    TK_FATARROW,
    // &
    TK_AMPERSAND,
    // |
    TK_PIPE,
    // ^
    TK_CARET,
    // ~
    TK_TILDE,
    // <<
    TK_LSHIFT,
    // >>
    TK_RSHIFT,
    // &=
    TK_AMPEQ,
    // |=
    TK_PIPEEQ,
    // ^=
    TK_CARETEQ,
    // <<=
    TK_LSHIFTEQ,
    // >>=
    TK_RSHIFTEQ,
    // regex literal /pattern/flags
    TK_REGEX,
    // unknown token
    TK_UNKNOWN
};

struct Token {
    TokenKind kind = TokenKind::TK_UNKNOWN;
    std::string text; // identifier name, string contents (unquoted), number text, or symbol text
    int line = 0;
    int col = 0;
    std::string filename; // for error reporting

    // For TK_INTERP_STRING: stores alternating string parts and expression source
    std::vector<std::string> interp_parts;   // String literals
    std::vector<std::string> interp_exprs;   // Expression source code to parse
    std::vector<std::string> interp_formats; // Optional format specs after ':'

    std::string stringify() const {
        if (text.empty()) {
            return "<empty>";
        }
        return text;
    }
};

} // namespace Parser
