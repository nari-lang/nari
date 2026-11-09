/*
  parse_core.cpp
  Parser class methods split verbatim from parser.cpp.
*/

#include "parser/parser.h"

namespace Parser {

std::vector<FunctionPtr> Parser::parse_program(bool create_aggregator) {
    std::vector<FunctionPtr> functions;
    const std::string module_name = current_module_key();

    // Detect "use strict" directive: a bare string literal as the very first
    // token sequence before any real statement.
    // Matches both: "use strict"  and  "use strict";
    if (peek().kind == TokenKind::TK_STRING && peek().text == "use strict") {
        next(); // consume the string token
        if (peek().kind == TokenKind::TK_SEMICOLON) {
            next();
        }
        this->strict_mode = true;
    }

    // collect top-level statements into synthetic __top_level__ function
    auto top_block = std::make_unique<nari::BlockStmt>();

    auto is_probable_ffi_library_path = [](const std::string &path) {
        namespace fs = nari::fs;

        const std::string ext = fs::Path(path).extension().string();
        if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
            return true;
        }

        // Versioned sonames ("libc.so.6", "libfoo.so.1.2.3") have a numeric
        // extension, so match ".so." followed by digits/dots in the filename.
        // These are resolved by the dynamic linker's search path, so the file
        // need not exist relative to the CWD.
        const std::string filename = fs::Path(path).filename().string();
        const size_t so_pos = filename.find(".so.");
        if (so_pos != std::string::npos && so_pos + 4 < filename.size()) {
            bool version_suffix = true;
            for (size_t i = so_pos + 4; i < filename.size(); ++i) {
                const char c = filename[i];
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') {
                    version_suffix = false;
                    break;
                }
            }
            if (version_suffix) {
                return true;
            }
        }

#if defined(__linux__)
        // if it's an file with a weird ending, check if it's an ELF file
        // only linux adds stuff to the end of library extensions i think, so this should be fine?
        FILE *fp = fopen(path.c_str(), "rb");
        if (!fp)
            return false;

        Elf64_Ehdr hdr{};
        const size_t n = fread(&hdr, 1, sizeof(hdr), fp);
        fclose(fp);

        if (n < EI_NIDENT)
            return false;

        // ELF magic
        if (hdr.e_ident[EI_MAG0] != ELFMAG0 || hdr.e_ident[EI_MAG1] != ELFMAG1 || hdr.e_ident[EI_MAG2] != ELFMAG2 ||
            hdr.e_ident[EI_MAG3] != ELFMAG3)
            return false;

        return hdr.e_type == ET_DYN;
#else
        return false;
#endif
    };
    auto register_module_export = [&](const std::string &export_name, const std::string &local_name) {
        auto &bindings = module_export_registry[module_name];
        for (const auto &binding : bindings) {
            if (binding.export_name == export_name) {
                error_and_exit("Duplicate export name '" + export_name + "'");
                return;
            }
        }
        bindings.push_back(ModuleExportBinding{ export_name, local_name });
    };

    auto ensure_exported_function_alias = [&](const std::string &local_name) {
        auto &aliases = module_function_alias_registry[module_name];
        if (aliases.find(local_name) != aliases.end()) {
            return;
        }

        for (auto it = functions.rbegin(); it != functions.rend(); ++it) {
            if (!*it) {
                continue;
            }
            if ((*it)->filename != module_name) {
                continue;
            }
            if ((*it)->name != local_name) {
                continue;
            }

            std::string internal_name = "__module_export_" + std::to_string(g_module_export_counter++) + "__" + local_name;
            aliases[local_name] = internal_name;
            (*it)->name = internal_name;
            return;
        }
    };

    auto append_named_import_binding = [&](const std::string &local_name, const std::string &namespace_global_name,
                                           const std::string &export_name, const Token &src_tok) {
        auto member_expr = std::make_unique<nari::MemberExpr>(std::make_unique<nari::IdentExpr>(namespace_global_name), export_name);
        member_expr->line = src_tok.line;
        member_expr->col = src_tok.col;
        member_expr->filename = src_tok.filename.empty() ? current_filename : src_tok.filename;
        auto var_decl = std::make_unique<nari::VarDeclStmt>(local_name, std::move(member_expr), VarDeclCtrl::LOCAL);
        var_decl->line = src_tok.line;
        var_decl->col = src_tok.col;
        var_decl->filename = src_tok.filename.empty() ? current_filename : src_tok.filename;
        top_block->stmts.push_back(std::move(var_decl));
    };

    auto append_namespace_import_binding = [&](const std::string &local_name, const std::string &namespace_global_name,
                                               const Token &src_tok) {
        auto ident_expr = std::make_unique<nari::IdentExpr>(namespace_global_name);
        ident_expr->line = src_tok.line;
        ident_expr->col = src_tok.col;
        ident_expr->filename = src_tok.filename.empty() ? current_filename : src_tok.filename;
        auto var_decl = std::make_unique<nari::VarDeclStmt>(local_name, std::move(ident_expr), VarDeclCtrl::LOCAL);
        var_decl->line = src_tok.line;
        var_decl->col = src_tok.col;
        var_decl->filename = src_tok.filename.empty() ? current_filename : src_tok.filename;
        top_block->stmts.push_back(std::move(var_decl));
    };

    auto include_source_import = [&](const Token &itok, const std::string &inc_name) -> std::string {
        std::string base_for_resolve = !itok.filename.empty() ? itok.filename : current_filename;
        std::string inc_path = resolve_include_path(inc_name, base_for_resolve);
        if (inc_path.empty() && !g_last_import_resolution_error.empty()) {
            error_and_exit(g_last_import_resolution_error);
            return {};
        }

        if (!is_std_virtual_path(inc_path) && !nari::fs::is_regular_file(nari::fs::Path(inc_path))) {
            error_and_exit("Import path is not a regular file: " + inc_path);
            return {};
        }

        bool is_naric_file = (inc_name.size() >= 6 && inc_name.substr(inc_name.size() - 6) == ".naric");
        if (is_naric_file) {
            if (g_visited_files.find(inc_path) == g_visited_files.end()) {
                g_visited_files.insert(inc_path);
                std::string init_func = "__init_naric_import_" + std::to_string(g_naric_import_counter++) + "__";
                g_pending_naric_imports.push_back({ init_func, inc_path });
                auto call_expr = std::make_unique<nari::CallExpr>(std::make_unique<nari::IdentExpr>(init_func));
                top_block->stmts.push_back(std::make_unique<nari::ExprStmt>(std::move(call_expr)));
            }
            return inc_path;
        }

        if (g_visited_files.find(inc_path) == g_visited_files.end()) {
            g_visited_files.insert(inc_path);

            std::string context = (current_filename.empty() ? std::string("<input>") : current_filename) + ":" + std::to_string(itok.line) +
                                  ":" + std::to_string(itok.col);
            push_error_context(context);

            std::string included_src;
            if (is_std_virtual_path(inc_path)) {
                // Source comes from the embedded std/* module table; no fopen.
                std::string name = inc_path.substr(6); // strip "<std>/"
                included_src = ::nari_std_module_source(name);
                if (included_src.empty()) {
                    std::string err = "Stdlib module 'std/" + name + "' has empty source";
                    error_and_exit(err);
                    return {};
                }
            } else {
                FILE *fp = fopen(inc_path.c_str(), "r");
                if (!fp) {
                    std::string err = "Failed to open import file " + inc_path;
                    error_and_exit(err);
                    return {};
                }

                if (fseek(fp, 0, SEEK_END) != 0) {
                    fclose(fp);
                    error_and_exit("Failed to seek import file " + inc_path);
                    return {};
                }
                long file_size = ftell(fp);
                if (file_size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
                    fclose(fp);
                    error_and_exit("Failed to determine import file size " + inc_path);
                    return {};
                }

                if (file_size > 0) {
                    included_src.resize(static_cast<size_t>(file_size));
                    size_t bytes_read = fread(&included_src[0], 1, static_cast<size_t>(file_size), fp);
                    included_src.resize(bytes_read);
                }
                fclose(fp);
            }

            std::string prev_file = current_filename;
            set_source_filename(inc_path);

            if (recover_mode) {
                auto included = parse_program_recovering(included_src, false);
                for (auto &error : included.errors) {
                    errors_.push_back(std::move(error));
                }
                for (auto &f : included.functions) {
                    functions.push_back(std::move(f));
                }
            } else {
                auto more_funcs = parse_program_from_source(included_src, false);
                for (auto &f : more_funcs) {
                    functions.push_back(std::move(f));
                }
            }

            set_source_filename(prev_file);
            pop_error_context();
        }

        return inc_path;
    };

    // Each top-level item is parsed inside a lambda so the recovery try/catch can wrap it without duplicating code
    // true = keep going, false = terminate
    auto parse_one = [&]() -> bool {
        const Token &tok = peek();

        if (tok.kind == TokenKind::TK_SEMICOLON) {
            next();
            return true;
        }

        // aggregate declaration: type Name<T> { ... } or union Name { ... }
        if (tok.kind == TokenKind::TK_IDENT &&
            ((tok.text == "type" && looks_like_type_decl()) || (tok.text == "union" && looks_like_union_decl()))) {
            auto type_decl = parse_type_decl();
            if (recover_failed_) {
                return false;
            }
            register_type(std::move(type_decl));
            return true;
        }

        // enum declaration: enum Name<T> { Variant1, Variant2(T), ... }
        if (tok.kind == TokenKind::TK_IDENT && tok.text == "enum" && looks_like_enum_decl()) {
            auto enum_decl = parse_enum_decl();
            if (recover_failed_) {
                return false;
            }
            auto ctors = generate_enum_constructors(*enum_decl);
            register_enum(std::move(enum_decl));
            for (auto &ctor : ctors) {
                functions.push_back(std::move(ctor));
            }
            return true;
        }

        // class declaration: class Name<T> { fields... methods... }
        if (tok.kind == TokenKind::TK_IDENT && tok.text == "class" && looks_like_class_decl()) {
            auto class_decl = parse_class_decl();
            if (recover_failed_) {
                return false;
            }
            register_class(std::move(class_decl));
            return true;
        }

        // function declaration: func foo(args...) { ... }
        if (tok.kind == TokenKind::TK_IDENT && tok.text == "func" && looks_like_func_keyword_decl()) {
            auto fn = parse_function();
            if (recover_failed_) {
                return false;
            }
            functions.push_back(std::move(fn));
            return true;
        }

        if (tok.kind == TokenKind::TK_IDENT && tok.text == "export") {
            next();

            if (peek().kind == TokenKind::TK_IDENT && peek().text == "func" && looks_like_func_keyword_decl()) {
                auto fn = parse_function();
                if (recover_failed_) {
                    return false;
                }
                register_module_export(fn->name, fn->name);
                functions.push_back(std::move(fn));
                return true;
            }

            if (peek().kind == TokenKind::TK_LBRACE) {
                next(); // consume '{'

                while (peek().kind != TokenKind::TK_RBRACE && !is_eof()) {
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected identifier in export list");
                        return false;
                    }

                    std::string local_name = next().text;
                    std::string export_name = local_name;

                    if (peek().kind == TokenKind::TK_IDENT && peek().text == "as") {
                        next();
                        if (peek().kind != TokenKind::TK_IDENT) {
                            error_and_exit("Expected exported name after 'as'");
                            return false;
                        }
                        export_name = next().text;
                    }

                    register_module_export(export_name, local_name);

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next();
                    } else if (peek().kind != TokenKind::TK_RBRACE) {
                        error_and_exit("Expected ',' or '}' in export list");
                        return false;
                    }
                }

                if (!expect(TokenKind::TK_RBRACE, "export list end '}'")) {
                    return false;
                }
                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }
                return true;
            }

            if (peek().kind == TokenKind::TK_IDENT && (peek().text == "let" || peek().text == "global" || peek().text == "const")) {
                auto stmt = parse_stmt();
                if (recover_failed_) {
                    return false;
                }
                auto *var_decl =
                    ((stmt.get()) != nullptr && (stmt.get())->stmt_kind == nari::StmtKind::VarDecl
                         ? static_cast<nari::VarDeclStmt *>(stmt.get())
                         : nullptr);
                if (!var_decl) {
                    error_and_exit("Expected variable declaration after 'export'");
                    return false;
                }

                if (var_decl->destructure_kind == nari::DestructureKind::None) {
                    register_module_export(var_decl->name, var_decl->name);
                } else if (var_decl->destructure_kind == nari::DestructureKind::Array) {
                    for (const auto &name : var_decl->array_names) {
                        register_module_export(name, name);
                    }
                } else if (var_decl->destructure_kind == nari::DestructureKind::Object) {
                    for (const auto &[_, name] : var_decl->object_bindings) {
                        register_module_export(name, name);
                    }
                }

                top_block->stmts.push_back(std::move(stmt));
                return true;
            }

            error_and_exit(
                "Top-level export currently supports 'export func ...', 'export let/global/const ...', "
                "or 'export { ... }'"
            );
            return false;
        }

        // import "file.nari" OR import name from "library.so"
        if (tok.kind == TokenKind::TK_IDENT && tok.text == "import") {
            next();

            if (peek().kind == TokenKind::TK_LBRACE) {
                next(); // consume '{'
                std::vector<std::pair<std::string, std::string>> named_imports;

                while (peek().kind != TokenKind::TK_RBRACE && !is_eof()) {
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected identifier in import list");
                        return false;
                    }

                    std::string export_name = next().text;
                    std::string local_name = export_name;
                    if (peek().kind == TokenKind::TK_IDENT && peek().text == "as") {
                        next();
                        if (peek().kind != TokenKind::TK_IDENT) {
                            error_and_exit("Expected local alias after 'as'");
                            return false;
                        }
                        local_name = next().text;
                    }

                    named_imports.push_back({ export_name, local_name });

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next();
                    } else if (peek().kind != TokenKind::TK_RBRACE) {
                        error_and_exit("Expected ',' or '}' in import list");
                        return false;
                    }
                }

                if (!expect(TokenKind::TK_RBRACE, "import list end '}'")) {
                    return false;
                }
                if (peek().kind != TokenKind::TK_IDENT || peek().text != "from") {
                    error_and_exit("Expected 'from' after import list");
                    return false;
                }
                next();
                if (peek().kind != TokenKind::TK_STRING) {
                    error_and_exit("Expected module path string after 'from'");
                    return false;
                }

                Token path_tok = next();
                std::string inc_name = path_tok.text;
                if (inc_name.size() >= 6 && inc_name.substr(inc_name.size() - 6) == ".naric") {
                    error_and_exit("Named imports from .naric modules are not supported yet");
                    return false;
                }

                std::string inc_path = include_source_import(path_tok, inc_name);
                std::string namespace_global = get_module_namespace_global_name(inc_path);
                if (namespace_global.empty()) {
                    error_and_exit("Module '" + inc_name + "' does not define explicit exports");
                    return false;
                }

                // Validate that every requested name is actually exported
                const auto &module_exports = get_module_exports(inc_path);
                for (const auto &[export_name, local_name] : named_imports) {
                    bool found = false;
                    for (const auto &binding : module_exports) {
                        if (binding.export_name == export_name) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        error_and_exit("Module '" + inc_name + "' does not export '" + export_name + "'");
                        return false;
                    }
                }

                for (const auto &[export_name, local_name] : named_imports) {
                    append_named_import_binding(local_name, namespace_global, export_name, path_tok);
                }

                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }
                return true;
            }

            // check if this is "import name from"  syntax
            if (peek().kind == TokenKind::TK_IDENT) {
                std::string var_name = next().text;

                if (peek().kind != TokenKind::TK_IDENT || peek().text != "from") {
                    error_and_exit("Expected 'from' after import variable name");
                    return false;
                }
                next(); // consume "from"

                if (peek().kind != TokenKind::TK_STRING) {
                    error_and_exit("Expected library path string after 'from'");
                    return false;
                }
                Token path_tok = next();
                std::string lib_path = path_tok.text;

                if (!is_probable_ffi_library_path(lib_path)) {
                    if (lib_path.size() >= 6 && lib_path.substr(lib_path.size() - 6) == ".naric") {
                        error_and_exit("Namespace imports from .naric modules are not supported yet");
                        return false;
                    }

                    std::string inc_path = include_source_import(path_tok, lib_path);
                    std::string namespace_global = get_module_namespace_global_name(inc_path);
                    if (namespace_global.empty()) {
                        error_and_exit("Module '" + lib_path + "' does not define explicit exports");
                        return false;
                    }
                    append_namespace_import_binding(var_name, namespace_global, path_tok);

                    if (peek().kind == TokenKind::TK_SEMICOLON) {
                        next();
                    }
                    return true;
                }

                // transform into `let var_name = __ffi_load_library("path");`
                auto load_call = std::make_unique<nari::CallExpr>(std::make_unique<nari::IdentExpr>("__ffi_load_library"));
                load_call->args.push_back(std::make_unique<nari::StringExpr>(lib_path));

                auto var_decl = std::make_unique<nari::VarDeclStmt>(var_name, std::move(load_call), VarDeclCtrl::LOCAL);

                top_block->stmts.push_back(std::move(var_decl));

                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }
                return true;
            }

            const Token &itok = peek(); // import path token (renamed to avoid shadowing)
            if (itok.kind != TokenKind::TK_STRING) {
                error_and_exit("import requires a string filename or 'name from \"path\"' syntax");
                return false;
            }
            std::string inc_name = next().text;
            include_source_import(itok, inc_name);

            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
            return true;
        }

        if (tok.kind == TokenKind::TK_EOF) {
            return false;
        }

        // for anything else, parse statement and store it in the synthetic top-level block
        auto stmt = parse_stmt();
        if (recover_failed_) {
            return false;
        }
        if (stmt) {
            top_block->stmts.push_back(std::move(stmt));
            return true;
        }
        // unrecognised / null statement
        return false;
    };

    while (!is_eof()) {
        if (this->recover_mode) {
            if (!parse_one() || recover_failed_) {
                if (!recover_failed_) {
                    break;
                }
                size_t before = idx;
                synchronize();
                // if synchronize() didn't advance, force past the current token so the loop always makes progress.
                if (idx == before && !is_eof()) {
                    next();
                }
                recover_failed_ = false;
            }
        } else {
            if (!parse_one()) {
                break;
            }
        }
    }

    const auto &exports = get_module_exports(module_name);
    for (const auto &binding : exports) {
        ensure_exported_function_alias(binding.local_name);
    }

    if (!exports.empty()) {
        std::string namespace_global = get_module_namespace_global_name(module_name);
        if (namespace_global.empty()) {
            namespace_global = "__module_namespace_" + std::to_string(g_module_namespace_counter++) + "__";
            module_namespace_registry[module_name] = namespace_global;
        }

        auto obj_expr = std::make_unique<nari::ObjectLiteralExpr>();
        obj_expr->filename = module_name;

        for (const auto &binding : exports) {
            std::string internal_name = get_module_function_internal_name(module_name, binding.local_name);
            auto value_expr = std::make_unique<nari::IdentExpr>(internal_name.empty() ? binding.local_name : internal_name);
            value_expr->filename = module_name;
            obj_expr->entries.push_back({ binding.export_name, std::move(value_expr) });
        }

        auto namespace_decl = std::make_unique<nari::VarDeclStmt>(namespace_global, std::move(obj_expr), VarDeclCtrl::GLOBAL);
        namespace_decl->filename = module_name;
        top_block->stmts.push_back(std::move(namespace_decl));
    }

    // if we collected any top-level statements, synthesize a __top_level__ function
    if (!top_block->stmts.empty()) {
        prune_dead_code(top_block.get());
        optimize_block(top_block.get());
        propagate_consts(top_block.get());
        std::string modname = current_filename.empty() ? std::string("<input>") : current_filename;
        std::string per_module_name = std::string("__top_level__@") + modname;

        auto top_fn = std::make_unique<nari::Function>();
        top_fn->name = per_module_name;
        top_fn->strict_mode = this->strict_mode;

        // attach location info if possible
        if (!top_block->stmts.empty()) {
            const auto &first = top_block->stmts.front();
            if (first) {
                top_fn->line = first->line;
                top_fn->col = first->col;
                top_fn->filename = first->filename.empty() ? modname : first->filename;
            }
        }
        top_fn->body = std::move(top_block);
        functions.push_back(std::move(top_fn));

        // build an aggregator __top_level__ function only if requested
        if (create_aggregator) {
            // build an aggregator __top_level__ function that will call all
            // per-module top functions discovered in this module's function list.
            auto agg_fn = std::make_unique<nari::Function>();
            agg_fn->name = std::string("__top_level__");
            auto aggregate_block = std::make_unique<nari::BlockStmt>();

            for (const auto &func_ptr : functions) {
                if (!func_ptr) {
                    continue;
                }
                const std::string &name = func_ptr->name;
                const std::string prefix = "__top_level__@";
                if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                    auto ident = std::make_unique<nari::IdentExpr>(name);
                    ident->line = func_ptr->line;
                    ident->col = func_ptr->col;
                    ident->filename = func_ptr->filename;

                    auto call_expr = std::make_unique<nari::CallExpr>(std::move(ident));
                    // attach position info
                    call_expr->line = func_ptr->line;
                    call_expr->col = func_ptr->col;
                    call_expr->filename = func_ptr->filename;
                    // snapshot metadata before moving the call so that we don't dereference a moved-from
                    // unique_ptr.
                    int c_line = call_expr->line;
                    int c_col = call_expr->col;
                    std::string c_filename = call_expr->filename;
                    auto call_stmt = std::make_unique<nari::ExprStmt>(std::move(call_expr));
                    call_stmt->line = c_line;
                    call_stmt->col = c_col;
                    call_stmt->filename = c_filename;
                    aggregate_block->stmts.push_back(std::move(call_stmt));
                }
            }

            // attach aggregator metadata and insert it at the front so callers that inspect function lists see the
            // aggregator early.
            agg_fn->body = std::move(aggregate_block);
            agg_fn->line = 0;
            agg_fn->col = 0;
            agg_fn->filename = modname;
            functions.insert(functions.begin(), std::move(agg_fn));
        }
    }

    for (auto &fn : functions) {
        if (fn && fn->body) {
            prune_dead_code(fn->body.get());
            optimize_block(fn->body.get());
            propagate_consts(fn->body.get());
        }
    }
    return functions;
}

// error recovery
// advance past tokens until a likely statement boundary so that the
// top-level loop can attempt to parse the next construct cleanly.
void Parser::synchronize() {
    static const std::unordered_set<std::string> boundaryKeywords = { "func",    "let",    "const",    "if",    "while",  "for",
                                                                      "foreach", "return", "continue", "break", "import", "export",
                                                                      "type",    "enum",   "class",    "match" };
    while (!is_eof()) {
        TokenKind kind = peek().kind;

        if (kind == TokenKind::TK_SEMICOLON) {
            next();
            return;
        }
        if (kind == TokenKind::TK_RBRACE) {
            return;
        }
        if (kind == TokenKind::TK_IDENT && boundaryKeywords.count(peek().text)) {
            return;
        }

        next();
    }
}

bool Parser::is_eof() const {
    return idx >= toks.size() || toks[idx].kind == TokenKind::TK_EOF;
}

const Token &Parser::peek(size_t n) const {
    size_t i = idx + n;
    if (i >= toks.size()) {
        return toks.back();
    }
    return toks[i];
}

const Token &Parser::next() {
    if (idx < toks.size()) {
        return toks[idx++];
    }
    return toks.back();
}

bool Parser::match(TokenKind k) {
    if (recover_failed_) {
        return false;
    }
    if (!is_eof() && peek().kind == k) {
        next();
        return true;
    }
    return false;
}

void Parser::error_and_exit(const std::string &msg) {
    const Token *tok = nullptr;
    if (!is_eof()) {
        tok = &peek();
    }

    std::string use_fname = current_filename.empty() ? std::string("<input>") : current_filename;
    int line = 0;
    int col = 0;

    if (tok) {
        if (!tok->filename.empty()) {
            use_fname = tok->filename;
        }
        line = tok->line;
        col = tok->col;
    }

    // Recovery mode: record the diagnostic and bail out. Callers must
    // check recover_failed_ (or a null/false result) and return their
    // failure value promptly so the per-iteration loop can synchronize.
    // Only the first error per statement is recorded.
    if (this->recover_mode) {
        if (!recover_failed_) {
            errors_.push_back({ use_fname, line, col, msg });
        }
        recover_failed_ = true;
        return;
    }

    fprintf(stderr, "Exception at %s:%d:%d\n", use_fname.c_str(), line, col);
    fprintf(stderr, "  %s %s:%d:%d\n", msg.c_str(), use_fname.c_str(), line, col);

    auto print_source_context = [&](const std::string &file, int lno, int cno) {
#ifndef NARI_ESP_IDF
        if (file.empty() || file[0] == '<' || lno <= 0) {
            return;
        }
        FILE *fp = fopen(file.c_str(), "r");
        if (!fp) {
            return;
        }

        char *line_buffer = nullptr;
        size_t buffer_size = 0;
        std::string line_text;

        for (int i = 1; i <= lno; ++i) {
            ssize_t line_length = getline(&line_buffer, &buffer_size, fp);
            if (line_length == -1) {
                free(line_buffer);
                fclose(fp);
                return;
            }
            if (i == lno) {
                line_text = line_buffer;
                // remove trailing newline if present
                if (!line_text.empty() && line_text.back() == '\n') {
                    line_text.pop_back();
                }
            }
        }
        free(line_buffer);
        fclose(fp);

        fprintf(stderr, "  %s\n", line_text.c_str());
        int caret_pos = cno > 0 ? cno - 1 : 0;
        std::string caret_line(caret_pos, ' ');
        caret_line.push_back('^');
        fprintf(stderr, "  %s\n", caret_line.c_str());
#else
        // ESP-IDF does not support getline, so we just print the filename and line number.
        fprintf(stderr, "  (source context not available on this platform!)\n");
#endif
    };

    print_source_context(use_fname, line, col);

    // print chain if we have more than one error context
    if (!error_contexts.empty()) {
        for (int i = error_contexts.size() - 1; i >= 0; --i) {
            fprintf(stderr, "  included from %s:%d:%d\n", error_contexts[i].c_str(), line, col);
        }
    }

    std::exit(1);
}

bool Parser::expect(TokenKind k, const char *msg) {
    if (recover_failed_) {
        return false;
    }
    if (!is_eof() && peek().kind == k) {
        next();
        return true;
    }
    std::string error_msg = "Parse error";
    if (!is_eof()) {
        error_msg += " token='" + token_desc(peek()) + "'";
    }
    if (msg) {
        error_msg += " (" + std::string(msg) + ")";
    }
    error_and_exit(error_msg);
    return false;
    return !recover_failed_;
}

} // namespace Parser
