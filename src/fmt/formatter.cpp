/*
  fmt/formatter.cpp
  Token-stream based source formatter for Nari
*/
#include "formatter.h"

#include "parser/lexer.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace nari::fmt {

using Parser::Token;
using Parser::TokenKind;

namespace {

// words that are never call/index targets: affects `(`, `[`, and unary-op spacing
bool is_keyword(const std::string &s) {
    static const std::unordered_set<std::string> kws = {
        "if",    "else",    "while",  "for",  "foreach", "in",     "return", "switch", "case",     "default", "match",
        "let",   "global",  "const",  "func", "import",  "export", "from",   "as",     "type",     "union",   "enum",
        "class", "extends", "static", "new",  "typeof",  "spawn",  "await",  "break",  "continue", "yield",
    };
    return kws.count(s) > 0;
}

// keywords followed by a space before `(`: `if (x)` but `foo(x)`, `func(x)`
bool is_paren_space_keyword(const std::string &s) {
    static const std::unordered_set<std::string> kws = {
        "if", "while", "for", "foreach", "switch", "match", "return", "new", "typeof", "await",
    };
    return kws.count(s) > 0;
}

bool can_end_expr(const Token &t) {
    switch (t.kind) {
        case TokenKind::TK_NUMBER:
        case TokenKind::TK_STRING:
        case TokenKind::TK_INTERP_STRING:
        case TokenKind::TK_REGEX:
        case TokenKind::TK_RPAREN:
        case TokenKind::TK_RBRACKET:
        case TokenKind::TK_PLUSPLUS:
        case TokenKind::TK_MINUSMINUS:
            return true;
        case TokenKind::TK_IDENT:
            return !is_keyword(t.text);
        default:
            return false;
    }
}

bool is_binary_op(TokenKind k) {
    switch (k) {
        case TokenKind::TK_PLUS:
        case TokenKind::TK_MINUS:
        case TokenKind::TK_STAR:
        case TokenKind::TK_SLASH:
        case TokenKind::TK_PERCENT:
        case TokenKind::TK_EXPONENT:
        case TokenKind::TK_EQEQ:
        case TokenKind::TK_NEQ:
        case TokenKind::TK_LE:
        case TokenKind::TK_GE:
        case TokenKind::TK_ANDAND:
        case TokenKind::TK_OROR:
        case TokenKind::TK_AMPERSAND:
        case TokenKind::TK_PIPE:
        case TokenKind::TK_CARET:
        case TokenKind::TK_LSHIFT:
        case TokenKind::TK_RSHIFT:
        case TokenKind::TK_AT:
        case TokenKind::TK_NULLISHCOALESCE:
        case TokenKind::TK_EQUAL:
        case TokenKind::TK_PLUSEQ:
        case TokenKind::TK_MINUSEQ:
        case TokenKind::TK_STAREQ:
        case TokenKind::TK_SLASHEQ:
        case TokenKind::TK_PERCENTEQ:
        case TokenKind::TK_AMPEQ:
        case TokenKind::TK_PIPEEQ:
        case TokenKind::TK_CARETEQ:
        case TokenKind::TK_LSHIFTEQ:
        case TokenKind::TK_RSHIFTEQ:
        case TokenKind::TK_ARROW:
        case TokenKind::TK_FATARROW:
            return true;
        default:
            return false;
    }
}

bool is_prefix_op(TokenKind k) {
    return k == TokenKind::TK_PLUS || k == TokenKind::TK_MINUS || k == TokenKind::TK_TILDE || k == TokenKind::TK_BANG;
}

// escape decoded string contents for re-emission inside `delim` quotes.
// for backtick strings, `{` must also be escaped (the lexer decodes `\{`).
std::string escape_string(const std::string &s, char delim) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '"':
                out += (delim == '"') ? "\\\"" : "\"";
                break;
            case '`':
                out += (delim == '`') ? "\\`" : "`";
                break;
            case '{':
                out += (delim == '`') ? "\\{" : "{";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

std::string render_token(const Token &t) {
    if (t.kind == TokenKind::TK_STRING) {
        return "\"" + escape_string(t.text, '"') + "\"";
    }
    if (t.kind == TokenKind::TK_INTERP_STRING) {
        std::string out = "`";
        for (size_t i = 0; i < t.interp_exprs.size(); ++i) {
            if (i < t.interp_parts.size()) {
                out += escape_string(t.interp_parts[i], '`');
            }
            out += "{";
            out += t.interp_exprs[i];
            if (i < t.interp_formats.size() && !t.interp_formats[i].empty()) {
                out += ":";
                out += t.interp_formats[i];
            }
            out += "}";
        }
        if (t.interp_parts.size() > t.interp_exprs.size()) {
            out += escape_string(t.interp_parts.back(), '`');
        }
        out += "`";
        return out;
    }
    if (t.kind == TokenKind::TK_REGEX) {
        std::string out = "/";
        out += t.text;
        out += "/";
        if (!t.interp_parts.empty()) {
            out += t.interp_parts[0];
        }
        return out;
    }
    // identifiers, numbers, operators, comments: verbatim
    return t.text;
}

struct Formatter {
    const FmtOptions &opts;
    std::string out;

    int brace_depth = 0;
    int paren_depth = 0; // ( and [
    int ternary_q = 0;   // unmatched ternary '?'
    int angle_depth = 0; // heuristic generic-argument <> nesting
    int case_boost = 0;  // extra indent level after a `case:`/`default:` label
    std::vector<int> case_boost_stack;

    bool at_line_start = true;
    bool line_starts_with_case = false; // current line's first token is `case`/`default`

    // classification of the previously emitted token
    bool prev_was_unary = false;         // prefix op: no space after (`-x`, `!x`)
    bool prev_was_generic_close = false; // `>`/`>>` that closed generic args

    explicit Formatter(const FmtOptions &o) : opts(o) {
    }

    void emit_indent() {
        int levels = brace_depth + paren_depth + case_boost;
        out.append(static_cast<size_t>(levels) * static_cast<size_t>(opts.indent_width), ' ');
    }

    // decide whether a space goes between prev and cur on the same line
    bool need_space(const Token &prev, const Token &cur, bool cur_opens_generic) const {
        using K = TokenKind;
        if (prev.kind == K::TK_COMMENT || cur.kind == K::TK_COMMENT) {
            return true;
        }
        // member access
        if (cur.kind == K::TK_DOT || cur.kind == K::TK_COLONCOLON || cur.kind == K::TK_QUESTIONDOT ||
            prev.kind == K::TK_DOT || prev.kind == K::TK_COLONCOLON || prev.kind == K::TK_QUESTIONDOT) {
            return false;
        }
        // closers and separators hug the left side
        if (cur.kind == K::TK_RPAREN || cur.kind == K::TK_RBRACKET || cur.kind == K::TK_COMMA ||
            cur.kind == K::TK_SEMICOLON) {
            return false;
        }
        // nothing right after ( or [
        if (prev.kind == K::TK_LPAREN || prev.kind == K::TK_LBRACKET) {
            return false;
        }
        // spread hugs its operand
        if (prev.kind == K::TK_ELLIPSIS) {
            return false;
        }
        // call paren: `foo(` tight, `if (` spaced
        if (cur.kind == K::TK_LPAREN) {
            if (prev.kind == K::TK_IDENT) {
                return is_paren_space_keyword(prev.text);
            }
            if (prev.kind == K::TK_RPAREN || prev.kind == K::TK_RBRACKET || prev_was_generic_close) {
                return false;
            }
            return true;
        }
        // index bracket: `a[0]` tight, `return [` spaced
        if (cur.kind == K::TK_LBRACKET) {
            if (prev.kind == K::TK_IDENT) {
                return is_keyword(prev.text);
            }
            if (prev.kind == K::TK_RPAREN || prev.kind == K::TK_RBRACKET || prev_was_generic_close) {
                return false;
            }
            return true;
        }
        // braces
        if (cur.kind == K::TK_LBRACE) {
            return prev.kind != K::TK_LBRACE;
        }
        if (cur.kind == K::TK_RBRACE) {
            return prev.kind != K::TK_LBRACE;
        }
        if (prev.kind == K::TK_RBRACE) {
            return true; // `} else`, `} foo` (closers handled above)
        }
        if (prev.kind == K::TK_LBRACE) {
            return true; // `{ x }` (empty `{}` handled above)
        }
        if (prev.kind == K::TK_COMMA || prev.kind == K::TK_SEMICOLON) {
            return true;
        }
        // generic angle brackets (heuristic): tight while inside `Name<...>`
        if (cur.kind == K::TK_LT && cur_opens_generic) {
            return false;
        }
        if (prev.kind == K::TK_LT && angle_depth > 0) {
            return false; // just after the opening `<` of `Name<...>`
        }
        if ((cur.kind == K::TK_GT || cur.kind == K::TK_RSHIFT) && angle_depth > 0) {
            return false;
        }
        if (prev_was_generic_close) {
            return true; // `Foo<T> x` (calls/indexing handled above)
        }
        // postfix ++/-- hug the operand
        if ((cur.kind == K::TK_PLUSPLUS || cur.kind == K::TK_MINUSMINUS) && can_end_expr(prev)) {
            return false;
        }
        // prefix ops: no space after (`-x`), space before unless after an opener
        if (is_prefix_op(cur.kind) && !can_end_expr(prev)) {
            return true;
        }
        if (prev_was_unary) {
            return false;
        }
        if ((cur.kind == K::TK_PLUSPLUS || cur.kind == K::TK_MINUSMINUS) && !can_end_expr(prev)) {
            return true; // prefix ++/--
        }
        // colons: ternary gets space on both sides, annotation/object only after
        if (cur.kind == K::TK_COLON) {
            return ternary_q > 0;
        }
        if (prev.kind == K::TK_COLON) {
            return true;
        }
        if (cur.kind == K::TK_QUESTION || prev.kind == K::TK_QUESTION) {
            return true;
        }
        // binary operators
        if (is_binary_op(cur.kind) || cur.kind == K::TK_LT || cur.kind == K::TK_GT || cur.kind == K::TK_RSHIFT ||
            is_binary_op(prev.kind) || prev.kind == K::TK_LT || prev.kind == K::TK_GT || prev.kind == K::TK_RSHIFT) {
            return true;
        }
        // words and literals next to each other
        return true;
    }

    // `<` opens generic arguments when written tight against an identifier and
    // followed by a type name: `Box<T>`. `a<b` (less-than, also tight) formats back to itself.
    bool opens_generic(const std::vector<const Token *> &items, size_t i) const {
        if (i == 0 || i + 1 >= items.size()) {
            return false;
        }
        const Token &prev = *items[i - 1];
        const Token &cur = *items[i];
        const Token &next = *items[i + 1];
        if (prev.kind != TokenKind::TK_IDENT || is_keyword(prev.text)) {
            return false;
        }
        if (next.kind != TokenKind::TK_IDENT) {
            return false;
        }
        // originally written without a space: prev ends where `<` starts
        return prev.line == cur.line && prev.col + static_cast<int>(prev.text.size()) == cur.col;
    }

    std::string run(const std::vector<const Token *> &items) {
        out.clear();
        if (items.empty()) {
            return out;
        }

        int prev_end_line = items.front()->line; // first item emits no leading newline
        const Token *prev = nullptr;

        for (size_t i = 0; i < items.size(); ++i) {
            const Token &cur = *items[i];

            // closing brackets dedent their own line
            if (cur.kind == TokenKind::TK_RBRACE) {
                brace_depth = std::max(0, brace_depth - 1);
                if (!case_boost_stack.empty()) {
                    case_boost = case_boost_stack.back();
                    case_boost_stack.pop_back();
                }
            }
            if (cur.kind == TokenKind::TK_RPAREN || cur.kind == TokenKind::TK_RBRACKET) {
                paren_depth = std::max(0, paren_depth - 1);
            }

            bool cur_opens_generic = cur.kind == TokenKind::TK_LT && opens_generic(items, i);

            int gap = cur.line - prev_end_line;
            if (gap > 0) {
                // a new `case`/`default` label ends the previous case body's
                // extra indent (closing braces restore it via case_boost_stack)
                if (cur.kind == TokenKind::TK_IDENT && (cur.text == "case" || cur.text == "default")) {
                    case_boost = 0;
                }
                line_starts_with_case =
                    cur.kind == TokenKind::TK_IDENT && (cur.text == "case" || cur.text == "default");
                int blanks = std::min(gap - 1, opts.max_blank_lines);
                out.append(static_cast<size_t>(1 + blanks), '\n');
                emit_indent();
                at_line_start = true;
            } else if (!at_line_start && prev != nullptr) {
                if (need_space(*prev, cur, cur_opens_generic)) {
                    out += ' ';
                }
            }

            std::string text = render_token(cur);
            out += text;
            at_line_start = false;

            size_t newlines_in_text = static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
            prev_end_line = cur.line + static_cast<int>(newlines_in_text);

            // update state
            prev_was_unary = false;
            prev_was_generic_close = false;
            switch (cur.kind) {
                case TokenKind::TK_LBRACE:
                    case_boost_stack.push_back(case_boost);
                    case_boost = 0;
                    ++brace_depth;
                    break;
                case TokenKind::TK_LPAREN:
                case TokenKind::TK_LBRACKET:
                    ++paren_depth;
                    break;
                case TokenKind::TK_QUESTION:
                    ++ternary_q;
                    break;
                case TokenKind::TK_COLON:
                    if (ternary_q > 0) {
                        --ternary_q;
                    }
                    // `case`/`default` labels boost the following lines by one
                    // indent level, but only when the label ends the line.
                    if (line_starts_with_case && i + 1 < items.size() && items[i + 1]->line > cur.line) {
                        case_boost = 1;
                    }
                    break;
                case TokenKind::TK_LT:
                    if (cur_opens_generic) {
                        ++angle_depth;
                    }
                    break;
                case TokenKind::TK_GT:
                    if (angle_depth > 0) {
                        --angle_depth;
                        prev_was_generic_close = true;
                    }
                    break;
                case TokenKind::TK_RSHIFT:
                    if (angle_depth > 0) {
                        angle_depth = std::max(0, angle_depth - 2);
                        prev_was_generic_close = true;
                    }
                    break;
                case TokenKind::TK_PLUS:
                case TokenKind::TK_MINUS:
                case TokenKind::TK_TILDE:
                case TokenKind::TK_BANG:
                case TokenKind::TK_PLUSPLUS:
                case TokenKind::TK_MINUSMINUS:
                    prev_was_unary = prev == nullptr || !can_end_expr(*prev);
                    break;
                default:
                    break;
            }
            prev = &cur;
        }

        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        return out;
    }
};

} // namespace

bool format_source(const std::string &src, const std::string &filename, const FmtOptions &opts, std::string &out,
                   std::string &error_out) {
    std::vector<Parser::LexError> errors;
    std::vector<Token> comments;
    std::vector<Token> tokens = Parser::tokenize(src, filename, &errors, &comments);
    if (!errors.empty()) {
        error_out = errors.front().filename + ":" + std::to_string(errors.front().line) + ":" +
                    std::to_string(errors.front().col) + ": " + errors.front().message;
        return false;
    }

    // merge tokens and comments into one position-ordered stream
    std::vector<const Token *> items;
    items.reserve(tokens.size() + comments.size());
    for (const auto &t : tokens) {
        if (t.kind != TokenKind::TK_EOF) {
            items.push_back(&t);
        }
    }
    for (const auto &c : comments) {
        items.push_back(&c);
    }
    std::stable_sort(items.begin(), items.end(), [](const Token *a, const Token *b) {
        if (a->line != b->line) {
            return a->line < b->line;
        }
        return a->col < b->col;
    });

    Formatter f(opts);
    out = f.run(items);
    return true;
}

} // namespace nari::fmt
