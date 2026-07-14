/*
  parser/lexer.cpp
  The tokenizer, split out of parser.cpp. Turns source text into a Token stream
  for the precedence-climbing parser. Handles comments, multi-char operators,
  string + backtick-interpolated-string literals, regex literals, numbers and
  identifiers.
*/

#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Parser {

static std::string trim_ascii(std::string_view s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

static size_t find_top_level_format_colon(const std::string &expr) {
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    char quote = '\0';
    bool escape = false;

    for (size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if (quote) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }

        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            continue;
        }

        switch (c) {
            case '(':
                ++paren_depth;
                break;
            case ')':
                if (paren_depth > 0) {
                    --paren_depth;
                }
                break;
            case '[':
                ++bracket_depth;
                break;
            case ']':
                if (bracket_depth > 0) {
                    --bracket_depth;
                }
                break;
            case '{':
                ++brace_depth;
                break;
            case '}':
                if (brace_depth > 0) {
                    --brace_depth;
                }
                break;
            case ':':
                if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
                    bool is_ternary_false_branch = false;
                    for (size_t j = i + 1; j < expr.size(); ++j) {
                        char next = expr[j];
                        if (std::isspace(static_cast<unsigned char>(next))) {
                            continue;
                        }
                        is_ternary_false_branch = next != '.';
                        break;
                    }
                    if (!is_ternary_false_branch) {
                        return i;
                    }
                }
                break;
            default:
                break;
        }
    }

    return std::string::npos;
}

std::vector<Token> tokenize(const std::string &src, const std::string &filename, std::vector<LexError> *errors) {
    std::vector<Token> tokens_out;
    auto push_tok = [&](TokenKind k, std::string txt, int line, int col) {
        Token tk;
        tk.kind = k;
        tk.text = std::move(txt);
        tk.line = line;
        tk.col = col;
        tk.filename = filename;
        tokens_out.push_back(std::move(tk));
    };

    size_t pos = 0;
    int line = 1, col = 1;
    auto cur = [&]() -> char { return pos < src.size() ? src[pos] : '\0'; };
    auto peek = [&](size_t n = 1) -> char {
        size_t p = pos + n;
        return p < src.size() ? src[p] : '\0';
    };
    auto advance = [&](size_t n = 1) {
        for (size_t i = 0; i < n; ++i) {
            if (pos >= src.size()) {
                return;
            }
            if (src[pos] == '\n') {
                ++line;
                col = 1;
                ++pos;
            } else {
                ++pos;
                ++col;
            }
        }
    };

    while (cur()) {
        char c = cur();
        // whitespace and comments
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }
        // # starts a line comment (covers shebangs #! at the start of a file too)
        if (c == '#') {
            while (cur() && cur() != '\n') {
                advance();
            }
            continue;
        }
        if (c == '/' && peek() == '/') {
            advance(2);
            while (cur() && cur() != '\n') {
                advance();
            }
            continue;
        }
        if (c == '/' && peek() == '*') {
            advance(2);
            while (cur()) {
                if (cur() == '*' && peek() == '/') {
                    advance(2);
                    break;
                }
                advance();
            }
            continue;
        }

        // multi-char operators
        if (c == '&' && peek() == '&') {
            push_tok(TokenKind::TK_ANDAND, "&&", line, col);
            advance(2);
            continue;
        }
        if (c == '&' && peek() == '=') {
            push_tok(TokenKind::TK_AMPEQ, "&=", line, col);
            advance(2);
            continue;
        }
        if (c == '|' && peek() == '|') {
            push_tok(TokenKind::TK_OROR, "||", line, col);
            advance(2);
            continue;
        }
        if (c == '|' && peek() == '=') {
            push_tok(TokenKind::TK_PIPEEQ, "|=", line, col);
            advance(2);
            continue;
        }
        if (c == '^' && peek() == '=') {
            push_tok(TokenKind::TK_CARETEQ, "^=", line, col);
            advance(2);
            continue;
        }
        if (c == '=' && peek() == '=') {
            push_tok(TokenKind::TK_EQEQ, "==", line, col);
            advance(2);
            continue;
        }
        if (c == '=' && peek() == '>') {
            push_tok(TokenKind::TK_FATARROW, "=>", line, col);
            advance(2);
            continue;
        }
        if (c == '!' && peek() == '=') {
            push_tok(TokenKind::TK_NEQ, "!=", line, col);
            advance(2);
            continue;
        }
        if (c == '<' && peek() == '<' && peek(2) == '=') {
            push_tok(TokenKind::TK_LSHIFTEQ, "<<=", line, col);
            advance(3);
            continue;
        }
        if (c == '<' && peek() == '<') {
            push_tok(TokenKind::TK_LSHIFT, "<<", line, col);
            advance(2);
            continue;
        }
        if (c == '<' && peek() == '=') {
            push_tok(TokenKind::TK_LE, "<=", line, col);
            advance(2);
            continue;
        }
        if (c == '>' && peek() == '>' && peek(2) == '=') {
            push_tok(TokenKind::TK_RSHIFTEQ, ">>=", line, col);
            advance(3);
            continue;
        }
        if (c == '>' && peek() == '>') {
            push_tok(TokenKind::TK_RSHIFT, ">>", line, col);
            advance(2);
            continue;
        }
        if (c == '>' && peek() == '=') {
            push_tok(TokenKind::TK_GE, ">=", line, col);
            advance(2);
            continue;
        }
        if (c == '+' && peek() == '+') {
            push_tok(TokenKind::TK_PLUSPLUS, "++", line, col);
            advance(2);
            continue;
        }
        if (c == '-' && peek() == '-') {
            push_tok(TokenKind::TK_MINUSMINUS, "--", line, col);
            advance(2);
            continue;
        }
        if (c == '-' && peek() == '>') {
            push_tok(TokenKind::TK_ARROW, "->", line, col);
            advance(2);
            continue;
        }
        if (c == '+' && peek() == '=') {
            push_tok(TokenKind::TK_PLUSEQ, "+=", line, col);
            advance(2);
            continue;
        }
        if (c == '-' && peek() == '=') {
            push_tok(TokenKind::TK_MINUSEQ, "-=", line, col);
            advance(2);
            continue;
        }
        if (c == '*' && peek() == '*') {
            push_tok(TokenKind::TK_EXPONENT, "**", line, col);
            advance(2);
            continue;
        }
        if (c == '*' && peek() == '=') {
            push_tok(TokenKind::TK_STAREQ, "*=", line, col);
            advance(2);
            continue;
        }
        if (c == '/' && peek() == '=') {
            push_tok(TokenKind::TK_SLASHEQ, "/=", line, col);
            advance(2);
            continue;
        }
        if (c == '%' && peek() == '=') {
            push_tok(TokenKind::TK_PERCENTEQ, "%=", line, col);
            advance(2);
            continue;
        }
        if (c == '?' && peek() == '?') {
            push_tok(TokenKind::TK_NULLISHCOALESCE, "??", line, col);
            advance(2);
            continue;
        }
        if (c == '?' && peek() == '.') {
            push_tok(TokenKind::TK_QUESTIONDOT, "?.", line, col);
            advance(2);
            continue;
        }
        if (c == '.' && peek() == '.' && peek(2) == '.') {
            push_tok(TokenKind::TK_ELLIPSIS, "...", line, col);
            advance(3);
            continue;
        }

        // single-char tokens
        if (c == '(') {
            push_tok(TokenKind::TK_LPAREN, "(", line, col);
            advance();
            continue;
        }
        if (c == ')') {
            push_tok(TokenKind::TK_RPAREN, ")", line, col);
            advance();
            continue;
        }
        if (c == '{') {
            push_tok(TokenKind::TK_LBRACE, "{", line, col);
            advance();
            continue;
        }
        if (c == '}') {
            push_tok(TokenKind::TK_RBRACE, "}", line, col);
            advance();
            continue;
        }
        if (c == '[') {
            push_tok(TokenKind::TK_LBRACKET, "[", line, col);
            advance();
            continue;
        }
        if (c == ']') {
            push_tok(TokenKind::TK_RBRACKET, "]", line, col);
            advance();
            continue;
        }
        if (c == ',') {
            push_tok(TokenKind::TK_COMMA, ",", line, col);
            advance();
            continue;
        }
        if (c == ';') {
            push_tok(TokenKind::TK_SEMICOLON, ";", line, col);
            advance();
            continue;
        }
        if (c == ':') {
            if (pos + 1 < src.size() && src[pos + 1] == ':') {
                push_tok(TokenKind::TK_COLONCOLON, "::", line, col);
                advance();
                advance();
            } else {
                push_tok(TokenKind::TK_COLON, ":", line, col);
                advance();
            }
            continue;
        }
        if (c == '.') {
            push_tok(TokenKind::TK_DOT, ".", line, col);
            advance();
            continue;
        }
        if (c == '?') {
            push_tok(TokenKind::TK_QUESTION, "?", line, col);
            advance();
            continue;
        }
        if (c == '@') {
            push_tok(TokenKind::TK_AT, "@", line, col);
            advance();
            continue;
        }
        if (c == '=') {
            push_tok(TokenKind::TK_EQUAL, "=", line, col);
            advance();
            continue;
        }
        if (c == '+') {
            push_tok(TokenKind::TK_PLUS, "+", line, col);
            advance();
            continue;
        }
        if (c == '-') {
            push_tok(TokenKind::TK_MINUS, "-", line, col);
            advance();
            continue;
        }
        if (c == '*') {
            push_tok(TokenKind::TK_STAR, "*", line, col);
            advance();
            continue;
        }
        if (c == '/') {
            // determine if this / starts a regex literal or is a division operator
            static const std::unordered_set<std::string> kPrefixKeywords = {
                "return",
                "typeof",
                "new",
                "delete",
                "await",
                "yield",
                "void",
                "not",
                "case",
                "in",
            };
            bool prev_can_end_expr =
                !tokens_out.empty() &&
                (tokens_out.back().kind == TokenKind::TK_NUMBER ||
                 tokens_out.back().kind == TokenKind::TK_STRING ||
                 tokens_out.back().kind == TokenKind::TK_RPAREN ||
                 tokens_out.back().kind == TokenKind::TK_RBRACKET ||
                 tokens_out.back().kind == TokenKind::TK_RBRACE ||
                 tokens_out.back().kind == TokenKind::TK_PLUSPLUS ||
                 tokens_out.back().kind == TokenKind::TK_MINUSMINUS ||
                 // TK_REGEX itself can end an expression (/pattern/.test(x) / 2)
                 tokens_out.back().kind == TokenKind::TK_REGEX ||
                 (tokens_out.back().kind == TokenKind::TK_IDENT && !kPrefixKeywords.count(tokens_out.back().text)));
            if (prev_can_end_expr) {
                push_tok(TokenKind::TK_SLASH, "/", line, col);
                advance();
                continue;
            }
            // Regex literal: scan until unescaped closing /
            int regex_start_col = col;
            int regex_start_line = line;
            advance();
            std::string regex_pattern;
            bool in_char_class = false;
            bool regex_ok = false;
            while (cur()) {
                char rc = cur();
                if (rc == '\\' && peek() != '\0') {
                    regex_pattern += rc;
                    advance();
                    regex_pattern += cur();
                    advance();
                    continue;
                }
                if (rc == '[') {
                    in_char_class = true;
                    regex_pattern += rc;
                    advance();
                    continue;
                }
                if (rc == ']') {
                    in_char_class = false;
                    regex_pattern += rc;
                    advance();
                    continue;
                }
                if (!in_char_class && rc == '/') {
                    advance();
                    regex_ok = true;
                    break;
                }
                if (rc == '\n') {
                    break; // unterminated
                }
                regex_pattern += rc;
                advance();
            }
            if (!regex_ok) {
                fprintf(stderr, "Unterminated regex literal at line %d\n", regex_start_line);
                push_tok(TokenKind::TK_UNKNOWN, "/", regex_start_line, regex_start_col);
                continue;
            }
            // Read optional flags (g i m s u v y)
            std::string regex_flags;
            while (cur() == 'g' || cur() == 'i' || cur() == 'm' || cur() == 's' || cur() == 'u' || cur() == 'v' || cur() == 'y') {
                regex_flags += cur();
                advance();
            }
            // pattern -> tok.text, flags -> tok.interp_parts[0]
            Token re_tok;
            re_tok.kind = TokenKind::TK_REGEX;
            re_tok.text = regex_pattern;
            re_tok.interp_parts = { regex_flags };
            re_tok.line = regex_start_line;
            re_tok.col = regex_start_col;
            re_tok.filename = filename;
            tokens_out.push_back(std::move(re_tok));
            continue;
        }
        if (c == '!') {
            push_tok(TokenKind::TK_BANG, "!", line, col);
            advance();
            continue;
        }
        if (c == '<') {
            push_tok(TokenKind::TK_LT, "<", line, col);
            advance();
            continue;
        }
        if (c == '>') {
            push_tok(TokenKind::TK_GT, ">", line, col);
            advance();
            continue;
        }
        if (c == '%') {
            push_tok(TokenKind::TK_PERCENT, "%", line, col);
            advance();
            continue;
        }
        if (c == '&') {
            push_tok(TokenKind::TK_AMPERSAND, "&", line, col);
            advance();
            continue;
        }
        if (c == '|') {
            push_tok(TokenKind::TK_PIPE, "|", line, col);
            advance();
            continue;
        }
        if (c == '^') {
            push_tok(TokenKind::TK_CARET, "^", line, col);
            advance();
            continue;
        }
        if (c == '~') {
            push_tok(TokenKind::TK_TILDE, "~", line, col);
            advance();
            continue;
        }

        // string literal (supports escapes: \n, \t, \", \\, others map to the escaped char)
        if (c == '\"') {
            int start_line = line;
            int start_col = col;
            advance();
            std::string out;
            while (cur() && cur() != '\"') {
                if (cur() == '\\') {
                    advance();
                    char e = cur();
                    if (!e) {
                        break;
                    }
                    if (e == 'n') {
                        out.push_back('\n');
                    } else if (e == 'r') {
                        out.push_back('\r');
                    } else if (e == 't') {
                        out.push_back('\t');
                    } else if (e == '\\') {
                        out.push_back('\\');
                    } else if (e == '\"') {
                        out.push_back('\"');
                    } else {
                        out.push_back(e);
                    }
                    advance();
                } else {
                    out.push_back(cur());
                    advance();
                }
            }
            if (cur() == '\"') {
                advance();
            } else {
                Token tok;
                tok.filename = filename;
                tok.line = start_line;
                tok.col = start_col;
                if (errors) {
                    errors->push_back({ filename, start_line, start_col, "Unterminated string literal" });
                    push_tok(TokenKind::TK_EOF, "", line, col);
                    return tokens_out;
                }
                fatal_error(std::string("Unterminated string literal"), &tok);
            }
            push_tok(TokenKind::TK_STRING, out, start_line, start_col);
            continue;
        }

        // interpolated string literal with backticks: `Hello {name}!`
        if (c == '`') {
            int start_line = line;
            int start_col = col;
            advance();

            Token tok;
            tok.kind = TokenKind::TK_INTERP_STRING;
            tok.line = start_line;
            tok.col = start_col;
            tok.filename = filename;

            std::string current_part;

            while (cur() && cur() != '`') {
                if (cur() == '\\') {
                    advance();
                    char e = cur();
                    if (!e) {
                        break;
                    }
                    if (e == 'n') {
                        current_part.push_back('\n');
                    } else if (e == 'r') {
                        current_part.push_back('\r');
                    } else if (e == 't') {
                        current_part.push_back('\t');
                    } else if (e == '\\') {
                        current_part.push_back('\\');
                    } else if (e == '`') {
                        current_part.push_back('`');
                    } else if (e == '{') {
                        current_part.push_back('{');
                    } else {
                        current_part.push_back(e);
                    }
                    advance();
                } else if (cur() == '{') {
                    tok.interp_parts.push_back(current_part);
                    current_part.clear();
                    advance();

                    // extract until '}'
                    std::string expr;
                    int brace_depth = 1;
                    while (cur() && brace_depth > 0) {
                        if (cur() == '{') {
                            brace_depth++;
                        } else if (cur() == '}') {
                            brace_depth--;
                        }

                        if (brace_depth > 0) {
                            expr.push_back(cur());
                        }
                        advance();
                    }

                    std::string expr_source = expr;
                    std::string format_spec;
                    size_t format_colon = find_top_level_format_colon(expr);
                    if (format_colon != std::string::npos) {
                        expr_source = trim_ascii(std::string_view(expr).substr(0, format_colon));
                        format_spec = trim_ascii(std::string_view(expr).substr(format_colon + 1));
                    }

                    tok.interp_exprs.push_back(expr_source);
                    tok.interp_formats.push_back(format_spec);
                } else {
                    current_part.push_back(cur());
                    advance();
                }
            }

            // add the final part (after last expression or whole string if no expressions)
            tok.interp_parts.push_back(current_part);

            if (cur() == '`') {
                advance();
            } else {
                if (errors) {
                    errors->push_back({ filename, start_line, start_col,
                                        "Unterminated interpolated string literal" });
                    push_tok(TokenKind::TK_EOF, "", line, col);
                    return tokens_out;
                }
                fatal_error(std::string("Unterminated interpolated string literal"), &tok);
            }

            tokens_out.push_back(tok);
            continue;
        }

        // numbers: integer, hex integer, or floating
        if ((c >= '0' && c <= '9') || (c == '.' && (peek() >= '0' && peek() <= '9'))) {
            int start_line = line;
            int start_col = col;
            size_t st = pos;

            if (c == '0' && (peek() == 'x' || peek() == 'X')) {
                advance(); // 0
                advance(); // x/X

                size_t hex_start = pos;
                while (cur() && std::isxdigit(static_cast<unsigned char>(cur()))) {
                    advance();
                }

                if (pos == hex_start) {
                    Token tok;
                    tok.filename = filename;
                    tok.line = start_line;
                    tok.col = start_col;
                    if (errors) {
                        errors->push_back({ filename,
                                            start_line,
                                            start_col,
                                            "Invalid hex literal: expected at least one hex digit after 0x" });
                        push_tok(TokenKind::TK_EOF, "", line, col);
                        return tokens_out;
                    }
                    fatal_error(std::string("Invalid hex literal: expected at least one hex digit after 0x"), &tok);
                }

                std::string num = src.substr(st, pos - st);
                push_tok(TokenKind::TK_NUMBER, num, start_line, start_col);
                continue;
            }

            while (cur() && (cur() >= '0' && cur() <= '9')) {
                advance();
            }
            if (cur() == '.') {
                advance();
                while (cur() && (cur() >= '0' && cur() <= '9')) {
                    advance();
                }
            }
            std::string num = src.substr(st, pos - st);
            push_tok(TokenKind::TK_NUMBER, num, start_line, start_col);
            continue;
        }

        // letters, digits, underscore
        if (isalpha(c) || c == '_') {
            int start_line = line;
            int start_col = col;
            size_t st = pos;
            advance();
            while (cur() && (isalnum(cur()) || cur() == '_')) {
                advance();
            }
            std::string id = src.substr(st, pos - st);
            push_tok(TokenKind::TK_IDENT, id, start_line, start_col);
            continue;
        }

        // unknown single char
        std::string tmp(1, c);
        push_tok(TokenKind::TK_UNKNOWN, tmp, line, col);
        advance();
    }

    // EOF
    push_tok(TokenKind::TK_EOF, "", line, col);

    return tokens_out;
}

} // namespace Parser
