/*
  parser/module_resolver.cpp
  Package / module import path resolution, split out of parser.cpp.
  These are pure path + TOML helpers: they walk the filesystem for nari.toml /
  nari.lock.toml manifests and resolve a bare package spec to a file path. They
  have no dependency on the token stream or AST.
*/

#include "module_resolver.h"

#include "../nari_fs.h"
#include "../util.h"

#include <ctype.h>
#include <map>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <vector>

namespace Parser {

static std::string unquote_toml(const std::string &text) {
    std::string trimmed = trim_ascii(text);
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        return trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed;
}

static std::string strip_toml_comment(const std::string &line) {
    bool in_string = false;
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        if (ch == '"' && (i == 0 || line[i - 1] != '\\')) {
            in_string = !in_string;
            continue;
        }
        if (ch == '#' && !in_string) {
            return line.substr(0, i);
        }
    }
    return line;
}

static size_t find_unquoted_char(const std::string &text, char needle) {
    bool in_string = false;
    int depth_brace = 0;
    int depth_bracket = 0;
    for (size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if (ch == '"' && (i == 0 || text[i - 1] != '\\')) {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (ch == '{') {
            depth_brace++;
        } else if (ch == '}') {
            depth_brace--;
        } else if (ch == '[') {
            depth_bracket++;
        } else if (ch == ']') {
            depth_bracket--;
        } else if (ch == needle && depth_brace == 0 && depth_bracket == 0) {
            return i;
        }
    }
    return std::string::npos;
}

static std::vector<std::string> split_top_level(const std::string &text, char separator) {
    std::vector<std::string> parts;
    bool in_string = false;
    int depth_brace = 0;
    int depth_bracket = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if (ch == '"' && (i == 0 || text[i - 1] != '\\')) {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (ch == '{') {
            depth_brace++;
        } else if (ch == '}') {
            depth_brace--;
        } else if (ch == '[') {
            depth_bracket++;
        } else if (ch == ']') {
            depth_bracket--;
        } else if (ch == separator && depth_brace == 0 && depth_bracket == 0) {
            parts.push_back(trim_ascii(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    std::string tail = trim_ascii(text.substr(start));
    if (!tail.empty()) {
        parts.push_back(tail);
    }
    return parts;
}

static std::string read_text_file(const nari::fs::Path &path) {
    FILE *fp = fopen(path.string().c_str(), "rb");
    if (!fp) {
        return {};
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::string content;
    if (file_size > 0) {
        content.resize(static_cast<size_t>(file_size));
        size_t bytes_read = fread(&content[0], 1, static_cast<size_t>(file_size), fp);
        content.resize(bytes_read);
    }
    fclose(fp);
    return content;
}

static bool starts_with(const std::string &text, const std::string &prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool is_package_import_spec(const std::string &inc) {
    if (inc.empty()) {
        return false;
    }
    if (starts_with(inc, "./") || starts_with(inc, "../") || inc[0] == '/' || inc[0] == '\\') {
        return false;
    }
    static const char *exts[] = { ".nari", ".naric", ".so", ".dll", ".dylib" };
    for (const char *ext : exts) {
        size_t ext_len = std::string(ext).size();
        if (inc.size() >= ext_len && inc.substr(inc.size() - ext_len) == ext) {
            return false;
        }
    }
    return true;
}

static nari::fs::Path find_nearest_project_root(const nari::fs::Path &basefile) {
    namespace fs = nari::fs;
    // Always resolve to an absolute path so that walking up via parent_path()
    // reaches the filesystem root rather than stopping at an empty relative path
    fs::Path dir = basefile.empty() ? fs::current_path() : fs::absolute(basefile);
    if (!fs::is_directory(dir)) {
        dir = dir.parent_path();
    }
    if (dir.empty()) {
        dir = fs::current_path();
    }
    dir = dir.lexically_normal();
    while (!dir.empty()) {
        if (fs::exists(dir / "nari.toml")) {
            return dir;
        }
        fs::Path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

typedef std::map<std::string, std::string> StringMap;

static StringMap parse_manifest_dependency_paths(const nari::fs::Path &manifest_path) {
    StringMap result;
    std::string content = read_text_file(manifest_path);
    std::stringstream ss(content);
    std::string line;
    std::string section;
    while (std::getline(ss, line)) {
        std::string cleaned = trim_ascii(strip_toml_comment(line));
        if (cleaned.empty()) {
            continue;
        }
        if (cleaned.front() == '[' && cleaned.back() == ']') {
            section = trim_ascii(cleaned.substr(1, cleaned.size() - 2));
            continue;
        }
        if (section != "dependencies") {
            continue;
        }
        size_t eq = find_unquoted_char(cleaned, '=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string dep_name = unquote_toml(cleaned.substr(0, eq));
        std::string raw_spec = trim_ascii(cleaned.substr(eq + 1));
        if (raw_spec.size() < 2 || raw_spec.front() != '{' || raw_spec.back() != '}') {
            continue;
        }
        std::string inner = trim_ascii(raw_spec.substr(1, raw_spec.size() - 2));
        for (const std::string &part : split_top_level(inner, ',')) {
            size_t inner_eq = find_unquoted_char(part, '=');
            if (inner_eq == std::string::npos) {
                continue;
            }
            std::string key = unquote_toml(part.substr(0, inner_eq));
            if (key != "path") {
                continue;
            }
            result[dep_name] = unquote_toml(part.substr(inner_eq + 1));
            break;
        }
    }
    return result;
}

static StringMap parse_package_exports(const nari::fs::Path &manifest_path) {
    StringMap result;
    std::string content = read_text_file(manifest_path);
    std::stringstream ss(content);
    std::string line;
    std::string section;
    while (std::getline(ss, line)) {
        std::string cleaned = trim_ascii(strip_toml_comment(line));
        if (cleaned.empty()) {
            continue;
        }
        if (cleaned.front() == '[' && cleaned.back() == ']') {
            section = trim_ascii(cleaned.substr(1, cleaned.size() - 2));
            continue;
        }
        if (section != "exports") {
            continue;
        }
        size_t eq = find_unquoted_char(cleaned, '=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = unquote_toml(cleaned.substr(0, eq));
        std::string value = unquote_toml(cleaned.substr(eq + 1));
        if (!key.empty() && !value.empty()) {
            result[key] = value;
        }
    }
    return result;
}

std::string resolve_package_directory_path(const std::string &path, std::string &error_out) {
    namespace fs = nari::fs;
    fs::Path package_root(path);
    if (!fs::is_directory(package_root)) {
        return path;
    }

    fs::Path manifest = package_root / "nari.toml";
    if (!fs::is_regular_file(manifest)) {
        error_out = "Import path '" + package_root.lexically_normal().string() + "' is a directory without a nari.toml manifest";
        return "";
    }

    StringMap exports = parse_package_exports(manifest);
    auto root_export = exports.find(".");
    if (root_export == exports.end()) {
        error_out = "Package directory '" + package_root.lexically_normal().string() + "' does not define the root export '.'";
        return "";
    }

    return (package_root / fs::Path(root_export->second)).lexically_normal().string();
}

// `store_path` is the absolute install location npkg recorded on this machine;
// `version` + `integrity` are the portable fields the store location
// can be derived from when the lockfile came from another machine
struct LockfilePackage {
    std::string store_path;
    std::string version;
    std::string integrity;
};

static std::map<std::string, LockfilePackage> parse_lockfile_packages(const nari::fs::Path &lockfile_path) {
    std::map<std::string, LockfilePackage> result;
    std::string content = read_text_file(lockfile_path);
    std::stringstream ss(content);
    std::string line;
    std::string current_package;
    while (std::getline(ss, line)) {
        std::string cleaned = trim_ascii(strip_toml_comment(line));
        if (cleaned.empty()) {
            continue;
        }
        if (cleaned.front() == '[' && cleaned.back() == ']') {
            current_package.clear();
            std::string section = trim_ascii(cleaned.substr(1, cleaned.size() - 2));
            if (!starts_with(section, "packages.")) {
                continue;
            }
            std::string rest = section.substr(std::string("packages.").size());
            if (rest.empty() || rest[0] != '"') {
                continue;
            }
            size_t quote_end = 1;
            while (quote_end < rest.size()) {
                if (rest[quote_end] == '"' && rest[quote_end - 1] != '\\') {
                    break;
                }
                quote_end++;
            }
            if (quote_end >= rest.size()) {
                continue;
            }
            std::string trailing = rest.substr(quote_end + 1);
            if (!trailing.empty()) {
                continue;
            }
            current_package = rest.substr(1, quote_end - 1);
            continue;
        }
        if (current_package.empty()) {
            continue;
        }
        size_t eq = find_unquoted_char(cleaned, '=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = unquote_toml(cleaned.substr(0, eq));
        std::string value = unquote_toml(cleaned.substr(eq + 1));
        if (key == "storePath") {
            result[current_package].store_path = value;
        } else if (key == "version") {
            result[current_package].version = value;
        } else if (key == "integrity") {
            result[current_package].integrity = value;
        }
    }
    return result;
}

// $NARI_HOME if set, else ~/.nari. This is the interpreter's only piece of
// built-in knowledge about where npkg keeps its store; everything below it
// is derived from lockfile fields.
static std::string nari_home_dir() {
    const char *nari_home = getenv("NARI_HOME");
    if (nari_home && *nari_home) {
        return nari_home;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) {
        home = getenv("USERPROFILE");
    }
#endif
    if (!home || !*home) {
        return {};
    }
    return (nari::fs::Path(home) / ".nari").string();
}

// Re-derive the store directory npkg would have installed a package into:
//   <nari_home>/store/pkg/<name with '/' -> '-'>@<version>-<hash8> (first 8 chars of integrity)
static nari::fs::Path derived_store_package_dir(const std::string &package_name, const LockfilePackage &locked) {
    if (locked.version.empty()) {
        return {};
    }
    std::string home = nari_home_dir();
    if (home.empty()) {
        return {};
    }
    std::string dir_name = package_name;
    for (char &ch : dir_name) {
        if (ch == '/') {
            ch = '-';
        }
    }
    dir_name += "@";
    dir_name += locked.version;
    size_t dash = locked.integrity.find('-');
    if (dash != std::string::npos && locked.integrity.size() - dash - 1 >= 8) {
        dir_name += "-";
        dir_name += locked.integrity.substr(dash + 1, 8);
    }
    return (nari::fs::Path(home) / "store" / "pkg" / dir_name).lexically_normal();
}

static std::string package_short_name(const std::string &package_name) {
    size_t slash = package_name.rfind('/');
    if (slash == std::string::npos) {
        return package_name;
    }
    return package_name.substr(slash + 1);
}

// Walk upward from `start_dir` looking for the nearest nari.lock.toml.
// Used so workspace members can resolve through the lockfile that lives at
// the workspace root rather than next to their own nari.toml.
static nari::fs::Path find_nearest_lockfile(const nari::fs::Path &start_dir) {
    namespace fs = nari::fs;
    fs::Path dir = start_dir.lexically_normal();
    while (!dir.empty()) {
        fs::Path candidate = dir / "nari.lock.toml";
        if (fs::exists(candidate)) {
            return candidate;
        }
        fs::Path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

std::string resolve_package_import_path(const std::string &inc, const std::string &basefile, std::string &error_out) {
    namespace fs = nari::fs;
    fs::Path base_path = basefile.empty() ? fs::current_path() : fs::Path(basefile);
    fs::Path project_root = find_nearest_project_root(base_path);
    if (project_root.empty()) {
        error_out = "Package import '" + inc + "' could not be resolved because no enclosing nari.toml was found";
        return "";
    }

    StringMap dependency_paths = parse_manifest_dependency_paths(project_root / "nari.toml");
    std::map<std::string, LockfilePackage> lockfile_packages;
    fs::Path lockfile_path = find_nearest_lockfile(project_root);
    if (!lockfile_path.empty()) {
        lockfile_packages = parse_lockfile_packages(lockfile_path);
    }

    StringMap alias_map;
    std::map<std::string, bool> alias_ambiguous;
    auto register_alias = [&](const std::string &canonical_name) {
        std::string alias = package_short_name(canonical_name);
        if (alias.empty() || alias == canonical_name) {
            return;
        }
        auto it = alias_map.find(alias);
        if (it == alias_map.end()) {
            alias_map[alias] = canonical_name;
            alias_ambiguous[alias] = false;
        } else if (it->second != canonical_name) {
            alias_ambiguous[alias] = true;
        }
    };

    for (const auto &[package_name, _] : dependency_paths) {
        register_alias(package_name);
    }
    for (const auto &[package_name, _] : lockfile_packages) {
        register_alias(package_name);
    }

    std::string matched_package;
    std::string matched_prefix;
    auto try_match = [&](const std::string &prefix, const std::string &canonical_name) {
        if (inc == prefix || starts_with(inc, prefix + "/")) {
            if (prefix.size() > matched_prefix.size()) {
                matched_prefix = prefix;
                matched_package = canonical_name;
            }
        }
    };

    for (const auto &[package_name, _] : dependency_paths) {
        try_match(package_name, package_name);
    }
    for (const auto &[package_name, _] : lockfile_packages) {
        try_match(package_name, package_name);
    }
    for (const auto &[alias, canonical_name] : alias_map) {
        auto amb_it = alias_ambiguous.find(alias);
        if (amb_it != alias_ambiguous.end() && amb_it->second) {
            continue;
        }
        try_match(alias, canonical_name);
    }

    if (matched_package.empty()) {
        error_out = "Package import '" + inc + "' is not listed in " + (project_root / "nari.toml").lexically_normal().string();
        return "";
    }

    std::string export_key = ".";
    if (inc.size() > matched_prefix.size()) {
        export_key = inc.substr(matched_prefix.size() + 1);
    }

    fs::Path package_root;
    fs::Path derived_dir;
    auto lock_it = lockfile_packages.find(matched_package);
    if (lock_it != lockfile_packages.end()) {
        const LockfilePackage &locked = lock_it->second;
        derived_dir = derived_store_package_dir(matched_package, locked);
        // A storePath that is valid on this machine is prioritized, otherwise re-derive
        // the store location from the portable lockfile fields.
        if (!locked.store_path.empty() && fs::exists(fs::Path(locked.store_path) / "nari.toml")) {
            package_root = fs::Path(locked.store_path);
        } else if (!derived_dir.empty() && fs::exists(derived_dir / "nari.toml")) {
            package_root = derived_dir;
        } else if (!locked.store_path.empty()) {
            package_root = fs::Path(locked.store_path);
        }
    }
    if (package_root.empty()) {
        auto dep_it = dependency_paths.find(matched_package);
        if (dep_it != dependency_paths.end()) {
            package_root = (project_root / fs::Path(dep_it->second)).lexically_normal();
        }
    }

    if (package_root.empty()) {
        error_out = "Package import '" + inc + "' matched dependency '" + matched_package +
                    "' but no installed package or local path could be resolved";
        if (!derived_dir.empty()) {
            error_out += " (looked for " + derived_dir.string() + ")";
        }
        return "";
    }

    fs::Path package_manifest = package_root / "nari.toml";
    if (!fs::exists(package_manifest)) {
        error_out =
            "Package import '" + inc + "' resolved to '" + package_root.lexically_normal().string() + "' but no nari.toml was found there";
        if (!derived_dir.empty() && derived_dir != package_root) {
            error_out += " (also looked for " + derived_dir.string() + ")";
        }
        return "";
    }

    StringMap exports = parse_package_exports(package_manifest);
    auto export_it = exports.find(export_key);
    if (export_it == exports.end()) {
        error_out = "Package import '" + inc + "' could not be resolved because dependency '" + matched_package +
                    "' does not export subpath '" + export_key + "'";
        return "";
    }

    fs::Path resolved_path = (package_root / fs::Path(export_it->second)).lexically_normal();
    return resolved_path.string();
}

} // namespace Parser
