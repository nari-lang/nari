#include "common.h"
#include "thirdparty/json.hpp"
#include <charconv>

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

Value ScriptRuntime::builtin_push(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_array()) {
        // push mutates the array, so we need to cast away const
        auto &arr_ptr = const_cast<Value &>(argvals[0]).get_array();
        arr_ptr.push_back(argvals[1]);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_pop(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_array()) {
        auto &arr_ptr = const_cast<Value &>(argvals[0]).get_array();
        if (!arr_ptr.empty()) {
            Value last = arr_ptr.back();
            arr_ptr.pop_back();
            return last;
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_length(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        if (argvals[0].is_array()) {
            auto &arr = argvals[0].get_array();
            return Value::make_int(arr.size());
        } else if (argvals[0].is_string()) {
            return Value::make_int(argvals[0].get_string().size());
        } else if (argvals[0].is_object()) {
            return Value::make_int(argvals[0].get_obj_ptr()->field_count());
        }
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_slice(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_array()) {
        auto &arr_ptr = argvals[0].get_array();

        int start = 0;
        if (argc > 1) {
            if (!argvals[1].is_int()) {
                return Value::make_array();
            }
            start = argvals[1].get_int();
        }
        int end = arr_ptr.size();
        if (argc > 2) {
            if (!argvals[2].is_int()) {
                return Value::make_array();
            }
            end = argvals[2].get_int();
        }

        if (start < 0) {
            start = 0;
        }
        if (end > (int)arr_ptr.size()) {
            end = arr_ptr.size();
        }
        if (start > end) {
            start = end;
        }

        std::vector<Value> result;
        for (int i = start; i < end; ++i) {
            result.push_back(arr_ptr[i]);
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_concat(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_array() && argvals[1].is_array()) {
        auto &arr1 = argvals[0].get_array();
        auto &arr2 = argvals[1].get_array();

        std::vector<Value> result(arr1.begin(), arr1.end());
        result.insert(result.end(), arr2.begin(), arr2.end());
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

// Object builtins
Value ScriptRuntime::builtin_keys(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();

        std::vector<Value> result;
        for (const auto &name : oobj->get_keys()) {
            result.push_back(Value::make_string(name));
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_values(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();

        std::vector<Value> result;
        for (const auto &name : oobj->get_keys()) {
            if (const Value *v = oobj->get_field(name)) {
                result.push_back(*v);
            }
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_hasKey(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();

        std::string key = argvals[1].to_string();
        return Value::make_bool(oobj->has_field(key));
    }
    // Delegate has trap: has_key(delegate, key) -> handler.has(target, key).
    if (argc >= 2 && argvals[0].is_delegate()) {
        return Value::make_bool(delegate_has(argvals[0], argvals[1]));
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_entries(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();
        std::vector<Value> result;
        for (const auto &name : oobj->get_keys()) {
            const Value *val = oobj->get_field(name);
            if (!val) {
                continue;
            }
            std::vector<Value> pair;
            pair.push_back(Value::make_string(name));
            pair.push_back(*val);
            result.push_back(Value::make_array(std::move(pair)));
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_assign(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2 && argvals[0].is_object()) {
        ObjectObj *target = const_cast<Value &>(argvals[0]).get_obj_ptr();
        for (size_t i = 1; i < argc; i++) {
            if (!argvals[i].is_object()) {
                continue;
            }
            const ObjectObj *src = argvals[i].get_obj_ptr();
            for (const auto &name : src->get_keys()) {
                if (const Value *val = src->get_field(name)) {
                    target->set_field(name, *val);
                }
            }
        }
        return argvals[0];
    }
    return argc > 0 ? argvals[0] : Value::none();
}

Value ScriptRuntime::builtin_freeze(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_object()) {
        ObjectObj *oobj = const_cast<Value &>(argvals[0]).get_obj_ptr();
        oobj->frozen = true;
        return argvals[0];
    }
    return argc > 0 ? argvals[0] : Value::none();
}

Value ScriptRuntime::builtin_isFrozen(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_object()) {
        return Value::make_bool(argvals[0].get_obj_ptr()->frozen);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_yield(const Value *, size_t, const nari::CallExpr *) {
    if (Runtime::g_shutdown_requested.load()) {
        if (io_pool) {
#ifndef NO_THREADS
            // close all server sockets to unblock accept() calls
            std::lock_guard<std::mutex> lock(server_sockets_mutex);
            for (int fd : server_sockets) {
                NARI_CLOSE_SOCKET(fd);
            }
            server_sockets.clear();
#endif
            io_pool->shutdown();
        }
    }

    process_completed_io();

    int tasks_processed = 0;
    while (!task_queue.empty() && tasks_processed < 10) {
        HandlePtr next_task = task_queue.front();
        task_queue.pop();
        step_task(next_task);
        if (next_task->state == HandleData::Running) {
            task_queue.push(next_task);
        }
        tasks_processed++;
    }

    NARI_SLEEP_MILLIS(1);
    process_completed_io();

    return Value::none();
}

Value ScriptRuntime::builtin_shutdown_requested(const Value *, size_t, const nari::CallExpr *) {
    return Value::make_bool(Runtime::g_shutdown_requested.load());
}

// String builtins
Value ScriptRuntime::builtin_substr(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str_storage;
        const std::string &str = argvals[0].is_string() ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        int start = 0;
        if (argc > 1) {
            if (!coerce_numeric_index(argvals[1], start)) {
                return Value::make_string("");
            }
        }
        int len = str.size() - start;
        if (argc > 2) {
            if (!coerce_numeric_index(argvals[2], len)) {
                return Value::make_string("");
            }
        }

        if (start < 0) {
            start = 0;
        }
        if (start >= str.size()) {
            return Value::make_string("");
        }
        if (len < 0) {
            len = 0;
        }
        if (start + len > str.size()) {
            len = str.size() - start;
        }

        return Value::make_string(str.substr(start, len));
    }
    return Value::make_string("");
}

// TODO: make index_of only show up on string and array, like we do with methods exclusive to those types, instead of being universal
Value ScriptRuntime::builtin_indexOf(const Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
    if (argc < 2) {
        return Value::make_int(-1);
    }

    // array.index_of(val)
    if (argvals[0].is_array()) {
        const auto &arr = argvals[0].get_array();
        for (size_t i = 0; i < arr.size(); i++) {
            if (Value::values_equal(arr[i], argvals[1])) {
                return Value::make_int((int64_t)i);
            }
        }
        return Value::make_int(-1);
    }

    // string.index_of(search)
    if (!argvals[0].is_string()) {
        runtime_fatal("TypeError: 'index_of' can only be called on a string or array, got " + value_type_name(argvals[0]), callExpr);
    }
    std::string str = argvals[0].to_string();
    std::string search = argvals[1].to_string();
    size_t pos = str.find(search);
    if (pos == std::string::npos) {
        return Value::make_int(-1);
    }

    return Value::make_int((int64_t)pos);
}

Value ScriptRuntime::builtin_lastIndexOf(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str = argvals[0].to_string();
        std::string search = argvals[1].to_string();

        if (search.empty()) {
            if (argc > 2 && (argvals[2].is_int() || argvals[2].is_float())) {
                int64_t fi = (argvals[2].is_int()) ? argvals[2].get_int() : static_cast<int64_t>(argvals[2].get_float());
                if (fi < 0) {
                    return Value::make_int(-1);
                }

                size_t pos = fi > str.size() ? str.size() : fi;
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
                from_pos = fi > maxpos ? maxpos : fi;
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

Value ScriptRuntime::builtin_split(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str_storage;
        std::string delim_storage;
        const std::string &str = (argvals[0].is_string() && !argvals[0].is_sso()) ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        const std::string &delim = (argvals[1].is_string() && !argvals[1].is_sso()) ? argvals[1].get_string() : (delim_storage = argvals[1].to_string());
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

Value ScriptRuntime::builtin_replace(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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

Value ScriptRuntime::builtin_replaceAll(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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
    return (argc == 0) ? Value::make_string("")
                       : Value::make_string(argvals[0].to_string());
}

Value ScriptRuntime::builtin_trim(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        size_t start = 0;
        while (start < str.size() &&
               std::isspace(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        size_t end = str.size();
        while (end > start &&
               std::isspace(static_cast<unsigned char>(str[end - 1]))) {
            --end;
        }
        return Value::make_string(str.substr(start, end - start));
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_trimStart(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        size_t start = 0;
        while (start < str.size() &&
               std::isspace(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        return Value::make_string(str.substr(start));
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_trimEnd(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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

Value ScriptRuntime::builtin_string_at(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
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

Value ScriptRuntime::builtin_toCharArray(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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

Value ScriptRuntime::builtin_toUpper(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        for (char &c : str) {
            c = std::toupper(c);
        }
        return Value::make_string(str);
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_toLower(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc > 0) {
        std::string str = argvals[0].to_string();
        for (char &c : str) {
            c = std::tolower(c);
        }
        return Value::make_string(str);
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_startsWith(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str_storage;
        std::string prefix_storage;
        const std::string &str = (argvals[0].is_string() && !argvals[0].is_sso()) ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        const std::string &prefix = (argvals[1].is_string() && !argvals[1].is_sso()) ? argvals[1].get_string() : (prefix_storage = argvals[1].to_string());
        return Value::make_bool(str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_endsWith(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        std::string str_storage;
        std::string suffix_storage;
        const std::string &str = (argvals[0].is_string() && !argvals[0].is_sso()) ? argvals[0].get_string() : (str_storage = argvals[0].to_string());
        const std::string &suffix = (argvals[1].is_string() && !argvals[1].is_sso()) ? argvals[1].get_string() : (suffix_storage = argvals[1].to_string());
        return Value::make_bool(str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_charAt(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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

Value ScriptRuntime::builtin_charCodeAt(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2) {
        // Avoid copying the receiver: read the byte directly from the live string
        // buffer when possible. byte_hash calls this ~720K times on ~30-char
        // strings, where a per-call to_string() copy dominated the profile.
        std::string str_storage;
        const std::string &str = argvals[0].is_string()
                                     ? argvals[0].get_string()
                                     : (str_storage = argvals[0].to_string());
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

Value ScriptRuntime::builtin_fromCharCode(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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
            runtime_fatal("TypeError: 'from_char_code' expects an integer argument", call);
        }
        int code = argvals[0].get_int();
        if (code < 0 || code > 127) {
            return Value::make_string("");
        }
        return Value::make_string(std::string(1, static_cast<char>(code)));
    }
    return Value::make_string("");
}

// ---- Encoding builtins (hex / base64) ----
// Nari strings are raw byte buffers, so these are binary-safe over bytes
// 0-255 (unlike from_char_code, which caps at 127). Backing the Hex/Base64
// stdlib globals.

static const char HEX_DIGITS[] = "0123456789abcdef";

Value ScriptRuntime::builtin_hex_encode(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_hex_decode(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1) {
        return Value::make_string("");
    }
    std::string s = argvals[0].to_string();
    if (s.size() % 2 != 0) {
        runtime_fatal("ValueError: hex.decode expects an even-length string", call);
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
            runtime_fatal("ValueError: hex.decode found a non-hex character", call);
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return Value::make_string(out);
}

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

Value ScriptRuntime::builtin_base64_encode(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1) {
        return Value::make_string("");
    }
    std::string s = argvals[0].to_string();
    std::string out;
    out.reserve(((s.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < s.size(); i += 3) {
        unsigned int n = (static_cast<unsigned char>(s[i]) << 16) |
                         (static_cast<unsigned char>(s[i + 1]) << 8) |
                         (static_cast<unsigned char>(s[i + 2]));
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
        unsigned int n = (static_cast<unsigned char>(s[i]) << 16) |
                         (static_cast<unsigned char>(s[i + 1]) << 8);
        out.push_back(B64_ALPHABET[(n >> 18) & 0x3F]);
        out.push_back(B64_ALPHABET[(n >> 12) & 0x3F]);
        out.push_back(B64_ALPHABET[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return Value::make_string(out);
}

Value ScriptRuntime::builtin_base64_decode(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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
            runtime_fatal("ValueError: base64.decode found an invalid character", call);
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

Value ScriptRuntime::builtin_padStart(const Value *argvals, size_t argc, const nari::CallExpr *call) {
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

Value ScriptRuntime::builtin_padEnd(const Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
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

Value ScriptRuntime::builtin_repeat(const Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
    if (argc >= 2) {
        if (!argvals[0].is_string()) {
            runtime_fatal("TypeError: 'repeat' can only be called on a string, got " + value_type_name(argvals[0]), callExpr);
        }
        std::string str = argvals[0].to_string();
        int count = argvals[1].get_int();
        if (count < 0) {
            runtime_fatal("RangeError: repeat count must be non-negative", callExpr);
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

Value ScriptRuntime::builtin_join(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_sort(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::none();
    }
    auto &arr = const_cast<Value &>(argvals[0]).get_array();
    if (argc >= 2 && argvals[1].is_function()) {
        Value cmp = argvals[1];
        // root the comparator and the array under sort.
        // the callback runs nested bytecode, and without rooting, a precise collection could cause a dangling ref
        Value arr_val = argvals[0];
        GcTempRoot _gr(*this);
        _gr.add(&cmp);
        _gr.add(&arr_val);
        std::stable_sort(
            arr.begin(),
            arr.end(),
            [&](const Value &a, const Value &b) {
                Value r = call_function_value(cmp, { a, b });
                return r.is_int() ? r.get_int() < 0 : r.as_number() < 0.0;
            });
    } else {
        std::stable_sort(
            arr.begin(),
            arr.end(),
            [](const Value &a, const Value &b) {
                if (a.is_int() && b.is_int()) {
                    return a.get_int() < b.get_int();
                }
                if (a.is_numeric() && b.is_numeric()) {
                    return a.as_number() < b.as_number();
                }
                return a.to_string() < b.to_string();
            });
    }
    return argvals[0];
}

Value ScriptRuntime::builtin_map(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_array();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    std::vector<Value> result;
    result.reserve(arr.size());
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    _gr.add_vec(&result);
    for (size_t i = 0; i < arr.size(); i++) {
        result.push_back(call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val }));
    }
    return Value::make_array(std::move(result));
}

Value ScriptRuntime::builtin_filter(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_array();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    std::vector<Value> result;
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    _gr.add_vec(&result);
    for (size_t i = 0; i < arr.size(); i++) {
        Value keep = call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val });
        if (keep.as_bool()) {
            result.push_back(arr[i]);
        }
    }
    return Value::make_array(std::move(result));
}

Value ScriptRuntime::builtin_reduce(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::none();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    Value acc;
    size_t start = 0;
    if (argc >= 3) {
        acc = argvals[2];
    } else {
        if (arr.empty()) {
            return Value::none();
        }
        acc = arr[0];
        start = 1;
    }
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    _gr.add(&acc);
    for (size_t i = start; i < arr.size(); i++) {
        acc = call_function_value(fn, { acc, arr[i], Value::make_int((int64_t)i), arr_val });
    }
    return acc;
}

Value ScriptRuntime::builtin_find(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::none();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r =
            call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val });
        if (r.as_bool()) {
            return arr[i];
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_findIndex(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_int(-1);
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r = call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val });
        if (r.as_bool()) {
            return Value::make_int((int64_t)i);
        }
    }
    return Value::make_int(-1);
}

Value ScriptRuntime::builtin_reverse(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::none();
    }
    auto &arr = const_cast<Value &>(argvals[0]).get_array();
    std::reverse(arr.begin(), arr.end());
    return argvals[0];
}

Value ScriptRuntime::builtin_includes(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2) {
        return Value::make_bool(false);
    }
    if (argvals[0].is_array()) {
        for (const auto &el : argvals[0].get_array()) {
            if (Value::values_equal(el, argvals[1])) {
                return Value::make_bool(true);
            }
        }
        return Value::make_bool(false);
    }
    // string includes
    std::string str = argvals[0].to_string();
    std::string search = argvals[1].to_string();
    return Value::make_bool(str.find(search) != std::string::npos);
}

Value ScriptRuntime::builtin_every(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_bool(true);
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r =
            call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val });
        if (!r.as_bool()) {
            return Value::make_bool(false);
        }
    }
    return Value::make_bool(true);
}

Value ScriptRuntime::builtin_some(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_bool(false);
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r =
            call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val });
        if (r.as_bool()) {
            return Value::make_bool(true);
        }
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_forEach(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::none();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val });
    }
    return Value::none();
}

Value ScriptRuntime::builtin_splice(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::make_array();
    }
    // TODO: I would really love to not use const_cast here, but this is simpler than refactoring several hundred functions to not be const
    auto &arr = const_cast<Value &>(argvals[0]).get_array();
    int64_t sz = (int64_t)arr.size();

    int64_t start = (argc >= 2) ? argvals[1].get_int() : 0;
    if (start < 0) {
        start = std::max((int64_t)0, sz + start);
    }
    if (start > sz) {
        start = sz;
    }

    int64_t del_count = sz - start;
    if (argc >= 3) {
        del_count = argvals[2].get_int();
        if (del_count < 0) {
            del_count = 0;
        }
        if (del_count > sz - start) {
            del_count = sz - start;
        }
    }

    std::vector<Value> removed(arr.begin() + start, arr.begin() + start + del_count);
    arr.erase(arr.begin() + start, arr.begin() + start + del_count);
    for (size_t i = 3; i < argc; i++) {
        arr.insert(arr.begin() + start + (int64_t)(i - 3), argvals[i]);
    }
    return Value::make_array(std::move(removed));
}

// arr.fill(value, count) - resize and fill with count copies of value
// arr.fill(value)  - fill existing elements with value
Value ScriptRuntime::builtin_fill(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::make_array();
    }
    auto &arr = const_cast<Value &>(argvals[0]).get_array();
    Value fill_val = (argc >= 2) ? argvals[1] : Value::none();
    if (argc >= 3 && argvals[2].is_int()) {
        int64_t count = argvals[2].get_int();
        if (count < 0) {
            count = 0;
        }
        if (count > 10000000) {
            count = 10000000;
        }
        arr.resize(static_cast<size_t>(count), fill_val);
        // resize only sets NEW elements; overwrite existing ones too
        for (size_t i = 0; i < arr.size(); i++) {
            arr[i] = fill_val;
        }
    } else {
        // No count: fill existing elements
        for (size_t i = 0; i < arr.size(); i++) {
            arr[i] = fill_val;
        }
    }
    return argvals[0];
}

static void flatten_into(std::vector<Value> &result, const Array &arr, int64_t depth) {
    for (const auto &v : arr) {
        if (depth > 0 && v.is_array()) {
            flatten_into(result, v.get_array(), depth - 1);
        } else {
            result.push_back(v);
        }
    }
}

Value ScriptRuntime::builtin_flat(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::make_array();
    }
    int64_t depth = 1;
    if (argc >= 2 && argvals[1].is_int()) {
        depth = argvals[1].get_int();
    } else if (argc >= 2 && argvals[1].is_float()) {
        depth = (int64_t)argvals[1].get_float();
    }
    if (depth < 0) {
        depth = 0;
    }
    const auto &arr = argvals[0].get_array();
    std::vector<Value> result;
    flatten_into(result, arr, depth);
    return Value::make_array(std::move(result));
}

Value ScriptRuntime::builtin_flatMap(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_array();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    std::vector<Value> result;
    GcTempRoot _gr(*this);
    _gr.add(&fn);
    _gr.add(&arr_val);
    _gr.add_vec(&result);
    for (size_t i = 0; i < arr.size(); i++) {
        Value mapped = call_function_value(fn, { arr[i], Value::make_int((int64_t)i), arr_val });
        if (mapped.is_array()) {
            const auto &inner = mapped.get_array();
            result.insert(result.end(), inner.begin(), inner.end());
        } else {
            result.push_back(mapped);
        }
    }
    return Value::make_array(std::move(result));
}

// Type checking and conversion builtins
Value ScriptRuntime::builtin_typeof(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        if (argvals[0].is_none()) {
            return Value::make_string("null");
        }
        if (argvals[0].is_int()) {
            return Value::make_string("int");
        }
        if (argvals[0].is_float()) {
            return Value::make_string("float");
        }
        if (argvals[0].is_string()) {
            return Value::make_string("string");
        }
        if (argvals[0].is_bool()) {
            return Value::make_string("bool");
        }
        if (argvals[0].is_array()) {
            return Value::make_string("array");
        }
        if (argvals[0].is_object()) {
            return Value::make_string("object");
        }
        if (argvals[0].is_function()) {
            return Value::make_string("function");
        }
        if (argvals[0].is_regex()) {
            return Value::make_string("regex");
        }
        if (argvals[0].is_handle()) {
            return Value::make_string("handle");
        }
        if (argvals[0].is_class_instance()) {
            return Value::make_string(argvals[0].get_class_instance()->class_name);
        }
        return Value::make_string("null");
    }
    return Value::make_string("null");
}

// Build SRELL syntax_option from a flags string
static srell::regex_constants::syntax_option_type srell_flags_from_string(const std::string &flags) {
    using namespace srell::regex_constants;
    syntax_option_type opts = ECMAScript;
    for (char f : flags) {
        if (f == 'i') {
            opts = opts | icase;
        }
        if (f == 'm') {
            opts = opts | multiline;
        }
        if (f == 's') {
            opts = opts | dotall;
        }
        // g, u, v, y are silently accepted (global/sticky handled by caller)
    }
    return opts;
}

// Regex.new(pattern, flags?) -> Result<regex, string>. Validates pattern by
// attempting to compile it; returns Err(RegexError message) on malformed input.
Value ScriptRuntime::builtin_regex_new(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1 || !argvals[0].is_string()) {
        runtime_fatal("TypeError: 'Regex.new' expects a pattern string", call);
        return Value::none();
    }
    std::string pattern = argvals[0].get_string();
    std::string flags;
    if (argc >= 2 && argvals[1].is_string()) {
        flags = argvals[1].get_string();
    }
    // check up front so that we can easily return a RegexError
    srell::u8cregex re(pattern, srell_flags_from_string(flags));
    if (re.ecode() != 0) {
        return make_err(Value::make_string("RegexError: invalid pattern (srell error " + std::to_string((int)re.ecode()) + ")"));
    }
    return make_ok(Value::make_regex(std::move(pattern), std::move(flags)));
}

// Regex.test(string) -> bool
Value ScriptRuntime::builtin_regex_test(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2) {
        runtime_fatal("TypeError: 'test' requires a string argument", call);
        return Value::none();
    }
    if (!argvals[0].is_regex()) {
        runtime_fatal("TypeError: 'test' called on non-regex", call);
        return Value::none();
    }
    if (!argvals[1].is_string()) {
        return Value::make_bool(false);
    }
    const RegexObj *re_obj = argvals[0].get_regex();
    const std::string &subject = argvals[1].get_string();
    try {
        srell::u8cregex re(re_obj->pattern, srell_flags_from_string(re_obj->flags));
        srell::u8csmatch m;
        return Value::make_bool(srell::regex_search(subject, m, re));
    } catch (const srell::regex_error &e) {
        runtime_fatal(std::string("RegexError: ") + e.what(), call);
        return Value::none();
    }
}

// Regex.exec(string) -> object {match, index, groups:[...]} or null
Value ScriptRuntime::builtin_regex_exec(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2) {
        runtime_fatal("TypeError: 'exec' requires a string argument", call);
        return Value::none();
    }
    if (!argvals[0].is_regex()) {
        runtime_fatal("TypeError: 'exec' called on non-regex", call);
        return Value::none();
    }
    if (!argvals[1].is_string()) {
        return Value::none(); // null
    }
    const RegexObj *re_obj = argvals[0].get_regex();
    const std::string &subject = argvals[1].get_string();
    try {
        srell::u8cregex re(re_obj->pattern, srell_flags_from_string(re_obj->flags));
        srell::u8csmatch m;
        if (!srell::regex_search(subject, m, re)) {
            return Value::none();
        }
        // build result object: { value: str, index: n, groups: [...] }
        Value result = Value::make_object();
        ObjectObj *res_oobj = result.get_obj_ptr();
        res_oobj->set_field("value", Value::make_string(m[0].str()));
        res_oobj->set_field("index", Value::make_int(static_cast<int64_t>(m.position(0))));
        Value groups = Value::make_array();
        auto &arr = groups.get_array();
        for (size_t i = 0; i < m.size(); i++) {
            if (m[i].matched) {
                arr.push_back(Value::make_string(m[i].str()));
            } else {
                arr.push_back(Value::none());
            }
        }
        res_oobj->set_field("groups", groups);
        return result;
    } catch (const srell::regex_error &e) {
        runtime_fatal(std::string("RegexError: ") + e.what(), call);
        return Value::none();
    }
}

Value ScriptRuntime::builtin_toNumber(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        const Value &v = argvals[0];
        if (v.is_int()) {
            return Value::make_int(v.get_int());
        }
        if (v.is_float()) {
            return Value::make_float(v.get_float());
        }
        if (v.is_bool()) {
            return Value::make_int(v.get_bool() ? 1 : 0);
        }
        if (v.is_string()) {
            const std::string &s = v.get_string();
            // in the case where it's a clean, fully-consumed base-10 integer, from_chars is faster than strtoll
            {
                const char *b = s.data();
                const char *e = b + s.size();
                int64_t iv = 0;
                auto [ptr, ec] = std::from_chars(b, e, iv);
                if (ec == std::errc() && ptr == e) {
                    return Value::make_int(iv);
                }
            }
            bool is_float =
                (s.find('.') != std::string::npos) ||
                (s.find('e') != std::string::npos) ||
                (s.find('E') != std::string::npos);

            if (is_float) {
                char *end = nullptr;
                double dv = std::strtod(s.c_str(), &end);
                if (end && *end == '\0') {
                    return Value::make_float(dv);
                }
                return Value::make_float(0.0);
            }
            errno = 0;
            char *end = nullptr;
            int64_t iv = std::strtoll(s.c_str(), &end, 10);
            if (end && *end == '\0' && errno != ERANGE) {
                return Value::make_int(static_cast<int64_t>(iv));
            }
            return Value::make_float(std::strtod(s.c_str(), &end));
        }
        return Value::make_int(0);
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_toString(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_string(argvals[0].to_string());
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_formatValue(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 0) {
        return Value::make_string("");
    }
    if (argc < 2 || !argvals[1].is_string()) {
        return Value::make_string(argvals[0].to_string());
    }

    const std::string &spec = argvals[1].get_string();
    if (spec.empty()) {
        return Value::make_string(argvals[0].to_string());
    }

    size_t pos = 0;
    int precision = -1;
    if (pos < spec.size() && spec[pos] == '.') {
        ++pos;
        if (pos >= spec.size() || !std::isdigit(static_cast<unsigned char>(spec[pos]))) {
            return Value::make_string(argvals[0].to_string());
        }
        precision = 0;
        while (pos < spec.size() && std::isdigit(static_cast<unsigned char>(spec[pos]))) {
            precision = precision * 10 + (spec[pos] - '0');
            ++pos;
        }
    }

    char presentation = pos < spec.size() ? spec[pos++] : '\0';
    if (pos != spec.size() || presentation != 'f') {
        return Value::make_string(argvals[0].to_string());
    }

    if (!argvals[0].is_int() && !argvals[0].is_float()) {
        return Value::make_string(argvals[0].to_string());
    }

    char fmt[16];
    if (precision >= 0) {
        if (precision > 100) {
            precision = 100;
        }
        std::snprintf(fmt, sizeof(fmt), "%%.%df", precision);
    } else {
        std::snprintf(fmt, sizeof(fmt), "%%f");
    }

    char stack_buf[128];
    double value = argvals[0].as_number();
    int needed = std::snprintf(stack_buf, sizeof(stack_buf), fmt, value);
    if (needed < 0) {
        return Value::make_string(argvals[0].to_string());
    }
    if ((size_t)needed < sizeof(stack_buf)) {
        return Value::make_string(std::string(stack_buf, static_cast<size_t>(needed)));
    }

    std::string out((size_t)needed, '\0');
    std::snprintf(out.data(), out.size() + 1, fmt, value);
    return Value::make_string(out);
}

Value ScriptRuntime::builtin_toBool(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].as_bool());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isNumber(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_int() || argvals[0].is_float());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isString(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_string());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isBool(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_bool());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isArray(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_array());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isObject(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_object());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isFunction(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_function());
    }
    return Value::make_bool(false);
}

// Delegate(target, handler) -> a wrapper object to proxy object access.
// `handler` is an object whose optional "get"/"set"/"has"/"call" fields 
// intercept the corresponding operations on the delegate; any absent trap falls through to `target`.
Value ScriptRuntime::builtin_delegate_new(const Value *argvals, size_t argc, const nari::CallExpr *) {
    Value target = argc > 0 ? argvals[0] : Value::none();
    Value handler = argc > 1 ? argvals[1] : Value::none();
    return Value::make_delegate(target, handler);
}

Value ScriptRuntime::builtin_isDelegate(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_delegate());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_delegateTarget(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        if (DelegateData *d = argvals[0].get_delegate()) {
            return d->target;
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_delegateHandler(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        if (DelegateData *d = argvals[0].get_delegate()) {
            return d->handler;
        }
    }
    return Value::none();
}
Value ScriptRuntime::builtin_math_floor(const Value *args, size_t argc, const CallExpr *) {
    if (argc > 0) {
        return Value::make_int(static_cast<int64_t>(std::floor(args[0].as_number())));
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_math_ceil(const Value *args, size_t argc, const CallExpr *) {
    if (argc > 0) {
        return Value::make_int(static_cast<int64_t>(std::ceil(args[0].as_number())));
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_parseInt(const Value *args, size_t argc, const CallExpr *) {
    if (argc == 0) {
        return Value::make_int(0);
    }
    if (args[0].is_int()) {
        return args[0];
    }
    if (args[0].is_float()) {
        return Value::make_int(static_cast<int64_t>(args[0].get_float()));
    }
    if (args[0].is_string()) {
        char *end = nullptr;
        int64_t v = std::strtoll(args[0].get_string().c_str(), &end, 10);
        return Value::make_int(v);
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_parseFloat(const Value *args, size_t argc, const CallExpr *) {
    if (argc == 0) {
        return Value::make_float(0.0);
    }
    if (args[0].is_float()) {
        return args[0];
    }
    if (args[0].is_int()) {
        return Value::make_float(static_cast<double>(args[0].get_int()));
    }
    if (args[0].is_string()) {
        char *end = nullptr;
        double v = std::strtod(args[0].get_string().c_str(), &end);
        return Value::make_float(v);
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_random(const Value *, size_t, const CallExpr *) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    return Value::make_float(dis(gen));
}

Value ScriptRuntime::builtin_range(const Value *args, size_t argc, const CallExpr *) {
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) {
        stop = static_cast<int64_t>(args[0].as_number());
    } else if (argc >= 2) {
        start = static_cast<int64_t>(args[0].as_number());
        stop = static_cast<int64_t>(args[1].as_number());
    }
    if (argc >= 3) {
        step = static_cast<int64_t>(args[2].as_number());
        if (step == 0) {
            step = 1;
        }
    }
    std::vector<Value> arr;
    if (step > 0) {
        for (int64_t i = start; i < stop; i += step) {
            arr.push_back(Value::make_int(i));
        }
    } else {
        for (int64_t i = start; i > stop; i += step) {
            arr.push_back(Value::make_int(i));
        }
    }
    return Value::make_array(std::move(arr));
}

Value ScriptRuntime::builtin_contains(const Value *args, size_t argc, const CallExpr *) {
    if (argc >= 2 && args[0].is_array()) {
        auto &arr = args[0].get_array();
        for (const auto &item : arr) {
            if (Value::values_equal(item, args[1])) {
                return Value::make_bool(true);
            }
        }
        return Value::make_bool(false);
    }
    return Value::make_bool(false);
}

// JSON builtins

static Value json_to_value(const nlohmann::json &j) {
    if (j.is_null()) {
        return Value::none();
    }
    if (j.is_boolean()) {
        return Value::make_bool(j.get<bool>());
    }
    if (j.is_number_integer()) {
        return Value::make_int(j.get<int64_t>());
    }
    if (j.is_number_unsigned()) {
        return Value::make_int(static_cast<int64_t>(j.get<uint64_t>()));
    }
    if (j.is_number_float()) {
        return Value::make_float(j.get<double>());
    }
    if (j.is_string()) {
        return Value::make_string(j.get<std::string>());
    }
    if (j.is_array()) {
        std::vector<Value> arr;
        arr.reserve(j.size());
        for (const auto &el : j) {
            arr.push_back(json_to_value(el));
        }
        return Value::make_array(std::move(arr));
    }
    if (j.is_object()) {
        Value obj = Value::make_object();
        ObjectObj *oobj = obj.get_obj_ptr();
        for (const auto &[key, val] : j.items()) {
            oobj->set_field(key, json_to_value(val));
        }
        return obj;
    }
    return Value::none();
}

// max container nesting for JSON serialization; also bounds C-stack recursion.
static constexpr size_t kJsonMaxDepth = 512;

static bool json_enter(std::vector<const void *> &seen, const void *p) {
    if (seen.size() >= kJsonMaxDepth) {
        return false;
    }
    for (const void *q : seen) {
        if (q == p) {
            return false;
        }
    }
    seen.push_back(p);
    return true;
}

// on cyclic or too-deeply-nested input, sets ok=false and returns null.
static nlohmann::json value_to_json(const Value &v, std::vector<const void *> &seen, bool &ok) {
    if (v.is_none()) {
        return nullptr;
    }
    if (v.is_bool()) {
        return v.get_bool();
    }
    if (v.is_int()) {
        return v.get_int();
    }
    if (v.is_float()) {
        return v.get_float();
    }
    if (v.is_string()) {
        return v.get_string();
    }
    if (v.is_array()) {
        if (!json_enter(seen, v.heap_ptr())) {
            ok = false;
            return nullptr;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &el : v.get_array()) {
            arr.push_back(value_to_json(el, seen, ok));
            if (!ok) {
                return nullptr;
            }
        }
        seen.pop_back();
        return arr;
    }
    if (v.is_object()) {
        if (!json_enter(seen, v.heap_ptr())) {
            ok = false;
            return nullptr;
        }
        nlohmann::json obj = nlohmann::json::object();
        const ObjectObj *oobj = v.get_obj_ptr();
        for (const auto &name : oobj->get_keys()) {
            if (const Value *val = oobj->get_field(name)) {
                obj[name] = value_to_json(*val, seen, ok);
                if (!ok) {
                    return nullptr;
                }
            }
        }
        seen.pop_back();
        return obj;
    }
    // class instances: serialize fields like an object
    if (v.is_class_instance()) {
        if (!json_enter(seen, v.heap_ptr())) {
            ok = false;
            return nullptr;
        }
        nlohmann::json obj = nlohmann::json::object();
        const ClassInstance *ci = v.get_class_instance();
        if (ci->layout) {
            for (size_t i = 0; i < ci->field_values.size(); i++) {
                obj[ci->layout->names[i]] = value_to_json(ci->field_values[i], seen, ok);
                if (!ok) {
                    return nullptr;
                }
            }
        }
        seen.pop_back();
        return obj;
    }
    return nullptr;
}

// Direct recursive-descent JSON -> Nari Value parser, no intermediate nlohmann DOM.
// Eliminates the lex->DOM-build->convert->DOM-destroy pipeline
namespace {
// parse errors are recorded in `ok`/`err` and unwound by normal returns
struct JsonDirectParser {
    const char *p;
    const char *end;
    bool ok = true;
    std::string err;
    // current container nesting; capped so malicious/degenerate input
    // (e.g. 100k "[") can't overflow the C stack via recursion
    int depth = 0;
    // Shape of the most recently built object
    const ObjectShape *spec_shape = nullptr;

    void fail(const char *msg) {
        if (ok) {
            ok = false;
            err = msg;
        }
    }

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            if (p >= end) {
                fail("invalid \\u escape");
                return 0;
            }
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= (unsigned)(c - 'A' + 10);
            } else {
                fail("invalid hex digit in \\u escape");
                return 0;
            }
        }
        return v;
    }

    static void append_utf8(std::string &out, unsigned cp) {
        if (cp <= 0x7F) {
            out += (char)cp;
        } else if (cp <= 0x7FF) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    std::string parse_string() {
        // assumes *p == '"'
        p++;
        std::string out;
        while (ok) {
            const char *run = p;
            while (p < end && *p != '"' && *p != '\\') {
                p++;
            }
            if (p > run) {
                out.append(run, (size_t)(p - run));
            }
            if (p >= end) {
                fail("unterminated string");
                break;
            }
            char c = *p++;
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (p >= end) {
                    fail("unterminated escape");
                    break;
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
                        if (!ok) {
                            break;
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // high surrogate: expect a following \uXXXX low surrogate
                            if (p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                                p += 2;
                                unsigned lo = hex4();
                                if (!ok) {
                                    break;
                                }
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    append_utf8(out, cp);
                                    cp = lo;
                                }
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        fail("invalid escape character");
                        break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    bool match_shape_key(const std::string &expected) {
        if (p >= end || *p != '"') {
            return false;
        }
        for (unsigned char c : expected) {
            if (c < 0x20 || c == '"' || c == '\\') {
                return false;
            }
        }
        const size_t size = expected.size();
        if ((size_t)(end - p) < size + 2 || p[size + 1] != '"' ||
            std::memcmp(p + 1, expected.data(), size) != 0) {
            return false;
        }
        p += size + 2;
        return true;
    }

    Value parse_number() {
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
                    const uint64_t digit = (uint64_t)(c - '0');
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
        // Fast path: a plain integer (no '.'/'e'/'E'/'+').
        // The scanner already delimited [start, p), std::from_chars parses it in place with no string allocation and no locale.
        if (!is_float && have_digit && !int_overflow) {
            if (negative) {
                if (magnitude == uint64_t(INT64_MAX) + 1) {
                    return Value::make_int(INT64_MIN);
                }
                return Value::make_int(-(int64_t)magnitude);
            }
            return Value::make_int((int64_t)magnitude);
        }
        if (!is_float) {
            int64_t iv = 0;
            auto [ptr, ec] = std::from_chars(start, p, iv);
            if (ec == std::errc() && ptr == p) {
                return Value::make_int(iv);
            }
        }
        std::string num(start, (size_t)(p - start));
        if (num.empty() || num == "-") {
            fail("invalid number");
            return Value::none();
        }
        if (is_float) {
            return Value::make_float(std::strtod(num.c_str(), nullptr));
        }
        errno = 0;
        char *endp = nullptr;
        long long iv = std::strtoll(num.c_str(), &endp, 10);
        if (errno == ERANGE) {
            // out of int64 range: fall back to double like nlohmann does
            return Value::make_float(std::strtod(num.c_str(), nullptr));
        }
        return Value::make_int((int64_t)iv);
    }

    Value parse_array() {
        p++; // '['
        std::vector<Value> arr;
        skip_ws();
        if (p < end && *p == ']') {
            p++;
            return Value::make_array(std::move(arr));
        }
        arr.reserve(4);
        while (ok) {
            arr.push_back(parse_value());
            if (!ok) {
                break;
            }
            skip_ws();
            if (p >= end) {
                fail("unterminated array");
                break;
            }
            char c = *p++;
            if (c == ']') {
                break;
            }
            if (c != ',') {
                fail("expected ',' or ']' in array");
                break;
            }
        }
        return Value::make_array(std::move(arr));
    }

    Value parse_object() {
        p++; // '{'
        skip_ws();
        if (p < end && *p == '}') {
            p++;
            return Value::make_object();
        }

        // collect key/value pairs first then build, so we can add a cached shape in one shot.
        constexpr size_t kInline = 8;
        std::string ik[kInline];
        Value iv[kInline];
        std::vector<std::pair<std::string, Value>> overflow;
        size_t count = 0;
        const ObjectShape *expected_shape = spec_shape;
        bool shape_matches = expected_shape != nullptr;

        while (ok) {
            skip_ws();
            if (p >= end || *p != '"') {
                fail("expected string key in object");
                break;
            }
            std::string key;
            if (!shape_matches || count >= expected_shape->names.size() ||
                !match_shape_key(expected_shape->names[count])) {
                if (shape_matches) {
                    for (size_t i = 0; i < count; i++) {
                        if (i < kInline) {
                            ik[i] = expected_shape->names[i];
                        } else {
                            overflow[i - kInline].first = expected_shape->names[i];
                        }
                    }
                    shape_matches = false;
                }
                key = parse_string();
            }
            if (!ok) {
                break;
            }
            skip_ws();
            if (p >= end || *p++ != ':') {
                fail("expected ':' in object");
                break;
            }
            Value v = parse_value();
            if (!ok) {
                break;
            }
            if (count < kInline) {
                ik[count] = std::move(key);
                iv[count] = std::move(v);
            } else {
                overflow.emplace_back(std::move(key), std::move(v));
            }
            count++;
            skip_ws();
            if (p >= end) {
                fail("unterminated object");
                break;
            }
            char c = *p++;
            if (c == '}') {
                break;
            }
            if (c != ',') {
                fail("expected ',' or '}' in object");
                break;
            }
        }
        if (!ok) {
            return Value::make_object();
        }

        if (shape_matches && expected_shape->names.size() != count) {
            for (size_t i = 0; i < count; i++) {
                if (i < kInline) {
                    ik[i] = expected_shape->names[i];
                } else {
                    overflow[i - kInline].first = expected_shape->names[i];
                }
            }
            shape_matches = false;
        }

        Value obj = Value::make_object();
        ObjectObj *oobj = obj.get_obj_ptr();
        auto key_at = [&](size_t i) -> std::string & {
            return i < kInline ? ik[i] : overflow[i - kInline].first;
        };
        auto val_at = [&](size_t i) -> Value & {
            return i < kInline ? iv[i] : overflow[i - kInline].second;
        };

        // Fast path: same keys (in order) as the previous object, we reuse its
        // shape, fill fields by slot, skips interning/transition hashing.
        if (shape_matches && count > 0 && expected_shape->names.size() == count) {
            oobj->shape = expected_shape;
            oobj->fields.resize(count);
            for (size_t i = 0; i < count; i++) {
                oobj->fields[i] = std::move(val_at(i));
            }
            oobj->shape_version = (uint32_t)count;
            spec_shape = expected_shape;
            return obj;
        }
        // slow path: build via set_field, then remember the resulting shape.
        for (size_t i = 0; i < count; i++) {
            oobj->set_field(key_at(i), std::move(val_at(i)));
        }
        if (!oobj->dict_mode && !oobj->shape->names.empty()) {
            spec_shape = oobj->shape;
        }
        return obj;
    }

    bool match_lit(const char *lit) {
        size_t n = std::strlen(lit);
        if ((size_t)(end - p) < n || std::memcmp(p, lit, n) != 0) {
            return false;
        }
        p += n;
        return true;
    }

    Value parse_value() {
        if (!ok) {
            return Value::none();
        }
        skip_ws();
        if (p >= end) {
            fail("unexpected end of JSON input");
            return Value::none();
        }
        char c = *p;
        switch (c) {
            case '{': {
                if (depth >= (int)kJsonMaxDepth) {
                    fail("nesting too deep");
                    return Value::none();
                }
                depth++;
                Value v = parse_object();
                depth--;
                return v;
            }
            case '[': {
                if (depth >= (int)kJsonMaxDepth) {
                    fail("nesting too deep");
                    return Value::none();
                }
                depth++;
                Value v = parse_array();
                depth--;
                return v;
            }
            case '"':
                return Value::make_string(parse_string());
            case 't':
                if (!match_lit("true")) {
                    fail("invalid literal");
                    return Value::none();
                }
                return Value::make_bool(true);
            case 'f':
                if (!match_lit("false")) {
                    fail("invalid literal");
                    return Value::none();
                }
                return Value::make_bool(false);
            case 'n':
                if (!match_lit("null")) {
                    fail("invalid literal");
                    return Value::none();
                }
                return Value::none();
            default:
                return parse_number();
        }
    }
};
} // namespace

// __json_parse(jsonString) -> Result<value, string>
Value ScriptRuntime::builtin_json_parse(const Value *argvals, size_t argc, const CallExpr *call) {
    if (argc < 1 || !argvals[0].is_string()) {
        runtime_fatal("TypeError: JSON.parse requires a string argument", call);
        return Value::none();
    }
    const std::string &src = argvals[0].get_string();
    // persist the last object shape to attempt to avoid recompiling for homogeneous records
    static thread_local const ObjectShape *json_last_shape = nullptr;
    JsonDirectParser jp{ src.data(), src.data() + src.size() };
    jp.spec_shape = json_last_shape;
    Value result = jp.parse_value();
    json_last_shape = jp.spec_shape;
    if (jp.ok) {
        jp.skip_ws();
        if (jp.p != jp.end) {
            jp.fail("unexpected trailing characters");
        }
    }
    if (!jp.ok) {
        return make_err(Value::make_string("SyntaxError: JSON.parse failed: " + jp.err));
    }
    return make_ok(result);
}

// append JSON-escaped string contents (no surrounding quotes) to `out`, matching nlohmann's compact dump()
static void json_escape_into(const std::string &s, std::string &out) {
    static const char *hex = "0123456789abcdef";
    // most strings (field names, "user-123", etc.) contain nothing that needs escaping.
    size_t clean = 0;
    while (clean < s.size()) {
        unsigned char c = (unsigned char)s[clean];
        if (c < 0x20 || c == '"' || c == '\\') {
            break;
        }
        clean++;
    }
    if (clean == s.size()) {
        out.append(s);
        return;
    }
    out.append(s, 0, clean);
    for (size_t i = clean; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += (char)c;
                }
        }
    }
}

// Direct Value -> compact JSON text. Object/class keys are emitted in insertion order.
// Returns false on cyclic or too-deeply-nested input (out is left partial).
static bool value_to_json_string(const Value &v, std::string &out, std::vector<const void *> &seen) {
    if (v.is_none()) {
        out += "null";
        return true;
    }
    if (v.is_bool()) {
        out += v.get_bool() ? "true" : "false";
        return true;
    }
    if (v.is_int()) {
        char buf[24];
        auto res = std::to_chars(buf, buf + sizeof(buf), v.get_int());
        out.append(buf, res.ptr - buf); // no temp string allocation
        return true;
    }
    if (v.is_float()) {
        out += nlohmann::json(v.get_float()).dump();
        return true;
    }
    if (v.is_string()) {
        out += '"';
        json_escape_into(v.get_string(), out);
        out += '"';
        return true;
    }
    if (v.is_array()) {
        if (!json_enter(seen, v.heap_ptr())) {
            return false;
        }
        out += '[';
        const auto &a = v.get_array();
        for (size_t i = 0; i < a.size(); i++) {
            if (i) {
                out += ',';
            }
            if (!value_to_json_string(a[i], out, seen)) {
                return false;
            }
        }
        out += ']';
        seen.pop_back();
        return true;
    }
    if (v.is_object()) {
        if (!json_enter(seen, v.heap_ptr())) {
            return false;
        }
        out += '{';
        const ObjectObj *o = v.get_obj_ptr();
        if (!o->dict_mode) {
            // shape mode: fields[i] is the value for shape->names[i].
            // Iterate by slot to avoid hashing each field name (get_keys + get_field).
            const auto &names = o->shape->names;
            for (size_t i = 0; i < names.size() && i < o->fields.size(); i++) {
                if (i) {
                    out += ',';
                }
                out += '"';
                json_escape_into(names[i], out);
                out += "\":";
                if (!value_to_json_string(o->fields[i], out, seen)) {
                    return false;
                }
            }
        } else {
            bool first = true;
            for (const auto &name : o->get_keys()) {
                if (const Value *val = o->get_field(name)) {
                    if (!first) {
                        out += ',';
                    }
                    first = false;
                    out += '"';
                    json_escape_into(name, out);
                    out += "\":";
                    if (!value_to_json_string(*val, out, seen)) {
                        return false;
                    }
                }
            }
        }
        out += '}';
        seen.pop_back();
        return true;
    }
    if (v.is_class_instance()) {
        if (!json_enter(seen, v.heap_ptr())) {
            return false;
        }
        out += '{';
        const ClassInstance *ci = v.get_class_instance();
        if (ci->layout) {
            for (size_t i = 0; i < ci->field_values.size(); i++) {
                if (i) {
                    out += ',';
                }
                out += '"';
                json_escape_into(ci->layout->names[i], out);
                out += "\":";
                if (!value_to_json_string(ci->field_values[i], out, seen)) {
                    return false;
                }
            }
        }
        out += '}';
        seen.pop_back();
        return true;
    }
    out += "null";
    return true;
}

// __json_stringify(value[, indent]) -> Result<string, string>
Value ScriptRuntime::builtin_json_stringify(const Value *argvals, size_t argc, const CallExpr *) {
    if (argc < 1) {
        return make_ok(Value::make_string("null"));
    }
    int indent = -1;
    if (argc >= 2) {
        if (argvals[1].is_int()) {
            indent = static_cast<int>(argvals[1].get_int());
        } else if (argvals[1].is_float()) {
            indent = static_cast<int>(argvals[1].get_float());
        }
    }
    // reused across calls to avoid a per-stringify allocation
    static thread_local std::vector<const void *> seen;
    seen.clear();
    // attempt to use our implementation instead of nlohmann's slower (DOM-based) code
    if (indent < 0) {
        static thread_local size_t output_size_hint = 64;
        std::string out;
        out.reserve(output_size_hint);
        if (!value_to_json_string(argvals[0], out, seen)) {
            return make_err(Value::make_string("TypeError: JSON.stringify: cyclic or too deeply nested structure"));
        }
        size_t new_hint = out.size();
        if (new_hint < 64) {
            new_hint = 64;
        } else if (new_hint > 64 * 1024) {
            new_hint = 64 * 1024;
        }
        output_size_hint = new_hint;
        return make_ok(Value::make_string(std::move(out)));
    }
    bool ok = true;
    try {
        nlohmann::json j = value_to_json(argvals[0], seen, ok);
        if (!ok) {
            return make_err(Value::make_string("TypeError: JSON.stringify: cyclic or too deeply nested structure"));
        }
        return make_ok(Value::make_string(j.dump(indent)));
    } catch (...) {
        return make_ok(Value::make_string("null"));
    }
}
