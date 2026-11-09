// Module export registry + parse session state
#include "parser_api.h"
#include "parser_internal.h"

#include <utility>

namespace Parser {

static const std::vector<ModuleExportBinding> kEmptyModuleExports;

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
    for (const auto &[module_filename, aliases] : module_function_alias_registry) {
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

const StringMap &get_module_namespace_registry() {
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

} // namespace Parser
