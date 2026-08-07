#pragma once

#include "core_types.h"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

typedef std::unordered_map<std::string, Value> ValuesList;

namespace Parser {

struct ModuleExportBinding {
    std::string export_name;
    std::string local_name;
};

struct ParseError {
    std::string filename;
    int line = 0;
    int col = 0;
    std::string message;
};

// return value for the recovering parser: partial AST + all collected errors.
struct ParseResult {
    FuncList functions;
    std::vector<ParseError> errors;
    bool ok() const {
        return errors.empty();
    }
};

// sets the source filename used by the lexer/parser for diagnostics.
void set_source_filename(const std::string &filename);
void push_error_context(const std::string &ctx);
void pop_error_context();

// standard parse: exits the process on the first error
// NOTE: this will *probably* be removed in the future
FuncList parse_program_from_source(const std::string &src, bool create_aggregator = true);

// recovering parse: collects all errors, returns a ParseResult.
// returned AST is partial when errors.size() > 0. 
// statements that triggered errors are skipped and parsing resumes at the next boundary.
ParseResult parse_program_recovering(const std::string &src, bool create_aggregator = true);

// type registry stuff

bool is_registered_type(const std::string &name);
const nari::TypeDecl *get_registered_type(const std::string &name);
bool is_registered_enum(const std::string &name);
bool is_registered_class(const std::string &name);
const nari::ClassDecl *get_registered_class(const std::string &name);
const std::map<std::string, const nari::ClassDecl *> &get_all_registered_classes();
ValuesList &get_static_fields();
std::unordered_set<std::string> &get_static_inited_classes();
void register_type(nari::TypeDeclPtr type_decl);
const std::map<std::string, const nari::TypeDecl *> &get_all_registered_types();
void clear_type_registry();

// pre-compiled (.naric) module imports
// Returns list of {init_func_name, file_path} pairs collected during the last parse
const std::vector<std::pair<std::string, std::string>> &get_pending_naric_imports();
void clear_pending_naric_imports();

// module exports / aliases
const std::vector<ModuleExportBinding> &get_module_exports(const std::string &module_filename);
std::string get_module_function_internal_name(const std::string &module_filename, const std::string &local_name);
std::string get_exported_function_local_name(const std::string &internal_name);
std::string get_module_namespace_global_name(const std::string &module_filename);
// Returns a map of { module_file_path -> namespace_global_name } populated after the most recent parse
const StringMap &get_module_namespace_registry();
// clears module export registry, self explanatory, automatically called by reset_parse_session()
void clear_module_export_registry();

// reset all persistent parse-session state, also call after a completed parse+compile+run cycle to avoid stale state
void reset_parse_session();

// resolve an import specifier (like "libfoo", "./utils.nari", etc.)
std::string resolve_import_path(const std::string &inc, const std::string &basefile);

} // namespace Parser
