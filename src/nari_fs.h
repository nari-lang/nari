#pragma once

#include <string>
#include <system_error>
#include <vector>

namespace nari::fs {

class Path {
  public:
    Path() = default;
    Path(const char *value);
    Path(const std::string &value);
    Path(std::string &&value);

    bool empty() const;
    bool is_absolute() const;
    bool has_parent_path() const;

    std::string string() const;
    std::string generic_string() const;

    Path filename() const;
    Path extension() const;
    Path parent_path() const;
    Path lexically_normal() const;

    const std::string &native() const;

  private:
    std::string value_;
};

Path operator/(const Path &lhs, const Path &rhs);
bool operator==(const Path &lhs, const Path &rhs);
bool operator!=(const Path &lhs, const Path &rhs);

Path current_path();
Path absolute(const Path &path);
Path weakly_canonical(const Path &path, std::error_code &ec);

bool exists(const Path &path);
bool exists(const Path &path, std::error_code &ec);
bool is_directory(const Path &path);
bool is_directory(const Path &path, std::error_code &ec);
bool remove(const Path &path, std::error_code &ec);
bool create_directories(const Path &path, std::error_code &ec);
std::vector<Path> list_directory(const Path &path, std::error_code &ec);

} // namespace nari::fs
