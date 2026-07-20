/*
  parser.cpp
  Expression-precedence parser (precedence-climbing) that builds the AST for
  expressions.
  read: https://en.wikipedia.org/wiki/Operator-precedence_parser#Precedence_climbing_method
*/

#include "ast.h"
#include "int_overflow.h"
#include "nari_fs.h"
#include "parser/lexer.h"
#include "parser/module_resolver.h"
#include "parser/token.h"
#include "parser_api.h"

#include <cstdio>
#include <ctype.h>
#include <errno.h>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include "win_funcs.h"
#endif

#ifdef __linux__
#include <elf.h>
#endif

using nari::BlockPtr;
using nari::ExprPtr;
using nari::FunctionPtr;
using nari::StmtPtr;
using nari::VarDeclCtrl;

namespace Parser {

static std::string token_desc(const Token &tok) {
    if (tok.text.empty()) {
        return "<empty>";
    }
    return tok.text;
}

// every mutable global is thread_local so that two threads can run independent parse sessions at the same time
static thread_local std::string current_filename;
static thread_local std::vector<std::string> error_contexts;

static thread_local std::vector<nari::TypeDeclPtr> type_declarations;
static thread_local std::map<std::string, const nari::TypeDecl *> type_registry;

static thread_local std::vector<nari::EnumDeclPtr> enum_declarations;
static thread_local std::map<std::string, const nari::EnumDecl *> enum_registry;

static thread_local std::vector<nari::ClassDeclPtr> class_declarations;
static thread_local std::map<std::string, const nari::ClassDecl *> class_registry;
static thread_local ValuesList static_class_fields;
static thread_local std::unordered_set<std::string> static_inited_classes;
static thread_local std::map<std::string, std::vector<ModuleExportBinding>> module_export_registry;
static thread_local std::map<std::string, std::map<std::string, std::string>> module_function_alias_registry;
static thread_local std::map<std::string, std::string> module_namespace_registry;
static thread_local int g_module_export_counter = 0;
static thread_local int g_module_namespace_counter = 0;

void register_type(nari::TypeDeclPtr type_decl) {
    const std::string &name = type_decl->name;
    const nari::TypeDecl *ptr = type_decl.get();
    type_declarations.push_back(std::move(type_decl));
    type_registry[name] = ptr;
}

void register_enum(nari::EnumDeclPtr enum_decl) {
    const std::string &name = enum_decl->name;
    const nari::EnumDecl *ptr = enum_decl.get();
    enum_declarations.push_back(std::move(enum_decl));
    enum_registry[name] = ptr;
}

// generate synthetic constructor functions for each enum variant
static std::vector<nari::FunctionPtr> generate_enum_constructors(const nari::EnumDecl &decl) {
    std::vector<nari::FunctionPtr> result;
    for (const auto &variant : decl.variants) {
        auto fn = std::make_unique<nari::Function>(variant.name);
        fn->line = decl.line;
        fn->col = decl.col;

        // build the return object: { __variant: "VariantName", __enum: "EnumName", ... }
        auto obj = std::make_unique<nari::ObjectLiteralExpr>();
        obj->entries.push_back({ "__variant", std::make_unique<nari::StringExpr>(variant.name) });
        obj->entries.push_back({ "__enum", std::make_unique<nari::StringExpr>(decl.name) });

        if (variant.is_struct()) {
            // struct variant: params are the named fields, __data is an object
            for (const auto &[field_name, field_type] : variant.named_fields) {
                fn->params.push_back(nari::Param(field_name));
            }
            auto data_obj = std::make_unique<nari::ObjectLiteralExpr>();
            for (const auto &[field_name, field_type] : variant.named_fields) {
                data_obj->entries.push_back({ field_name, std::make_unique<nari::IdentExpr>(field_name) });
            }
            obj->entries.push_back({ "__data", std::move(data_obj) });
        } else if (variant.is_tuple()) {
            // Tuple variant: params named _0, _1, etc. or "value" for single-field
            if (variant.tuple_fields.size() == 1) {
                fn->params.push_back(nari::Param("value"));
                obj->entries.push_back({ "__data", std::make_unique<nari::IdentExpr>("value") });
            } else {
                auto data_arr = std::make_unique<nari::ArrayLiteralExpr>();
                for (size_t i = 0; i < variant.tuple_fields.size(); i++) {
                    std::string pname = "_" + std::to_string(i);
                    fn->params.push_back(nari::Param(pname));
                    data_arr->elements.push_back(std::make_unique<nari::IdentExpr>(pname));
                }
                obj->entries.push_back({ "__data", std::move(data_arr) });
            }
        }
        // unit variant: no params, no __data

        auto ret_stmt = std::make_unique<nari::ReturnStmt>(std::move(obj));
        auto body = std::make_unique<nari::BlockStmt>();
        body->stmts.push_back(std::move(ret_stmt));
        fn->body = std::move(body);
        result.push_back(std::move(fn));
    }
    return result;
}

void register_class(nari::ClassDeclPtr class_decl) {
    const std::string &name = class_decl->name;
    const nari::ClassDecl *ptr = class_decl.get();
    class_declarations.push_back(std::move(class_decl));
    class_registry[name] = ptr;
}

bool is_registered_type(const std::string &name) {
    return type_registry.find(name) != type_registry.end();
}

bool is_registered_enum(const std::string &name) {
    return enum_registry.find(name) != enum_registry.end();
}

bool is_registered_class(const std::string &name) {
    return class_registry.find(name) != class_registry.end();
}

const nari::TypeDecl *get_registered_type(const std::string &name) {
    auto it = type_registry.find(name);
    if (it != type_registry.end()) {
        return it->second;
    }
    return nullptr;
}

const nari::ClassDecl *get_registered_class(const std::string &name) {
    auto it = class_registry.find(name);
    if (it != class_registry.end()) {
        return it->second;
    }
    return nullptr;
}

const std::map<std::string, const nari::ClassDecl *> &get_all_registered_classes() {
    return class_registry;
}

ValuesList &get_static_fields() {
    return static_class_fields;
}

std::unordered_set<std::string> &get_static_inited_classes() {
    return static_inited_classes;
}

const std::map<std::string, const nari::TypeDecl *> &get_all_registered_types() {
    return type_registry;
}

void clear_type_registry() {
    type_registry.clear();
    type_declarations.clear();
    enum_registry.clear();
    enum_declarations.clear();
    class_registry.clear();
    class_declarations.clear();
    static_class_fields.clear();
    static_inited_classes.clear();
    module_export_registry.clear();
    module_function_alias_registry.clear();
    module_namespace_registry.clear();
    g_module_export_counter = 0;
    g_module_namespace_counter = 0;
}

void set_source_filename(const std::string &file) {
    current_filename = file;
}

static thread_local std::unordered_set<std::string> g_visited_files;

// pending pre-compiled .naric module imports collected during parse.
// each entry is { init_func_name, resolved_file_path }.
static thread_local std::vector<std::pair<std::string, std::string>> g_pending_naric_imports;
static thread_local int g_naric_import_counter = 0;
static thread_local std::string g_last_import_resolution_error;

static const std::vector<ModuleExportBinding> kEmptyModuleExports;

static std::string current_module_key() {
    return current_filename.empty() ? std::string("<input>") : current_filename;
}

// Forward decl from embedded_std_modules.cpp (global namespace; the parser
// implementation lives in `namespace Parser`).
} // namespace Parser
const std::string &nari_std_module_source(const std::string &name);
namespace Parser {

// Stdlib std/* virtual paths: anything imported as "std/<name>" maps to the
// virtual path "<std>/<name>". The include_source_import lambda recognizes
// this prefix and pulls the source from the embedded module table instead of
// touching the filesystem.
static constexpr const char *kStdVirtualPrefix = "<std>/";

static bool is_std_module_import(const std::string &inc) {
    return inc.size() > 4 && inc.compare(0, 4, "std/") == 0;
}

static std::string std_module_name_from_spec(const std::string &inc) {
    if (!is_std_module_import(inc)) {
        return "";
    }
    return inc.substr(4);
}

static std::string make_std_virtual_path(const std::string &name) {
    return std::string(kStdVirtualPrefix) + name;
}

static bool is_std_virtual_path(const std::string &path) {
    return path.size() > 6 && path.compare(0, 6, kStdVirtualPrefix) == 0;
}

static std::string resolve_include_path(const std::string &inc, const std::string &basefile) {
    namespace fs = nari::fs;
    g_last_import_resolution_error.clear();
    if (inc.empty()) {
        return inc;
    }

    if (is_std_module_import(inc)) {
        std::string name = std_module_name_from_spec(inc);
        const std::string &src = ::nari_std_module_source(name);
        if (src.empty()) {
            g_last_import_resolution_error =
                "Unknown stdlib module: 'std/" + name + "'";
            return "";
        }
        return make_std_virtual_path(name);
    }

    fs::Path incp(inc);
    if (incp.is_absolute()) {
        return incp.lexically_normal().string();
    }

    if (is_package_import_spec(inc)) {
        std::string package_path = resolve_package_import_path(inc, basefile, g_last_import_resolution_error);
        if (!package_path.empty()) {
            return package_path;
        }
        return "";
    }

    // get base path to resolve relative includes against
    fs::Path basepath;
    if (basefile.empty()) {
        basepath = fs::current_path();
    } else {
        fs::Path bf(basefile);
        if (bf.has_parent_path()) {
            basepath = bf.parent_path();
        } else {
            basepath = fs::current_path();
        }
    }

    fs::Path combined = basepath / incp;
    return combined.lexically_normal().string();
}

/*
  NOTE: these functions do not imply that an error *has* occured
  it just adds the context to the error stack for if one does happen.
*/
void push_error_context(const std::string &ctx) {
    error_contexts.push_back(ctx);
}

void pop_error_context() {
    if (!error_contexts.empty()) {
        error_contexts.pop_back();
    }
}

// prints filename:line:col header + message, and then exits.
// (declared in parser/lexer.h so the lexer can report lexical errors.)
void fatal_error(const std::string &msg, const Token *tok) {
    std::string fname =
        current_filename.empty() ? std::string("<input>") : current_filename;
    int l = 0, c = 0;
    if (tok) {
        if (!tok->filename.empty()) {
            fname = tok->filename;
        }
        l = tok->line;
        c = tok->col;
    }

    fprintf(stderr, "Exception at %s:%d:%d\n", fname.c_str(), l, c);
    fprintf(stderr, "  %s\n", msg.c_str());

    auto print_source_context = [&](const std::string &file, int line, int col) {
#ifndef NARI_ESP_IDF
        if (file.empty() || file[0] == '<' || line <= 0)
            return;
        FILE *fp = fopen(file.c_str(), "r");
        if (!fp)
            return;

        char *line_buffer = nullptr;
        size_t buffer_size = 0;
        std::string line_text;

        for (int i = 1; i <= line; ++i) {
            ssize_t line_length = getline(&line_buffer, &buffer_size, fp);
            if (line_length == -1) {
                free(line_buffer);
                fclose(fp);
                return;
            }
            if (i == line) {
                line_text = line_buffer;
                if (!line_text.empty() && line_text.back() == '\n') {
                    line_text.pop_back();
                }
            }
        }
        free(line_buffer);
        fclose(fp);

        fprintf(stderr, "  %s\n", line_text.c_str());
        int caret_pos = col > 0 ? col - 1 : 0;
        std::string caret_line(caret_pos, ' ');
        caret_line.push_back('^');
        fprintf(stderr, "  %s\n", caret_line.c_str());
#else
        fprintf(stderr, "  (source context not available on this platform!)\n");
#endif
    };

    print_source_context(fname, l, c);

    // prints out a trace of included files up to this point
    if (!error_contexts.empty()) {
        for (int i = static_cast<int>(error_contexts.size()) - 1; i >= 0; --i) {
            fprintf(stderr, "  included from %s\n", error_contexts[i].c_str());
        }
    }

    exit(1);
}

// Thrown internally when recover_mode_ is true and an error is recorded.
// Caught by the per-iteration try/catch in parse_program().
struct ParseRecoverSignal {};

class Parser {
  public:
    // Standard mode: first error calls std::exit(1).
    explicit Parser(const std::vector<Token> &tokens) : toks(tokens), idx(0), recover_mode(false) {
    }
    // Recovery mode: errors are collected; parsing resumes after each bad site.
    Parser(const std::vector<Token> &tokens, bool recover_mode) : toks(tokens), idx(0), recover_mode(recover_mode) {
    }

    // Returns accumulated diagnostics (valid only when recover_mode == true).
    std::vector<ParseError> take_errors() {
        return std::move(errors_);
    }

    std::vector<FunctionPtr> parse_program(bool create_aggregator = true) {
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
            if (hdr.e_ident[EI_MAG0] != ELFMAG0 ||
                hdr.e_ident[EI_MAG1] != ELFMAG1 ||
                hdr.e_ident[EI_MAG2] != ELFMAG2 ||
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

        auto append_named_import_binding =
            [&](const std::string &local_name, const std::string &namespace_global_name, const std::string &export_name, const Token &src_tok) {
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

        auto append_namespace_import_binding = [&](const std::string &local_name, const std::string &namespace_global_name, const Token &src_tok) {
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

                std::string context = (current_filename.empty() ? std::string("<input>") : current_filename) +
                                      ":" + std::to_string(itok.line) + ":" +
                                      std::to_string(itok.col);
                push_error_context(context);

                std::string included_src;
                if (is_std_virtual_path(inc_path)) {
                    // Source comes from the embedded std/* module table; no fopen.
                    std::string name = inc_path.substr(6); // strip "<std>/"
                    included_src = ::nari_std_module_source(name);
                    if (included_src.empty()) {
                        std::string err = "Stdlib module 'std/" + name + "' has empty source";
                        error_and_exit(err);
                    }
                } else {
                    FILE *fp = fopen(inc_path.c_str(), "r");
                    if (!fp) {
                        std::string err = "Failed to open import file " + inc_path;
                        error_and_exit(err);
                    }

                    fseek(fp, 0, SEEK_END);
                    long file_size = ftell(fp);
                    fseek(fp, 0, SEEK_SET);

                    if (file_size > 0) {
                        included_src.resize(file_size);
                        size_t bytes_read = fread(&included_src[0], 1, file_size, fp);
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
                ((tok.text == "type" && looks_like_type_decl()) ||
                 (tok.text == "union" && looks_like_union_decl()))) {
                auto type_decl = parse_type_decl();
                register_type(std::move(type_decl));
                return true;
            }

            // enum declaration: enum Name<T> { Variant1, Variant2(T), ... }
            if (tok.kind == TokenKind::TK_IDENT && tok.text == "enum" && looks_like_enum_decl()) {
                auto enum_decl = parse_enum_decl();
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
                register_class(std::move(class_decl));
                return true;
            }

            // function declaration: func foo(args...) { ... }
            if (tok.kind == TokenKind::TK_IDENT && tok.text == "func" && looks_like_func_keyword_decl()) {
                auto fn = parse_function();
                functions.push_back(std::move(fn));
                return true;
            }

            if (tok.kind == TokenKind::TK_IDENT && tok.text == "export") {
                next();

                if (peek().kind == TokenKind::TK_IDENT && peek().text == "func" &&
                    looks_like_func_keyword_decl()) {
                    auto fn = parse_function();
                    register_module_export(fn->name, fn->name);
                    functions.push_back(std::move(fn));
                    return true;
                }

                if (peek().kind == TokenKind::TK_LBRACE) {
                    next(); // consume '{'

                    while (peek().kind != TokenKind::TK_RBRACE && !is_eof()) {
                        if (peek().kind != TokenKind::TK_IDENT) {
                            error_and_exit("Expected identifier in export list");
                        }

                        std::string local_name = next().text;
                        std::string export_name = local_name;

                        if (peek().kind == TokenKind::TK_IDENT && peek().text == "as") {
                            next();
                            if (peek().kind != TokenKind::TK_IDENT) {
                                error_and_exit("Expected exported name after 'as'");
                            }
                            export_name = next().text;
                        }

                        register_module_export(export_name, local_name);

                        if (peek().kind == TokenKind::TK_COMMA) {
                            next();
                        } else if (peek().kind != TokenKind::TK_RBRACE) {
                            error_and_exit("Expected ',' or '}' in export list");
                        }
                    }

                    expect(TokenKind::TK_RBRACE, "export list end '}'");
                    if (peek().kind == TokenKind::TK_SEMICOLON) {
                        next();
                    }
                    return true;
                }

                if (peek().kind == TokenKind::TK_IDENT &&
                    (peek().text == "let" || peek().text == "global" || peek().text == "const")) {
                    auto stmt = parse_stmt();
                    auto *var_decl = dynamic_cast<nari::VarDeclStmt *>(stmt.get());
                    if (!var_decl) {
                        error_and_exit("Expected variable declaration after 'export'");
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

                error_and_exit("Top-level export currently supports 'export func ...', 'export let/global/const ...', or 'export { ... }'");
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
                        }

                        std::string export_name = next().text;
                        std::string local_name = export_name;
                        if (peek().kind == TokenKind::TK_IDENT && peek().text == "as") {
                            next();
                            if (peek().kind != TokenKind::TK_IDENT) {
                                error_and_exit("Expected local alias after 'as'");
                            }
                            local_name = next().text;
                        }

                        named_imports.push_back({ export_name, local_name });

                        if (peek().kind == TokenKind::TK_COMMA) {
                            next();
                        } else if (peek().kind != TokenKind::TK_RBRACE) {
                            error_and_exit("Expected ',' or '}' in import list");
                        }
                    }

                    expect(TokenKind::TK_RBRACE, "import list end '}'");
                    if (peek().kind != TokenKind::TK_IDENT || peek().text != "from") {
                        error_and_exit("Expected 'from' after import list");
                    }
                    next();
                    if (peek().kind != TokenKind::TK_STRING) {
                        error_and_exit("Expected module path string after 'from'");
                    }

                    Token path_tok = next();
                    std::string inc_name = path_tok.text;
                    if (inc_name.size() >= 6 && inc_name.substr(inc_name.size() - 6) == ".naric") {
                        error_and_exit("Named imports from .naric modules are not supported yet");
                    }

                    std::string inc_path = include_source_import(path_tok, inc_name);
                    std::string namespace_global = get_module_namespace_global_name(inc_path);
                    if (namespace_global.empty()) {
                        error_and_exit("Module '" + inc_name + "' does not define explicit exports");
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
                    }
                    next(); // consume "from"

                    if (peek().kind != TokenKind::TK_STRING) {
                        error_and_exit("Expected library path string after 'from'");
                    }
                    Token path_tok = next();
                    std::string lib_path = path_tok.text;

                    if (!is_probable_ffi_library_path(lib_path)) {
                        if (lib_path.size() >= 6 && lib_path.substr(lib_path.size() - 6) == ".naric") {
                            error_and_exit("Namespace imports from .naric modules are not supported yet");
                        }

                        std::string inc_path = include_source_import(path_tok, lib_path);
                        std::string namespace_global = get_module_namespace_global_name(inc_path);
                        if (namespace_global.empty()) {
                            error_and_exit("Module '" + lib_path + "' does not define explicit exports");
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

                const Token &itok =
                    peek(); // import path token (renamed to avoid shadowing)
                if (itok.kind != TokenKind::TK_STRING) {
                    error_and_exit("import requires a string filename or 'name from \"path\"' syntax");
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
            if (stmt) {
                top_block->stmts.push_back(std::move(stmt));
                return true;
            }
            // unrecognised / null statement
            return false;
        };

        while (!is_eof()) {
            if (this->recover_mode) {
                try {
                    if (!parse_one()) {
                        break;
                    }
                } catch (const ParseRecoverSignal &) {
                    size_t before = idx;
                    synchronize();
                    // if synchronize() didn't advance, force past the current token so the loop always makes progress.
                    if (idx == before && !is_eof()) {
                        next();
                    }
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
                    if (name.size() >= prefix.size() &&
                        name.compare(0, prefix.size(), prefix) == 0) {
                        auto ident = std::make_unique<nari::IdentExpr>(name);
                        ident->line = func_ptr->line;
                        ident->col = func_ptr->col;
                        ident->filename = func_ptr->filename;

                        auto call_expr = std::make_unique<nari::CallExpr>(std::move(ident));
                        // attach position info
                        call_expr->line = func_ptr->line;
                        call_expr->col = func_ptr->col;
                        call_expr->filename = func_ptr->filename;
                        // snapshot metadata before moving the call so that we don't dereference a moved-from unique_ptr.
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

                // attach aggregator metadata and insert it at the front so callers that inspect function lists see the aggregator early.
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

  private:
    const std::vector<Token> &toks;
    size_t idx;
    bool recover_mode;
    bool strict_mode = false; // set when "use strict" is the first statement
    std::vector<ParseError> errors_;

    // error recovery
    // advance past tokens until a likely statement boundary so that the
    // top-level loop can attempt to parse the next construct cleanly.
    void synchronize() {
        static const std::unordered_set<std::string> boundaryKeywords = {
            "func", "let", "const", "if", "while", "for",
            "foreach", "return", "continue", "break", "import", "export", "type",
            "enum", "class", "match"
        };
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

    bool is_eof() const {
        return idx >= toks.size() || toks[idx].kind == TokenKind::TK_EOF;
    }

    const Token &peek(size_t n = 0) const {
        size_t i = idx + n;
        if (i >= toks.size()) {
            return toks.back();
        }
        return toks[i];
    }

    const Token &next() {
        if (idx < toks.size()) {
            return toks[idx++];
        }
        return toks.back();
    }

    bool match(TokenKind k) {
        if (!is_eof() && peek().kind == k) {
            next();
            return true;
        }
        return false;
    }

    void error_and_exit(const std::string &msg) {
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

        // Recovery mode: record the diagnostic and unwind to the nearest
        // per-iteration catch block instead of terminating the process.
        if (this->recover_mode) {
            errors_.push_back({ use_fname, line, col, msg });
            throw ParseRecoverSignal{};
        }

        fprintf(stderr, "Exception at %s:%d:%d\n", use_fname.c_str(), line, col);
        fprintf(stderr, "  %s %s:%d:%d\n", msg.c_str(), use_fname.c_str(), line,
                col);

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
            // ESP-IDF does not support getline, so we just print the filename and
            // line number.
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

    void expect(TokenKind k, const char *msg = nullptr) {
        if (!is_eof() && peek().kind == k) {
            next();
            return;
        }
        std::string error_msg = "Parse error";
        if (!is_eof()) {
            error_msg += " token='" + token_desc(peek()) + "'";
        }
        if (msg) {
            error_msg += " (" + std::string(msg) + ")";
        }
        error_and_exit(error_msg);
    }

    bool looks_like_func_keyword_decl() {
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
            if (next_idx < toks.size() &&
                toks[next_idx].kind == TokenKind::TK_IDENT) {
                next_idx++; // skip type name
                // check for array syntax: type[]
                if (next_idx < toks.size() &&
                    toks[next_idx].kind == TokenKind::TK_LBRACKET &&
                    next_idx + 1 < toks.size() &&
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

    bool looks_like_foreach_loop() {
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
        if (peek(v + 1).kind == TokenKind::TK_IDENT &&
            (peek(v + 1).text == "in" || peek(v + 1).text == "of")) {
            return true;
        }
        // two-var: for ( [let] IDENT , IDENT in|of ... )
        if (peek(v + 1).kind == TokenKind::TK_COMMA &&
            peek(v + 2).kind == TokenKind::TK_IDENT &&
            peek(v + 3).kind == TokenKind::TK_IDENT &&
            (peek(v + 3).text == "in" || peek(v + 3).text == "of")) {
            return true;
        }
        return false;
    }

    bool looks_like_type_decl() {
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

    bool looks_like_union_decl() {
        return peek().kind == TokenKind::TK_IDENT && peek().text == "union" &&
               peek(1).kind == TokenKind::TK_IDENT && peek(2).kind == TokenKind::TK_LBRACE;
    }

    bool looks_like_enum_decl() {
        // enum IDENT { or enum IDENT< (generics)
        if (peek().kind != TokenKind::TK_IDENT || peek().text != "enum") {
            return false;
        }
        if (peek(1).kind != TokenKind::TK_IDENT) {
            return false;
        }
        if (peek(2).kind == TokenKind::TK_LBRACE ||
            peek(2).kind == TokenKind::TK_LT) {
            return true;
        }
        return false;
    }

    bool looks_like_class_decl() {
        // class IDENT { or class IDENT< (generics) or class IDENT extends
        if (peek().kind != TokenKind::TK_IDENT || peek().text != "class") {
            return false;
        }
        if (peek(1).kind != TokenKind::TK_IDENT) {
            return false;
        }
        if (peek(2).kind == TokenKind::TK_LBRACE || peek(2).kind == TokenKind::TK_LT || (peek(2).kind == TokenKind::TK_IDENT && peek(2).text == "extends")) {
            return true;
        }
        return false;
    }

    template <typename T>
    void parse_generic_arguments(T *type) {
        if (peek().kind == TokenKind::TK_LT) {
            next(); // consume '<'
            while (!is_eof() && peek().kind != TokenKind::TK_GT) {
                if (peek().kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected generic parameter name");
                }
                type->generic_params.push_back(next().text);

                if (peek().kind == TokenKind::TK_COMMA) {
                    next(); // consume ','
                } else if (peek().kind != TokenKind::TK_GT) {
                    error_and_exit("Expected ',' or '>' in generic parameters");
                }
            }
            expect(TokenKind::TK_GT, "'>' after generic parameters");
        }
    }

    nari::TypeDeclPtr parse_type_decl() {
        const bool is_union = peek().text == "union";
        next(); // consume 'type' or 'union'
        const Token &nameTok = next();
        if (nameTok.kind != TokenKind::TK_IDENT) {
            error_and_exit(is_union ? "Expected union name after 'union'" : "Expected type name after 'type'");
        }
        auto type_decl = std::make_unique<nari::TypeDecl>(
            nameTok.text, is_union ? nari::TypeDeclKind::Union : nari::TypeDeclKind::Struct);
        type_decl->line = nameTok.line;
        type_decl->col = nameTok.col;
        type_decl->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;

        if (!is_union) {
            parse_generic_arguments(type_decl.get());
        }

        // Check if this is a type alias or a struct definition
        if (peek().kind == TokenKind::TK_LBRACE) {
            expect(TokenKind::TK_LBRACE, "type body start '{'");

            while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
                // Parse field: name: type or name: type[]
                if (peek().kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected field name in type declaration");
                }
                std::string field_name = next().text;

                expect(TokenKind::TK_COLON, "':' after field name");

                auto field_type = parse_type_annotation();
                type_decl->fields.emplace_back(field_name, std::move(field_type));

                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }
            }

            expect(TokenKind::TK_RBRACE, "type body end '}'");
        } else if (!is_union && peek().kind == TokenKind::TK_IDENT) {
            // type alias: type NewName ExistingType
            type_decl->alias_target = parse_type_annotation();

            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
            }
        } else {
            error_and_exit("Expected '{' for type definition or type name for alias");
        }

        return type_decl;
    }

    // type annotation with optional generics
    std::unique_ptr<nari::TypeAnnotation> parse_type_annotation() {
        if (peek().kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected type name");
        }
        std::string type_name = next().text;
        auto type_ann = std::make_unique<nari::TypeAnnotation>(type_name);

        parse_generic_arguments(type_ann.get());

        if (peek().kind == TokenKind::TK_LBRACKET) {
            next(); // consume '['
            if (peek().kind == TokenKind::TK_RBRACKET) {
                next(); // consume ']'
                type_ann->is_array = true;
            } else {
                if (peek().kind != TokenKind::TK_NUMBER) {
                    error_and_exit("Expected a positive integer fixed array size");
                }
                const std::string count_text = next().text;
                if (count_text.empty() || count_text.find_first_not_of("0123456789") != std::string::npos) {
                    error_and_exit("Fixed array size must be a positive integer literal");
                }
                char *end = nullptr;
                unsigned long long count = std::strtoull(count_text.c_str(), &end, 10);
                if (!end || *end != '\0' || count == 0 || count > MAX_FIXED_ARRAY_COUNT) {
                    error_and_exit("Fixed array size is out of range");
                }
                expect(TokenKind::TK_RBRACKET, "']' after fixed array size");
                type_ann->fixed_array_count = static_cast<size_t>(count);
            }
        }

        return type_ann;
    }

    nari::EnumDeclPtr parse_enum_decl() {
        next(); // consume 'enum'
        const Token &nameTok = next();
        if (nameTok.kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected enum name after 'enum'");
        }
        auto enum_decl = std::make_unique<nari::EnumDecl>(nameTok.text);
        enum_decl->line = nameTok.line;
        enum_decl->col = nameTok.col;
        enum_decl->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;

        // parse generic parameters
        parse_generic_arguments(enum_decl.get());

        expect(TokenKind::TK_LBRACE, "enum body start '{'");

        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
            // Parse variant: Name or Name(T) or Name { field: T }
            if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected variant name in enum declaration");
            }
            std::string variant_name = next().text;
            nari::EnumVariant variant(variant_name);

            // Check for tuple variant: Some(T)
            if (peek().kind == TokenKind::TK_LPAREN) {
                next(); // consume '('
                while (!is_eof() && peek().kind != TokenKind::TK_RPAREN) {
                    auto field_type = parse_type_annotation();
                    variant.tuple_fields.push_back(std::move(field_type));

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next(); // consume ','
                    } else if (peek().kind != TokenKind::TK_RPAREN) {
                        error_and_exit("Expected ',' or ')' in variant fields");
                    }
                }
                expect(TokenKind::TK_RPAREN, "')' after variant fields");
            }

            else if (peek().kind == TokenKind::TK_LBRACE) {
                next(); // consume '{'
                while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected field name in variant");
                    }
                    std::string field_name = next().text;
                    expect(TokenKind::TK_COLON, "':' after field name");

                    auto field_type = parse_type_annotation();
                    variant.named_fields.emplace_back(field_name, std::move(field_type));

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next(); // consume ','
                    } else if (peek().kind != TokenKind::TK_RBRACE) {
                        error_and_exit("Expected ',' or '}' in variant fields");
                    }
                }
                expect(TokenKind::TK_RBRACE, "'}' after variant fields");
            }

            enum_decl->variants.push_back(std::move(variant));

            // optional comma
            if (peek().kind == TokenKind::TK_COMMA) {
                next();
            }
        }

        expect(TokenKind::TK_RBRACE, "enum body end '}'");
        return enum_decl;
    }

    nari::ClassDeclPtr parse_class_decl() {
        next(); // consume 'class'
        const Token &nameTok = next();
        if (nameTok.kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected class name after 'class'");
        }
        auto class_decl = std::make_unique<nari::ClassDecl>(nameTok.text);
        class_decl->line = nameTok.line;
        class_decl->col = nameTok.col;
        class_decl->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;

        // parse generic parameters
        parse_generic_arguments(class_decl.get());

        // Parse optional extends clause
        if (peek().kind == TokenKind::TK_IDENT && peek().text == "extends") {
            next(); // consume 'extends'
            const Token &parentTok = peek();
            if (parentTok.kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected parent class name after 'extends'");
            }
            class_decl->parent_name = next().text;
        }

        expect(TokenKind::TK_LBRACE, "class body start '{'");

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
                expect(TokenKind::TK_LPAREN, "'(' for constructor parameters");
                if (peek().kind != TokenKind::TK_RPAREN) {
                    while (true) {
                        if (peek().kind != TokenKind::TK_IDENT) {
                            error_and_exit("Expected parameter name");
                        }
                        std::string param_name = next().text;

                        std::unique_ptr<nari::TypeAnnotation> param_type = nullptr;
                        if (peek().kind == TokenKind::TK_COLON) {
                            next(); // consume ':'
                            param_type = parse_type_annotation();
                        }

                        method.params.emplace_back(param_name, nullptr, false, std::move(param_type));

                        if (peek().kind == TokenKind::TK_COMMA) {
                            next();
                        } else if (peek().kind == TokenKind::TK_RPAREN) {
                            break;
                        } else {
                            error_and_exit("Expected ',' or ')' in constructor parameters");
                        }
                    }
                }
                expect(TokenKind::TK_RPAREN, "')' after constructor parameters");

                // parse body
                method.body = parse_block();

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
                expect(TokenKind::TK_LPAREN, "'(' for method parameters");
                if (peek().kind != TokenKind::TK_RPAREN) {
                    while (true) {
                        if (peek().kind != TokenKind::TK_IDENT) {
                            error_and_exit("Expected parameter name");
                        }
                        std::string param_name = next().text;

                        std::unique_ptr<nari::TypeAnnotation> param_type = nullptr;
                        if (peek().kind == TokenKind::TK_COLON) {
                            next(); // consume ':'
                            param_type = parse_type_annotation();
                        }

                        method.params.emplace_back(param_name, nullptr, false, std::move(param_type));

                        if (peek().kind == TokenKind::TK_COMMA) {
                            next();
                        } else if (peek().kind == TokenKind::TK_RPAREN) {
                            break;
                        } else {
                            error_and_exit("Expected ',' or ')' in method parameters");
                        }
                    }
                }
                expect(TokenKind::TK_RPAREN, "')' after method parameters");

                // parse optional return type
                if (peek().kind == TokenKind::TK_ARROW) {
                    next(); // consume '->'
                    method.return_type = parse_type_annotation();
                }

                method.body = parse_block();

                method.is_static = is_static;
                class_decl->methods.push_back(std::move(method));
                continue;
            }

            // otherwise it's a field: name: type or name: type = default
            if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected field or method name in class body");
            }
            const Token &fieldTok = next();
            std::string field_name = fieldTok.text;

            expect(TokenKind::TK_COLON, "':' after field name");

            auto field_type = parse_type_annotation();

            // Parse optional default value
            ExprPtr default_value = nullptr;
            if (peek().kind == TokenKind::TK_EQUAL) {
                next(); // consume '='
                default_value = parse_expression();
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

        expect(TokenKind::TK_RBRACE, "class body end '}'");
        return class_decl;
    }

    FunctionPtr parse_function() {
        // consume 'func'
        next();
        const Token &nameTok = next();
        if (nameTok.kind != TokenKind::TK_IDENT) {
            error_and_exit("Expected function name after 'func'");
        }
        auto fn = std::make_unique<nari::Function>();
        fn->name = nameTok.text;
        fn->line = nameTok.line;
        fn->col = nameTok.col;
        fn->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;
        expect(TokenKind::TK_LPAREN, "function params start '('");
        bool seen_rest = false;
        if (peek().kind != TokenKind::TK_RPAREN) {
            while (true) {
                bool is_rest = false;
                if (peek().kind == TokenKind::TK_ELLIPSIS) {
                    if (seen_rest) {
                        error_and_exit("Only one rest parameter is allowed");
                    }
                    seen_rest = true;
                    is_rest = true;
                    next(); // consume '...'
                }

                const Token &p = peek();
                if (p.kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected parameter name");
                }
                std::string pname = next().text;

                // parse optional type annotation, name: type or name: type[]
                nari::TypeAnnotationPtr param_type = nullptr;
                if (peek().kind == TokenKind::TK_COLON) {
                    next(); // consume ':'
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected type name after ':'");
                    }
                    std::string type_name = next().text;
                    bool is_array = false;
                    // Check for array syntax: type[]
                    if (peek().kind == TokenKind::TK_LBRACKET &&
                        peek(1).kind == TokenKind::TK_RBRACKET) {
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
                } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
                    error_and_exit("Rest parameter cannot have a default value");
                }

                fn->params.emplace_back(pname, std::move(default_value), is_rest, std::move(param_type));

                if (is_rest) {
                    if (peek().kind == TokenKind::TK_COMMA) {
                        error_and_exit("Rest parameter must be last");
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
        expect(TokenKind::TK_RPAREN, "function params end ')'");

        // parse optional return type: -> type or -> type[]
        if (peek().kind == TokenKind::TK_ARROW) {
            next(); // consume '->'
            if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected return type after '->'");
            }
            std::string return_type_name = next().text;
            bool is_array = false;
            // Check for array syntax: type[]
            if (peek().kind == TokenKind::TK_LBRACKET &&
                peek(1).kind == TokenKind::TK_RBRACKET) {
                next(); // consume '['
                next(); // consume ']'
                is_array = true;
            }
            fn->return_type = std::make_unique<nari::TypeAnnotation>(return_type_name, is_array);
        }

        fn->body = parse_block();
        fn->strict_mode = this->strict_mode;
        return fn;
    }

    BlockPtr parse_block() {
        expect(TokenKind::TK_LBRACE, "block start '{'");
        auto blk = std::make_unique<nari::BlockStmt>();
        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
            if (peek().kind == TokenKind::TK_SEMICOLON) {
                next();
                continue;
            }
            auto stmt = parse_stmt();
            if (stmt) {
                blk->stmts.push_back(std::move(stmt));
            } else {
                break;
            }
        }
        expect(TokenKind::TK_RBRACE, "block end '}'");
        prune_dead_code(blk.get());
        optimize_block(blk.get());
        propagate_consts(blk.get());
        return blk;
    }

    void prune_dead_code(nari::BlockStmt *blk) {
        if (!blk) {
            return;
        }
        bool seen_return = false;
        for (size_t i = 0; i < blk->stmts.size(); ++i) {
            if (seen_return) {
                blk->stmts.erase(blk->stmts.begin() + i, blk->stmts.end());
                break;
            }
            if (dynamic_cast<nari::ReturnStmt *>(blk->stmts[i].get())) {
                seen_return = true;
            }
        }
    }

    void optimize_block(nari::BlockStmt *blk) {
        if (!blk) {
            return;
        }

        for (size_t i = 0; i < blk->stmts.size(); ++i) {
            auto *fs = dynamic_cast<nari::ForStmt *>(blk->stmts[i].get());
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

    // constant propagation: replaces never-reassigned `let x = <literal>` references with the literal value, then re-folds.

    using ConstMap = std::unordered_map<std::string, const nari::Expr *>;
    using NameSet = std::unordered_set<std::string>;

    static ExprPtr clone_literal(const nari::Expr *src, const nari::Expr *loc) {
        ExprPtr out;
        if (const auto *ne = dynamic_cast<const nari::NumberExpr *>(src)) {
            out = ne->is_float ? std::make_unique<nari::NumberExpr>(ne->f) : std::make_unique<nari::NumberExpr>(ne->i);
        } else if (const auto *be = dynamic_cast<const nari::BoolExpr *>(src)) {
            out = std::make_unique<nari::BoolExpr>(be->value);
        } else if (const auto *se = dynamic_cast<const nari::StringExpr *>(src)) {
            out = std::make_unique<nari::StringExpr>(se->value);
        }
        if (out && loc) {
            copy_loc(out.get(), loc);
        }
        return out;
    }

    // collect_mutated_expr / collect_mutated_stmt: find every variable that is ever *assigned to* or *re-declared in a nested scope* within a subtree.
    static void collect_mutated_stmt(const nari::Stmt *s, NameSet &out) {
        if (!s) {
            return;
        }
        if (const auto *as = dynamic_cast<const nari::AssignStmt *>(s)) {
            out.insert(as->target);
            collect_mutated_expr(as->value.get(), out);
            return;
        }
        if (const auto *es = dynamic_cast<const nari::ExprStmt *>(s)) {
            collect_mutated_expr(es->expr.get(), out);
            return;
        }
        if (const auto *vd = dynamic_cast<const nari::VarDeclStmt *>(s)) {
            collect_mutated_expr(vd->initializerExpr.get(), out);
            return;
        }
        if (const auto *ia = dynamic_cast<const nari::IndexAssignStmt *>(s)) {
            collect_mutated_expr(ia->target.get(), out);
            collect_mutated_expr(ia->value.get(), out);
            return;
        }
        if (const auto *rs = dynamic_cast<const nari::ReturnStmt *>(s)) {
            collect_mutated_expr(rs->value.get(), out);
            return;
        }
        if (const auto *blk = dynamic_cast<const nari::BlockStmt *>(s)) {
            for (const auto &st : blk->stmts) {
                // Any VarDeclStmt inside a nested block potentially shadows an outer name.
                if (const auto *vd2 = dynamic_cast<const nari::VarDeclStmt *>(st.get())) {
                    out.insert(vd2->name);
                }
                collect_mutated_stmt(st.get(), out);
            }
            return;
        }
        if (const auto *is = dynamic_cast<const nari::IfStmt *>(s)) {
            collect_mutated_expr(is->cond.get(), out);
            collect_mutated_stmt(is->then_branch.get(), out);
            collect_mutated_stmt(is->else_branch.get(), out);
            return;
        }
        if (const auto *ws = dynamic_cast<const nari::WhileStmt *>(s)) {
            collect_mutated_expr(ws->cond.get(), out);
            collect_mutated_stmt(ws->body.get(), out);
            return;
        }
        if (const auto *fs = dynamic_cast<const nari::ForStmt *>(s)) {
            // for-loop init may introduce a new binding (like `for (let i = 0; ...)`)
            if (const auto *vd = dynamic_cast<const nari::VarDeclStmt *>(fs->init.get())) {
                out.insert(vd->name);
            } else {
                collect_mutated_stmt(fs->init.get(), out);
            }
            collect_mutated_expr(fs->cond.get(), out);
            collect_mutated_stmt(fs->post.get(), out);
            collect_mutated_stmt(fs->body.get(), out);
            return;
        }
        if (const auto *fes = dynamic_cast<const nari::ForEachStmt *>(s)) {
            out.insert(fes->var); // loop variable is a new binding
            collect_mutated_expr(fes->iterable.get(), out);
            collect_mutated_stmt(fes->body.get(), out);
            return;
        }
        if (const auto *ss = dynamic_cast<const nari::SwitchStmt *>(s)) {
            collect_mutated_expr(ss->value.get(), out);
            for (const auto &c : ss->cases) {
                collect_mutated_expr(c.match.get(), out);
                if (c.body) {
                    for (const auto &st : c.body->stmts) {
                        collect_mutated_stmt(st.get(), out);
                    }
                }
            }
            if (ss->default_body) {
                for (const auto &st : ss->default_body->stmts) {
                    collect_mutated_stmt(st.get(), out);
                }
            }
            return;
        }
    }

    static void collect_mutated_expr(const nari::Expr *e, NameSet &out) {
        if (!e) {
            return;
        }
        if (const auto *ue = dynamic_cast<const nari::UnaryExpr *>(e)) {
            if (ue->op == "++" || ue->op == "--" || ue->op == "post++" || ue->op == "post--") {
                if (const auto *id = dynamic_cast<const nari::IdentExpr *>(ue->operand.get())) {
                    out.insert(id->name);
                }
            }
            collect_mutated_expr(ue->operand.get(), out);
            return;
        }
        if (const auto *be = dynamic_cast<const nari::BinaryExpr *>(e)) {
            collect_mutated_expr(be->left.get(), out);
            collect_mutated_expr(be->right.get(), out);
            return;
        }
        if (const auto *ce = dynamic_cast<const nari::CallExpr *>(e)) {
            collect_mutated_expr(ce->callee.get(), out);
            for (const auto &a : ce->args) {
                collect_mutated_expr(a.get(), out);
            }
            return;
        }
        if (const auto *ae = dynamic_cast<const nari::ArrayLiteralExpr *>(e)) {
            for (const auto &el : ae->elements) {
                collect_mutated_expr(el.get(), out);
            }
            return;
        }
        if (const auto *oe = dynamic_cast<const nari::ObjectLiteralExpr *>(e)) {
            for (const auto &kv : oe->entries) {
                collect_mutated_expr(kv.second.get(), out);
            }
            return;
        }
        if (const auto *ie = dynamic_cast<const nari::IndexExpr *>(e)) {
            collect_mutated_expr(ie->object.get(), out);
            collect_mutated_expr(ie->index.get(), out);
            return;
        }
        if (const auto *me = dynamic_cast<const nari::MemberExpr *>(e)) {
            collect_mutated_expr(me->object.get(), out);
            return;
        }
        if (const auto *ne = dynamic_cast<const nari::NewExpr *>(e)) {
            for (const auto &a : ne->args) {
                collect_mutated_expr(a.get(), out);
            }
            return;
        }
        if (const auto *te = dynamic_cast<const nari::TernaryExpr *>(e)) {
            collect_mutated_expr(te->condition.get(), out);
            collect_mutated_expr(te->true_expr.get(), out);
            collect_mutated_expr(te->false_expr.get(), out);
            return;
        }
        // Scan closures and spawn blocks for assignments to outer variables.
        if (const auto *fe = dynamic_cast<const nari::FunctionExpr *>(e)) {
            if (fe->body) {
                for (const auto &st : fe->body->stmts) {
                    collect_mutated_stmt(st.get(), out);
                }
            }
            return;
        }
        if (const auto *se = dynamic_cast<const nari::SpawnExpr *>(e)) {
            if (se->body) {
                for (const auto &st : se->body->stmts) {
                    collect_mutated_stmt(st.get(), out);
                }
            }
            return;
        }
    }

    // subst_fold: substitute known constants into an expression and re-fold.
    ExprPtr subst_fold(ExprPtr expr, const ConstMap &m) {
        if (!expr || m.empty()) {
            return expr;
        }
        if (auto *id = dynamic_cast<nari::IdentExpr *>(expr.get())) {
            auto it = m.find(id->name);
            if (it != m.end()) {
                auto lit = clone_literal(it->second, id);
                if (lit) {
                    return lit;
                }
            }
            return expr;
        }
        if (auto *be = dynamic_cast<nari::BinaryExpr *>(expr.get())) {
            be->left = subst_fold(std::move(be->left), m);
            be->right = subst_fold(std::move(be->right), m);
        } else if (auto *ue = dynamic_cast<nari::UnaryExpr *>(expr.get())) {
            ue->operand = subst_fold(std::move(ue->operand), m);
        } else if (auto *ce = dynamic_cast<nari::CallExpr *>(expr.get())) {
            // Keep identifier callees intact so runtime call diagnostics retain
            // the source-level name instead of seeing only a folded literal.
            if (!dynamic_cast<nari::IdentExpr *>(ce->callee.get())) {
                ce->callee = subst_fold(std::move(ce->callee), m);
            }
            for (auto &a : ce->args) {
                a = subst_fold(std::move(a), m);
            }
        } else if (auto *ae = dynamic_cast<nari::ArrayLiteralExpr *>(expr.get())) {
            for (auto &el : ae->elements) {
                el = subst_fold(std::move(el), m);
            }
        } else if (auto *oe = dynamic_cast<nari::ObjectLiteralExpr *>(expr.get())) {
            for (auto &kv : oe->entries) {
                kv.second = subst_fold(std::move(kv.second), m);
            }
        } else if (auto *ie = dynamic_cast<nari::IndexExpr *>(expr.get())) {
            ie->object = subst_fold(std::move(ie->object), m);
            ie->index = subst_fold(std::move(ie->index), m);
        } else if (auto *me = dynamic_cast<nari::MemberExpr *>(expr.get())) {
            me->object = subst_fold(std::move(me->object), m);
        } else if (auto *ne = dynamic_cast<nari::NewExpr *>(expr.get())) {
            for (auto &a : ne->args) {
                a = subst_fold(std::move(a), m);
            }
        } else if (auto *te = dynamic_cast<nari::TernaryExpr *>(expr.get())) {
            te->condition = subst_fold(std::move(te->condition), m);
            te->true_expr = subst_fold(std::move(te->true_expr), m);
            te->false_expr = subst_fold(std::move(te->false_expr), m);
        }
        return fold_expr(std::move(expr));
    }

    void subst_stmt(nari::Stmt *s, const ConstMap &m) {
        if (!s || m.empty()) {
            return;
        }
        if (auto *es = dynamic_cast<nari::ExprStmt *>(s)) {
            es->expr = subst_fold(std::move(es->expr), m);
            return;
        }
        if (auto *vd = dynamic_cast<nari::VarDeclStmt *>(s)) {
            // do not substitute the init of a variable that is itself a const candidate.
            if (!m.count(vd->name) && vd->initializerExpr) {
                vd->initializerExpr = subst_fold(std::move(vd->initializerExpr), m);
            }
            return;
        }
        if (auto *as = dynamic_cast<nari::AssignStmt *>(s)) {
            as->value = subst_fold(std::move(as->value), m);
            return;
        }
        if (auto *ia = dynamic_cast<nari::IndexAssignStmt *>(s)) {
            ia->target = subst_fold(std::move(ia->target), m);
            ia->value = subst_fold(std::move(ia->value), m);
            return;
        }
        if (auto *rs = dynamic_cast<nari::ReturnStmt *>(s)) {
            rs->value = subst_fold(std::move(rs->value), m);
            return;
        }
        if (auto *blk = dynamic_cast<nari::BlockStmt *>(s)) {
            subst_block(blk, m);
            return; // passes copy so nested re-decls are handled
        }
        if (auto *is = dynamic_cast<nari::IfStmt *>(s)) {
            is->cond = subst_fold(std::move(is->cond), m);
            subst_stmt(is->then_branch.get(), m);
            subst_stmt(is->else_branch.get(), m);
            return;
        }
        if (auto *ws = dynamic_cast<nari::WhileStmt *>(s)) {
            ws->cond = subst_fold(std::move(ws->cond), m);
            subst_stmt(ws->body.get(), m);
            return;
        }
        if (auto *fs = dynamic_cast<nari::ForStmt *>(s)) {
            subst_stmt(fs->init.get(), m);
            fs->cond = subst_fold(std::move(fs->cond), m);
            subst_stmt(fs->post.get(), m);
            subst_stmt(fs->body.get(), m);
            return;
        }
        if (auto *fes = dynamic_cast<nari::ForEachStmt *>(s)) {
            fes->iterable = subst_fold(std::move(fes->iterable), m);
            subst_stmt(fes->body.get(), m);
            return;
        }
        if (auto *ss = dynamic_cast<nari::SwitchStmt *>(s)) {
            ss->value = subst_fold(std::move(ss->value), m);
            for (auto &c : ss->cases) {
                c.match = subst_fold(std::move(c.match), m);
                subst_block(c.body.get(), m);
            }
            subst_block(ss->default_body.get(), m);
            return;
        }
    }

    void subst_block(nari::BlockStmt *blk, ConstMap m) {
        if (!blk || m.empty()) {
            return;
        }
        for (auto &s : blk->stmts) {
            if (m.empty()) {
                break;
            }
            if (auto *vd = dynamic_cast<nari::VarDeclStmt *>(s.get())) {
                // substitute in the init first, then stop folding this name.
                if (!m.count(vd->name) && vd->initializerExpr) {
                    vd->initializerExpr = subst_fold(std::move(vd->initializerExpr), m);
                }
                m.erase(vd->name);
                continue;
            }
            subst_stmt(s.get(), m);
        }
    }

    void propagate_consts(nari::BlockStmt *blk) {
        if (!blk) {
            return;
        }
        // pass 1: collect every name that is mutated or re-declared anywhere.
        NameSet mutated;
        for (const auto &s : blk->stmts) {
            collect_mutated_stmt(s.get(), mutated);
        }
        // pass 2: identify constant candidates (simple `let x = <literal>`).
        ConstMap consts;
        for (const auto &s : blk->stmts) {
            if (const auto *vd = dynamic_cast<const nari::VarDeclStmt *>(s.get())) {
                if (vd->destructure_kind == nari::DestructureKind::None &&
                    !vd->name.empty() && !vd->is_global && vd->initializerExpr &&
                    literal_any(vd->initializerExpr.get()) && !mutated.count(vd->name)) {
                    consts[vd->name] = vd->initializerExpr.get();
                }
            }
        }
        if (consts.empty()) {
            return;
        }
        // substitute and fold.
        for (auto &s : blk->stmts) {
            subst_stmt(s.get(), consts);
        }
        // Re-run loop optimiser: newly constant loop bounds may now be collapsible.
        optimize_block(blk);
    }

    bool get_loop_init(nari::Stmt *init, std::string &var, int64_t &start) {
        if (auto *vd = dynamic_cast<nari::VarDeclStmt *>(init)) {
            if (!vd->initializerExpr) {
                return false;
            }
            int64_t v = 0;
            if (!literal_int(vd->initializerExpr.get(), v)) {
                return false;
            }
            var = vd->name;
            start = v;
            return true;
        }
        if (auto *as = dynamic_cast<nari::AssignStmt *>(init)) {
            if (!as->value) {
                return false;
            }
            int64_t v = 0;
            if (!literal_int(as->value.get(), v)) {
                return false;
            }
            var = as->target;
            start = v;
            return true;
        }
        return false;
    }

    bool get_loop_cond(nari::Expr *cond, const std::string &var, int64_t &end, bool &inclusive) {
        auto *be = dynamic_cast<nari::BinaryExpr *>(cond);
        if (!be || !be->left || !be->right) {
            return false;
        }
        if (be->op != "<" && be->op != "<=") {
            return false;
        }
        auto *id = dynamic_cast<nari::IdentExpr *>(be->left.get());
        if (!id || id->name != var) {
            return false;
        }
        int64_t v = 0;
        if (!literal_int(be->right.get(), v)) {
            return false;
        }
        inclusive = (be->op == "<=");
        end = v;
        return true;
    }

    bool get_loop_step(nari::Stmt *post, const std::string &var, int64_t &step) {
        if (!post) {
            return false;
        }
        if (auto *as = dynamic_cast<nari::AssignStmt *>(post)) {
            if (as->target != var || !as->value) {
                return false;
            }
            auto *be = dynamic_cast<nari::BinaryExpr *>(as->value.get());
            if (!be || !be->left || !be->right) {
                return false;
            }
            auto *id = dynamic_cast<nari::IdentExpr *>(be->left.get());
            if (!id || id->name != var) {
                return false;
            }
            int64_t v = 0;
            if (!literal_int(be->right.get(), v)) {
                return false;
            }
            if (be->op == "+") {
                step = v;
                return true;
            }
            if (be->op == "-") {
                step = -v;
                return true;
            }
            return false;
        }
        if (auto *es = dynamic_cast<nari::ExprStmt *>(post)) {
            auto *ue = dynamic_cast<nari::UnaryExpr *>(es->expr.get());
            if (!ue) {
                return false;
            }
            auto *id = dynamic_cast<nari::IdentExpr *>(ue->operand.get());
            if (!id || id->name != var) {
                return false;
            }
            if (ue->op == "post++") {
                step = 1;
                return true;
            }
            if (ue->op == "post--") {
                step = -1;
                return true;
            }
        }
        return false;
    }

    bool get_body_increment(nari::Stmt *body, std::string &target, int64_t &inc) {
        auto *blk = dynamic_cast<nari::BlockStmt *>(body);
        if (!blk || blk->stmts.size() != 1) {
            return false;
        }

        if (auto *as = dynamic_cast<nari::AssignStmt *>(blk->stmts[0].get())) {
            auto *be = dynamic_cast<nari::BinaryExpr *>(as->value.get());
            if (!be || !be->left || !be->right) {
                return false;
            }
            auto *id = dynamic_cast<nari::IdentExpr *>(be->left.get());
            if (!id || id->name != as->target) {
                return false;
            }
            int64_t v = 0;
            if (!literal_int(be->right.get(), v)) {
                return false;
            }
            if (be->op == "+") {
                target = as->target;
                inc = v;
                return true;
            }
            if (be->op == "-") {
                target = as->target;
                inc = -v;
                return true;
            }
            return false;
        }

        if (auto *es = dynamic_cast<nari::ExprStmt *>(blk->stmts[0].get())) {
            auto *ue = dynamic_cast<nari::UnaryExpr *>(es->expr.get());
            if (!ue) {
                return false;
            }
            auto *id = dynamic_cast<nari::IdentExpr *>(ue->operand.get());
            if (!id) {
                return false;
            }
            if (ue->op == "post++") {
                target = id->name;
                inc = 1;
                return true;
            }
            if (ue->op == "post--") {
                target = id->name;
                inc = -1;
                return true;
            }
        }

        return false;
    }

    std::unique_ptr<nari::Stmt> try_optimize_for(nari::ForStmt *fs, nari::Stmt *prev) {
        if (!fs || !fs->init || !fs->cond || !fs->post || !fs->body) {
            return nullptr;
        }

        std::string loop_var;
        int64_t start = 0;
        if (!get_loop_init(fs->init.get(), loop_var, start)) {
            return nullptr;
        }

        int64_t end = 0;
        bool inclusive = false;
        if (!get_loop_cond(fs->cond.get(), loop_var, end, inclusive)) {
            return nullptr;
        }

        int64_t step = 0;
        if (!get_loop_step(fs->post.get(), loop_var, step)) {
            return nullptr;
        }

        if (step <= 0) {
            return nullptr;
        }
        if (step != 1) {
            return nullptr;
        }

        // check if body is empty - optimize to just setting the loop variable to the end value
        auto *blk = dynamic_cast<nari::BlockStmt *>(fs->body.get());
        if (blk && blk->stmts.empty()) {
            int64_t final_value = inclusive ? end + 1 : end;
            auto val_expr = std::make_unique<nari::NumberExpr>(final_value);
            val_expr->line = fs->line;
            val_expr->col = fs->col;
            val_expr->filename = fs->filename;

            auto assign = std::make_unique<nari::AssignStmt>(loop_var, std::move(val_expr));
            assign->line = fs->line;
            assign->col = fs->col;
            assign->filename = fs->filename;
            return assign;
        }

        std::string target;
        int64_t inc = 0;
        if (!get_body_increment(fs->body.get(), target, inc)) {
            return nullptr;
        }
        if (target == loop_var) {
            return nullptr;
        }

        if (!prev) {
            return nullptr;
        }
        bool prev_numeric = false;
        if (auto *vd = dynamic_cast<nari::VarDeclStmt *>(prev)) {
            int64_t v = 0;
            if (vd->name == target && vd->initializerExpr && literal_int(vd->initializerExpr.get(), v)) {
                prev_numeric = true;
            }
        } else if (auto *as = dynamic_cast<nari::AssignStmt *>(prev)) {
            int64_t v = 0;
            if (as->target == target && as->value &&
                literal_int(as->value.get(), v)) {
                prev_numeric = true;
            }
        }
        if (!prev_numeric) {
            return nullptr;
        }

        int64_t diff;
        if (sub_overflow_i64(end, start, &diff)) {
            return nullptr;
        }
        if (inclusive) {
            if (add_overflow_i64(diff, INT64_C(1), &diff)) {
                return nullptr;
            }
        }
        if (diff <= 0) {
            diff = 0;
        }
        int64_t delta;
        if (mul_overflow_i64(diff, inc, &delta)) {
            return nullptr;
        }
        auto left = std::make_unique<nari::IdentExpr>(target);
        auto right = std::make_unique<nari::NumberExpr>(delta);
        auto be = std::make_unique<nari::BinaryExpr>("+", std::move(left), std::move(right));
        be->line = fs->line;
        be->col = fs->col;
        be->filename = fs->filename;
        auto assign = std::make_unique<nari::AssignStmt>(target, std::move(be));
        assign->line = fs->line;
        assign->col = fs->col;
        assign->filename = fs->filename;
        return assign;
    }

    static bool literal_int(const nari::Expr *e, int64_t &out) {
        if (const auto *ne = dynamic_cast<const nari::NumberExpr *>(e)) {
            if (!ne->is_float) {
                out = ne->i;
                return true;
            }
        }
        return false;
    }

    static bool literal_float(const nari::Expr *e, double &out) {
        if (const auto *ne = dynamic_cast<const nari::NumberExpr *>(e)) {
            if (ne->is_float) {
                out = ne->f;
                return true;
            }
        }
        return false;
    }

    static double number_to_double(const nari::NumberExpr *ne) {
        if (!ne) {
            return 0.0;
        }
        if (ne->is_float) {
            return ne->f;
        }
        return static_cast<double>(ne->i);
    }

    static bool literal_bool(const nari::Expr *e, bool &out) {
        if (const auto *be = dynamic_cast<const nari::BoolExpr *>(e)) {
            out = be->value;
            return true;
        }
        return false;
    }

    static bool literal_string(const nari::Expr *e, std::string &out) {
        if (const auto *se = dynamic_cast<const nari::StringExpr *>(e)) {
            out = se->value;
            return true;
        }
        return false;
    }

    static bool literal_any(const nari::Expr *e) {
        return dynamic_cast<const nari::NumberExpr *>(e) || dynamic_cast<const nari::StringExpr *>(e) || dynamic_cast<const nari::BoolExpr *>(e);
    }

    static std::string literal_to_string(const nari::Expr *e) {
        if (const auto *se = dynamic_cast<const nari::StringExpr *>(e)) {
            return se->value;
        }
        if (const auto *be = dynamic_cast<const nari::BoolExpr *>(e)) {
            return be->value ? "true" : "false";
        }
        if (const auto *ne = dynamic_cast<const nari::NumberExpr *>(e)) {
            if (ne->is_float) {
                return std::to_string(ne->f);
            } else {
                return std::to_string(ne->i);
            }
        }
        return "";
    }

    static void copy_loc(nari::Expr *dst, const nari::Expr *src) {
        if (!dst || !src) {
            return;
        }
        dst->line = src->line;
        dst->col = src->col;
        dst->filename = src->filename;
    }

    ExprPtr fold_expr(ExprPtr expr) {
        if (!expr) {
            return expr;
        }

        if (auto *ue = dynamic_cast<nari::UnaryExpr *>(expr.get())) {
            ue->operand = fold_expr(std::move(ue->operand));
            if (!ue->operand) {
                return expr;
            }
            int64_t ni = 0;
            double nf = 0.0;
            bool b = false;
            if (ue->op == "neg" && literal_int(ue->operand.get(), ni)) {
                auto result = std::make_unique<nari::NumberExpr>(-ni);
                copy_loc(result.get(), ue);
                return result;
            }
            if (ue->op == "+" && literal_int(ue->operand.get(), ni)) {
                auto result = std::make_unique<nari::NumberExpr>(ni);
                copy_loc(result.get(), ue);
                return result;
            }
            if (ue->op == "+" && literal_float(ue->operand.get(), nf)) {
                auto result = std::make_unique<nari::NumberExpr>(nf);
                copy_loc(result.get(), ue);
                return result;
            }
            if (ue->op == "!" && literal_bool(ue->operand.get(), b)) {
                auto result = std::make_unique<nari::BoolExpr>(!b);
                copy_loc(result.get(), ue);
                return result;
            }
            return expr;
        }

        if (auto *binaryExpr = dynamic_cast<nari::BinaryExpr *>(expr.get())) {
            binaryExpr->left = fold_expr(std::move(binaryExpr->left));
            binaryExpr->right = fold_expr(std::move(binaryExpr->right));
            if (!binaryExpr->left || !binaryExpr->right) {
                return expr;
            }

            int64_t li = 0, ri = 0;
            double lf = 0.0, rf = 0.0;
            bool lb = false, rb = false;
            std::string ls, rs;

            bool lint = literal_int(binaryExpr->left.get(), li);
            bool rint = literal_int(binaryExpr->right.get(), ri);
            bool lfloat = literal_float(binaryExpr->left.get(), lf);
            bool rfloat = literal_float(binaryExpr->right.get(), rf);
            bool lbool = literal_bool(binaryExpr->left.get(), lb);
            bool rbool = literal_bool(binaryExpr->right.get(), rb);
            bool lstr = literal_string(binaryExpr->left.get(), ls);
            bool rstr = literal_string(binaryExpr->right.get(), rs);

            const std::string &op = binaryExpr->op;
            if (lint && rint) {
                if (op == "+") {
                    int64_t v;
                    if (add_overflow_i64(li, ri, &v)) {
                        return expr;
                    }
                    auto ne = std::make_unique<nari::NumberExpr>(v);
                    copy_loc(ne.get(), binaryExpr);
                    return ne;
                }
                if (op == "-") {
                    int64_t v;
                    if (sub_overflow_i64(li, ri, &v)) {
                        return expr;
                    }
                    auto ne = std::make_unique<nari::NumberExpr>(v);
                    copy_loc(ne.get(), binaryExpr);
                    return ne;
                }
                if (op == "*") {
                    int64_t v;
                    if (mul_overflow_i64(li, ri, &v)) {
                        return expr;
                    }
                    auto ne = std::make_unique<nari::NumberExpr>(v);
                    copy_loc(ne.get(), binaryExpr);
                    return ne;
                }
                if (op == "/") {
                    double val =
                        (ri == 0) ? std::numeric_limits<double>::quiet_NaN() : (static_cast<double>(li) / static_cast<double>(ri));
                    auto numExpr = std::make_unique<nari::NumberExpr>(val);
                    copy_loc(numExpr.get(), binaryExpr);
                    return numExpr;
                }
                if (op == "%") {
                    if (ri == 0) {
                        auto result = std::make_unique<nari::NumberExpr>(std::numeric_limits<double>::quiet_NaN());
                        copy_loc(result.get(), binaryExpr);
                        return result;
                    }
                    // INT_MIN % -1 is UB and SIGFPEs on x86 idiv. Match runtime semantics.
                    if (li == std::numeric_limits<int64_t>::min() && ri == -1) {
                        auto result = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(0));
                        copy_loc(result.get(), binaryExpr);
                        return result;
                    }
                    auto result = std::make_unique<nari::NumberExpr>(li % ri);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "**") {
                    if (ri < 0) {
                        auto result = std::make_unique<nari::NumberExpr>(std::pow(static_cast<double>(li), static_cast<double>(ri)));
                        copy_loc(result.get(), binaryExpr);
                        return result;
                    }
                    if (ri > std::numeric_limits<uint64_t>::max()) {
                        return expr;
                    }
                    uint64_t exp = (uint64_t)ri;
                    int64_t base = li;
                    int64_t total = 1;
                    while (exp > 0) {
                        if (exp & 1ULL) {
                            if (mul_overflow_i64(total, base, &total)) {
                                return expr;
                            }
                        }
                        if (exp > 1) {
                            if (mul_overflow_i64(base, base, &base)) {
                                return expr;
                            }
                        }
                        exp >>= 1ULL;
                    }
                    auto result = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(total));
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
            }

            if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
                double l = lfloat ? lf
                                  : (lint ? (double)li
                                          : number_to_double(dynamic_cast<const nari::NumberExpr *>(binaryExpr->left.get())));
                double r = rfloat ? rf
                                  : (rint ? (double)ri
                                          : number_to_double(dynamic_cast<const nari::NumberExpr *>(binaryExpr->right.get())));
                if (op == "+") {
                    auto result = std::make_unique<nari::NumberExpr>(l + r);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "-") {
                    auto result = std::make_unique<nari::NumberExpr>(l - r);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "/") {
                    double v = (r == 0.0) ? std::numeric_limits<double>::quiet_NaN() : (l / r);
                    auto result = std::make_unique<nari::NumberExpr>(v);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "%") {
                    double v = (r == 0.0) ? std::numeric_limits<double>::quiet_NaN() : std::fmod(l, r);
                    auto result = std::make_unique<nari::NumberExpr>(v);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "**") {
                    auto result = std::make_unique<nari::NumberExpr>(std::pow(l, r));
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
            }

            if (op == "@" && literal_any(binaryExpr->left.get()) &&
                literal_any(binaryExpr->right.get()) && (lstr || rstr)) {
                std::string out = literal_to_string(binaryExpr->left.get()) + literal_to_string(binaryExpr->right.get());
                auto result = std::make_unique<nari::StringExpr>(out);
                copy_loc(result.get(), binaryExpr);
                return result;
            }

            if (op == "==" || op == "!=") {
                bool eq = false;
                if (lint && rint) {
                    eq = (li == ri);
                } else if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
                    double l = lfloat
                                   ? lf
                                   : (lint ? static_cast<double>(li)
                                           : number_to_double(dynamic_cast<const nari::NumberExpr *>(binaryExpr->left.get())));
                    double r = rfloat
                                   ? rf
                                   : (rint ? static_cast<double>(ri)
                                           : number_to_double(dynamic_cast<const nari::NumberExpr *>(binaryExpr->right.get())));
                    eq = std::fabs(l - r) < 1e-12;
                } else if (lbool && rbool) {
                    eq = (lb == rb);
                } else if ((lstr || lbool || lint || lfloat) && (rstr || rbool || rint || rfloat)) {
                    eq = (literal_to_string(binaryExpr->left.get()) == literal_to_string(binaryExpr->right.get()));
                } else {
                    return expr;
                }
                auto result = std::make_unique<nari::BoolExpr>(op == "==" ? eq : !eq);
                copy_loc(result.get(), binaryExpr);
                return result;
            }

            if (lint && rint) {
                if (op == "<") {
                    auto result = std::make_unique<nari::BoolExpr>(li < ri);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == ">") {
                    auto result = std::make_unique<nari::BoolExpr>(li > ri);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "<=") {
                    auto result = std::make_unique<nari::BoolExpr>(li <= ri);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == ">=") {
                    auto result = std::make_unique<nari::BoolExpr>(li >= ri);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
            }

            if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
                double l = lfloat ? lf
                                  : (lint ? static_cast<double>(li)
                                          : number_to_double(dynamic_cast<const nari::NumberExpr *>(binaryExpr->left.get())));
                double r = rfloat ? rf
                                  : (rint ? static_cast<double>(ri)
                                          : number_to_double(dynamic_cast<const nari::NumberExpr *>(binaryExpr->right.get())));
                if (op == "<") {
                    auto result = std::make_unique<nari::BoolExpr>(l < r);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == ">") {
                    auto result = std::make_unique<nari::BoolExpr>(l > r);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "<=") {
                    auto result = std::make_unique<nari::BoolExpr>(l <= r);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == ">=") {
                    auto result = std::make_unique<nari::BoolExpr>(l >= r);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
            }

            if (lbool && rbool) {
                if (op == "&&") {
                    auto result = std::make_unique<nari::BoolExpr>(lb && rb);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                if (op == "||") {
                    auto result = std::make_unique<nari::BoolExpr>(lb || rb);
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
            }

            return expr;
        }

        if (auto *te = dynamic_cast<nari::TernaryExpr *>(expr.get())) {
            te->condition = fold_expr(std::move(te->condition));
            te->true_expr = fold_expr(std::move(te->true_expr));
            te->false_expr = fold_expr(std::move(te->false_expr));
            bool cond = false;
            if (literal_bool(te->condition.get(), cond)) {
                return cond ? std::move(te->true_expr) : std::move(te->false_expr);
            }
            return expr;
        }

        if (auto *ce = dynamic_cast<nari::CallExpr *>(expr.get())) {
            ce->callee = fold_expr(std::move(ce->callee));
            for (auto &arg : ce->args) {
                arg = fold_expr(std::move(arg));
            }
            return expr;
        }

        if (auto *ae = dynamic_cast<nari::ArrayLiteralExpr *>(expr.get())) {
            for (auto &el : ae->elements) {
                el = fold_expr(std::move(el));
            }
            return expr;
        }

        if (auto *oe = dynamic_cast<nari::ObjectLiteralExpr *>(expr.get())) {
            for (auto &entry : oe->entries) {
                entry.second = fold_expr(std::move(entry.second));
            }
            return expr;
        }

        if (auto *ie = dynamic_cast<nari::IndexExpr *>(expr.get())) {
            ie->object = fold_expr(std::move(ie->object));
            ie->index = fold_expr(std::move(ie->index));
            return expr;
        }

        if (auto *me = dynamic_cast<nari::MemberExpr *>(expr.get())) {
            me->object = fold_expr(std::move(me->object));
            return expr;
        }

        return expr;
    }

    bool looks_like_arrow_function() {
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
    ExprPtr parse_function_expression() {
        const Token &funcTok = peek();
        expect(TokenKind::TK_IDENT, "func keyword");

        auto func_expr = std::make_unique<nari::FunctionExpr>();
        func_expr->line = funcTok.line;
        func_expr->col = funcTok.col;
        func_expr->filename = funcTok.filename.empty() ? current_filename : funcTok.filename;

        // parse parameters
        expect(TokenKind::TK_LPAREN, "function params start '('");
        bool seen_rest = false;
        if (peek().kind != TokenKind::TK_RPAREN) {
            while (true) {
                bool is_rest = false;
                if (peek().kind == TokenKind::TK_ELLIPSIS) {
                    if (seen_rest) {
                        error_and_exit("Only one rest parameter is allowed");
                    }
                    seen_rest = true;
                    is_rest = true;
                    next(); // consume '...'
                }

                const Token &p = peek();
                if (p.kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected parameter name");
                }
                std::string pname = next().text;

                // parse optional type annotation: name: type or name: type[]
                nari::TypeAnnotationPtr param_type = nullptr;
                if (peek().kind == TokenKind::TK_COLON) {
                    next(); // consume ':'
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected type name after ':'");
                    }
                    std::string type_name = next().text;
                    bool is_array = false;
                    // Check for array syntax: type[]
                    if (peek().kind == TokenKind::TK_LBRACKET && peek(1).kind == TokenKind::TK_RBRACKET) {
                        next(); // consume '['
                        next(); // consume ']'
                        is_array = true;
                    }
                    param_type =
                        std::make_unique<nari::TypeAnnotation>(type_name, is_array);
                }

                ExprPtr default_value = nullptr;
                if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
                    next(); // consume '='
                    default_value = parse_expression();
                } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
                    error_and_exit("Rest parameter cannot have a default value");
                }

                func_expr->params.emplace_back(pname, std::move(default_value), is_rest, std::move(param_type));

                if (is_rest) {
                    if (peek().kind == TokenKind::TK_COMMA) {
                        error_and_exit("Rest parameter must be last");
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
        expect(TokenKind::TK_RPAREN, "function params end ')'");

        // parse optional return type: -> type or -> type[]
        if (peek().kind == TokenKind::TK_ARROW) {
            next(); // consume '->'
            if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected return type after '->'");
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
        return std::move(func_expr);
    }

    // parse spawn expression: spawn { ... }
    ExprPtr parse_spawn_expression() {
        const Token &spawnTok = peek();
        expect(TokenKind::TK_IDENT, "spawn keyword");

        auto spawn_expr = std::make_unique<nari::SpawnExpr>(nullptr);
        spawn_expr->line = spawnTok.line;
        spawn_expr->col = spawnTok.col;
        spawn_expr->filename = spawnTok.filename.empty() ? current_filename : spawnTok.filename;

        // parse the block
        spawn_expr->body = parse_block();
        return spawn_expr;
    }

    // parse arrow function expression: (params) => { ... } or (params) => expr
    ExprPtr parse_arrow_function_expression() {
        const Token &parenTok = peek();
        auto func_expr = std::make_unique<nari::FunctionExpr>();
        func_expr->line = parenTok.line;
        func_expr->col = parenTok.col;
        func_expr->filename = parenTok.filename.empty() ? current_filename : parenTok.filename;

        // Parse parameters
        expect(TokenKind::TK_LPAREN, "arrow function params start '('");
        bool seen_rest = false;
        if (peek().kind != TokenKind::TK_RPAREN) {
            while (true) {
                bool is_rest = false;
                if (peek().kind == TokenKind::TK_ELLIPSIS) {
                    if (seen_rest) {
                        error_and_exit("Only one rest parameter is allowed");
                    }
                    seen_rest = true;
                    is_rest = true;
                    next(); // consume '...'
                }

                const Token &p = peek();
                if (p.kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected parameter name");
                }
                std::string pname = next().text;

                // Parse optional type annotation: name: type or name: type[]
                nari::TypeAnnotationPtr param_type = nullptr;
                if (peek().kind == TokenKind::TK_COLON) {
                    next(); // consume ':'
                    if (peek().kind != TokenKind::TK_IDENT) {
                        error_and_exit("Expected type name after ':'");
                    }
                    std::string type_name = next().text;
                    bool is_array = false;
                    // Check for array syntax: type[]
                    if (peek().kind == TokenKind::TK_LBRACKET &&
                        peek(1).kind == TokenKind::TK_RBRACKET) {
                        next(); // consume '['
                        next(); // consume ']'
                        is_array = true;
                    }
                    param_type =
                        std::make_unique<nari::TypeAnnotation>(type_name, is_array);
                }

                ExprPtr default_value = nullptr;
                if (!is_rest && peek().kind == TokenKind::TK_EQUAL) {
                    next(); // consume '='
                    default_value = parse_expression();
                } else if (is_rest && peek().kind == TokenKind::TK_EQUAL) {
                    error_and_exit("Rest parameter cannot have a default value");
                }

                func_expr->params.emplace_back(pname, std::move(default_value), is_rest, std::move(param_type));

                if (is_rest) {
                    if (peek().kind == TokenKind::TK_COMMA) {
                        error_and_exit("Rest parameter must be last");
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
        expect(TokenKind::TK_RPAREN, "arrow function params end ')'");
        expect(TokenKind::TK_FATARROW, "'=>' in arrow function");

        // parse body: either a block or a single expression
        if (peek().kind == TokenKind::TK_LBRACE) {
            // block body: (x) => { return x + 1; }
            func_expr->body = parse_block();
        } else {
            // Expression body: (x) => x + 1
            // convert to implicit return
            ExprPtr body_expr = parse_expression();
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
    void check_statement_boundary() {
        if (idx == 0 || idx > toks.size()) {
            return;
        }
        const Token &last = toks[idx - 1];
        if (last.kind == TokenKind::TK_SEMICOLON || last.kind == TokenKind::TK_RBRACE) {
            return;
        }
        const Token &nt = peek();
        if (nt.kind == TokenKind::TK_EOF || nt.kind == TokenKind::TK_RBRACE ||
            nt.kind == TokenKind::TK_SEMICOLON) {
            return;
        }
        if (nt.kind == TokenKind::TK_IDENT &&
            (nt.text == "else" || nt.text == "case" || nt.text == "default")) {
            return;
        }
        if (nt.line == last.line) {
            error_and_exit("Expected ';' or newline between statements, found '" + token_desc(nt) + "'");
        }
    }

    StmtPtr parse_stmt() {
        StmtPtr stmt = parse_stmt_core();
        if (stmt) {
            check_statement_boundary();
        }
        return stmt;
    }

    StmtPtr parse_stmt_core() {
        const Token &tok = peek();
        if (tok.kind == TokenKind::TK_IDENT) {
            // control flow keywords
            if (tok.text == "if") {
                next(); // consume 'if'
                expect(TokenKind::TK_LPAREN, "if condition (");
                ExprPtr cond = parse_expression();
                expect(TokenKind::TK_RPAREN, "if condition )");
                StmtPtr then_branch = parse_stmt();
                if (!then_branch) {
                    error_and_exit("Expected statement or block after if condition");
                }
                StmtPtr else_branch = nullptr;
                if (peek().kind == TokenKind::TK_IDENT && peek().text == "else") {
                    next(); // consume 'else'
                    else_branch = parse_stmt();
                    if (!else_branch) {
                        error_and_exit("Expected statement or block after else");
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

                expect(TokenKind::TK_LPAREN, "while condition (");
                ExprPtr cond = parse_expression();
                expect(TokenKind::TK_RPAREN, "while condition )");
                StmtPtr body = parse_block();
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
                    expect(TokenKind::TK_LPAREN, "for (");
                    // optional 'let' keyword: for (let x of ...) or for (let x, y of ...)
                    if (peek().kind == TokenKind::TK_IDENT && peek().text == "let") {
                        next(); // consume 'let'
                    }
                    Token varTok = peek();
                    expect(TokenKind::TK_IDENT, "foreach variable");
                    // optional second variable: for (key, value in ...) or (key, value of ...)
                    std::string val_var;
                    if (peek().kind == TokenKind::TK_COMMA) {
                        next(); // consume ','
                        val_var = peek().text;
                        expect(TokenKind::TK_IDENT, "foreach value variable");
                    }
                    if (peek().kind != TokenKind::TK_IDENT || (peek().text != "in" && peek().text != "of")) {
                        error_and_exit("Expected 'in' or 'of' in for-each loop");
                    }
                    next(); // consume 'in' or 'of'
                    ExprPtr iterable = parse_expression();
                    expect(TokenKind::TK_RPAREN, "for )");
                    StmtPtr body = parse_block();
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
                expect(TokenKind::TK_LPAREN, "for (");
                // either a declaration or an expression statement (or empty)
                StmtPtr init = nullptr;
                if (peek().kind != TokenKind::TK_SEMICOLON) {
                    if (peek().kind == TokenKind::TK_IDENT && (peek().text == "let" || peek().text == "global" || peek().text == "const")) {
                        init = parse_stmt();
                    } else {
                        // Handle assignment (i = 0) or expression
                        if (peek().kind == TokenKind::TK_IDENT && peek(1).kind == TokenKind::TK_EQUAL) {
                            const Token &nameTok = peek();
                            std::string pname = next().text; // consume identifier
                            next();                          // consume '='

                            ExprPtr prhs = parse_expression();
                            auto assign = std::make_unique<nari::AssignStmt>(pname, std::move(prhs));
                            assign->line = nameTok.line;
                            assign->col = nameTok.col;
                            assign->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;
                            init = std::move(assign);
                        } else {
                            init = std::make_unique<nari::ExprStmt>(parse_expression());
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
                }
                expect(TokenKind::TK_SEMICOLON, "for ;");
                StmtPtr post = nullptr;
                if (peek().kind != TokenKind::TK_RPAREN) {
                    // allow either an expression (e.g. i = i + 1) or an assignment statement as the post clause
                    if (peek().kind == TokenKind::TK_IDENT && peek(1).kind == TokenKind::TK_EQUAL) {
                        const Token &nameTok = peek();
                        std::string pname = next().text; // consume identifier
                        next();                          // consume '='

                        ExprPtr prhs = parse_expression();
                        auto assign = std::make_unique<nari::AssignStmt>(pname, std::move(prhs));
                        assign->line = nameTok.line;
                        assign->col = nameTok.col;
                        assign->filename = nameTok.filename.empty() ? current_filename : nameTok.filename;
                        post = std::move(assign);
                    } else {
                        post = std::make_unique<nari::ExprStmt>(parse_expression());
                    }
                }
                expect(TokenKind::TK_RPAREN, "for )");
                StmtPtr body = parse_block();
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
                    }
                    next(); // consume "from"

                    if (peek().kind != TokenKind::TK_STRING) {
                        error_and_exit("Expected library path string after 'from'");
                    }
                    std::string lib_path = next().text;

                    // create `let var_name = __ffi_load_library("path");`
                    auto load_call = std::make_unique<nari::CallExpr>(std::make_unique<nari::IdentExpr>("__ffi_load_library"));
                    load_call->args.push_back(std::make_unique<nari::StringExpr>(lib_path));

                    if (peek().kind == TokenKind::TK_SEMICOLON) {
                        next();
                    }

                    auto var_decl = std::make_unique<nari::VarDeclStmt>(
                        var_name, std::move(load_call), VarDeclCtrl::LOCAL);
                    var_decl->line = importTok.line;
                    var_decl->col = importTok.col;
                    var_decl->filename = importTok.filename.empty() ? current_filename : importTok.filename;

                    return var_decl;
                } else {
                    error_and_exit("import inside functions only supports 'import name from \"library.so\"' syntax");
                }
            }

            if (tok.text == "var" && peek(1).kind == TokenKind::TK_IDENT &&
                (peek(2).kind == TokenKind::TK_EQUAL || peek(2).kind == TokenKind::TK_SEMICOLON)) {
                error_and_exit("'var' is not a valid variable declaration; use 'let', 'global', or 'const'");
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
                        }
                        names.push_back(next().text);

                        if (peek().kind == TokenKind::TK_COMMA) {
                            next(); // consume ','
                        } else if (peek().kind != TokenKind::TK_RBRACKET) {
                            error_and_exit("Expected ',' or ']' in array destructuring");
                        }
                    }
                    expect(TokenKind::TK_RBRACKET, "']' to close array destructuring");

                    ExprPtr init = nullptr;
                    if (peek().kind == TokenKind::TK_EQUAL) {
                        next(); // consume '='
                        init = parse_expression();
                    } else {
                        error_and_exit("Array destructuring requires initialization");
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
                        }
                        std::string key = next().text;
                        std::string name = key; // default: use same name

                        // check for key: name syntax
                        if (peek().kind == TokenKind::TK_COLON) {
                            next(); // consume ':'
                            if (peek().kind != TokenKind::TK_IDENT) {
                                error_and_exit("Expected identifier after ':' in object destructuring");
                            }
                            name = next().text;
                        }

                        bindings.emplace_back(key, name);

                        if (peek().kind == TokenKind::TK_COMMA) {
                            next(); // consume ','
                        } else if (peek().kind != TokenKind::TK_RBRACE) {
                            error_and_exit("Expected ',' or '}' in object destructuring");
                        }
                    }
                    expect(TokenKind::TK_RBRACE, "'}' to close object destructuring");

                    ExprPtr init = nullptr;
                    if (peek().kind == TokenKind::TK_EQUAL) {
                        next(); // consume '='
                        init = parse_expression();
                    } else {
                        error_and_exit("Object destructuring requires initialization");
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
                }
                std::string name = next().text;
                ExprPtr init = nullptr;
                if (peek().kind == TokenKind::TK_EQUAL) {
                    next(); // consume '='
                    init = parse_expression();
                }
                if (is_const && !init) {
                    error_and_exit("Const declaration requires initialization");
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
            if (next_kind == TokenKind::TK_PLUSEQ ||
                next_kind == TokenKind::TK_MINUSEQ ||
                next_kind == TokenKind::TK_STAREQ ||
                next_kind == TokenKind::TK_SLASHEQ ||
                next_kind == TokenKind::TK_PERCENTEQ ||
                next_kind == TokenKind::TK_AMPEQ ||
                next_kind == TokenKind::TK_PIPEEQ ||
                next_kind == TokenKind::TK_CARETEQ ||
                next_kind == TokenKind::TK_LSHIFTEQ ||
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
                        if (lookahead_idx < toks.size() &&
                            peek(lookahead_idx).kind == TokenKind::TK_IDENT) {
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
                expect(TokenKind::TK_EQUAL, "indexed assignment =");
                ExprPtr value = parse_expression();
                if (peek().kind == TokenKind::TK_SEMICOLON) {
                    next();
                }
                auto idxAssignStmt = std::make_unique<nari::IndexAssignStmt>(
                    std::move(target), std::move(value));
                idxAssignStmt->line = tok.line;
                idxAssignStmt->col = tok.col;
                idxAssignStmt->filename = tok.filename.empty() ? current_filename : tok.filename;
                return idxAssignStmt;
            }

            // call or expr statement
            if (peek(1).kind == TokenKind::TK_LPAREN) {
                ExprPtr e = parse_expression();
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
            return blk;
        } else if (tok.kind == TokenKind::TK_PLUSPLUS || tok.kind == TokenKind::TK_MINUSMINUS) {
            // handle prefix increment/decrement as statements
            ExprPtr e = parse_expression();
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
            unreachable();
        }
    }

    StmtPtr parse_switch_stmt() {
        const Token &kw = peek();
        expect(TokenKind::TK_IDENT, "switch");
        expect(TokenKind::TK_LPAREN, "switch (");
        ExprPtr value = parse_expression();
        expect(TokenKind::TK_RPAREN, "switch )");
        expect(TokenKind::TK_LBRACE, "switch { ");

        auto sw = std::make_unique<nari::SwitchStmt>(std::move(value));
        sw->line = kw.line;
        sw->col = kw.col;
        sw->filename = kw.filename.empty() ? current_filename : kw.filename;

        bool seen_default = false;
        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
            if (peek().kind != TokenKind::TK_IDENT) {
                error_and_exit("Expected 'case' or 'default' in switch");
            }

            if (peek().text == "case") {
                next(); // consume 'case'
                ExprPtr match = parse_expression();
                expect(TokenKind::TK_COLON, "case :");
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
                }
                seen_default = true;
                next(); // consume 'default'
                expect(TokenKind::TK_COLON, "default :");
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
        }

        expect(TokenKind::TK_RBRACE, "switch }");
        return sw;
    }

    // higher value = tighter binding
    enum Precedence {
        PREC_NONE = 0,
        PREC_TERNARY = 1,       // ? :
        PREC_NULL_COALESCE = 2, // ??
        PREC_LOGICAL_OR = 3,    // ||
        PREC_LOGICAL_AND = 4,   // &&
        PREC_BITWISE_OR = 5,    // |
        PREC_BITWISE_XOR = 6,   // ^
        PREC_BITWISE_AND = 7,   // &
        PREC_EQUALITY = 8,      // == !=
        PREC_COMPARISON = 9,    // < > <= >=
        PREC_SHIFT = 10,        // << >>
        PREC_TERM = 11,         // + - @
        PREC_FACTOR = 12,       // * / %
        PREC_EXPONENT = 13,     // **
        PREC_UNARY = 14,        // unary + - ! ~
        PREC_PRIMARY = 15
    };

    static Precedence token_precedence(const Token &t) {
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
            case TokenKind::TK_AT:
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

    // Guard against unbounded recursion in the expression parser when input
    // is deeply nested (e.g. `(((...(1)...)))` with thousands of parens)
    int expr_recursion_depth = 0;
    static constexpr int MAX_EXPR_RECURSION_DEPTH = 256;

    struct ExprRecursionGuard {
        int &depth;
        explicit ExprRecursionGuard(int &d) : depth(d) {
            ++depth;
        }
        ~ExprRecursionGuard() {
            --depth;
        }
    };

    ExprPtr parse_expression() {
        return fold_expr(parse_precedence(PREC_TERNARY));
    }

    ExprPtr parse_precedence(Precedence min_prec) {
        if (expr_recursion_depth >= MAX_EXPR_RECURSION_DEPTH) {
            error_and_exit("Expression nesting too deep");
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
                }
                expect(TokenKind::TK_COLON, "':' in ternary operator");
                ExprPtr false_expr = parse_precedence(static_cast<Precedence>(prec + 1));
                if (!false_expr) {
                    error_and_exit("Expected expression after ':'");
                }
                auto te = std::make_unique<nari::TernaryExpr>(
                    std::move(lhs), std::move(true_expr), std::move(false_expr));
                te->line = qmark.line;
                te->col = qmark.col;
                te->filename = qmark.filename.empty() ? current_filename : qmark.filename;
                lhs = std::move(te);
                continue;
            }

            Token op = t;
            next(); // consume operator

            // For left-assoc operators, the next minimum precedence is prec + 1
            Precedence next_min = static_cast<Precedence>(prec + 1);
            ExprPtr rhs = parse_precedence(next_min);
            if (!rhs) {
                error_and_exit("Expected RHS expression for binary operator");
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
                case TokenKind::TK_AT:
                    opname = "@";
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
    ExprPtr parse_unary() {
        const Token &t = peek();
        // `await expr` desugars to `expr.value`, handles block on .value access,
        // so this is just sugar for the existing handle-fetch pattern.
        if (t.kind == TokenKind::TK_IDENT && t.text == "await") {
            Token op = t;
            next();
            ExprPtr operand = parse_unary();
            if (!operand) {
                error_and_exit("Expected expression after 'await'");
            }
            auto me = std::make_unique<nari::MemberExpr>(std::move(operand), "value");
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
    ExprPtr parse_postfix() {
        ExprPtr expr = parse_primary();
        while (true) {
            const Token &tok = peek();
            if (tok.kind == TokenKind::TK_PLUSPLUS ||
                tok.kind == TokenKind::TK_MINUSMINUS) {
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
                            call->args.push_back(std::move(spread));
                            call->has_spread = true;
                        } else {
                            call->args.push_back(parse_expression());
                        }
                        if (peek().kind == TokenKind::TK_COMMA) {
                            next();
                            continue;
                        }
                        break;
                    }
                }
                expect(TokenKind::TK_RPAREN, "call end )");
                expr = std::move(call);
            } else if (tok.kind == TokenKind::TK_LBRACKET) {
                // index access: expr[index]
                Token op = tok;
                next(); // consume '['
                ExprPtr index = parse_expression();
                expect(TokenKind::TK_RBRACKET, "index access end ]");
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
                expect(TokenKind::TK_IDENT, "member name after .");
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
                    expect(TokenKind::TK_RBRACKET, "optional index access end ]");
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
                                call->args.push_back(std::move(spread));
                                call->has_spread = true;
                            } else {
                                call->args.push_back(parse_expression());
                            }
                            if (peek().kind == TokenKind::TK_COMMA) {
                                next();
                                continue;
                            }
                            break;
                        }
                    }
                    expect(TokenKind::TK_RPAREN, "optional call end )");
                    expr = std::move(call);
                } else {
                    // expr?.prop
                    Token memberTok = peek();
                    expect(TokenKind::TK_IDENT, "member name after ?.");
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
    nari::PatternPtr parse_pattern() {
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

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next(); // consume ','
                    } else if (peek().kind != TokenKind::TK_RPAREN) {
                        error_and_exit("Expected ',' or ')' in variant pattern");
                    }
                }
                expect(TokenKind::TK_RPAREN, "')' after variant fields");
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
                    }
                    pattern->named_bindings.push_back(next().text);

                    if (peek().kind == TokenKind::TK_COMMA) {
                        next(); // consume ','
                    } else if (peek().kind != TokenKind::TK_RBRACE) {
                        error_and_exit("Expected ',' or '}' in struct pattern");
                    }
                }
                expect(TokenKind::TK_RBRACE, "'}' after struct pattern fields");
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
        return nullptr;
    }

    // parse match expression itself: match value { pattern => expr, ... }
    ExprPtr parse_match_expression() {
        const Token &matchTok = peek();
        next(); // consume 'match'

        // parse the value being matched
        auto scrutinee = parse_expression();

        auto match_expr = std::make_unique<nari::MatchExpr>(std::move(scrutinee));
        match_expr->line = matchTok.line;
        match_expr->col = matchTok.col;
        match_expr->filename = matchTok.filename.empty() ? current_filename : matchTok.filename;

        expect(TokenKind::TK_LBRACE, "'{' after match scrutinee");

        while (!is_eof() && peek().kind != TokenKind::TK_RBRACE) {
            auto pattern = parse_pattern();

            // expect '=>' (either as FATARROW or as '=' followed by '>')
            if (peek().kind == TokenKind::TK_FATARROW) {
                next(); // consume '=>'
            } else if (peek().kind == TokenKind::TK_EQUAL &&
                       peek(1).kind == TokenKind::TK_GT) {
                next(); // consume '='
                next(); // consume '>'
            } else {
                error_and_exit("Expected '=>' after pattern in match arm");
            }

            auto body_expr = parse_expression();

            match_expr->arms.emplace_back(std::move(pattern), std::move(body_expr));

            // optional comma after arm
            if (peek().kind == TokenKind::TK_COMMA) {
                next();
            }
        }

        expect(TokenKind::TK_RBRACE, "'}' after match arms");

        if (match_expr->arms.empty()) {
            error_and_exit("Match expression must have at least one arm");
        }

        return match_expr;
    }

    ExprPtr parse_primary() {
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
                }
                std::string class_name = next().text;
                auto new_expr = std::make_unique<nari::NewExpr>(class_name);
                new_expr->line = tok.line;
                new_expr->col = tok.col;
                new_expr->filename = tok.filename.empty() ? current_filename : tok.filename;

                // parse constructor arguments
                expect(TokenKind::TK_LPAREN, "'(' for constructor arguments");
                if (peek().kind != TokenKind::TK_RPAREN) {
                    while (true) {
                        new_expr->args.push_back(parse_expression());
                        if (peek().kind == TokenKind::TK_COMMA) {
                            next();
                        } else if (peek().kind == TokenKind::TK_RPAREN) {
                            break;
                        } else {
                            error_and_exit("Expected ',' or ')' in constructor arguments");
                        }
                    }
                }
                expect(TokenKind::TK_RPAREN, "')' after constructor arguments");
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
            bool is_float = !is_hex_prefix && ((s.find('.') != std::string::npos) || (s.find('e') != std::string::npos) || (s.find('E') != std::string::npos));
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

                bool is_hex =
                    s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
                int base = is_hex ? 16 : 10;

                int64_t v = std::strtoll(s.c_str(), &endptr, base);
                if (endptr == s.c_str() || errno == ERANGE) {
                    if (is_hex) {
                        errno = 0;
                        endptr = nullptr;
                        unsigned long long uv = std::strtoull(s.c_str(), &endptr, 16);
                        if (endptr == s.c_str() || errno == ERANGE) {
                            num_expr = std::make_unique<nari::NumberExpr>(0.0);
                        } else if (uv <= static_cast<unsigned long long>(
                                             std::numeric_limits<int64_t>::max())) {
                            num_expr =
                                std::make_unique<nari::NumberExpr>(static_cast<int64_t>(uv));
                        } else {
                            num_expr =
                                std::make_unique<nari::NumberExpr>(static_cast<double>(uv));
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
            expect(TokenKind::TK_RPAREN, "closing )");
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
                        auto spread =
                            std::make_unique<nari::SpreadExpr>(parse_expression());
                        arr->elements.push_back(std::move(spread));
                        arr->has_spread = true;
                    } else {
                        arr->elements.push_back(parse_expression());
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
            expect(TokenKind::TK_RBRACKET, "array literal end ]");
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
                        }

                        expect(TokenKind::TK_COLON, "object literal :");

                        ExprPtr value;
                        if (peek().kind == TokenKind::TK_IDENT && peek().text == "func") {
                            // func(params) { ... }
                            value = parse_function_expression();
                        } else if (peek().kind == TokenKind::TK_LPAREN &&
                                   looks_like_arrow_function()) {
                            // (params) => { ... } arrow function expression
                            value = parse_arrow_function_expression();
                        } else {
                            // Regular expression
                            value = parse_expression();
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
            expect(TokenKind::TK_RBRACE, "object literal end }");
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
            return nullptr;
        }
    }
};

// pre-parse expression fragments in StringInterpolationExpr nodes so the
// runtime and bytecode compiler don't need to re-invoke the parser.

static void ri_stmt(nari::Stmt *s, std::vector<ParseError> *errors);
static void ri_block(nari::BlockStmt *blk, std::vector<ParseError> *errors);

static void ri_expr(nari::Expr *e, std::vector<ParseError> *errors) {
    if (!e) {
        return;
    }
    if (auto *sie = dynamic_cast<nari::StringInterpolationExpr *>(e)) {
        if (sie->exprs.empty() && !sie->expr_sources.empty()) {
            for (const auto &src : sie->expr_sources) {
                std::string previous_filename = current_filename;
                set_source_filename(sie->filename);
                FuncList funcs;
                if (errors) {
                    auto result = parse_program_recovering(src);
                    for (auto &error : result.errors) {
                        errors->push_back(std::move(error));
                    }
                    funcs = std::move(result.functions);
                } else {
                    funcs = parse_program_from_source(src);
                }
                set_source_filename(previous_filename);
                nari::ExprPtr parsed;
                if (funcs.size() >= 2 && funcs[1] && funcs[1]->body && !funcs[1]->body->stmts.empty()) {
                    if (auto *es = dynamic_cast<nari::ExprStmt *>(funcs[1]->body->stmts[0].get())) {
                        parsed = std::move(es->expr);
                    }
                }
                sie->exprs.push_back(std::move(parsed));
            }
        }
        return;
    }
    if (auto *be = dynamic_cast<nari::BinaryExpr *>(e)) {
        ri_expr(be->left.get(), errors);
        ri_expr(be->right.get(), errors);
        return;
    }
    if (auto *ue = dynamic_cast<nari::UnaryExpr *>(e)) {
        ri_expr(ue->operand.get(), errors);
        return;
    }
    if (auto *ce = dynamic_cast<nari::CallExpr *>(e)) {
        ri_expr(ce->callee.get(), errors);
        for (auto &a : ce->args) {
            ri_expr(a.get(), errors);
        }
        return;
    }
    if (auto *ae = dynamic_cast<nari::ArrayLiteralExpr *>(e)) {
        for (auto &el : ae->elements) {
            ri_expr(el.get(), errors);
        }
        return;
    }
    if (auto *oe = dynamic_cast<nari::ObjectLiteralExpr *>(e)) {
        for (auto &kv : oe->entries) {
            ri_expr(kv.second.get(), errors);
        }
        return;
    }
    if (auto *ie = dynamic_cast<nari::IndexExpr *>(e)) {
        ri_expr(ie->object.get(), errors);
        ri_expr(ie->index.get(), errors);
        return;
    }
    if (auto *me = dynamic_cast<nari::MemberExpr *>(e)) {
        ri_expr(me->object.get(), errors);
        return;
    }
    if (auto *ne = dynamic_cast<nari::NewExpr *>(e)) {
        for (auto &a : ne->args) {
            ri_expr(a.get(), errors);
        }
        return;
    }
    if (auto *te = dynamic_cast<nari::TernaryExpr *>(e)) {
        ri_expr(te->condition.get(), errors);
        ri_expr(te->true_expr.get(), errors);
        ri_expr(te->false_expr.get(), errors);
        return;
    }
    if (auto *fe = dynamic_cast<nari::FunctionExpr *>(e)) {
        for (auto &p : fe->params) {
            ri_expr(p.default_value.get(), errors);
        }
        ri_block(fe->body.get(), errors);
        return;
    }
    if (auto *se = dynamic_cast<nari::SpawnExpr *>(e)) {
        ri_block(se->body.get(), errors);
        return;
    }
    if (auto *me2 = dynamic_cast<nari::MatchExpr *>(e)) {
        ri_expr(me2->scrutinee.get(), errors);
        for (auto &arm : me2->arms) {
            ri_expr(arm.body.get(), errors);
        }
        return;
    }
}

static void ri_stmt(nari::Stmt *s, std::vector<ParseError> *errors) {
    if (!s) {
        return;
    }
    if (auto *es = dynamic_cast<nari::ExprStmt *>(s)) {
        ri_expr(es->expr.get(), errors);
        return;
    }
    if (auto *vd = dynamic_cast<nari::VarDeclStmt *>(s)) {
        ri_expr(vd->initializerExpr.get(), errors);
        return;
    }
    if (auto *as = dynamic_cast<nari::AssignStmt *>(s)) {
        ri_expr(as->value.get(), errors);
        return;
    }
    if (auto *ia = dynamic_cast<nari::IndexAssignStmt *>(s)) {
        ri_expr(ia->target.get(), errors);
        ri_expr(ia->value.get(), errors);
        return;
    }
    if (auto *rs = dynamic_cast<nari::ReturnStmt *>(s)) {
        ri_expr(rs->value.get(), errors);
        return;
    }
    if (auto *blk = dynamic_cast<nari::BlockStmt *>(s)) {
        ri_block(blk, errors);
        return;
    }
    if (auto *is = dynamic_cast<nari::IfStmt *>(s)) {
        ri_expr(is->cond.get(), errors);
        ri_stmt(is->then_branch.get(), errors);
        ri_stmt(is->else_branch.get(), errors);
        return;
    }
    if (auto *ws = dynamic_cast<nari::WhileStmt *>(s)) {
        ri_expr(ws->cond.get(), errors);
        ri_stmt(ws->body.get(), errors);
        return;
    }
    if (auto *fs = dynamic_cast<nari::ForStmt *>(s)) {
        ri_stmt(fs->init.get(), errors);
        ri_expr(fs->cond.get(), errors);
        ri_stmt(fs->post.get(), errors);
        ri_stmt(fs->body.get(), errors);
        return;
    }
    if (auto *fes = dynamic_cast<nari::ForEachStmt *>(s)) {
        ri_expr(fes->iterable.get(), errors);
        ri_stmt(fes->body.get(), errors);
        return;
    }
    if (auto *ss = dynamic_cast<nari::SwitchStmt *>(s)) {
        ri_expr(ss->value.get(), errors);
        for (auto &c : ss->cases) {
            ri_expr(c.match.get(), errors);
            ri_block(c.body.get(), errors);
        }
        ri_block(ss->default_body.get(), errors);
        return;
    }
}

static void ri_block(nari::BlockStmt *blk, std::vector<ParseError> *errors) {
    if (!blk) {
        return;
    }
    for (auto &s : blk->stmts) {
        ri_stmt(s.get(), errors);
    }
}

static void resolve_interp_exprs(std::vector<FunctionPtr> &funcs, std::vector<ParseError> *errors = nullptr) {
    for (auto &fn : funcs) {
        if (fn && fn->body) {
            ri_block(fn->body.get(), errors);
        }
    }
}

static ParseResult parse_impl(const std::string &src, bool create_aggregator, bool recover_mode) {
    std::vector<LexError> lex_errors;
    std::vector<Token> tokens_out = tokenize(src, current_filename, recover_mode ? &lex_errors : nullptr);
    if (!lex_errors.empty()) {
        std::vector<ParseError> errors;
        errors.reserve(lex_errors.size());
        for (auto &error : lex_errors) {
            errors.push_back({ std::move(error.filename), error.line, error.col, std::move(error.message) });
        }
        return ParseResult{ {}, std::move(errors) };
    }

    // invoke parser with the specified aggregator creation flag
    Parser parser(tokens_out, recover_mode);
    auto funcs = parser.parse_program(create_aggregator);
    return ParseResult{ std::move(funcs), parser.take_errors() };
}

std::vector<FunctionPtr> parse_program_from_source(const std::string &src, bool create_aggregator) {
    // standard mode: parser calls exit(1) on error, so errors is always empty.
    auto result = parse_impl(src, create_aggregator, false);
    resolve_interp_exprs(result.functions);
    return std::move(result.functions);
}

ParseResult parse_program_recovering(const std::string &src, bool create_aggregator) {
    auto result = parse_impl(src, create_aggregator, true);
    resolve_interp_exprs(result.functions, &result.errors);
    return result;
}

const std::vector<std::pair<std::string, std::string>> &get_pending_naric_imports() {
    return g_pending_naric_imports;
}

void clear_pending_naric_imports() {
    g_pending_naric_imports.clear();
    g_naric_import_counter = 0;
}

const std::vector<ModuleExportBinding> &get_module_exports(const std::string &module_filename) {
    auto it = module_export_registry.find(module_filename);
    if (it != module_export_registry.end()) {
        return it->second;
    }
    return kEmptyModuleExports;
}

std::string get_module_function_internal_name(const std::string &module_filename, const std::string &local_name) {
    auto mod_it = module_function_alias_registry.find(module_filename);
    if (mod_it == module_function_alias_registry.end()) {
        return "";
    }

    auto alias_it = mod_it->second.find(local_name);
    if (alias_it == mod_it->second.end()) {
        return "";
    }

    return alias_it->second;
}

std::string get_exported_function_local_name(const std::string &internal_name) {
    for (const auto &[module_filename, aliases] :
         module_function_alias_registry) {
        (void)module_filename;
        for (const auto &[local_name, aliased_name] : aliases) {
            if (aliased_name == internal_name) {
                return local_name;
            }
        }
    }
    return "";
}

std::string get_module_namespace_global_name(const std::string &module_filename) {
    auto it_ns = module_namespace_registry.find(module_filename);
    if (it_ns == module_namespace_registry.end()) {
        return "";
    }
    return it_ns->second;
}

const std::map<std::string, std::string> &get_module_namespace_registry() {
    return module_namespace_registry;
}

void clear_module_export_registry() {
    module_export_registry.clear();
    module_function_alias_registry.clear();
    module_namespace_registry.clear();
    g_module_export_counter = 0;
    g_module_namespace_counter = 0;
}

void reset_parse_session() {
    // Clear everything that accumulates across parse calls so that each independent session starts from a clean state.
    g_visited_files.clear();
    g_last_import_resolution_error.clear();
    g_pending_naric_imports.clear();
    g_naric_import_counter = 0;
    clear_module_export_registry();
    clear_type_registry();
}

std::string resolve_import_path(const std::string &inc, const std::string &basefile) {
    return resolve_include_path(inc, basefile);
}

} // namespace Parser
