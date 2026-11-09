#pragma once
// Analysis helpers shared by the LSP server translation units.
// (Declarations split from lsp/lsp_server.cpp; definitions live in lsp_analysis.cpp.)

#include "lsp_symbols.h"

#include "json_dom.h"
using json = JsonValue;

#ifndef DISABLE_PARSER
#include "ast.h"
#include "core_types.h"
#include "parser_api.h"
#endif

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct LocalDecl {
    std::string name;
    int line = 0;
    int col = 0;
};

#ifndef DISABLE_PARSER
void collect_expr_reads(const nari::Expr *e, std::unordered_set<std::string> &reads);
void collect_stmt_reads(const nari::Stmt *s, std::unordered_set<std::string> &reads);
void collect_stmts_reads(const std::vector<nari::StmtPtr> &stmts, std::unordered_set<std::string> &reads);
void collect_local_decls_stmts(const std::vector<nari::StmtPtr> &stmts, std::vector<LocalDecl> &out);
void collect_local_decls_stmt(const nari::Stmt *s, std::vector<LocalDecl> &out);
void emit_strict_type_warnings(const FuncList &funcs, const std::string &doc_filename, const std::vector<std::string> &source_lines, json &diagnostics);
void emit_unused_warnings(const FuncList &funcs, const std::string &doc_filename, const std::vector<std::string> &source_lines, json &diagnostics);
std::string build_func_sig(const nari::Function &fn, const std::string &name_override = "");
std::string build_method_sig(const nari::ClassMethod &method, const std::string &owner_class = "");
int find_identifier_col_in_line(const std::string &src_line, const std::string &word, size_t search_from = 0);
int resolve_identifier_col(const std::vector<std::string> &source_lines, const std::string &name, int ast_line, int ast_col, size_t search_from = 0);
std::string infer_annotation_type(const nari::TypeAnnotation *t);
std::string normalize_method_receiver_type(std::string type_name);
std::string infer_expr_type(const nari::Expr *e);
bool is_scope_owner_kind(int kind);
int find_enclosing_scope_line(const std::vector<SymInfo> &symbols, const std::string &source_file, int cursor_line_1);
const SymInfo *find_symbol_in_scope(const std::vector<SymInfo> &symbols, const std::string &name, const std::string &source_file, int cursor_line_1);
const ClassInfo *find_class_info(const std::vector<ClassInfo> &classes, const std::string &name);
const MemberInfo *find_class_member(const std::vector<ClassInfo> &classes, const std::string &class_name, const std::string &member_name, bool static_only, std::unordered_set<std::string> &visited);
const MemberInfo *find_class_member(const std::vector<ClassInfo> &classes, const std::string &class_name, const std::string &member_name, bool static_only = false);
std::filesystem::path find_package_declarations(const std::string &specifier, const std::string &source_file);
bool load_package_declarations(const std::filesystem::path &path, const std::string &binding_name, SymInfo &binding, std::vector<ClassInfo> &classes);
std::string infer_expr_type(const nari::Expr *e, const std::vector<SymInfo> &symbols, const std::vector<ClassInfo> &classes, const std::string &source_file, int line);
std::string resolve_receiver_class_name(const std::vector<SymInfo> &symbols, const std::vector<ClassInfo> &classes, const std::string &receiver, const std::string &source_file, int cursor_line_1);
std::string build_function_expr_sig(const std::string &name, const nari::FunctionExpr *fn);
std::vector<SymInfo> collect_object_literal_members(const nari::ObjectLiteralExpr *object_expr);
const ObjectLiteralInfo *find_object_literal_info(const std::vector<ObjectLiteralInfo> &infos, const std::vector<SymInfo> &symbols, const std::string &receiver, const std::string &source_file, int cursor_line_1);
void walk_stmts(const std::vector<nari::StmtPtr> &stmts, std::vector<SymInfo> &out, const StringMap &ns_to_path, const std::vector<std::string> &source_lines, int scope_fn_line = 0, std::vector<ObjectLiteralInfo> *object_literals = nullptr, std::vector<ClassInfo> *classes = nullptr, const std::string &source_file = "");
std::vector<SymInfo> collect_symbols(const FuncList &funcs, const std::string &doc_filename, const std::vector<std::string> &source_lines, std::vector<ObjectLiteralInfo> *object_literals = nullptr, std::vector<ClassInfo> *classes = nullptr);
std::vector<ClassInfo> collect_classes();
#endif // DISABLE_PARSER

std::vector<std::string> lsp_split_lines(const std::string &text);
int lsp_find_word_col(const std::string &src_line, const std::string &word, size_t from = 0);
std::pair<std::string, int> word_at(const std::string &text, int line0, int col0);
std::string extract_doc_comment(const std::vector<std::string> &lines, int sym_line_1based);
std::string extract_doc_comment(const std::string &text, int sym_line_1based);
std::string receiver_at(const std::string &text, int line0, int word_start);
std::string import_string_at(const std::string &text, int line0, int col0, int &out_start, int &out_end);
std::string path_to_uri(const std::filesystem::path &p);
std::filesystem::path lsp_std_module_file(const std::string &name);
