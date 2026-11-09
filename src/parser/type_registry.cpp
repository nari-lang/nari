// Token descriptions + type/enum/class registries (split out of parser.cpp).
#include "ast.h"
#include "parser_api.h"
#include "parser_internal.h"
#include "token.h"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Parser {

std::string token_desc(const Token &tok) {
    if (tok.text.empty()) {
        return "<empty>";
    }
    return tok.text;
}

// every mutable global is thread_local so that two threads can run independent parse sessions at the same time
thread_local std::string current_filename;
thread_local std::vector<std::string> error_contexts;

thread_local std::vector<nari::TypeDeclPtr> type_declarations;
thread_local std::map<std::string, const nari::TypeDecl *> type_registry;

thread_local std::vector<nari::EnumDeclPtr> enum_declarations;
thread_local std::map<std::string, const nari::EnumDecl *> enum_registry;

thread_local std::vector<nari::ClassDeclPtr> class_declarations;
thread_local std::map<std::string, const nari::ClassDecl *> class_registry;
thread_local ValuesList static_class_fields;
thread_local std::unordered_set<std::string> static_inited_classes;
thread_local std::map<std::string, std::vector<ModuleExportBinding>> module_export_registry;
thread_local std::map<std::string, StringMap> module_function_alias_registry;
thread_local StringMap module_namespace_registry;
thread_local int g_module_export_counter = 0;
thread_local int g_module_namespace_counter = 0;

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
std::vector<nari::FunctionPtr> generate_enum_constructors(const nari::EnumDecl &decl) {
    std::vector<nari::FunctionPtr> result;
    for (const auto &variant : decl.variants) {
        auto fn = std::make_unique<nari::Function>(variant.name);
        fn->line = decl.line;
        fn->col = decl.col;
        fn->is_enum_ctor = true;

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

thread_local std::unordered_set<std::string> g_visited_files;

// pending pre-compiled .naric module imports collected during parse.
// each entry is { init_func_name, resolved_file_path }.
thread_local std::vector<std::pair<std::string, std::string>> g_pending_naric_imports;
thread_local int g_naric_import_counter = 0;
thread_local std::string g_last_import_resolution_error;

std::string current_module_key() {
    return current_filename.empty() ? std::string("<input>") : current_filename;
}

} // namespace Parser
