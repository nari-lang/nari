#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <corecrt_io.h> // _isatty, _fileno
#else
#include <unistd.h> // isatty, fileno
#endif
#include "compiler_support.h"
#include "nari_fs.h"
#ifdef __linux__
#include <endian.h>
#endif
#if defined(__APPLE__) && defined(__MACH__)
#include <machine/endian.h>
#endif
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "runtime.h"
#define SRELL_NO_THROW
#include "thirdparty/srell.hpp"
#ifndef DISABLE_HTTP
#include "io.h"
#endif
#ifndef DISABLE_FFI
#include "nari_ffi.h"
#endif
#include "parser_api.h"

// Returns a human-readable type name for a Value (used in TypeError messages).
static std::string value_type_name(const Value &v) {
    if (v.is_none()) {
        return "null";
    }
    if (v.is_int()) {
        return "int";
    }
    if (v.is_float()) {
        return "float";
    }
    if (v.is_string()) {
        return "string";
    }
    if (v.is_bool()) {
        return "bool";
    }
    if (v.is_array()) {
        return "array";
    }
    if (v.is_object()) {
        return "object";
    }
    if (v.is_function()) {
        return "function";
    }
    if (v.is_regex()) {
        return "regex";
    }
    if (v.is_class_instance()) {
        return v.get_class_instance()->class_name;
    }
    return "unknown";
}

#ifdef _WIN32
#include "win_funcs.h"
#endif

#ifndef DISABLE_FFI
namespace {

bool utf8_to_utf16(const std::string &input, std::u16string &output) {
    output.clear();

#ifdef _WIN32
    if (input.empty()) {
        return true;
    }

    int required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), static_cast<int>(input.size()), nullptr, 0);

    if (required <= 0) {
        return false;
    }

    output.resize(static_cast<size_t>(required));

    int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.c_str(),
        static_cast<int>(input.size()),
        reinterpret_cast<wchar_t *>(output.data()), required);

    return converted == required;
#else
    output.reserve(input.size());

    size_t i = 0;
    while (i < input.size()) {
        uint32_t codepoint = 0;
        unsigned char c = static_cast<unsigned char>(input[i]);

        if ((c & 0x80u) == 0) {
            codepoint = c;
            i += 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            if (i + 1 >= input.size()) {
                return false;
            }
            unsigned char c1 = static_cast<unsigned char>(input[i + 1]);
            if ((c1 & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = ((c & 0x1Fu) << 6) | (c1 & 0x3Fu);
            if (codepoint < 0x80u) {
                return false;
            }
            i += 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            if (i + 2 >= input.size()) {
                return false;
            }
            unsigned char c1 = static_cast<unsigned char>(input[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(input[i + 2]);
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
            if (codepoint < 0x800u) {
                return false;
            }
            if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
                return false;
            }
            i += 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            if (i + 3 >= input.size()) {
                return false;
            }
            unsigned char c1 = static_cast<unsigned char>(input[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(input[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(input[i + 3]);
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u ||
                (c3 & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
            if (codepoint < 0x10000u || codepoint > 0x10FFFFu) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }

        if (codepoint <= 0xFFFFu) {
            output.push_back(static_cast<char16_t>(codepoint));
        } else {
            codepoint -= 0x10000u;
            char16_t high = static_cast<char16_t>(0xD800u + (codepoint >> 10));
            char16_t low = static_cast<char16_t>(0xDC00u + (codepoint & 0x3FFu));
            output.push_back(high);
            output.push_back(low);
        }
    }

    return true;
#endif
}

bool utf16_to_utf8(const std::u16string &input, std::string &output) {
    output.clear();

#ifdef _WIN32
    if (input.empty()) {
        return true;
    }

    int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        reinterpret_cast<const wchar_t *>(input.data()),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);

    if (required <= 0) {
        return false;
    }

    output.resize(static_cast<size_t>(required));

    int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        reinterpret_cast<const wchar_t *>(input.data()),
        static_cast<int>(input.size()), output.data(),
        required, nullptr, nullptr);

    return converted == required;
#else
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        uint32_t codepoint = static_cast<uint16_t>(input[i]);

        if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            if (i + 1 >= input.size()) {
                return false;
            }
            uint32_t low = static_cast<uint16_t>(input[i + 1]);
            if (low < 0xDC00u || low > 0xDFFFu) {
                return false;
            }
            codepoint = ((codepoint - 0xD800u) << 10) + (low - 0xDC00u) + 0x10000u;
            ++i;
        } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
            return false;
        }

        if (codepoint <= 0x7Fu) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFu) {
            output.push_back(static_cast<char>(0xC0u | ((codepoint >> 6) & 0x1Fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        } else if (codepoint <= 0xFFFFu) {
            output.push_back(static_cast<char>(0xE0u | ((codepoint >> 12) & 0x0Fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        } else {
            output.push_back(static_cast<char>(0xF0u | ((codepoint >> 18) & 0x07u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        }
    }

    return true;
#endif
}

} // namespace
#endif

#define STDLIB_VERSION "0.0.3"

// Builtin implementations are split across src/builtins/*.cpp and share this
// common prelude header.
