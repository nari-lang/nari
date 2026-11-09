#include "json_dom.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

// parse errors unwind by returning false, parse() maps any failure to JsonValue::discarded().
struct JsonParser {
    const char *p;
    const char *end;
    int depth = 0;
    static constexpr int max_depth = 512;

    bool fail = false;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
    }

    bool match_lit(const char *lit) {
        size_t n = std::strlen(lit);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, lit, n) != 0) {
            return false;
        }
        p += n;
        return true;
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            if (p >= end) {
                return 0xFFFFFFFFu;
            }
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned>((c - 'a' + 10));
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned>((c - 'A' + 10));
            } else {
                return 0xFFFFFFFFu;
            }
        }
        return v;
    }

    static void append_utf8(std::string &out, unsigned cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>((0xC0 | (cp >> 6)));
            out += static_cast<char>((0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>((0xE0 | (cp >> 12)));
            out += static_cast<char>((0x80 | ((cp >> 6) & 0x3F)));
            out += static_cast<char>((0x80 | (cp & 0x3F)));
        } else {
            out += static_cast<char>((0xF0 | (cp >> 18)));
            out += static_cast<char>((0x80 | ((cp >> 12) & 0x3F)));
            out += static_cast<char>((0x80 | ((cp >> 6) & 0x3F)));
            out += static_cast<char>((0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(std::string &out) {
        // assumes *p == '"'
        p++;
        while (p < end) {
            const char *run = p;
            while (p < end && *p != '"' && *p != '\\') {
                p++;
            }
            if (p > run) {
                out.append(run, static_cast<size_t>(p - run));
            }
            if (p >= end) {
                return false;
            }
            char c = *p++;
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= end) {
                return false;
            }
            char e = *p++;
            switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp == 0xFFFFFFFFu) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            unsigned lo = hex4();
                            if (lo == 0xFFFFFFFFu) {
                                return false;
                            }
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                append_utf8(out, cp);
                                cp = lo;
                            }
                        }
                    }
                    // a lone surrogate is malformed, keep the bytes so dump() can replace them
                    append_utf8(out, cp <= 0x10FFFF ? cp : 0xFFFD);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool parse_number(JsonValue &out) {
        const char *start = p;
        bool negative = false;
        if (p < end && *p == '-') {
            negative = true;
            p++;
        }
        bool is_float = false;
        bool have_digit = false;
        bool int_overflow = false;
        uint64_t magnitude = 0;
        const uint64_t max_magnitude = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
        while (p < end) {
            char c = *p;
            if (c >= '0' && c <= '9') {
                have_digit = true;
                if (!is_float && !int_overflow) {
                    const uint64_t digit = static_cast<uint64_t>(c - '0');
                    if (magnitude > (max_magnitude - digit) / 10) {
                        int_overflow = true;
                    } else {
                        magnitude = magnitude * 10 + digit;
                    }
                }
                p++;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_float = true;
                p++;
            } else {
                break;
            }
        }
        if (!have_digit || p == start || (p == start + 1 && negative)) {
            return false;
        }
        if (!is_float && !int_overflow) {
            if (negative) {
                if (magnitude == uint64_t(INT64_MAX) + 1) {
                    out = JsonValue(INT64_MIN);
                } else {
                    out = JsonValue(-static_cast<int64_t>(magnitude));
                }
            } else {
                out = JsonValue(
                    static_cast<uint64_t>(magnitude) <= uint64_t(INT64_MAX) ? JsonValue(static_cast<int64_t>(magnitude))
                                                                            : JsonValue(magnitude)
                );
            }
            return true;
        }
        std::string num(start, static_cast<size_t>(p - start));
        if (is_float) {
            out = JsonValue(std::strtod(num.c_str(), nullptr));
            return true;
        }
        errno = 0;
        char *endp = nullptr;
        long long iv = std::strtoll(num.c_str(), &endp, 10);
        if (errno == ERANGE || endp != num.c_str() + num.size()) {
            out = JsonValue(std::strtod(num.c_str(), nullptr));
        } else {
            out = JsonValue(static_cast<int64_t>(iv));
        }
        return true;
    }

    bool parse_value(JsonValue &out) {
        skip_ws();
        if (p >= end) {
            return false;
        }
        char c = *p;
        switch (c) {
            case '{':
                return parse_object(out);
            case '[':
                return parse_array(out);
            case '"': {
                std::string s;
                if (!parse_string(s)) {
                    return false;
                }
                out = JsonValue(std::move(s));
                return true;
            }
            case 't':
                return match_lit("true") ? (out = JsonValue(true), true) : false;
            case 'f':
                return match_lit("false") ? (out = JsonValue(false), true) : false;
            case 'n':
                return match_lit("null") ? (out = JsonValue(), true) : false;
            default:
                return parse_number(out);
        }
    }

    bool parse_array(JsonValue &out) {
        if (depth >= max_depth) {
            return false;
        }
        depth++;
        p++; // '['
        JsonValue arr = JsonValue::array();
        skip_ws();
        if (p < end && *p == ']') {
            p++;
            depth--;
            out = std::move(arr);
            return true;
        }
        while (true) {
            JsonValue el;
            if (!parse_value(el)) {
                return false;
            }
            arr.tp_array.push_back(std::move(el));
            skip_ws();
            if (p >= end) {
                return false;
            }
            char c = *p++;
            if (c == ']') {
                break;
            }
            if (c != ',') {
                return false;
            }
        }
        depth--;
        out = std::move(arr);
        return true;
    }

    bool parse_object(JsonValue &out) {
        if (depth >= max_depth) {
            return false;
        }
        depth++;
        p++; // '{'
        JsonValue obj = JsonValue::object();
        skip_ws();
        if (p < end && *p == '}') {
            p++;
            depth--;
            out = std::move(obj);
            return true;
        }
        while (true) {
            skip_ws();
            if (p >= end || *p != '"') {
                return false;
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (p >= end || *p++ != ':') {
                return false;
            }
            JsonValue val;
            if (!parse_value(val)) {
                return false;
            }
            obj.tp_object[std::move(key)] = std::move(val);
            skip_ws();
            if (p >= end) {
                return false;
            }
            char c = *p++;
            if (c == '}') {
                break;
            }
            if (c != ',') {
                return false;
            }
        }
        depth--;
        out = std::move(obj);
        return true;
    }
};

// Maps a byte offset in a UTF-8 string to the length of the code point starting there, or 0 when the sequence is invalid.
size_t utf8_len(const std::string &s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        return 1;
    }
    size_t want = 0;
    unsigned min_cp = 0;
    if ((c & 0xE0) == 0xC0) {
        want = 2;
        min_cp = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        want = 3;
        min_cp = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        want = 4;
        min_cp = 0x10000;
    } else {
        return 0;
    }
    if (i + want > s.size()) {
        return 0;
    }
    unsigned cp = c & (0xFF >> (want + 1));
    for (size_t k = 1; k < want; k++) {
        unsigned char d = static_cast<unsigned char>(s[i + k]);
        if ((d & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return want;
}

void dump_string_into(const std::string &s, std::string &out, bool replace_invalid) {
    static const char *hex = "0123456789abcdef";
    out += '"';
    size_t i = 0;
    size_t clean = 0;
    // scan for the first byte needing attention
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == '"' || c == '\\' || (replace_invalid && c >= 0x80)) {
            break;
        }
        i++;
    }
    clean = i;
    out.append(s, 0, clean);
    for (; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (replace_invalid && c >= 0x80) {
            size_t n = utf8_len(s, i);
            if (n == 0) {
                out += "\xEF\xBF\xBD"; // U+FFFD
                i++;
                continue;
            }
            out.append(s, i, n);
            i += n;
            continue;
        }
        switch (c) {
            case '"':
                out += "\\\"";
                i++;
                break;
            case '\\':
                out += "\\\\";
                i++;
                break;
            case '\b':
                out += "\\b";
                i++;
                break;
            case '\f':
                out += "\\f";
                i++;
                break;
            case '\n':
                out += "\\n";
                i++;
                break;
            case '\r':
                out += "\\r";
                i++;
                break;
            case '\t':
                out += "\\t";
                i++;
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
                i++;
                break;
        }
    }
    out += '"';
}

void dump_into(const JsonValue &val, std::string &out, int indent, int depth) {
    switch (val.type) {
        case JsonValue::Type::Null:
        case JsonValue::Type::Discarded:
            out += "null";
            return;
        case JsonValue::Type::Bool:
            out += val.tp_boolean ? "true" : "false";
            return;
        case JsonValue::Type::Int: {
            char buf[24];
            auto res = std::to_chars(buf, buf + sizeof(buf), val.tp_int);
            out.append(buf, res.ptr - buf);
            return;
        }
        case JsonValue::Type::UInt: {
            char buf[24];
            auto res = std::to_chars(buf, buf + sizeof(buf), val.tp_uint);
            out.append(buf, res.ptr - buf);
            return;
        }
        case JsonValue::Type::Float:
            append_json_float(out, val.tp_float);
            return;
        case JsonValue::Type::String:
            dump_string_into(val.tp_string, out, indent >= 0);
            return;
        case JsonValue::Type::Array: {
            if (val.tp_array.empty()) {
                out += "[]";
                return;
            }
            out += '[';
            std::string pad(indent >= 0 ? static_cast<size_t>(indent) * static_cast<size_t>(depth + 1) : 0, ' ');
            std::string close_pad(indent >= 0 ? static_cast<size_t>(indent) * static_cast<size_t>(depth) : 0, ' ');
            for (size_t i = 0; i < val.tp_array.size(); i++) {
                if (i) {
                    out += ',';
                }
                if (indent >= 0) {
                    out += '\n';
                    out += pad;
                }
                dump_into(val.tp_array[i], out, indent, depth + 1);
            }
            if (indent >= 0) {
                out += '\n';
                out += close_pad;
            }
            out += ']';
            return;
        }
        case JsonValue::Type::Object: {
            if (val.tp_object.empty()) {
                out += "{}";
                return;
            }
            out += '{';
            std::string pad(indent >= 0 ? static_cast<size_t>(indent) * static_cast<size_t>(depth + 1) : 0, ' ');
            std::string close_pad(indent >= 0 ? static_cast<size_t>(indent) * static_cast<size_t>(depth) : 0, ' ');
            bool first = true;
            for (const auto &kv : val.tp_object) {
                if (!first) {
                    out += ',';
                }
                first = false;
                if (indent >= 0) {
                    out += '\n';
                    out += pad;
                }
                dump_string_into(kv.first, out, indent >= 0);
                out += ':';
                if (indent >= 0) {
                    out += ' ';
                }
                dump_into(kv.second, out, indent, depth + 1);
            }
            if (indent >= 0) {
                out += '\n';
                out += close_pad;
            }
            out += '}';
            return;
        }
    }
}

} // namespace

void append_json_float(std::string &out, double d) {
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    std::string s(buf, res.ptr - buf);
    if (s.find_first_of(".eEnN") == std::string::npos) {
        s += ".0";
    }
    out += s;
}

JsonValue JsonValue::parse(const std::string &text) {
    JsonParser jp{ text.data(), text.data() + text.size() };
    JsonValue out;
    if (!jp.parse_value(out)) {
        return JsonValue::discarded();
    }
    while (jp.p != jp.end && (*jp.p == ' ' || *jp.p == '\t' || *jp.p == '\n' || *jp.p == '\r' || *jp.p == '\0')) {
        jp.p++;
    }
    if (jp.p != jp.end) {
        return JsonValue::discarded();
    }
    return out;
}

std::string JsonValue::dump() const {
    std::string out;
    dump_into(*this, out, -1, 0);
    return out;
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dump_into(*this, out, indent < 0 ? -1 : indent, 0);
    return out;
}
