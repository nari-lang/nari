/*
  parse_expr.cpp
  Parser class methods split verbatim from parser.cpp.
*/

#include "parser/parser.h"

namespace Parser {

bool Parser::looks_like_arrow_function() {
    if (peek().kind != TokenKind::TK_LPAREN) {
        return false;
    }

    // lookahead to find matching ) and check for =>
    size_t i = idx + 1;
    int depth = 1;
    while (i < toks.size() && depth > 0) {
        if (toks[i].kind == TokenKind::TK_LPAREN) {
            ++depth;
        } else if (toks[i].kind == TokenKind::TK_RPAREN) {
            --depth;
        } else if (toks[i].kind == TokenKind::TK_EOF) {
            return false;
        }
        ++i;
    }

    // check if next token after matching ) is =>
    if (i < toks.size() && toks[i].kind == TokenKind::TK_FATARROW) {
        return true;
    }

    return false;
}

// parse function expression: func(params) { ... }
ExprPtr Parser::parse_function_expression() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &funcTok = peek();
    if (!expect(TokenKind::TK_IDENT, "func keyword")) {
        return {};
    }

    auto func_expr = std::make_unique<nari::FunctionExpr>();
    func_expr->line = funcTok.line;
    func_expr->col = funcTok.col;
    func_expr->filename = funcTok.filename.empty() ? current_filename : funcTok.filename;

    // parse parameters
    if (!expect(TokenKind::TK_LPAREN, "function params start '('")) {
        return {};
    }
    bool seen_rest = false;
    if (peek().kind != TokenKind::TK_RPAREN) {
        while (true) {
            bool is_rest = false;
            if (peek().kind == TokenKind::TK_ELLIPSIS) {
                if (seen_rest) {
                    error_and_exit("Only one rest parameter is allowed");
                    return {};
                }
                seen_rest = true;
                is_rest = true;
                next(); // consume '...'
            }

            const Token &p = peek();
            if (p.kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected parameter name");
                return {};
            }
            std::string pname = next().text;

            // parse optional type annotation: name: type or name: type[]
            nari::TypeAnnotationPtr param_type = nullptr;
            if (peek().kind == TokenKind::TK_COLON) {
                next(); // consume ':'
                if (peek().kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected type name after ':'");
                    return {};
                }
                std::string type_name = next().text;
                bool is_array = false;
                // Check for array syntax: type[]
                if (peek().kind == TokenKind::TK_LBRACKET && peek(1).kind == TokenKind::TK_RBRACKET) {
                    next(); // consume '['
                    next(); // consume ']'
                    is_array = true;
                }
                param_type = std::make_unique<nari::TypeAnnotation>(type_name, is_array);
            }

            ExprPtr default_value = nullptr;
            if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
                next(); // consume '='
                default_value = parse_expression();
                if (recover_failed_) {
                    return {};
                }
            } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
                error_and_exit("Rest parameter cannot have a default value");
                return {};
            }

            func_expr->params.emplace_back(pname, std::move(default_value), is_rest, std::move(param_type));

            if (is_rest) {
                if (peek().kind == TokenKind::TK_COMMA) {
                    error_and_exit("Rest parameter must be last");
                    return {};
                }
                break;
            }

            if (peek().kind == TokenKind::TK_COMMA) {
                next();
            } else {
                break;
            }
        }
    }
    if (!expect(TokenKind::TK_RPAREN, "function params end ')'")) {
        return {};
    }

    // parse optional return type: -> type or -> type[]
    if (peek().kind == TokenKind::TK_ARROW) {
        next(); // consume '->'
        if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected return type after '->'");
            return {};
        }
        std::string return_type_name = next().text;
        bool is_array = false;
        // Check for array syntax: type[]
        if (peek().kind == TokenKind::TK_LBRACKET && peek(1).kind == TokenKind::TK_RBRACKET) {
            next(); // consume '['
            next(); // consume ']'
            is_array = true;
        }
        func_expr->return_type = std::make_unique<nari::TypeAnnotation>(return_type_name, is_array);
    }

    func_expr->body = parse_block();
    if (recover_failed_) {
        return {};
    }
    return std::move(func_expr);
}

// parse spawn expression: spawn { ... }
ExprPtr Parser::parse_spawn_expression() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &spawnTok = peek();
    if (!expect(TokenKind::TK_IDENT, "spawn keyword")) {
        return {};
    }

    auto spawn_expr = std::make_unique<nari::SpawnExpr>(nullptr);
    spawn_expr->line = spawnTok.line;
    spawn_expr->col = spawnTok.col;
    spawn_expr->filename = spawnTok.filename.empty() ? current_filename : spawnTok.filename;

    // parse the block
    spawn_expr->body = parse_block();
    if (recover_failed_) {
        return {};
    }
    return spawn_expr;
}

// parse arrow function expression: (params) => { ... } or (params) => expr
ExprPtr Parser::parse_arrow_function_expression() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &parenTok = peek();
    auto func_expr = std::make_unique<nari::FunctionExpr>();
    func_expr->line = parenTok.line;
    func_expr->col = parenTok.col;
    func_expr->filename = parenTok.filename.empty() ? current_filename : parenTok.filename;

    // Parse parameters
    if (!expect(TokenKind::TK_LPAREN, "arrow function params start '('")) {
        return {};
    }
    bool seen_rest = false;
    if (peek().kind != TokenKind::TK_RPAREN) {
        while (true) {
            bool is_rest = false;
            if (peek().kind == TokenKind::TK_ELLIPSIS) {
                if (seen_rest) {
                    error_and_exit("Only one rest parameter is allowed");
                    return {};
                }
                seen_rest = true;
                is_rest = true;
                next(); // consume '...'
            }

            const Token &p = peek();
            if (p.kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected parameter name");
                return {};
            }
            std::string pname = next().text;

            // Parse optional type annotation: name: type or name: type[]
            nari::TypeAnnotationPtr param_type = nullptr;
            if (peek().kind == TokenKind::TK_COLON) {
                next(); // consume ':'
                if (peek().kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected type name after ':'");
                    return {};
                }
                std::string type_name = next().text;
                bool is_array = false;
                // Check for array syntax: type[]
                if (peek().kind == TokenKind::TK_LBRACKET && peek(1).kind == TokenKind::TK_RBRACKET) {
                    next(); // consume '['
                    next(); // consume ']'
                    is_array = true;
                }
                param_type = std::make_unique<nari::TypeAnnotation>(type_name, is_array);
            }

            ExprPtr default_value = nullptr;
            if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
                next(); // consume '='
                default_value = parse_expression();
                if (recover_failed_) {
                    return {};
                }
            } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
                error_and_exit("Rest parameter cannot have a default value");
                return {};
            }

            func_expr->params.emplace_back(pname, std::move(default_value), is_rest, std::move(param_type));

            if (is_rest) {
                if (peek().kind == TokenKind::TK_COMMA) {
                    error_and_exit("Rest parameter must be last");
                    return {};
                }
                break;
            }

            if (peek().kind == TokenKind::TK_COMMA) {
                next();
            } else {
                break;
            }
        }
    }
    if (!expect(TokenKind::TK_RPAREN, "arrow function params end ')'")) {
        return {};
    }
    if (!expect(TokenKind::TK_FATARROW, "'=>' in arrow function")) {
        return {};
    }

    // parse body: either a block or a single expression
    if (peek().kind == TokenKind::TK_LBRACE) {
        // block body: (x) => { return x + 1; }
        func_expr->body = parse_block();
        if (recover_failed_) {
            return {};
        }
    } else {
        // Expression body: (x) => x + 1
        // convert to implicit return
        ExprPtr body_expr = parse_expression();
        if (recover_failed_) {
            return {};
        }
        auto ret_stmt = std::make_unique<nari::ReturnStmt>(std::move(body_expr));
        ret_stmt->line = func_expr->line;
        ret_stmt->col = func_expr->col;
        ret_stmt->filename = func_expr->filename;

        auto block = std::make_unique<nari::BlockStmt>();
        block->stmts.push_back(std::move(ret_stmt));

        func_expr->body = std::move(block);
    }

    return std::move(func_expr);
}

// parse assignment, expression-statement, and block statements.
// semicolons are optional, but two statements on the same line *must* be separated by one
void Parser::check_statement_boundary() {
    if (recover_failed_) {
        return;
    }
    if (idx == 0 || idx > toks.size()) {
        return;
    }
    const Token &last = toks[idx - 1];
    if (last.kind == TokenKind::TK_SEMICOLON || last.kind == TokenKind::TK_RBRACE) {
        return;
    }
    const Token &nt = peek();
    if (nt.kind == TokenKind::TK_EOF || nt.kind == TokenKind::TK_RBRACE || nt.kind == TokenKind::TK_SEMICOLON) {
        return;
    }
    if (nt.kind == TokenKind::TK_IDENT && (nt.text == "else" || nt.text == "case" || nt.text == "default")) {
        return;
    }
    if (nt.line == last.line) {
        error_and_exit("Expected ';' or newline between statements, found '" + token_desc(nt) + "'");
        return;
    }
}

Parser::Precedence Parser::token_precedence(const Token &t) {
    switch (t.kind) {
        case TokenKind::TK_QUESTION:
            return PREC_TERNARY;
        case TokenKind::TK_NULLISHCOALESCE:
            return PREC_NULL_COALESCE;
        case TokenKind::TK_OROR:
            return PREC_LOGICAL_OR;
        case TokenKind::TK_ANDAND:
            return PREC_LOGICAL_AND;
        case TokenKind::TK_PIPE:
            return PREC_BITWISE_OR;
        case TokenKind::TK_CARET:
            return PREC_BITWISE_XOR;
        case TokenKind::TK_AMPERSAND:
            return PREC_BITWISE_AND;
        case TokenKind::TK_EQEQ:
        case TokenKind::TK_NEQ:
        case TokenKind::TK_STRICT_EQ:
        case TokenKind::TK_STRICT_NEQ:
            return PREC_EQUALITY;
        case TokenKind::TK_LT:
        case TokenKind::TK_GT:
        case TokenKind::TK_LE:
        case TokenKind::TK_GE:
            return PREC_COMPARISON;
        case TokenKind::TK_LSHIFT:
        case TokenKind::TK_RSHIFT:
            return PREC_SHIFT;
        case TokenKind::TK_PLUS:
        case TokenKind::TK_MINUS:
            return PREC_TERM;
        case TokenKind::TK_STAR:
        case TokenKind::TK_SLASH:
        case TokenKind::TK_PERCENT:
            return PREC_FACTOR;
        case TokenKind::TK_EXPONENT:
            return PREC_EXPONENT;
        default:
            return PREC_NONE;
    }
}

ExprPtr Parser::parse_expression() {
    if (recover_failed_) {
        return nullptr;
    }
    return fold_expr(parse_precedence(PREC_TERNARY));
}

ExprPtr Parser::parse_precedence(Precedence min_prec) {
    if (recover_failed_) {
        return nullptr;
    }
    if (expr_recursion_depth >= MAX_EXPR_RECURSION_DEPTH) {
        error_and_exit("Expression nesting too deep");
        return {};
    }
    ExprRecursionGuard guard(expr_recursion_depth);
    ExprPtr lhs = parse_unary();
    if (!lhs) {
        return nullptr;
    }

    while (true) {
        const Token &t = peek();
        Precedence prec = token_precedence(t);
        if (prec < min_prec || prec == PREC_NONE) {
            break;
        }

        // Special handling for ternary operator
        if (t.kind == TokenKind::TK_QUESTION) {
            Token qmark = t;
            next(); // consume '?'
            ExprPtr true_expr = parse_expression();
            if (!true_expr) {
                error_and_exit("Expected expression after '?'");
                return {};
            }
            if (!expect(TokenKind::TK_COLON, "':' in ternary operator")) {
                return {};
            }
            ExprPtr false_expr = parse_precedence((Precedence)(prec + 1));
            if (!false_expr) {
                error_and_exit("Expected expression after ':'");
                return {};
            }
            auto te = std::make_unique<nari::TernaryExpr>(std::move(lhs), std::move(true_expr), std::move(false_expr));
            te->line = qmark.line;
            te->col = qmark.col;
            te->filename = qmark.filename.empty() ? current_filename : qmark.filename;
            lhs = std::move(te);
            continue;
        }

        Token op = t;
        next(); // consume operator

        // For left-assoc operators, the next minimum precedence is prec + 1
        Precedence next_min = (Precedence)(prec + 1);
        ExprPtr rhs = parse_precedence(next_min);
        if (!rhs) {
            error_and_exit("Expected RHS expression for binary operator");
            return {};
        }

        std::string opname;
        switch (op.kind) {
            case TokenKind::TK_PLUS:
                opname = "+";
                break;
            case TokenKind::TK_MINUS:
                opname = "-";
                break;
            case TokenKind::TK_STAR:
                opname = "*";
                break;
            case TokenKind::TK_SLASH:
                opname = "/";
                break;
            case TokenKind::TK_PERCENT:
                opname = "%";
                break;
            case TokenKind::TK_EXPONENT:
                opname = "**";
                break;
            case TokenKind::TK_EQEQ:
                opname = "==";
                break;
            case TokenKind::TK_NEQ:
                opname = "!=";
                break;
            case TokenKind::TK_STRICT_EQ:
                opname = "===";
                break;
            case TokenKind::TK_STRICT_NEQ:
                opname = "!==";
                break;
            case TokenKind::TK_LT:
                opname = "<";
                break;
            case TokenKind::TK_GT:
                opname = ">";
                break;
            case TokenKind::TK_LE:
                opname = "<=";
                break;
            case TokenKind::TK_GE:
                opname = ">=";
                break;
            case TokenKind::TK_ANDAND:
                opname = "&&";
                break;
            case TokenKind::TK_OROR:
                opname = "||";
                break;
            case TokenKind::TK_NULLISHCOALESCE:
                opname = "??";
                break;
            case TokenKind::TK_AMPERSAND:
                opname = "&";
                break;
            case TokenKind::TK_PIPE:
                opname = "|";
                break;
            case TokenKind::TK_CARET:
                opname = "^";
                break;
            case TokenKind::TK_LSHIFT:
                opname = "<<";
                break;
            case TokenKind::TK_RSHIFT:
                opname = ">>";
                break;
            default:
                opname = token_desc(op);
                break;
        }

        auto be = std::make_unique<nari::BinaryExpr>(opname, std::move(lhs), std::move(rhs));
        // attach source position from operator token for better diagnostics
        be->line = op.line;
        be->col = op.col;
        // Attach filename (prefer token filename if present, otherwise current parser filename)
        be->filename = op.filename.empty() ? current_filename : op.filename;
        lhs = std::move(be);
    }

    return lhs;
}

// parse unary operators and primaries
ExprPtr Parser::parse_unary() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &t = peek();
    // `await expr` is sugar for `expr.await`. The member name must stay "await",
    // because that is the name the handle member lookup blocks on. A handle has
    // no "value" member, so any other name gives none and hides the wait.
    if (t.kind == TokenKind::TK_IDENT && t.text == "await") {
        Token op = t;
        next();
        ExprPtr operand = parse_unary();
        if (!operand) {
            error_and_exit("Expected expression after 'await'");
            return {};
        }
        auto me = std::make_unique<nari::MemberExpr>(std::move(operand), "await");
        me->line = op.line;
        me->col = op.col;
        me->filename = op.filename.empty() ? current_filename : op.filename;
        return me;
    }
    if (t.kind == TokenKind::TK_BANG || t.kind == TokenKind::TK_MINUS || t.kind == TokenKind::TK_PLUS || t.kind == TokenKind::TK_TILDE) {
        Token op = t;
        next();
        ExprPtr operand = parse_unary();
        if (!operand) {
            error_and_exit("Expected operand for unary operator");
            return {};
        }
        std::string opname;
        if (op.kind == TokenKind::TK_BANG) {
            opname = "!";
        } else if (op.kind == TokenKind::TK_MINUS) {
            opname = "neg";
        } else if (op.kind == TokenKind::TK_TILDE) {
            opname = "~";
        } else {
            opname = "+";
        }
        auto ue = std::make_unique<nari::UnaryExpr>(opname, std::move(operand));
        // attach source position from operator token
        ue->line = op.line;
        ue->col = op.col;
        ue->filename = op.filename.empty() ? current_filename : op.filename;
        return ue;
    }
    // prefix ++ and --
    if (t.kind == TokenKind::TK_PLUSPLUS || t.kind == TokenKind::TK_MINUSMINUS) {
        Token op = t;
        next();
        ExprPtr operand = parse_unary();
        if (!operand) {
            error_and_exit("Expected operand for ++ or --");
            return {};
        }
        std::string opname = (op.kind == TokenKind::TK_PLUSPLUS) ? "++" : "--";
        auto ue = std::make_unique<nari::UnaryExpr>(opname, std::move(operand));
        ue->line = op.line;
        ue->col = op.col;
        ue->filename = op.filename.empty() ? current_filename : op.filename;
        return ue;
    }
    return parse_postfix();
}

// parse postfix operators (like ++ and -- after an expression, and [] and . access)
ExprPtr Parser::parse_postfix() {
    if (recover_failed_) {
        return nullptr;
    }
    ExprPtr expr = parse_primary();
    if (recover_failed_) {
        return {};
    }
    while (true) {
        const Token &tok = peek();
        if (tok.kind == TokenKind::TK_PLUSPLUS || tok.kind == TokenKind::TK_MINUSMINUS) {
            // only treat as postfix if on the same line as the expression
            if (expr && expr->line != 0 && tok.line != expr->line) {
                break;
            }
            Token op = tok;
            next();
            std::string opname = (op.kind == TokenKind::TK_PLUSPLUS) ? "post++" : "post--";
            auto ue = std::make_unique<nari::UnaryExpr>(opname, std::move(expr));
            ue->line = op.line;
            ue->col = op.col;
            ue->filename = op.filename.empty() ? current_filename : op.filename;
            expr = std::move(ue);
        } else if (tok.kind == TokenKind::TK_LPAREN) {
            // function call: expr(args...)
            Token op = tok;
            next(); // consume '('
            auto call = std::make_unique<nari::CallExpr>(std::move(expr));
            call->line = op.line;
            call->col = op.col;
            call->filename = op.filename.empty() ? current_filename : op.filename;

            if (peek().kind != TokenKind::TK_RPAREN) {
                while (true) {
                    if (peek().kind == TokenKind::TK_ELLIPSIS) {
                        next(); // consume '...'
                        auto spread = std::make_unique<nari::SpreadExpr>(parse_expression());
                        if (recover_failed_) {
                            return {};
                        }
                        call->args.push_back(std::move(spread));
                        call->has_spread = true;
                    } else {
                        call->args.push_back(parse_expression());
                        if (recover_failed_) {
                            return {};
                        }
                    }
                    if (peek().kind == TokenKind::TK_COMMA) {
                        next();
                        continue;
                    }
                    break;
                }
            }
            if (!expect(TokenKind::TK_RPAREN, "call end )")) {
                return {};
            }
            expr = std::move(call);
        } else if (tok.kind == TokenKind::TK_LBRACKET) {
            // index access: expr[index]
            Token op = tok;
            next(); // consume '['
            ExprPtr index = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (!expect(TokenKind::TK_RBRACKET, "index access end ]")) {
                return {};
            }
            auto ie = std::make_unique<nari::IndexExpr>(std::move(expr), std::move(index));
            ie->line = op.line;
            ie->col = op.col;
            ie->filename = op.filename.empty() ? current_filename : op.filename;
            expr = std::move(ie);
        } else if (tok.kind == TokenKind::TK_DOT) {
            // member access: expr.member
            Token op = tok;
            next(); // consume '.'
            Token memberTok = peek();
            if (!expect(TokenKind::TK_IDENT, "member name after .")) {
                return {};
            }
            auto me = std::make_unique<nari::MemberExpr>(std::move(expr), memberTok.text);
            me->line = op.line;
            me->col = op.col;
            me->filename = op.filename.empty() ? current_filename : op.filename;
            expr = std::move(me);
        } else if (tok.kind == TokenKind::TK_QUESTIONDOT) {
            // optional chaining: expr?.prop, expr?.[index], expr?.(args)
            Token op = tok;
            next(); // consume '?.'
            Token next_tok = peek();
            if (next_tok.kind == TokenKind::TK_LBRACKET) {
                // expr?.[index]
                next(); // consume '['
                ExprPtr index = parse_expression();
                if (recover_failed_) {
                    return {};
                }
                if (!expect(TokenKind::TK_RBRACKET, "optional index access end ]")) {
                    return {};
                }
                auto ie = std::make_unique<nari::IndexExpr>(std::move(expr), std::move(index));
                ie->optional = true;
                ie->line = op.line;
                ie->col = op.col;
                ie->filename = op.filename.empty() ? current_filename : op.filename;
                expr = std::move(ie);
            } else if (next_tok.kind == TokenKind::TK_LPAREN) {
                // expr?.(args)
                next(); // consume '('
                auto call = std::make_unique<nari::CallExpr>(std::move(expr));
                call->optional = true;
                call->line = op.line;
                call->col = op.col;
                call->filename = op.filename.empty() ? current_filename : op.filename;
                if (peek().kind != TokenKind::TK_RPAREN) {
                    while (true) {
                        if (peek().kind == TokenKind::TK_ELLIPSIS) {
                            next();
                            auto spread = std::make_unique<nari::SpreadExpr>(parse_expression());
                            if (recover_failed_) {
                                return {};
                            }
                            call->args.push_back(std::move(spread));
                            call->has_spread = true;
                        } else {
                            call->args.push_back(parse_expression());
                            if (recover_failed_) {
                                return {};
                            }
                        }
                        if (peek().kind == TokenKind::TK_COMMA) {
                            next();
                            continue;
                        }
                        break;
                    }
                }
                if (!expect(TokenKind::TK_RPAREN, "optional call end )")) {
                    return {};
                }
                expr = std::move(call);
            } else {
                // expr?.prop
                Token memberTok = peek();
                if (!expect(TokenKind::TK_IDENT, "member name after ?.")) {
                    return {};
                }
                auto me = std::make_unique<nari::MemberExpr>(std::move(expr), memberTok.text);
                me->optional = true;
                me->line = op.line;
                me->col = op.col;
                me->filename = op.filename.empty() ? current_filename : op.filename;
                expr = std::move(me);
            }
        } else {
            break;
        }
    }
    return expr;
}

// parse a match expression pattern
nari::PatternPtr Parser::parse_pattern() {
    if (recover_failed_) {
        return {};
    }
    const Token &tok = peek();

    // wildcard pattern: _
    if (tok.kind == TokenKind::TK_IDENT && tok.text == "_") {
        next();
        auto pattern = std::make_unique<nari::WildcardPattern>();
        pattern->line = tok.line;
        pattern->col = tok.col;
        pattern->filename = tok.filename.empty() ? current_filename : tok.filename;
        return pattern;
    }

    // literal patterns: numbers, strings, booleans, null
    if (tok.kind == TokenKind::TK_NUMBER || tok.kind == TokenKind::TK_STRING) {
        auto expr = parse_primary(); // reuse literal expression parsing
        if (recover_failed_) {
            return {};
        }
        auto pattern = std::make_unique<nari::LiteralPattern>(std::move(expr));
        pattern->line = tok.line;
        pattern->col = tok.col;
        pattern->filename = tok.filename.empty() ? current_filename : tok.filename;
        return pattern;
    }

    if (tok.kind == TokenKind::TK_IDENT) {
        // true, false, null literals
        if (tok.text == "true" || tok.text == "false" || tok.text == "null") {
            auto expr = parse_primary();
            if (recover_failed_) {
                return {};
            }
            auto pattern = std::make_unique<nari::LiteralPattern>(std::move(expr));
            pattern->line = tok.line;
            pattern->col = tok.col;
            pattern->filename = tok.filename.empty() ? current_filename : tok.filename;
            return pattern;
        }

        // variant pattern or binding
        std::string name = next().text;

        // check for something like Ok(value)
        if (peek().kind == TokenKind::TK_LPAREN) {
            next(); // consume '('
            auto pattern = std::make_unique<nari::VariantPattern>(name);
            pattern->line = tok.line;
            pattern->col = tok.col;
            pattern->filename = tok.filename.empty() ? current_filename : tok.filename;

            // Parse nested patterns
            while (!is_eof() && peek().kind != TokenKind::TK_RPAREN) {
                pattern->fields.push_back(parse_pattern());
                if (recover_failed_) {
                    return {};
                }

                if (peek().kind == TokenKind::TK_COMMA) {
                    next(); // consume ','
                } else if (peek().kind != TokenKind::TK_RPAREN) {
                    error_and_exit("Expected ',' or ')' in variant pattern");
                    return {};
                }
            }
            if (!expect(TokenKind::TK_RPAREN, "')' after variant fields")) {
                return {};
            }
            return pattern;
        }

        // check for struct destructuring like Text { content, sender }
        if (peek().kind == TokenKind::TK_LBRACE) {
            next(); // consume '{'
            auto pattern = std::make_unique<nari::VariantPattern>(name);
            pattern->is_struct_pattern = true;
            pattern->line = tok.line;
            pattern->col = tok.col;
            pattern->filename = tok.filename.empty() ? current_filename : tok.filename;

            while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
                if (peek().kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected field name in struct pattern");
                    return {};
                }
                pattern->named_bindings.push_back(next().text);

                if (peek().kind == TokenKind::TK_COMMA) {
                    next(); // consume ','
                } else if (peek().kind != TokenKind::TK_RBRACE) {
                    error_and_exit("Expected ',' or '}' in struct pattern");
                    return {};
                }
            }
            if (!expect(TokenKind::TK_RBRACE, "'}' after struct pattern fields")) {
                return {};
            }
            return pattern;
        }

        // simple binding pattern: x, value, etc.
        auto pattern = std::make_unique<nari::BindingPattern>(name);
        pattern->line = tok.line;
        pattern->col = tok.col;
        pattern->filename = tok.filename.empty() ? current_filename : tok.filename;
        return pattern;
    }

    error_and_exit("Expected pattern");
    return {};
    return nullptr;
}

// parse match expression itself: match value { pattern => expr, ... }
ExprPtr Parser::parse_match_expression() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &matchTok = peek();
    next(); // consume 'match'

    // parse the value being matched
    auto scrutinee = parse_expression();
    if (recover_failed_) {
        return {};
    }

    auto match_expr = std::make_unique<nari::MatchExpr>(std::move(scrutinee));
    match_expr->line = matchTok.line;
    match_expr->col = matchTok.col;
    match_expr->filename = matchTok.filename.empty() ? current_filename : matchTok.filename;

    if (!expect(TokenKind::TK_LBRACE, "'{' after match scrutinee")) {
        return {};
    }

    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
        auto pattern = parse_pattern();
        if (recover_failed_) {
            return {};
        }

        // expect '=>' (either as FATARROW or as '=' followed by '>')
        if (peek().kind == TokenKind::TK_FATARROW) {
            next(); // consume '=>'
        } else if (peek().kind == TokenKind::TK_EQUAL && peek(1).kind == TokenKind::TK_GT) {
            next(); // consume '='
            next(); // consume '>'
        } else {
            error_and_exit("Expected '=>' after pattern in match arm");
            return {};
        }

        auto body_expr = parse_expression();
        if (recover_failed_) {
            return {};
        }

        match_expr->arms.emplace_back(std::move(pattern), std::move(body_expr));

        // optional comma after arm
        if (peek().kind == TokenKind::TK_COMMA) {
            next();
        }
    }

    if (!expect(TokenKind::TK_RBRACE, "'}' after match arms")) {
        return {};
    }

    if (match_expr->arms.empty()) {
        error_and_exit("Match expression must have at least one arm");
        return {};
    }

    return match_expr;
}

ExprPtr Parser::parse_primary() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &tok = peek();
    if (tok.kind == TokenKind::TK_IDENT) {
        if (tok.text == "func") {
            return parse_function_expression();
        }
        if (tok.text == "spawn") {
            return parse_spawn_expression();
        }
        if (tok.text == "match") {
            return parse_match_expression();
        }
        if (tok.text == "this") {
            auto this_expr = std::make_unique<nari::ThisExpr>();
            this_expr->line = tok.line;
            this_expr->col = tok.col;
            this_expr->filename = tok.filename.empty() ? current_filename : tok.filename;
            next();
            return this_expr;
        }
        if (tok.text == "new") {
            next(); // consume 'new'
            if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected class name after 'new'");
                return {};
            }
            std::string class_name = next().text;
            auto new_expr = std::make_unique<nari::NewExpr>(class_name);
            new_expr->line = tok.line;
            new_expr->col = tok.col;
            new_expr->filename = tok.filename.empty() ? current_filename : tok.filename;

            // parse constructor arguments
            if (!expect(TokenKind::TK_LPAREN, "'(' for constructor arguments")) {
                return {};
            }
            if (peek().kind != TokenKind::TK_RPAREN) {
                while (true) {
                    new_expr->args.push_back(parse_expression());
                    if (recover_failed_) {
                        return {};
                    }
                    if (peek().kind == TokenKind::TK_COMMA) {
                        next();
                    } else if (peek().kind == TokenKind::TK_RPAREN) {
                        break;
                    } else {
                        error_and_exit("Expected ',' or ')' in constructor arguments");
                        return {};
                    }
                }
            }
            if (!expect(TokenKind::TK_RPAREN, "')' after constructor arguments")) {
                return {};
            }
            return new_expr;
        }
        if (tok.text == "true") {
            auto b = std::make_unique<nari::BoolExpr>(true);
            b->line = tok.line;
            b->col = tok.col;
            next();
            return b;
        }
        if (tok.text == "false") {
            auto b = std::make_unique<nari::BoolExpr>(false);
            b->line = tok.line;
            b->col = tok.col;
            next();
            return b;
        }
        if (tok.text == "null") {
            auto n = std::make_unique<nari::NullExpr>();
            n->line = tok.line;
            n->col = tok.col;
            next();
            return n;
        }

        std::string ident = next().text;
        auto ie = std::make_unique<nari::IdentExpr>(ident);
        ie->line = tok.line;
        ie->col = tok.col;
        ie->filename = tok.filename.empty() ? current_filename : tok.filename;
        return ie;
    } else if (tok.kind == TokenKind::TK_STRING) {
        std::string str = tok.text;
        auto str_expr = std::make_unique<nari::StringExpr>(str);
        str_expr->line = tok.line;
        str_expr->col = tok.col;
        str_expr->filename = tok.filename.empty() ? current_filename : tok.filename;
        next();
        return str_expr;
    } else if (tok.kind == TokenKind::TK_INTERP_STRING) {
        // interpolated string: `Hello {name}!`
        auto interp = std::make_unique<nari::StringInterpolationExpr>();
        interp->line = tok.line;
        interp->col = tok.col;
        interp->filename = tok.filename.empty() ? current_filename : tok.filename;

        interp->parts = tok.interp_parts;
        interp->expr_sources = tok.interp_exprs;
        interp->format_specs = tok.interp_formats;

        next();
        return interp;
    } else if (tok.kind == TokenKind::TK_NUMBER) {
        std::string s = tok.text;
        // Check hex prefix first: 'e'/'E' in hex literals are hex digits, not
        // exponent markers. e.g. 0xDEAD must NOT be treated as float even though
        // it contains 'E'.
        bool is_hex_prefix = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
        bool is_float = !is_hex_prefix &&
                        ((s.find('.') != std::string::npos) || (s.find('e') != std::string::npos) || (s.find('E') != std::string::npos));
        std::unique_ptr<nari::NumberExpr> num_expr;
        if (is_float) {
            double v = 0.0;
            char *endptr = nullptr;
            v = std::strtod(s.c_str(), &endptr);
            if (endptr == s.c_str()) {
                v = 0.0;
            }
            num_expr = std::make_unique<nari::NumberExpr>(v);
        } else {
            errno = 0;
            char *endptr = nullptr;

            bool is_hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
            int base = is_hex ? 16 : 10;

            int64_t v = std::strtoll(s.c_str(), &endptr, base);
            if (endptr == s.c_str() || errno == ERANGE) {
                if (is_hex) {
                    errno = 0;
                    endptr = nullptr;
                    unsigned long long uv = std::strtoull(s.c_str(), &endptr, 16);
                    if (endptr == s.c_str() || errno == ERANGE) {
                        num_expr = std::make_unique<nari::NumberExpr>(0.0);
                    } else if (uv <= static_cast<unsigned long long>(std::numeric_limits<int64_t>::max())) {
                        num_expr = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(uv));
                    } else {
                        num_expr = std::make_unique<nari::NumberExpr>(static_cast<double>(uv));
                    }
                } else {
                    double dv = std::strtod(s.c_str(), &endptr);
                    if (endptr == s.c_str()) {
                        dv = 0.0;
                    }
                    num_expr = std::make_unique<nari::NumberExpr>(dv);
                }
            } else {
                num_expr = std::make_unique<nari::NumberExpr>(v);
            }
        }
        num_expr->line = tok.line;
        num_expr->col = tok.col;
        num_expr->filename = tok.filename.empty() ? current_filename : tok.filename;
        next();
        return num_expr;
    } else if (tok.kind == TokenKind::TK_LPAREN) {
        // Check if this is an arrow function: (params) => ...
        if (looks_like_arrow_function()) {
            return parse_arrow_function_expression();
        }
        // Otherwise, it's a parenthesized expression
        next(); // consume '('
        ExprPtr e = parse_expression();
        if (recover_failed_) {
            return {};
        }
        if (!expect(TokenKind::TK_RPAREN, "closing )")) {
            return {};
        }
        return e;
    } else if (tok.kind == TokenKind::TK_LBRACKET) {
        // array literal: [elem1, elem2, ...]
        Token start = tok;
        next(); // consume '['
        auto arr = std::make_unique<nari::ArrayLiteralExpr>();
        arr->line = start.line;
        arr->col = start.col;
        arr->filename = start.filename.empty() ? current_filename : start.filename;

        if (peek().kind != TokenKind::TK_RBRACKET) {
            while (true) {
                if (peek().kind == TokenKind::TK_ELLIPSIS) {
                    next(); // consume '...'
                    auto spread = std::make_unique<nari::SpreadExpr>(parse_expression());
                    if (recover_failed_) {
                        return {};
                    }
                    arr->elements.push_back(std::move(spread));
                    arr->has_spread = true;
                } else {
                    arr->elements.push_back(parse_expression());
                    if (recover_failed_) {
                        return {};
                    }
                }
                if (peek().kind == TokenKind::TK_COMMA) {
                    next();

                    if (peek().kind == TokenKind::TK_RBRACKET) {
                        break;
                    }
                    continue;
                }
                break;
            }
        }
        if (!expect(TokenKind::TK_RBRACKET, "array literal end ]")) {
            return {};
        }
        return arr;
    } else if (tok.kind == TokenKind::TK_LBRACE) {
        // object literal: {key1: val1, key2: val2, ...}
        Token start = tok;
        next(); // consume '{'
        auto obj = std::make_unique<nari::ObjectLiteralExpr>();
        obj->line = start.line;
        obj->col = start.col;
        obj->filename = start.filename.empty() ? current_filename : start.filename;

        if (peek().kind != TokenKind::TK_RBRACE) {
            while (true) {
                if (peek().kind == TokenKind::TK_ELLIPSIS) {
                    next(); // consume '...'
                    auto spread_val = parse_expression();
                    if (recover_failed_) {
                        return {};
                    }
                    obj->entries.push_back({ "", std::move(spread_val) });
                    obj->has_spread = true;
                } else {
                    // k:v
                    Token keyTok = peek();
                    std::string key;
                    if (keyTok.kind == TokenKind::TK_IDENT) {
                        key = keyTok.text;
                        next();
                    } else if (keyTok.kind == TokenKind::TK_STRING) {
                        key = keyTok.text;
                        next();
                    } else {
                        error_and_exit("Expected identifier or string as object key");
                        return {};
                    }

                    if (!expect(TokenKind::TK_COLON, "object literal :")) {
                        return {};
                    }

                    ExprPtr value;
                    if (peek().kind == TokenKind::TK_IDENT && peek().text == "func") {
                        // func(params) { ... }
                        value = parse_function_expression();
                        if (recover_failed_) {
                            return {};
                        }
                    } else if (peek().kind == TokenKind::TK_LPAREN && looks_like_arrow_function()) {
                        // (params) => { ... } arrow function expression
                        value = parse_arrow_function_expression();
                        if (recover_failed_) {
                            return {};
                        }
                    } else {
                        // Regular expression
                        value = parse_expression();
                        if (recover_failed_) {
                            return {};
                        }
                    }
                    obj->entries.push_back({ key, std::move(value) });
                }

                if (peek().kind == TokenKind::TK_COMMA) {
                    next();

                    if (peek().kind == TokenKind::TK_RBRACE) {
                        break;
                    }
                    continue;
                }
                break;
            }
        }
        if (!expect(TokenKind::TK_RBRACE, "object literal end }")) {
            return {};
        }
        return obj;
    } else if (tok.kind == TokenKind::TK_REGEX) {
        // regex literal /pattern/flags
        std::string pattern = tok.text;
        std::string flags = tok.interp_parts.empty() ? "" : tok.interp_parts[0];
        auto re_expr = std::make_unique<nari::RegexLiteralExpr>(pattern, flags);
        re_expr->line = tok.line;
        re_expr->col = tok.col;
        re_expr->filename = tok.filename.empty() ? current_filename : tok.filename;
        next();
        return re_expr;
    } else {
        error_and_exit("Unexpected primary token '" + token_desc(tok) + "'");
        return {};
        return nullptr;
    }
}

} // namespace Parser
