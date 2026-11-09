#pragma once

#include <cstdio>
#include <memory>
#include <string>

namespace nari {

// Read one line without its newline.
inline bool read_line(FILE *stream, std::string &line) {
    line.clear();
    int ch;
    while ((ch = std::fgetc(stream)) != EOF) {
        if (ch == '\n') {
            return true;
        }
        line.push_back(static_cast<char>(ch));
    }
    return !line.empty();
}

inline bool read_source_line(const std::string &path, int line_number, std::string &line) {
    line.clear();
    if (path.empty() || path.front() == '<' || line_number <= 0) {
        return false;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "r"), &std::fclose);
    if (!file) {
        return false;
    }
    for (int i = 0; i < line_number; ++i) {
        if (!read_line(file.get(), line)) {
            return false;
        }
    }
    return true;
}

inline void print_source_context(const std::string &path, int line_number, int column, FILE *output = stderr) {
#ifndef NARI_ESP_IDF
    std::string line;
    if (!read_source_line(path, line_number, line)) {
        return;
    }
    std::fprintf(output, "  %s\n", line.c_str());
    std::string caret(column > 0 ? column - 1 : 0, ' ');
    caret.push_back('^');
    std::fprintf(output, "  %s\n", caret.c_str());
#else
    std::fputs("  (source context not available on this platform!)\n", output);
#endif
}

} // namespace nari
