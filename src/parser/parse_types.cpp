/*
  parse_types.cpp
  Parser class methods split verbatim from parser.cpp.
*/

#include "parser/parser.h"

namespace Parser {

bool Parser::looks_like_func_keyword_decl() {
    // func IDENT '(' ... ')' [-> type] '{'
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "func") {
        return false;
    }
    if (peek(1).kind != TokenKind::TK_IDENT) {
        return false;
    }
    if (peek(2).kind != TokenKind::TK_LPAREN) {
        return false;
    }
    // find matching RPAREN
    size_t i = idx + 3;
    int depth = 1;
    while (i < toks.size()) {
        if (toks[i].kind == TokenKind::TK_LPAREN) {
            ++depth;
        } else if (toks[i].kind == TokenKind::TK_RPAREN) {
            --depth;
            if (depth == 0) {
                break;
            }
        } else if (toks[i].kind == TokenKind::TK_EOF) {
            break;
        }
        ++i;
    }
    // after ')' we might have '-> type' or '-> type[]' before '{'
    size_t next_idx = i + 1;
    if (next_idx < toks.size() && toks[next_idx].kind == TokenKind::TK_ARROW) {
        // skip '-> type'
        next_idx++; // skip '->'
        if (next_idx < toks.size() && toks[next_idx].kind == TokenKind::TK_IDENT) {
            next_idx++; // skip type name
            // check for array syntax: type[]
            if (next_idx < toks.size() && toks[next_idx].kind == TokenKind::TK_LBRACKET && next_idx + 1 < toks.size() &&
                toks[next_idx + 1].kind == TokenKind::TK_RBRACKET) {
                next_idx += 2; // skip '[]'
            }
        }
    }
    if (next_idx < toks.size() && toks[next_idx].kind == TokenKind::TK_LBRACE) {
        return true;
    }
    return false;
}

bool Parser::looks_like_foreach_loop() {
    // for ( IDENT in ... )
    // for ( IDENT , IDENT in ... )
    // for ( let IDENT of ... )
    // for ( let IDENT , IDENT of ... )
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "for") {
        return false;
    }
    if (peek(1).kind != TokenKind::TK_LPAREN) {
        return false;
    }

    // determine offset of the first variable name token
    bool has_let = (peek(2).kind == TokenKind::TK_IDENT && peek(2).text == "let");
    size_t v = has_let ? 3 : 2; // idx of first variable name

    if (peek(v).kind != TokenKind::TK_IDENT) {
        return false;
    }

    // single-var: for ( [let] IDENT in|of ... )
    if (peek(v + 1).kind == TokenKind::TK_IDENT && (peek(v + 1).text == "in" || peek(v + 1).text == "of")) {
        return true;
    }
    // two-var: for ( [let] IDENT , IDENT in|of ... )
    if (peek(v + 1).kind == TokenKind::TK_COMMA && peek(v + 2).kind == TokenKind::TK_IDENT && peek(v + 3).kind == TokenKind::TK_IDENT &&
        (peek(v + 3).text == "in" || peek(v + 3).text == "of")) {
        return true;
    }
    return false;
}

bool Parser::looks_like_type_decl() {
    // type IDENT { or type IDENT< (generics) or type IDENT IDENT (alias)
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "type") {
        return false;
    }
    if (peek(1).kind != TokenKind::TK_IDENT) {
        return false;
    }
    // Could be type Name { or type Name<T> { or type Alias BaseType
    if (peek(2).kind == TokenKind::TK_LBRACE || peek(2).kind == TokenKind::TK_LT || peek(2).kind == TokenKind::TK_IDENT) {
        return true;
    }
    return false;
}

bool Parser::looks_like_union_decl() {
    return peek().kind == TokenKind::TK_IDENT && peek().text == "union" && peek(1).kind == TokenKind::TK_IDENT &&
           peek(2).kind == TokenKind::TK_LBRACE;
}

bool Parser::looks_like_enum_decl() {
    // enum IDENT { or enum IDENT< (generics)
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "enum") {
        return false;
    }
    if (peek(1).kind != TokenKind::TK_IDENT) {
        return false;
    }
    if (peek(2).kind == TokenKind::TK_LBRACE || peek(2).kind == TokenKind::TK_LT) {
        return true;
    }
    return false;
}

bool Parser::looks_like_class_decl() {
    // class IDENT { or class IDENT< (generics) or class IDENT extends
    if (peek().kind != TokenKind::TK_IDENT || peek().text != "class") {
        return false;
    }
    if (peek(1).kind != TokenKind::TK_IDENT) {
        return false;
    }
    if (peek(2).kind == TokenKind::TK_LBRACE || peek(2).kind == TokenKind::TK_LT ||
        (peek(2).kind == TokenKind::TK_IDENT && peek(2).text == "extends")) {
        return true;
    }
    return false;
}

nari::TypeDeclPtr Parser::parse_type_decl() {
    if (recover_failed_) {
        return nullptr;
    }
    const bool is_union = peek().text == "union";
    next(); // consume 'type' or 'union'
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
        error_and_exit(is_union ? "Expected union name after 'union'" : "Expected type name after 'type'");
        return {};
    }
    auto type_decl = std::make_unique<nari::TypeDecl>(nameTok.text, is_union ? nari::TypeDeclKind::Union : nari::TypeDeclKind::Struct);
    type_decl->line = nameTok.line;
    type_decl->col = nameTok.col;
    type_decl->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;

    if (!is_union) {
        parse_generic_arguments(type_decl.get());
        if (recover_failed_) {
            return {};
        }
    }

    // Check if this is a type alias or a struct definition
    if (peek().kind == TokenKind::TK_LBRACE) {
        if (!expect(TokenKind::TK_LBRACE, "type body start '{'")) {
            return {};
        }

        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
            // Parse field: name: type or name: type[]
            if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected field name in type declaration");
                return {};
            }
            std::string field_name = next().text;

            if (!expect(TokenKind::TK_COLON, "':' after field name")) {
                return {};
            }

            auto field_type = parse_type_annotation();
            if (recover_failed_) {
                return {};
            }
            type_decl->fields.emplace_back(field_name, std::move(field_type));

            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
        }

        if (!expect(TokenKind::TK_RBRACE, "type body end '}'")) {
            return {};
        }
    } else if (!is_union && peek().kind == TokenKind::TK_IDENT) {
        // type alias: type NewName ExistingType
        type_decl->alias_target = parse_type_annotation();
        if (recover_failed_) {
            return {};
        }

        if (peek().kind == TokenKind::TK_SEMICOLON) {
            next();
        }
    } else {
        error_and_exit("Expected '{' for type definition or type name for alias");
        return {};
    }

    return type_decl;
}

// type annotation with optional generics
std::unique_ptr<nari::TypeAnnotation> Parser::parse_type_annotation() {
    if (recover_failed_) {
        return nullptr;
    }
    if (peek().kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected type name");
        return {};
    }
    std::string type_name = next().text;
    auto type_ann = std::make_unique<nari::TypeAnnotation>(type_name);

    parse_generic_arguments(type_ann.get());
    if (recover_failed_) {
        return {};
    }

    if (peek().kind == TokenKind::TK_LBRACKET) {
        next(); // consume '['
        if (peek().kind == TokenKind::TK_RBRACKET) {
            next(); // consume ']'
            type_ann->is_array = true;
        } else {
            if (peek().kind != TokenKind::TK_NUMBER) {
                error_and_exit("Expected a positive integer fixed array size");
                return {};
            }
            const std::string count_text = next().text;
            if (count_text.empty() || count_text.find_first_not_of("0123456789") != std::string::npos) {
                error_and_exit("Fixed array size must be a positive integer literal");
                return {};
            }
            char *end = nullptr;
            unsigned long long count = std::strtoull(count_text.c_str(), &end, 10);
            if (!end || *end != '\0' || count == 0 || count > nari::MAX_FIXED_ARRAY_COUNT) {
                error_and_exit("Fixed array size is out of range");
                return {};
            }
            if (!expect(TokenKind::TK_RBRACKET, "']' after fixed array size")) {
                return {};
            }
            type_ann->fixed_array_count = static_cast<size_t>(count);
        }
    }

    return type_ann;
}

nari::EnumDeclPtr Parser::parse_enum_decl() {
    if (recover_failed_) {
        return nullptr;
    }
    next(); // consume 'enum'
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected enum name after 'enum'");
        return {};
    }
    auto enum_decl = std::make_unique<nari::EnumDecl>(nameTok.text);
    enum_decl->line = nameTok.line;
    enum_decl->col = nameTok.col;
    enum_decl->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;

    // parse generic parameters
    parse_generic_arguments(enum_decl.get());
    if (recover_failed_) {
        return {};
    }

    if (!expect(TokenKind::TK_LBRACE, "enum body start '{'")) {
        return {};
    }

    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
        // Parse variant: Name or Name(T) or Name { field: T }
        if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected variant name in enum declaration");
            return {};
        }
        std::string variant_name = next().text;
        nari::EnumVariant variant(variant_name);

        // Check for tuple variant: Some(T)
        if (peek().kind == TokenKind::TK_LPAREN) {
            next(); // consume '('
            while (!is_eof() && peek().kind != TokenKind::TK_RPAREN) {
                auto field_type = parse_type_annotation();
                if (recover_failed_) {
                    return {};
                }
                variant.tuple_fields.push_back(std::move(field_type));

                if (peek().kind == TokenKind::TK_COMMA) {
                    next(); // consume ','
                } else if (peek().kind != TokenKind::TK_RPAREN) {
                    error_and_exit("Expected ',' or ')' in variant fields");
                    return {};
                }
            }
            if (!expect(TokenKind::TK_RPAREN, "')' after variant fields")) {
                return {};
            }
        }

        else if (peek().kind == TokenKind::TK_LBRACE) {
            next(); // consume '{'
            while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
                if (peek().kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected field name in variant");
                    return {};
                }
                std::string field_name = next().text;
                if (!expect(TokenKind::TK_COLON, "':' after field name")) {
                    return {};
                }

                auto field_type = parse_type_annotation();
                if (recover_failed_) {
                    return {};
                }
                variant.named_fields.emplace_back(field_name, std::move(field_type));

                if (peek().kind == TokenKind::TK_COMMA) {
                    next(); // consume ','
                } else if (peek().kind != TokenKind::TK_RBRACE) {
                    error_and_exit("Expected ',' or '}' in variant fields");
                    return {};
                }
            }
            if (!expect(TokenKind::TK_RBRACE, "'}' after variant fields")) {
                return {};
            }
        }

        enum_decl->variants.push_back(std::move(variant));

        // optional comma
        if (peek().kind == TokenKind::TK_COMMA) {
            next();
        }
    }

    if (!expect(TokenKind::TK_RBRACE, "enum body end '}'")) {
        return {};
    }
    return enum_decl;
}

nari::ClassDeclPtr Parser::parse_class_decl() {
    if (recover_failed_) {
        return nullptr;
    }
    next(); // consume 'class'
    const Token &nameTok = next();
    if (nameTok.kind != TokenKind::TK_IDENT) {
        error_and_exit("Expected class name after 'class'");
        return {};
    }
    auto class_decl = std::make_unique<nari::ClassDecl>(nameTok.text);
    class_decl->line = nameTok.line;
    class_decl->col = nameTok.col;
    class_decl->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;

    // parse generic parameters
    parse_generic_arguments(class_decl.get());
    if (recover_failed_) {
        return {};
    }

    // Parse optional extends clause
    if (peek().kind == TokenKind::TK_IDENT && peek().text == "extends") {
        next(); // consume 'extends'
        const Token &parentTok = peek();
        if (parentTok.kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected parent class name after 'extends'");
            return {};
        }
        class_decl->parent_name = next().text;
    }

    if (!expect(TokenKind::TK_LBRACE, "class body start '{'")) {
        return {};
    }

    // parse class body: fields and methods with visibility modifiers
    while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
        // parse visibility: public or private (default to public)
        nari::Visibility visibility = nari::Visibility::Public;

        if (peek().kind == TokenKind::TK_IDENT) {
            if (peek().text == "public") {
                next(); // consume 'public'
                visibility = nari::Visibility::Public;
            } else if (peek().text == "private") {
                next(); // consume 'private'
                visibility = nari::Visibility::Private;
            }
        }

        // parse optional 'static' modifier
        bool is_static = false;
        if (peek().kind == TokenKind::TK_IDENT && peek().text == "static") {
            next(); // consume 'static'
            is_static = true;
        }

        // now parse either field or method, check if it's a constructor
        if (peek().kind == TokenKind::TK_IDENT && peek().text == "init" && peek(1).kind == TokenKind::TK_LPAREN) {
            // constructor
            const Token &methodTok = next();
            std::string method_name = methodTok.text;
            nari::ClassMethod method(method_name, visibility);
            method.line = methodTok.line;
            method.col = methodTok.col;
            method.filename = methodTok.filename.empty() ? current_filename : methodTok.filename;
            method.is_constructor = true;

            // parse parameters
            if (!expect(TokenKind::TK_LPAREN, "'(' for constructor parameters")) {
                return {};
            }
            if (peek().kind != TokenKind::TK_RPAREN) {
                while (true) {
                    bool is_rest = false;
                    if (peek().kind == TokenKind::TK_ELLIPSIS) {
                        is_rest = true;
                        next();
                    }
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected parameter name");
                        return {};
                    }
                    std::string param_name = next().text;

                    std::unique_ptr<nari::TypeAnnotation> param_type = nullptr;
                    if (peek().kind == TokenKind::TK_COLON) {
                        next(); // consume ':'
                        param_type = parse_type_annotation();
                        if (recover_failed_) {
                            return {};
                        }
                    }

                    ExprPtr default_value = nullptr;
                    if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
                        next();
                        default_value = parse_expression();
                        if (recover_failed_) {
                            return {};
                        }
                    }
                    method.params.emplace_back(param_name, std::move(default_value), is_rest, std::move(param_type));

                    if (is_rest && peek().kind == TokenKind::TK_COMMA) {
                        error_and_exit("Rest parameter must be last");
                        return {};
                    } else if (peek().kind == TokenKind::TK_COMMA) {
                        next();
                    } else if (peek().kind == TokenKind::TK_RPAREN) {
                        break;
                    } else {
                        error_and_exit("Expected ',' or ')' in constructor parameters");
                        return {};
                    }
                }
            }
            if (!expect(TokenKind::TK_RPAREN, "')' after constructor parameters")) {
                return {};
            }

            // parse body
            method.body = parse_block();
            if (recover_failed_) {
                return {};
            }

            method.is_static = is_static;
            class_decl->methods.push_back(std::move(method));
            continue;
        }

        // check if it's a method: identifier followed by '('
        if (peek().kind == TokenKind::TK_IDENT && peek(1).kind == TokenKind::TK_LPAREN) {
            const Token &methodTok = next();
            std::string method_name = methodTok.text;
            nari::ClassMethod method(method_name, visibility);
            method.line = methodTok.line;
            method.col = methodTok.col;
            method.filename = methodTok.filename.empty() ? current_filename : methodTok.filename;

            // parse parameters
            if (!expect(TokenKind::TK_LPAREN, "'(' for method parameters")) {
                return {};
            }
            if (peek().kind != TokenKind::TK_RPAREN) {
                while (true) {
                    bool is_rest = false;
                    if (peek().kind == TokenKind::TK_ELLIPSIS) {
                        is_rest = true;
                        next();
                    }
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected parameter name");
                        return {};
                    }
                    std::string param_name = next().text;

                    std::unique_ptr<nari::TypeAnnotation> param_type = nullptr;
                    if (peek().kind == TokenKind::TK_COLON) {
                        next(); // consume ':'
                        param_type = parse_type_annotation();
                        if (recover_failed_) {
                            return {};
                        }
                    }

                    ExprPtr default_value = nullptr;
                    if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
                        next();
                        default_value = parse_expression();
                        if (recover_failed_) {
                            return {};
                        }
                    }
                    method.params.emplace_back(param_name, std::move(default_value), is_rest, std::move(param_type));

                    if (is_rest && peek().kind == TokenKind::TK_COMMA) {
                        error_and_exit("Rest parameter must be last");
                        return {};
                    } else if (peek().kind == TokenKind::TK_COMMA) {
                        next();
                    } else if (peek().kind == TokenKind::TK_RPAREN) {
                        break;
                    } else {
                        error_and_exit("Expected ',' or ')' in method parameters");
                        return {};
                    }
                }
            }
            if (!expect(TokenKind::TK_RPAREN, "')' after method parameters")) {
                return {};
            }

            // parse optional return type
            if (peek().kind == TokenKind::TK_ARROW) {
                next(); // consume '->'
                method.return_type = parse_type_annotation();
                if (recover_failed_) {
                    return {};
                }
            }

            method.body = parse_block();
            if (recover_failed_) {
                return {};
            }

            method.is_static = is_static;
            class_decl->methods.push_back(std::move(method));
            continue;
        }

        // otherwise it's a field: name: type or name: type = default
        if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected field or method name in class body");
            return {};
        }
        const Token &fieldTok = next();
        std::string field_name = fieldTok.text;

        if (!expect(TokenKind::TK_COLON, "':' after field name")) {
            return {};
        }

        auto field_type = parse_type_annotation();
        if (recover_failed_) {
            return {};
        }

        // Parse optional default value
        ExprPtr default_value = nullptr;
        if (peek().kind == TokenKind::TK_EQUAL) {
            next(); // consume '='
            default_value = parse_expression();
            if (recover_failed_) {
                return {};
            }
        }

        class_decl->fields.emplace_back(field_name, visibility, std::move(field_type), std::move(default_value));
        class_decl->fields.back().filename = fieldTok.filename.empty() ? current_filename : fieldTok.filename;
        class_decl->fields.back().line = fieldTok.line;
        class_decl->fields.back().col = fieldTok.col;
        class_decl->fields.back().is_static = is_static;

        if (peek().kind == TokenKind::TK_SEMICOLON) {
            next();
        }
    }

    if (!expect(TokenKind::TK_RBRACE, "class body end '}'")) {
        return {};
    }
    return class_decl;
}

} // namespace Parser
