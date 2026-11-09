#pragma once

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <istream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <corecrt_io.h>
#include <fcntl.h>
#endif

namespace nari::protocol {

inline void use_binary_stdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

inline bool is_content_length(std::string_view name) {
    constexpr std::string_view expected = "content-length";
    if (name.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch += 'a' - 'A';
        }
        if (ch != expected[i]) {
            return false;
        }
    }
    return true;
}

// LSP and DAP use the same Content-Length framing. Invalid framing ends the
// session: resuming in the middle of a body would misinterpret bytes as headers.
inline bool read_message(std::istream &input, std::string &body) {
    body.clear();
    size_t length = 0;
    bool have_length = false;
    std::string header;
    for (;;) {
        if (!std::getline(input, header)) {
            return false;
        }
        if (!header.empty() && header.back() == '\r') {
            header.pop_back();
        }
        if (header.empty()) {
            break;
        }
        const size_t colon = header.find(':');
        if (colon == std::string::npos || !is_content_length(std::string_view(header).substr(0, colon))) {
            continue;
        }
        std::string_view value(header.data() + colon + 1, header.size() - colon - 1);
        const size_t begin = value.find_first_not_of(" \t");
        if (begin == std::string_view::npos) {
            return false;
        }
        value = value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
        size_t parsed_length = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_length);
        if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || parsed_length == 0 ||
            (have_length && parsed_length != length)) {
            return false;
        }
        length = parsed_length;
        have_length = true;
    }
    if (!have_length || length > body.max_size()) {
        return false;
    }

    // Grow only as bytes arrive; a huge claimed length followed by EOF must
    // not trigger a huge allocation or produce a NUL-padded JSON document.
    char buffer[4096];
    while (length != 0) {
        const auto count = static_cast<std::streamsize>(std::min(length, sizeof(buffer)));
        input.read(buffer, count);
        if (input.gcount() != count) {
            body.clear();
            return false;
        }
        body.append(buffer, static_cast<size_t>(count));
        length -= static_cast<size_t>(count);
    }
    return true;
}

} // namespace nari::protocol
