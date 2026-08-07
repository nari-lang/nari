#include "nari_fs.h"

#include <algorithm>
#include <cctype>
#include <utility>

#if defined(NARI_WINDOWS_WIN7_COMPAT)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <filesystem>
#endif

namespace nari::fs {

Path::Path(const char *value) : value_(value ? value : "") {
}

Path::Path(const std::string &value) : value_(value) {
}

Path::Path(std::string &&value) : value_(std::move(value)) {
}

bool Path::empty() const {
    return value_.empty();
}

std::string Path::string() const {
    return value_;
}

std::string Path::generic_string() const {
    return value_;
}

const std::string &Path::native() const {
    return value_;
}

#if defined(NARI_WINDOWS_WIN7_COMPAT)

namespace {

bool is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

bool has_drive_prefix(const std::string &path) {
    return path.size() >= 2 && std::isalpha((unsigned char)path[0]) && path[1] == ':';
}

std::wstring utf8_to_wide(const std::string &value) {
    if (value.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::vector<wchar_t> wide((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), size);
    wide.pop_back();
    return std::wstring(wide.begin(), wide.end());
}

std::string wide_to_utf8(const std::wstring &value) {
    if (value.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::vector<char> utf8((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    utf8.pop_back();
    return std::string(utf8.begin(), utf8.end());
}

std::string normalize_component_separators(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

struct ParsedPath {
    std::string root;
    std::vector<std::string> parts;
    bool had_trailing_sep = false;
};

ParsedPath parse_path(const std::string &raw) {
    ParsedPath parsed;
    std::string path = normalize_component_separators(raw);
    parsed.had_trailing_sep = path.size() > 1 && path.back() == '/';

    size_t index = 0;
    if (has_drive_prefix(path)) {
        parsed.root = path.substr(0, 2);
        index = 2;
        if (index < path.size() && path[index] == '/') {
            parsed.root += "/";
            while (index < path.size() && path[index] == '/') {
                index++;
            }
        }
    } else if (path.size() >= 2 && path[0] == '/' && path[1] == '/') {
        parsed.root = "//";
        index = 2;
        while (index < path.size() && path[index] == '/') {
            index++;
        }
    } else if (!path.empty() && path[0] == '/') {
        parsed.root = "/";
        index = 1;
        while (index < path.size() && path[index] == '/') {
            index++;
        }
    }

    while (index < path.size()) {
        size_t next = index;
        while (next < path.size() && path[next] != '/') {
            next++;
        }
        if (next > index) {
            parsed.parts.push_back(path.substr(index, next - index));
        }
        while (next < path.size() && path[next] == '/') {
            next++;
        }
        index = next;
    }

    return parsed;
}

std::string build_path(const ParsedPath &parsed) {
    std::string out = parsed.root;
    for (const std::string &part : parsed.parts) {
        if (!out.empty() && out.back() != '/') {
            out.push_back('/');
        }
        out += part;
    }

    if (out.empty()) {
        if (parsed.parts.empty()) {
            return {};
        }
        return parsed.parts.front();
    }

    if (parsed.had_trailing_sep && !parsed.parts.empty() && out.back() != '/') {
        out.push_back('/');
    }

    return out;
}

std::string normalize_path(const std::string &raw) {
    ParsedPath parsed = parse_path(raw);
    std::vector<std::string> normalized;
    for (const std::string &part : parsed.parts) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!normalized.empty() && normalized.back() != "..") {
                normalized.pop_back();
            } else if (parsed.root.empty()) {
                normalized.push_back(part);
            }
            continue;
        }
        normalized.push_back(part);
    }
    parsed.parts = std::move(normalized);
    parsed.had_trailing_sep = false;
    std::string out = build_path(parsed);
    if (out.empty() && parsed.root.empty()) {
        return ".";
    }
    if (out.empty()) {
        return parsed.root;
    }
    return out;
}

Path make_win32_path_result(DWORD(WINAPI *fn)(DWORD, LPWSTR), DWORD initial_size = MAX_PATH) {
    DWORD size = initial_size;
    std::vector<wchar_t> buffer((size_t)size, L'\0');
    DWORD written = fn(size, buffer.data());
    if (written == 0) {
        return {};
    }
    if (written >= size) {
        size = written + 1;
        buffer.assign((size_t)size, L'\0');
        written = fn(size, buffer.data());
        if (written == 0 || written >= size) {
            return {};
        }
    }
    return Path(normalize_path(wide_to_utf8(std::wstring(buffer.data(), written))));
}

Path get_full_path(const Path &path, std::error_code *ec) {
    std::wstring wide = utf8_to_wide(path.string());
    if (wide.empty() && !path.empty()) {
        if (ec) {
            *ec = std::make_error_code(std::errc::invalid_argument);
        }
        return path.lexically_normal();
    }

    DWORD required = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        if (ec) {
            *ec = std::error_code((int)GetLastError(), std::system_category());
        }
        return path.lexically_normal();
    }

    std::vector<wchar_t> buffer((size_t)required, L'\0');
    DWORD written = GetFullPathNameW(wide.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        if (ec) {
            *ec = std::error_code((int)GetLastError(), std::system_category());
        }
        return path.lexically_normal();
    }

    if (ec) {
        ec->clear();
    }
    return Path(normalize_path(wide_to_utf8(std::wstring(buffer.data(), written))));
}

DWORD path_attributes(const Path &path, std::error_code *ec) {
    std::wstring wide = utf8_to_wide(path.string());
    if (wide.empty() && !path.empty()) {
        if (ec) {
            *ec = std::make_error_code(std::errc::invalid_argument);
        }
        return INVALID_FILE_ATTRIBUTES;
    }

    DWORD attrs = GetFileAttributesW(wide.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES && ec) {
        *ec = std::error_code((int)GetLastError(), std::system_category());
    } else if (ec) {
        ec->clear();
    }
    return attrs;
}

} // namespace

bool Path::is_absolute() const {
    if (value_.empty()) {
        return false;
    }
    std::string path = normalize_component_separators(value_);
    bool has_absolute_drive = has_drive_prefix(path) && path.size() >= 3 && path[2] == '/';
    return has_absolute_drive || (path.size() >= 2 && path[0] == '/' && path[1] == '/') || path[0] == '/';
}

bool Path::has_parent_path() const {
    return !parent_path().empty();
}

Path Path::filename() const {
    if (value_.empty()) {
        return {};
    }
    std::string path = normalize_component_separators(value_);
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return Path(path);
    }
    if (pos + 1 >= path.size()) {
        return {};
    }
    return Path(path.substr(pos + 1));
}

Path Path::extension() const {
    std::string file = filename().string();
    size_t pos = file.find_last_of('.');
    if (pos == std::string::npos || pos == 0) {
        return {};
    }
    return Path(file.substr(pos));
}

Path Path::parent_path() const {
    if (value_.empty()) {
        return {};
    }
    std::string path = normalize_component_separators(value_);
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    if (has_drive_prefix(path) && path.size() <= 3) {
        return Path(path.size() == 2 ? path : path.substr(0, 3));
    }
    if (path == "/" || path == "//") {
        return Path(path);
    }

    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return {};
    }
    if (pos == 0) {
        return Path(path.substr(0, 1));
    }
    if (has_drive_prefix(path) && pos == 2) {
        return Path(path.substr(0, 3));
    }
    return Path(path.substr(0, pos));
}

Path Path::lexically_normal() const {
    return Path(normalize_path(value_));
}

Path operator/(const Path &lhs, const Path &rhs) {
    if (lhs.empty()) {
        return rhs.lexically_normal();
    }
    if (rhs.empty()) {
        return lhs.lexically_normal();
    }
    if (rhs.is_absolute()) {
        return rhs.lexically_normal();
    }

    std::string combined = lhs.string();
    if (!combined.empty() && !is_sep(combined.back())) {
        combined.push_back('/');
    }
    combined += rhs.string();
    return Path(normalize_path(combined));
}

bool operator==(const Path &lhs, const Path &rhs) {
    return lhs.string() == rhs.string();
}

bool operator!=(const Path &lhs, const Path &rhs) {
    return !(lhs == rhs);
}

Path current_path() {
    return make_win32_path_result(GetCurrentDirectoryW);
}

Path absolute(const Path &path) {
    return get_full_path(path, nullptr);
}

Path weakly_canonical(const Path &path, std::error_code &ec) {
    return get_full_path(path, &ec);
}

bool exists(const Path &path) {
    std::error_code err;
    return exists(path, err);
}

bool exists(const Path &path, std::error_code &ec) {
    DWORD attrs = path_attributes(path, &ec);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (ec.value() == ERROR_FILE_NOT_FOUND || ec.value() == ERROR_PATH_NOT_FOUND) {
            ec.clear();
        }
        return false;
    }
    ec.clear();
    return true;
}

bool is_directory(const Path &path) {
    std::error_code err;
    return is_directory(path, err);
}

bool is_directory(const Path &path, std::error_code &ec) {
    DWORD attrs = path_attributes(path, &ec);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    ec.clear();
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_regular_file(const Path &path) {
    std::error_code err;
    return is_regular_file(path, err);
}

bool is_regular_file(const Path &path, std::error_code &ec) {
    DWORD attrs = path_attributes(path, &ec);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    ec.clear();
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool remove(const Path &path, std::error_code &ec) {
    DWORD attrs = path_attributes(path, &ec);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    std::wstring wide = utf8_to_wide(path.string());
    BOOL ok = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(wide.c_str()) : DeleteFileW(wide.c_str());
    if (!ok) {
        ec = std::error_code((int)GetLastError(), std::system_category());
        return false;
    }

    ec.clear();
    return true;
}

bool create_directories(const Path &path, std::error_code &ec) {
    Path full = absolute(path).lexically_normal();
    if (full.empty()) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }

    ParsedPath parsed = parse_path(full.string());
    std::string current = parsed.root;
    bool created_any = false;

    for (const std::string &part : parsed.parts) {
        if (!current.empty() && current.back() != '/') {
            current.push_back('/');
        }
        current += part;

        Path current_path_part(current);
        std::error_code step_ec;
        if (exists(current_path_part, step_ec)) {
            if (!is_directory(current_path_part, step_ec)) {
                ec = step_ec.value() != 0 ? step_ec : std::make_error_code(std::errc::not_a_directory);
                return false;
            }
            continue;
        }

        std::wstring wide = utf8_to_wide(current);
        if (!CreateDirectoryW(wide.c_str(), nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_EXISTS) {
                continue;
            }
            ec = std::error_code((int)err, std::system_category());
            return false;
        }
        created_any = true;
    }

    ec.clear();
    return created_any;
}

std::vector<Path> list_directory(const Path &path, std::error_code &ec) {
    std::vector<Path> entries;
    Path full = absolute(path).lexically_normal();
    std::string pattern = full.string();
    if (!pattern.empty() && pattern.back() != '/') {
        pattern.push_back('/');
    }
    pattern.push_back('*');

    std::wstring wide_pattern = utf8_to_wide(pattern);
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(wide_pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        ec = std::error_code((int)GetLastError(), std::system_category());
        return entries;
    }

    do {
        std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        entries.push_back(full / Path(wide_to_utf8(name)));
    } while (FindNextFileW(handle, &data));

    DWORD last_error = GetLastError();
    FindClose(handle);
    if (last_error != ERROR_NO_MORE_FILES) {
        ec = std::error_code((int)last_error, std::system_category());
        entries.clear();
        return entries;
    }

    ec.clear();
    return entries;
}

#else

namespace stdfs = std::filesystem;

namespace {

stdfs::path to_std_path(const Path &path) {
    return stdfs::path(path.native());
}

Path from_std_path(const stdfs::path &path) {
    return Path(path.generic_string());
}

} // namespace

bool Path::is_absolute() const {
    return to_std_path(*this).is_absolute();
}

bool Path::has_parent_path() const {
    return to_std_path(*this).has_parent_path();
}

Path Path::filename() const {
    return from_std_path(to_std_path(*this).filename());
}

Path Path::extension() const {
    return from_std_path(to_std_path(*this).extension());
}

Path Path::parent_path() const {
    return from_std_path(to_std_path(*this).parent_path());
}

Path Path::lexically_normal() const {
    return from_std_path(to_std_path(*this).lexically_normal());
}

Path operator/(const Path &lhs, const Path &rhs) {
    return from_std_path(to_std_path(lhs) / to_std_path(rhs));
}

bool operator==(const Path &lhs, const Path &rhs) {
    return to_std_path(lhs) == to_std_path(rhs);
}

bool operator!=(const Path &lhs, const Path &rhs) {
    return !(lhs == rhs);
}

Path current_path() {
    return from_std_path(stdfs::current_path());
}

Path absolute(const Path &path) {
    return from_std_path(stdfs::absolute(to_std_path(path)));
}

Path weakly_canonical(const Path &path, std::error_code &ec) {
    return from_std_path(stdfs::weakly_canonical(to_std_path(path), ec));
}

bool exists(const Path &path) {
    return stdfs::exists(to_std_path(path));
}

bool exists(const Path &path, std::error_code &ec) {
    return stdfs::exists(to_std_path(path), ec);
}

bool is_directory(const Path &path) {
    return stdfs::is_directory(to_std_path(path));
}

bool is_directory(const Path &path, std::error_code &ec) {
    return stdfs::is_directory(to_std_path(path), ec);
}

bool is_regular_file(const Path &path) {
    return stdfs::is_regular_file(to_std_path(path));
}

bool is_regular_file(const Path &path, std::error_code &ec) {
    return stdfs::is_regular_file(to_std_path(path), ec);
}

bool remove(const Path &path, std::error_code &ec) {
    return stdfs::remove(to_std_path(path), ec);
}

bool create_directories(const Path &path, std::error_code &ec) {
    return stdfs::create_directories(to_std_path(path), ec);
}

std::vector<Path> list_directory(const Path &path, std::error_code &ec) {
    std::vector<Path> entries;
    for (const auto &entry : stdfs::directory_iterator(to_std_path(path), ec)) {
        if (ec) {
            entries.clear();
            return entries;
        }
        entries.push_back(from_std_path(entry.path()));
    }
    return entries;
}

#endif

} // namespace nari::fs
