#pragma once
// Internal cross-TU declarations for the parser.
// Module/session state is defined in parser.cpp, type registries in
// type_registry.cpp; the registry accessors and session reset live in
// module_registry.cpp. Most function declarations come from parser_api.h;
// this header covers shared mutable state plus functions with no public API.
#include "ast.h"
#include "parser_api.h"
#include "token.h"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Parser {

extern thread_local std::map<std::string, std::vector<ModuleExportBinding>> module_export_registry;
extern thread_local std::map<std::string, StringMap> module_function_alias_registry;
extern thread_local StringMap module_namespace_registry;
extern thread_local int g_module_export_counter;
extern thread_local int g_module_namespace_counter;
extern thread_local std::vector<std::pair<std::string, std::string>> g_pending_naric_imports;
extern thread_local int g_naric_import_counter;
extern thread_local std::unordered_set<std::string> g_visited_files;
extern thread_local std::string g_last_import_resolution_error;

// Token descriptions + type/enum/class registries (defined in type_registry.cpp).
extern thread_local std::string current_filename;
extern thread_local std::vector<std::string> error_contexts;
extern thread_local std::vector<nari::TypeDeclPtr> type_declarations;
extern thread_local std::map<std::string, const nari::TypeDecl *> type_registry;
extern thread_local std::vector<nari::EnumDeclPtr> enum_declarations;
extern thread_local std::map<std::string, const nari::EnumDecl *> enum_registry;
extern thread_local std::vector<nari::ClassDeclPtr> class_declarations;
extern thread_local std::map<std::string, const nari::ClassDecl *> class_registry;
extern thread_local ValuesList static_class_fields;
extern thread_local std::unordered_set<std::string> static_inited_classes;
std::string token_desc(const Token &tok);
std::vector<nari::FunctionPtr> generate_enum_constructors(const nari::EnumDecl &decl);
std::string current_module_key();
void resolve_interp_exprs(std::vector<nari::FunctionPtr> &funcs, std::vector<ParseError> *errors = nullptr);
bool is_std_virtual_path(const std::string &path);
std::string resolve_include_path(const std::string &inc, const std::string &basefile);
void register_enum(nari::EnumDeclPtr enum_decl);
void register_class(nari::ClassDeclPtr class_decl);

} // namespace Parser
