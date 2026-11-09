/*
  parse_stmt.cpp
  Parser class methods split verbatim from parser.cpp.
*/

#include "parser/parser.h"

namespace Parser {

StmtPtr Parser::parse_stmt() {
    if (recover_failed_) {
        return nullptr;
    }
    StmtPtr stmt = parse_stmt_core();
    if (recover_failed_) {
        return {};
    }
    if (stmt) {
        check_statement_boundary();
    }
    return stmt;
}

StmtPtr Parser::parse_stmt_core() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &tok = peek();
    if (tok.kind == TokenKind::TK_IDENT) {
        // control flow keywords
        if (tok.text == "if") {
            next(); // consume 'if'
            if (!expect(TokenKind::TK_LPAREN, "if condition (")) {
                return {};
            }
            ExprPtr cond = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (!expect(TokenKind::TK_RPAREN, "if condition )")) {
                return {};
            }
            StmtPtr then_branch = parse_stmt();
            if (!then_branch) {
                error_and_exit("Expected statement or block after if condition");
                return {};
            }
            StmtPtr else_branch = nullptr;
            if (peek().kind == TokenKind::TK_IDENT && peek().text == "else") {
                next(); // consume 'else'
                else_branch = parse_stmt();
                if (!else_branch) {
                    error_and_exit("Expected statement or block after else");
                    return {};
                }
            }
            auto ifs = std::make_unique<nari::IfStmt>(std::move(cond), std::move(then_branch), std::move(else_branch));
            ifs->line = tok.line;
            ifs->col = tok.col;
            ifs->filename = tok.filename.empty() ? current_filename : tok.filename;
            return ifs;
        }

        if (tok.text == "while") {
            next(); // consume tok

            if (!expect(TokenKind::TK_LPAREN, "while condition (")) {
                return {};
            }
            ExprPtr cond = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (!expect(TokenKind::TK_RPAREN, "while condition )")) {
                return {};
            }
            StmtPtr body = parse_block();
            if (recover_failed_) {
                return {};
            }
            auto ws = std::make_unique<nari::WhileStmt>(std::move(cond), std::move(body));
            ws->line = tok.line;
            ws->col = tok.col;
            ws->filename = tok.filename.empty() ? current_filename : tok.filename;
            return ws;
        }

        if (tok.text == "for") {
            if (looks_like_foreach_loop()) {
                Token kw = tok;
                next(); // consume 'for'
                if (!expect(TokenKind::TK_LPAREN, "for (")) {
                    return {};
                }
                // optional 'let' keyword: for (let x of ...) or for (let x, y of ...)
                if (peek().kind == TokenKind::TK_IDENT && peek().text == "let") {
                    next(); // consume 'let'
                }
                Token varTok = peek();
                if (!expect(TokenKind::TK_IDENT, "foreach variable")) {
                    return {};
                }
                // optional second variable: for (key, value in ...) or (key, value of ...)
                std::string val_var;
                if (peek().kind == TokenKind::TK_COMMA) {
                    next(); // consume ','
                    val_var = peek().text;
                    if (!expect(TokenKind::TK_IDENT, "foreach value variable")) {
                        return {};
                    }
                }
                if (peek().kind != TokenKind::TK_IDENT || (peek().text != "in" && peek().text != "of")) {
                    error_and_exit("Expected 'in' or 'of' in for-each loop");
                    return {};
                }
                next(); // consume 'in' or 'of'
                ExprPtr iterable = parse_expression();
                if (recover_failed_) {
                    return {};
                }
                if (!expect(TokenKind::TK_RPAREN, "for )")) {
                    return {};
                }
                StmtPtr body = parse_block();
                if (recover_failed_) {
                    return {};
                }
                std::unique_ptr<nari::ForEachStmt> fs;
                if (val_var.empty()) {
                    fs = std::make_unique<nari::ForEachStmt>(varTok.text, std::move(iterable), std::move(body));
                } else {
                    fs = std::make_unique<nari::ForEachStmt>(varTok.text, val_var, std::move(iterable), std::move(body));
                }
                fs->line = kw.line;
                fs->col = kw.col;
                fs->filename = kw.filename.empty() ? current_filename : kw.filename;
                return fs;
            }

            Token kw = tok;
            next(); // consume 'for'
            if (!expect(TokenKind::TK_LPAREN, "for (")) {
                return {};
            }
            // either a declaration or an expression statement (or empty)
            StmtPtr init = nullptr;
            if (peek().kind != TokenKind::TK_SEMICOLON) {
                if (peek().kind == TokenKind::TK_IDENT && (peek().text == "let" || peek().text == "global" || peek().text == "const")) {
                    init = parse_stmt();
                    if (recover_failed_) {
                        return {};
                    }
                } else {
                    // Handle assignment (i = 0) or expression
                    if (peek().kind == TokenKind::TK_IDENT && peek(1).kind == TokenKind::TK_EQUAL) {
                        const Token &nameTok = peek();
                        std::string pname = next().text; // consume identifier
                        next();                          // consume '='

                        ExprPtr prhs = parse_expression();
                        if (recover_failed_) {
                            return {};
                        }
                        auto assign = std::make_unique<nari::AssignStmt>(pname, std::move(prhs));
                        assign->line = nameTok.line;
                        assign->col = nameTok.col;
                        assign->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;
                        init = std::move(assign);
                    } else {
                        init = std::make_unique<nari::ExprStmt>(parse_expression());
                        if (recover_failed_) {
                            return {};
                        }
                    }
                    if (peek().kind == TokenKind::TK_SEMICOLON) {
                        next();
                    }
                }
            } else {
                next();
            }

            ExprPtr cond = nullptr;
            if (peek().kind != TokenKind::TK_SEMICOLON) {
                cond = parse_expression();
                if (recover_failed_) {
                    return {};
                }
            }
            if (!expect(TokenKind::TK_SEMICOLON, "for ;")) {
                return {};
            }
            StmtPtr post = nullptr;
            if (peek().kind != TokenKind::TK_RPAREN) {
                // allow either an expression (e.g. i = i + 1) or an assignment statement as the post clause
                if (peek().kind == TokenKind::TK_IDENT && peek(1).kind == TokenKind::TK_EQUAL) {
                    const Token &nameTok = peek();
                    std::string pname = next().text; // consume identifier
                    next();                          // consume '='

                    ExprPtr prhs = parse_expression();
                    if (recover_failed_) {
                        return {};
                    }
                    auto assign = std::make_unique<nari::AssignStmt>(pname, std::move(prhs));
                    assign->line = nameTok.line;
                    assign->col = nameTok.col;
                    assign->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;
                    post = std::move(assign);
                } else {
                    post = std::make_unique<nari::ExprStmt>(parse_expression());
                    if (recover_failed_) {
                        return {};
                    }
                }
            }
            if (!expect(TokenKind::TK_RPAREN, "for )")) {
                return {};
            }
            StmtPtr body = parse_block();
            if (recover_failed_) {
                return {};
            }
            auto fs = std::make_unique<nari::ForStmt>(std::move(init), std::move(cond), std::move(post), std::move(body));
            fs->line = kw.line;
            fs->col = kw.col;
            fs->filename = kw.filename.empty() ? current_filename : kw.filename;
            return fs;
        }

        if (tok.text == "switch") {
            return parse_switch_stmt();
        }

        if (tok.text == "break") {
            next();
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto bs = std::make_unique<nari::BreakStmt>();
            bs->line = tok.line;
            bs->col = tok.col;
            bs->filename = tok.filename.empty() ? current_filename : tok.filename;
            return bs;
        }

        if (tok.text == "continue") {
            next();
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto cs = std::make_unique<nari::ContinueStmt>();
            cs->line = tok.line;
            cs->col = tok.col;
            cs->filename = tok.filename.empty() ? current_filename : tok.filename;
            return cs;
        }

        if (tok.text == "return") {
            next();
            ExprPtr val = nullptr;
            // optional return value
            if (peek().kind != TokenKind::TK_SEMICOLON && peek().kind != TokenKind::TK_RBRACE && peek().kind != TokenKind::TK_EOF) {
                val = parse_expression();
                if (recover_failed_) {
                    return {};
                }
            }
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto rs = std::make_unique<nari::ReturnStmt>(std::move(val));
            rs->line = tok.line;
            rs->col = tok.col;
            rs->filename = tok.filename.empty() ? current_filename : tok.filename;
            return rs;
        }

        // import statement: `import name from "library.so"`
        if (tok.text == "import") {
            Token importTok = tok;
            next(); // consume 'import'

            // check if this is "import name from" syntax (ffi)
            if (peek().kind == TokenKind::TK_IDENT) {
                std::string var_name = next().text;

                if (peek().kind != TokenKind::TK_IDENT || peek().text != "from") {
                    error_and_exit("Expected 'from' after import variable name");
                    return {};
                }
                next(); // consume "from"

                if (peek().kind != TokenKind::TK_STRING) {
                    error_and_exit("Expected library path string after 'from'");
                    return {};
                }
                std::string lib_path = next().text;

                // create `let var_name = __ffi_load_library("path");`
                auto load_call = std::make_unique<nari::CallExpr>(std::make_unique<nari::IdentExpr>("__ffi_load_library"));
                load_call->args.push_back(std::make_unique<nari::StringExpr>(lib_path));

                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }

                auto var_decl = std::make_unique<nari::VarDeclStmt>(var_name, std::move(load_call), VarDeclCtrl::LOCAL);
                var_decl->line = importTok.line;
                var_decl->col = importTok.col;
                var_decl->filename = importTok.filename.empty() ? current_filename : importTok.filename;

                return var_decl;
            } else {
                error_and_exit("import inside functions only supports 'import name from \"library.so\"' syntax");
                return {};
            }
        }

        if (tok.text == "var" && peek(1).kind == TokenKind::TK_IDENT &&
            (peek(2).kind == TokenKind::TK_EQUAL || peek(2).kind == TokenKind::TK_SEMICOLON)) {
            error_and_exit("'var' is not a valid variable declaration; use 'let', 'global', or 'const'");
            return {};
        }

        // variable declaration: `let IDENT = expr`, `global IDENT = expr`, or `const IDENT = expr`
        // or destructuring: `let/const [a, b] = expr` or `let/const {x, y} = expr`
        if (tok.text == "let" || tok.text == "global" || tok.text == "const") {
            VarDeclCtrl is_global = (VarDeclCtrl)(tok.text == "global");
            bool is_const = tok.text == "const";
            Token keyword = tok;
            next(); // consume declaration keyword

            // Check for array destructuring: let [a, b, c] = ...
            if (peek().kind == TokenKind::TK_LBRACKET) {
                next(); // consume '['

                std::vector<std::string> names;
                while (peek().kind != TokenKind::TK_RBRACKET && !is_eof()) {
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected identifier in array destructuring");
                        return {};
                    }
                    names.push_back(next().text);

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next(); // consume ','
                    } else if (peek().kind != TokenKind::TK_RBRACKET) {
                        error_and_exit("Expected ',' or ']' in array destructuring");
                        return {};
                    }
                }
                if (!expect(TokenKind::TK_RBRACKET, "']' to close array destructuring")) {
                    return {};
                }

                ExprPtr init = nullptr;
                if (peek().kind == TokenKind::TK_EQUAL) {
                    next(); // consume '='
                    init = parse_expression();
                    if (recover_failed_) {
                        return {};
                    }
                } else {
                    error_and_exit("Array destructuring requires initialization");
                    return {};
                }

                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }

                auto decl = std::make_unique<nari::VarDeclStmt>("", std::move(init), is_global, is_const);
                decl->destructure_kind = nari::DestructureKind::Array;
                decl->array_names = std::move(names);
                decl->line = keyword.line;
                decl->col = keyword.col;
                decl->filename = keyword.filename.empty() ? current_filename : keyword.filename;
                return decl;
            }

            // check for object destructuring: let {x, y} = ... or let {a: x, b: y} = ...
            if (peek().kind == TokenKind::TK_LBRACE) {
                next(); // consume '{'

                std::vector<std::pair<std::string, std::string>> bindings;
                while (peek().kind != TokenKind::TK_RBRACE && !is_eof()) {
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected identifier in object destructuring");
                        return {};
                    }
                    std::string key = next().text;
                    std::string name = key; // default: use same name

                    // check for key: name syntax
                    if (peek().kind == TokenKind::TK_COLON) {
                        next(); // consume ':'
                        if (peek().kind != TokenKind::TK_IDENT) {
                            error_and_exit("Expected identifier after ':' in object destructuring");
                            return {};
                        }
                        name = next().text;
                    }

                    bindings.emplace_back(key, name);

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next(); // consume ','
                    } else if (peek().kind != TokenKind::TK_RBRACE) {
                        error_and_exit("Expected ',' or '}' in object destructuring");
                        return {};
                    }
                }
                if (!expect(TokenKind::TK_RBRACE, "'}' to close object destructuring")) {
                    return {};
                }

                ExprPtr init = nullptr;
                if (peek().kind == TokenKind::TK_EQUAL) {
                    next(); // consume '='
                    init = parse_expression();
                    if (recover_failed_) {
                        return {};
                    }
                } else {
                    error_and_exit("Object destructuring requires initialization");
                    return {};
                }

                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }

                auto decl = std::make_unique<nari::VarDeclStmt>("", std::move(init), is_global, is_const);
                decl->destructure_kind = nari::DestructureKind::Object;
                decl->object_bindings = std::move(bindings);
                decl->line = keyword.line;
                decl->col = keyword.col;
                decl->filename = keyword.filename.empty() ? current_filename : keyword.filename;
                return decl;
            }

            // simple variable declaration
            const Token &nameTok = peek();
            if (nameTok.kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected identifier after '" + keyword.text + "'");
                return {};
            }
            std::string name = next().text;
            ExprPtr init = nullptr;
            if (peek().kind == TokenKind::TK_EQUAL) {
                next(); // consume '='
                init = parse_expression();
                if (recover_failed_) {
                    return {};
                }
            }
            if (is_const && !init) {
                error_and_exit("Const declaration requires initialization");
                return {};
            }
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto decl = std::make_unique<nari::VarDeclStmt>(name, std::move(init), is_global, is_const);
            decl->line = keyword.line;
            decl->col = keyword.col;
            decl->filename = keyword.filename.empty() ? current_filename : keyword.filename;
            return decl;
        }

        // '=' expr (simple assignment or indexed assignment)
        if (peek(1).kind == TokenKind::TK_EQUAL) {
            std::string name = next().text;
            next(); // consume '='

            ExprPtr rhs = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto as = std::make_unique<nari::AssignStmt>(name, std::move(rhs));
            as->line = tok.line;
            as->col = tok.col;
            as->filename = tok.filename.empty() ? current_filename : tok.filename;
            return as;
        }

        // compound assignment: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=
        TokenKind next_kind = peek(1).kind;
        if (next_kind == TokenKind::TK_PLUSEQ || next_kind == TokenKind::TK_MINUSEQ || next_kind == TokenKind::TK_STAREQ ||
            next_kind == TokenKind::TK_SLASHEQ || next_kind == TokenKind::TK_PERCENTEQ || next_kind == TokenKind::TK_AMPEQ ||
            next_kind == TokenKind::TK_PIPEEQ || next_kind == TokenKind::TK_CARETEQ || next_kind == TokenKind::TK_LSHIFTEQ ||
            next_kind == TokenKind::TK_RSHIFTEQ) {
            std::string name = next().text;
            Token opTok = next(); // consume compound operator

            // Determine the binary operator
            std::string binop;
            if (opTok.kind == TokenKind::TK_PLUSEQ) {
                binop = "+";
            } else if (opTok.kind == TokenKind::TK_MINUSEQ) {
                binop = "-";
            } else if (opTok.kind == TokenKind::TK_STAREQ) {
                binop = "*";
            } else if (opTok.kind == TokenKind::TK_SLASHEQ) {
                binop = "/";
            } else if (opTok.kind == TokenKind::TK_PERCENTEQ) {
                binop = "%";
            } else if (opTok.kind == TokenKind::TK_AMPEQ) {
                binop = "&";
            } else if (opTok.kind == TokenKind::TK_PIPEEQ) {
                binop = "|";
            } else if (opTok.kind == TokenKind::TK_CARETEQ) {
                binop = "^";
            } else if (opTok.kind == TokenKind::TK_LSHIFTEQ) {
                binop = "<<";
            } else if (opTok.kind == TokenKind::TK_RSHIFTEQ) {
                binop = ">>";
            }

            ExprPtr rhs = parse_expression();
            if (recover_failed_) {
                return {};
            }

            // name = name <operation> rhs (i.e name = name + rhs)
            auto lhs = std::make_unique<nari::IdentExpr>(name);
            lhs->line = tok.line;
            lhs->col = tok.col;
            lhs->filename = tok.filename.empty() ? current_filename : tok.filename;

            auto binexpr = std::make_unique<nari::BinaryExpr>(binop, std::move(lhs), std::move(rhs));
            binexpr->line = opTok.line;
            binexpr->col = opTok.col;
            binexpr->filename = opTok.filename.empty() ? current_filename : opTok.filename;

            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto as = std::make_unique<nari::AssignStmt>(name, std::move(binexpr));
            as->line = tok.line;
            as->col = tok.col;
            as->filename = tok.filename.empty() ? current_filename : tok.filename;
            return as;
        }

        // check for indexed/member assignment: expr["member"] = val, or
        // expr.member = val this part is kinda a mess :)
        bool is_indexed_assign = false;
        size_t lookahead_idx = 1;
        while (lookahead_idx < toks.size()) {
            TokenKind k = peek(lookahead_idx).kind;
            if (k == TokenKind::TK_LBRACKET || k == TokenKind::TK_DOT) {
                lookahead_idx++;

                if (k == TokenKind::TK_LBRACKET) {
                    // skip to matching ]
                    int depth = 1;
                    while (lookahead_idx < toks.size() && depth > 0) {
                        if (peek(lookahead_idx).kind == TokenKind::TK_LBRACKET) {
                            depth++;
                        } else if (peek(lookahead_idx).kind == TokenKind::TK_RBRACKET) {
                            depth--;
                        }
                        lookahead_idx++;
                    }
                } else if (k == TokenKind::TK_DOT) {
                    // skip identifier
                    if (lookahead_idx < toks.size() && peek(lookahead_idx).kind == TokenKind::TK_IDENT) {
                        lookahead_idx++;
                    }
                }
            } else if (k == TokenKind::TK_EQUAL) {
                is_indexed_assign = true;
                break;
            } else {
                break;
            }
        }

        if (is_indexed_assign) {
            // Parse the target as an expression (which will be IndexExpr or
            // MemberExpr)
            ExprPtr target = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (!expect(TokenKind::TK_EQUAL, "indexed assignment =")) {
                return {};
            }
            ExprPtr value = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto idxAssignStmt = std::make_unique<nari::IndexAssignStmt>(std::move(target), std::move(value));
            idxAssignStmt->line = tok.line;
            idxAssignStmt->col = tok.col;
            idxAssignStmt->filename = tok.filename.empty() ? current_filename : tok.filename;
            return idxAssignStmt;
        }

        // call or expr statement
        if (peek(1).kind == TokenKind::TK_LPAREN) {
            ExprPtr e = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            auto exprStmt = std::make_unique<nari::ExprStmt>(std::move(e));
            exprStmt->line = tok.line;
            exprStmt->col = tok.col;
            exprStmt->filename = tok.filename.empty() ? current_filename : tok.filename;
            return exprStmt;
        }

        ExprPtr expr = parse_expression();
        if (recover_failed_) {
            return {};
        }
        if (peek().kind == TokenKind::TK_SEMICOLON) {
            next();
        }
        auto exprStmt = std::make_unique<nari::ExprStmt>(std::move(expr));
        exprStmt->line = tok.line;
        exprStmt->col = tok.col;
        exprStmt->filename = tok.filename.empty() ? current_filename : tok.filename;
        return exprStmt;
    } else if (tok.kind == TokenKind::TK_LBRACE) {
        auto blk = parse_block();
        if (recover_failed_) {
            return {};
        }
        return blk;
    } else if (tok.kind == TokenKind::TK_PLUSPLUS || tok.kind == TokenKind::TK_MINUSMINUS) {
        // handle prefix increment/decrement as statements
        ExprPtr e = parse_expression();
        if (recover_failed_) {
            return {};
        }
        if (peek().kind == TokenKind::TK_SEMICOLON) {
            next();
        }
        auto es = std::make_unique<nari::ExprStmt>(std::move(e));
        es->line = tok.line;
        es->col = tok.col;
        es->filename = tok.filename.empty() ? current_filename : tok.filename;
        return es;
    } else if (tok.kind == TokenKind::TK_STRING) {
        ExprPtr s = parse_expression();
        if (recover_failed_) {
            return {};
        }
        if (peek().kind == TokenKind::TK_SEMICOLON) {
            next();
        }
        auto es = std::make_unique<nari::ExprStmt>(std::move(s));
        es->line = tok.line;
        es->col = tok.col;
        es->filename = tok.filename.empty() ? current_filename : tok.filename;
        return es;
    } else if (tok.kind == TokenKind::TK_EOF) {
        return nullptr;
    } else {
        error_and_exit("Unexpected token in statement: '" + token_desc(tok) + "'");
        return {};
        unreachable();
    }
}

StmtPtr Parser::parse_switch_stmt() {
    if (recover_failed_) {
        return nullptr;
    }
    const Token &kw = peek();
    if (!expect(TokenKind::TK_IDENT, "switch")) {
        return {};
    }
    if (!expect(TokenKind::TK_LPAREN, "switch (")) {
        return {};
    }
    ExprPtr value = parse_expression();
    if (recover_failed_) {
        return {};
    }
    if (!expect(TokenKind::TK_RPAREN, "switch )")) {
        return {};
    }
    if (!expect(TokenKind::TK_LBRACE, "switch { ")) {
        return {};
    }

    auto sw = std::make_unique<nari::SwitchStmt>(std::move(value));
    sw->line = kw.line;
    sw->col = kw.col;
    sw->filename = kw.filename.empty() ? current_filename : kw.filename;

    bool seen_default = false;
    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
        if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected 'case' or 'default' in switch");
            return {};
        }

        if (peek().text == "case") {
            next(); // consume 'case'
            ExprPtr match = parse_expression();
            if (recover_failed_) {
                return {};
            }
            if (!expect(TokenKind::TK_COLON, "case :")) {
                return {};
            }
            auto body = std::make_unique<nari::BlockStmt>();
            while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
                if (peek().kind == TokenKind::TK_IDENT && (peek().text == "case" || peek().text == "default")) {
                    break;
                }
                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                    continue;
                }
                StmtPtr st = parse_stmt();
                if (recover_failed_) {
                    return {};
                }
                if (st) {
                    body->stmts.push_back(std::move(st));
                } else {
                    break;
                }
            }
            sw->cases.emplace_back(std::move(match), std::move(body));
            continue;
        }

        if (peek().text == "default") {
            if (seen_default) {
                error_and_exit("Only one default case is allowed");
                return {};
            }
            seen_default = true;
            next(); // consume 'default'
            if (!expect(TokenKind::TK_COLON, "default :")) {
                return {};
            }
            auto body = std::make_unique<nari::BlockStmt>();
            while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
                if (peek().kind == TokenKind::TK_IDENT && (peek().text == "case" || peek().text == "default")) {
                    break;
                }
                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                    continue;
                }
                StmtPtr st = parse_stmt();
                if (recover_failed_) {
                    return {};
                }
                if (st) {
                    body->stmts.push_back(std::move(st));
                } else {
                    break;
                }
            }
            sw->default_body = std::move(body);
            continue;
        }

        error_and_exit("Expected 'case' or 'default' in switch");
        return {};
    }

    if (!expect(TokenKind::TK_RBRACE, "switch }")) {
        return {};
    }
    return sw;
}

} // namespace Parser
