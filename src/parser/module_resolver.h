#pragma once

#include <string>

// Package / module path resolution for the parser's import machinery.
// These were split out of parser.cpp; they are pure path/TOML helpers with no
// dependency on the parser's token stream or AST.
namespace Parser {

// True if `inc` is a bare package import spec, i.e. not a relative ("./", "../")
// or absolute path and not ending in a known file extension (.nari/.naric/.so/...).
bool is_package_import_spec(const std::string &inc);

// Resolve a bare package import (see is_package_import_spec) to an absolute file
// path by walking up to the nearest nari.toml, checking its [dependencies] and
// the nearest nari.lock.toml, then the matched package's [exports]
std::string resolve_package_import_path(const std::string &inc, const std::string &basefile, std::string &error_out);

// Resolve the root export of a package directory containing nari.toml.
// Returns the original path when it is not a directory.
std::string resolve_package_directory_path(const std::string &path, std::string &error_out);

} // namespace Parser
