#pragma once

#include <string>

namespace nari::fmt {

struct FmtOptions {
    int indent_width = 4;
    int max_blank_lines = 1;
};

bool format_source(const std::string &src, const std::string &filename, const FmtOptions &opts, std::string &out, std::string &error_out);

} // namespace nari::fmt
