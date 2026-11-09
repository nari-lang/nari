// String builtins (split out of collections.cpp).
#include "common.h"

#include <cmath>

// Coerce a numeric Value to an int index, accepting integer-valued floats
// (e.g. results of `/` which is always float division in Nari). Returns
// false if the argument isn't numeric or has a fractional part.
static bool coerce_numeric_index(const Value &v, int &out) {
    if (v.is_int()) {
        out = static_cast<int>(v.get_int());
        return true;
    }
    if (v.is_float()) {
        double f = v.get_float();
        if (std::floor(f) != f) {
            return false;
        }
        out = static_cast<int>(f);
        return true;
    }
    return false;
}

// String builtins
Value ScriptRuntime::builtin_substr(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str_storage;
        const std::string &str = argvals[0].is_string() ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        int start = 0;
        if (argc > 1) {
            if (!coerce_numeric_index(argvals[1], start)) {
                return Value::make_string("");
            }
        }
        size_t count = std::string::npos;
        if (argc > 2) {
            int len;
            if (!coerce_numeric_index(argvals[2], len)) {
                return Value::make_string("");
            }
            count = static_cast<size_t>(std::max(len, 0));
        }

        if (start < 0) {
            start = 0;
        }
        const size_t pos = static_cast<size_t>(start);
        if (pos >= str.size()) {
            return Value::make_string("");
        }

        return Value::make_string(str.substr(pos, count));
    }
    return Value::make_string("");
}

// TODO: make index_of only show up on string and array, like we do with methods exclusive to those types, instead of
// being universal
Value ScriptRuntime::builtin_index_of(Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
    if (argc < 2) {
        return Value::make_int(-1);
    }

    // array.index_of(val)
    if (argvals[0].is_array()) {
        const auto &arr = argvals[0].get_array();
        for (size_t i = 0; i < arr.size(); i++) {
            if (Value::values_equal(arr[i], argvals[1])) {
                return Value::make_int(static_cast<int64_t>(i));
            }
        }
        return Value::make_int(-1);
    }

    // string.index_of(search)
    if (!argvals[0].is_string()) {
        return runtime_fatal("TypeError: 'index_of' can only be called on a string or array, got " + value_type_name(argvals[0]), callExpr);
    }
    std::string str = argvals[0].to_string();
    std::string search = argvals[1].to_string();
    size_t pos = str.find(search);
    if (pos == std::string::npos) {
        return Value::make_int(-1);
    }

    return Value::make_int(static_cast<int64_t>(pos));
}

Value ScriptRuntime::builtin_last_index_of(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        // array.last_index_of(val, from_index)
        if (argvals[0].is_array()) {
            const auto &arr = argvals[0].get_array();
            size_t scan_end = arr.size(); // exclusive upper bound
            if (argc > 2 && (argvals[2].is_int() || argvals[2].is_float())) {
                int64_t fi = (argvals[2].is_int()) ? argvals[2].get_int() : static_cast<int64_t>(argvals[2].get_float());
                if (fi < 0) {
                    return Value::make_int(-1);
                }
                if (static_cast<uint64_t>(fi) + 1 < scan_end) {
                    scan_end = static_cast<size_t>(fi) + 1;
                }
            }
            for (size_t i = scan_end; i-- > 0;) {
                if (Value::values_equal(arr[i], argvals[1])) {
                    return Value::make_int(static_cast<int64_t>(i));
                }
            }
            return Value::make_int(-1);
        }

        // string.last_index_of(val, from_index)
        std::string str = argvals[0].to_string();
        std::string search = argvals[1].to_string();

        if (search.empty()) {
            if (argc > 2 && (argvals[2].is_int() || argvals[2].is_float())) {
                int64_t fi = (argvals[2].is_int()) ? argvals[2].get_int() : static_cast<int64_t>(argvals[2].get_float());
                if (fi < 0) {
                    return Value::make_int(-1);
                }

                size_t pos = static_cast<uint64_t>(fi) > str.size() ? str.size() : static_cast<size_t>(fi);
                return Value::make_int(pos);
            }
            return Value::make_int(str.size());
        }

        bool have_from = false;
        size_t from_pos = std::string::npos;
        if (argc > 2 && (argvals[2].is_int() || argvals[2].is_float())) {
            int64_t fi = (argvals[2].is_int()) ? argvals[2].get_int() : static_cast<int64_t>(argvals[2].get_float());
            if (fi < 0) {
                return Value::make_int(-1);
            }
            if (str.empty()) {
                from_pos = 0;
            } else {
                size_t maxpos = (str.size() > 0) ? (str.size() - 1) : 0;
                from_pos = static_cast<uint64_t>(fi) > maxpos ? maxpos : static_cast<size_t>(fi);
            }
            have_from = true;
        }

        size_t pos;
        if (have_from) {
            pos = str.rfind(search, from_pos);
        } else {
            pos = str.rfind(search);
        }

        if (pos == std::string::npos) {
            return Value::make_int(-1);
        }
        return Value::make_int(pos);
    }
    return Value::make_int(-1);
}

Value ScriptRuntime::builtin_split(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str_storage;
        std::string delim_storage;
        const std::string &str =
            (argvals[0].is_string() && !argvals[0].is_sso()) ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        const std::string &delim =
            (argvals[1].is_string() && !argvals[1].is_sso()) ? argvals[1].get_string() : (delim_storage = argvals[1].to_string());
        std::vector<Value> result;

        if (delim.empty()) {
            result.reserve(str.size());
            for (char c : str) {
                result.push_back(Value::make_string(std::string(1, c)));
            }
        } else if (delim.size() == 1) {
            char d = delim[0];
            result.reserve(8);
            size_t start = 0;
            size_t pos;
            while ((pos = str.find(d, start)) != std::string::npos) {
                result.push_back(Value::make_string(str.substr(start, pos - start)));
                start = pos + 1;
            }
            result.push_back(Value::make_string(str.substr(start)));
        } else {
            size_t start = 0;
            size_t pos;
            while ((pos = str.find(delim, start)) != std::string::npos) {
                result.push_back(Value::make_string(str.substr(start, pos - start)));
                start = pos + delim.size();
            }
            result.push_back(Value::make_string(str.substr(start)));
        }

        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_replace(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 3) {
        std::string str = argvals[0].to_string();
        std::string find = argvals[1].to_string();
        std::string replacement = argvals[2].to_string();

        size_t pos = str.find(find);
        if (pos != std::string::npos) {
            str.replace(pos, find.size(), replacement);
        }
        return Value::make_string(str);
    }
    return (argc == 0) ? Value::make_string("") : Value::make_string(argvals[0].to_string());
}

Value ScriptRuntime::builtin_replace_all(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 3) {
        std::string str = argvals[0].to_string();
        std::string find = argvals[1].to_string();
        std::string replacement = argvals[2].to_string();

        if (!find.empty()) {
            size_t pos = 0;
            while ((pos = str.find(find, pos)) != std::string::npos) {
                str.replace(pos, find.size(), replacement);
                pos += replacement.size();
            }
        }
        return Value::make_string(str);
    }
    return (argc == 0) ? Value::make_string("") : Value::make_string(argvals[0].to_string());
}

Value ScriptRuntime::builtin_trim(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        size_t start = 0;
        while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        size_t end = str.size();
        while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
            --end;
        }
        return Value::make_string(str.substr(start, end - start));
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_trim_start(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        size_t start = 0;
        while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        return Value::make_string(str.substr(start));
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_trim_end(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        size_t end = str.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
            --end;
        }
        return Value::make_string(str.substr(0, end));
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_string_at(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        // array.at(i)
        if (argvals[0].is_array()) {
            if (!argvals[1].is_int()) {
                return Value::none();
            }
            const auto &arr = argvals[0].get_array();
            int64_t index = argvals[1].get_int();
            const int64_t len = static_cast<int64_t>(arr.size());
            if (index < 0) {
                index += len;
            }
            if (index >= 0 && index < len) {
                return arr[static_cast<size_t>(index)];
            }
            return Value::none();
        }
        // string.at(i)
        std::string str = argvals[0].to_string();
        if (!argvals[1].is_int()) {
            return Value::make_string("");
        }
        int index = argvals[1].get_int();
        int len = static_cast<int>(str.size());
        if (index < 0) {
            index += len;
        }
        if (index >= 0 && index < len) {
            return Value::make_string(std::string(1, str[index]));
        }
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_to_char_array(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        std::vector<Value> chars;
        chars.reserve(str.size());
        for (char c : str) {
            chars.push_back(Value::make_string(std::string(1, c)));
        }
        return Value::make_array(std::move(chars));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_to_upper(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        for (char &c : str) {
            c = std::toupper(c);
        }
        return Value::make_string(str);
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_to_lower(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        for (char &c : str) {
            c = std::tolower(c);
        }
        return Value::make_string(str);
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_starts_with(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str_storage;
        std::string prefix_storage;
        const std::string &str =
            (argvals[0].is_string() && !argvals[0].is_sso()) ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        const std::string &prefix =
            (argvals[1].is_string() && !argvals[1].is_sso()) ? argvals[1].get_string() : (prefix_storage = argvals[1].to_string());
        return Value::make_bool(str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_ends_with(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str_storage;
        std::string suffix_storage;
        const std::string &str =
            (argvals[0].is_string() && !argvals[0].is_sso()) ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        const std::string &suffix =
            (argvals[1].is_string() && !argvals[1].is_sso()) ? argvals[1].get_string() : (suffix_storage = argvals[1].to_string());
        return Value::make_bool(str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_char_at(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str = argvals[0].to_string();
        int index = 0;
        if (!coerce_numeric_index(argvals[1], index)) {
            return Value::make_string("");
        }
        if (index >= 0 && index < static_cast<int>(str.size())) {
            return Value::make_string(std::string(1, str[index]));
        }
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_char_code_at(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        // Avoid copying the receiver: read the byte directly from the live string
        // buffer when possible. byte_hash calls this ~720K times on ~30-char
        // strings, where a per-call to_string() copy dominated the profile.
        std::string str_storage;
        const std::string &str = argvals[0].is_string() ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        int index = 0;
        if (!coerce_numeric_index(argvals[1], index)) {
            return Value::make_int(-1);
        }
        if (index >= 0 && index < static_cast<int>(str.size())) {
            return Value::make_int(static_cast<unsigned char>(str[index]));
        }
    }
    return Value::make_int(-1);
}

Value ScriptRuntime::builtin_from_char_code(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 1) {
        // attempt to coerce float -> int if needed, but reject non-numeric types
        if (argvals[0].is_float()) {
            double f = argvals[0].get_float();
            if (f < 0 || f > 127 || std::floor(f) != f) {
                return Value::make_string("");
            }
            return Value::make_string(std::string(1, static_cast<char>(static_cast<int>(f))));
        }
        if (!argvals[0].is_int()) {
            return runtime_fatal("TypeError: 'from_char_code' expects an integer argument", call);
        }
        int code = argvals[0].get_int();
        if (code < 0 || code > 127) {
            return Value::make_string("");
        }
        return Value::make_string(std::string(1, static_cast<char>(code)));
    }
    return Value::make_string("");
}

// -- encoding builtins --
static const char HEX_DIGITS[] = "0123456789abcdef";

Value ScriptRuntime::builtin_hex_encode(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1) {
        return Value::make_string("");
    }
    std::string s = argvals[0].to_string();
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(HEX_DIGITS[c >> 4]);
        out.push_back(HEX_DIGITS[c & 0x0F]);
    }
    return Value::make_string(out);
}

Value ScriptRuntime::builtin_hex_decode(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1) {
        return Value::make_string("");
    }
    std::string s = argvals[0].to_string();
    if (s.size() % 2 != 0) {
        return runtime_fatal("ValueError: hex.decode expects an even-length string", call);
    }
    auto nibble = [](char ch, bool &ok) -> int {
        if (ch >= '0' && ch <= '9') {
            ok = true;
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            ok = true;
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            ok = true;
            return ch - 'A' + 10;
        }
        ok = false;
        return 0;
    };
    std::string out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        bool ok_hi = false, ok_lo = false;
        int hi = nibble(s[i], ok_hi);
        int lo = nibble(s[i + 1], ok_lo);
        if (!ok_hi || !ok_lo) {
            return runtime_fatal("ValueError: hex.decode found a non-hex character", call);
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return Value::make_string(out);
}

static const char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

Value ScriptRuntime::builtin_base64_encode(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1) {
        return Value::make_string("");
    }
    std::string s = argvals[0].to_string();
    std::string out;
    out.reserve(((s.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < s.size(); i += 3) {
        unsigned int n =
            (static_cast<unsigned char>(s[i]) << 16) | (static_cast<unsigned char>(s[i + 1]) << 8) | (static_cast<unsigned char>(s[i + 2]));
        out.push_back(B64_ALPHABET[(n >> 18) & 0x3F]);
        out.push_back(B64_ALPHABET[(n >> 12) & 0x3F]);
        out.push_back(B64_ALPHABET[(n >> 6) & 0x3F]);
        out.push_back(B64_ALPHABET[n & 0x3F]);
    }
    size_t rem = s.size() - i;
    if (rem == 1) {
        unsigned int n = static_cast<unsigned char>(s[i]) << 16;
        out.push_back(B64_ALPHABET[(n >> 18) & 0x3F]);
        out.push_back(B64_ALPHABET[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        unsigned int n = (static_cast<unsigned char>(s[i]) << 16) | (static_cast<unsigned char>(s[i + 1]) << 8);
        out.push_back(B64_ALPHABET[(n >> 18) & 0x3F]);
        out.push_back(B64_ALPHABET[(n >> 12) & 0x3F]);
        out.push_back(B64_ALPHABET[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return Value::make_string(out);
}

Value ScriptRuntime::builtin_base64_decode(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1) {
        return Value::make_string("");
    }
    std::string s = argvals[0].to_string();
    int rev[256];
    for (int i = 0; i < 256; ++i) {
        rev[i] = -1;
    }
    for (int i = 0; i < 64; ++i) {
        rev[static_cast<unsigned char>(B64_ALPHABET[i])] = i;
    }
    std::string out;
    out.reserve((s.size() / 4) * 3);
    // Use unsigned to avoid UB from left-shift of int values into the sign bit.
    unsigned int buf = 0;
    int bits = 0;
    for (char ch : s) {
        if (ch == '=') {
            break;
        }
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') {
            continue;
        }
        int v = rev[static_cast<unsigned char>(ch)];
        if (v < 0) {
            return runtime_fatal("ValueError: base64.decode found an invalid character", call);
        }
        buf = (buf << 6) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return Value::make_string(out);
}

Value ScriptRuntime::builtin_pad_start(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str = argvals[0].to_string();
        int target_len = argvals[1].get_int();
        std::string pad_str = (argc >= 3) ? argvals[2].to_string() : " ";
        if (pad_str.empty() || static_cast<int>(str.size()) >= target_len) {
            return Value::make_string(str);
        }
        std::string padding;
        while (static_cast<int>(padding.size() + str.size()) < target_len) {
            for (size_t ci = 0; ci < pad_str.size(); ci++) {
                if (static_cast<int>(padding.size() + str.size()) >= target_len) {
                    break;
                }
                padding += pad_str[ci];
            }
        }
        return Value::make_string(padding + str);
    }
    return Value::make_string(argc > 0 ? argvals[0].to_string() : "");
}

Value ScriptRuntime::builtin_pad_end(Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
    if (argc >= 2) {
        std::string str = argvals[0].to_string();
        int target_len = argvals[1].get_int();
        std::string pad_str = (argc >= 3) ? argvals[2].to_string() : " ";
        if (pad_str.empty() || static_cast<int>(str.size()) >= target_len) {
            return Value::make_string(str);
        }
        while (static_cast<int>(str.size()) < target_len) {
            for (size_t ci = 0; ci < pad_str.size(); ci++) {
                if (static_cast<int>(str.size()) >= target_len) {
                    break;
                }
                str += pad_str[ci];
            }
        }
        return Value::make_string(str);
    }
    return Value::make_string(argc > 0 ? argvals[0].to_string() : "");
}

Value ScriptRuntime::builtin_repeat(Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
    if (argc >= 2) {
        if (!argvals[0].is_string()) {
            return runtime_fatal("TypeError: 'repeat' can only be called on a string, got " + value_type_name(argvals[0]), callExpr);
        }
        std::string str = argvals[0].to_string();
        int count = argvals[1].get_int();
        if (count < 0) {
            return runtime_fatal("RangeError: repeat count must be non-negative", callExpr);
        }
        std::string result;
        result.reserve(str.size() * static_cast<size_t>(count));
        for (int i = 0; i < count; i++) {
            result += str;
        }
        return Value::make_string(result);
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_join(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_array()) {
        auto &arr = argvals[0].get_array();

        std::string delim = argvals[1].to_string();
        std::string result;
        bool first = true;
        for (const auto &val : arr) {
            if (!first) {
                result += delim;
            }
            result += val.to_string();
            first = false;
        }
        return Value::make_string(result);
    }
    return Value::make_string("");
}
