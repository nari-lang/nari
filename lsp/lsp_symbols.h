#pragma once
// Shared symbol-table types for the Nari language server.
// (Split verbatim from lsp/lsp_server.cpp.)

#include <map>
#include <string>
#include <vector>

// LSP CompletionItemKind values used in this server
enum LspCompletionKind {
    CK_Text = 1,
    CK_Method = 2,
    CK_Function = 3,
    CK_Constructor = 4,
    CK_Field = 5,
    CK_Variable = 6,
    CK_Class = 7,
    CK_Interface = 8,
    CK_Module = 9,
    CK_Property = 10,
    CK_Keyword = 14,
    CK_Enum = 13,
    CK_EnumMember = 20,
    CK_Struct = 22,
    CK_TypeParam = 25
};

struct SymInfo {
    std::string name;
    std::string detail;
    int kind = CK_Variable;
    int line = 0;
    int col = 0;
    std::string source_file; // path where symbol is declared
    std::string doc_comment; // leading // comment text
    int scope_fn_line = 0;   // 1-based start line of owning function (0 = top-level/global)
    // "string", "number", "bool", "regex", "array", "object", "function", "null", "" = unknown.
    std::string inferred_type;
    std::string owner_class;
    std::string call_return_type;
    std::string import_source_file; // module path for a named import
    std::string import_export_name; // exported name before any local alias
};

struct MemberInfo {
    std::string name;
    std::string detail;
    int line = 0;
    int col = 0;
    std::string source_file;
    bool is_method = false;
    bool is_public = true;
    bool is_static = false;
    std::string return_type;
    std::string doc_comment;
    std::vector<std::string> parameter_types;
};

struct ClassInfo {
    std::string name;
    int line = 0, col = 0;
    std::string source_file;
    std::string parent_name;
    std::vector<MemberInfo> members;
};

struct ObjectLiteralInfo {
    std::string name;
    int decl_line = 0;
    int scope_fn_line = 0;
    std::string source_file;
    std::vector<SymInfo> members;
};

struct DocState {
    std::string content;
    std::size_t content_hash = 0; // latest editor content
    std::size_t parsed_hash = 0;  // last content version with fresh symbols/diagnostics
    std::vector<SymInfo> symbols;
    std::vector<ClassInfo> classes;
    std::vector<ObjectLiteralInfo> object_literals;
};
