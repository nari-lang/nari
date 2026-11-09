/*
  parse_function.cpp
  Parser class methods split verbatim from parser.cpp.
*/

#include "parser/parser.h"

namespace Parser {

FunctionPtr Parser::parse_function() {
    if (recover_failed_) {
        return nullptr;
    }
    // consume 'func'
    next();
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected function name after 'func'");
        return {};
    }
    auto fn = std::make_unique<nari::Function>();
    fn->name = nameTok.text;
    fn->line = nameTok.line;
    fn->col = nameTok.col;
    fn->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;
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

            // parse optional type annotation, name: type or name: type[]
            nari::TypeAnnotationPtr param_type = nullptr;
            if (peek().kind == TokenKind::TK_COLON) {
                next(); // consume ':'
                param_type = parse_type_annotation();
                if (recover_failed_) {
                    return {};
                }
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

            fn->params.emplace_back(pname, std::move(default_value), is_rest, std::move(param_type));

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
        fn->return_type = parse_type_annotation();
        if (recover_failed_) {
            return {};
        }
    }

    fn->body = parse_block();
    if (recover_failed_) {
        return {};
    }
    fn->strict_mode = this->strict_mode;
    return fn;
}

BlockPtr Parser::parse_block() {
    if (recover_failed_) {
        return nullptr;
    }
    if (!expect(TokenKind::TK_LBRACE, "block start '{'")) {
        return {};
    }
    auto blk = std::make_unique<nari::BlockStmt>();
    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
        if (peek().kind == TokenKind::TK_SEMICOLON) {
            next();
            continue;
        }
        auto stmt = parse_stmt();
        if (recover_failed_) {
            return {};
        }
        if (stmt) {
            blk->stmts.push_back(std::move(stmt));
        } else {
            break;
        }
    }
    if (!expect(TokenKind::TK_RBRACE, "block end '}'")) {
        return {};
    }
    prune_dead_code(blk.get());
    optimize_block(blk.get());
    propagate_consts(blk.get());
    return blk;
}

void Parser::prune_dead_code(nari::BlockStmt *blk) {
    if (!blk) {
        return;
    }
    bool seen_return = false;
    for (size_t i = 0; i < blk->stmts.size(); ++i) {
        if (seen_return) {
            blk->stmts.erase(blk->stmts.begin() + i, blk->stmts.end());
            break;
        }
        if ((blk->stmts[i].get()) != nullptr && (blk->stmts[i].get())->stmt_kind == nari::StmtKind::Return) {
            seen_return = true;
        }
    }
}

void Parser::optimize_block(nari::BlockStmt *blk) {
    if (!blk) {
        return;
    }

    for (size_t i = 0; i < blk->stmts.size(); ++i) {
        auto *fs =
            ((blk->stmts[i].get()) != nullptr && (blk->stmts[i].get())->stmt_kind == nari::StmtKind::For
                 ? static_cast<nari::ForStmt *>(blk->stmts[i].get())
                 : nullptr);
        if (!fs) {
            continue;
        }
        nari::Stmt *prev = (i > 0) ? blk->stmts[i - 1].get() : nullptr;
        auto replacement = try_optimize_for(fs, prev);
        if (replacement) {
            blk->stmts[i] = std::move(replacement);
        }
    }
}

} // namespace Parser
