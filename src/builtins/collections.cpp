#include "common.h"
#include <charconv>

// Type checking and conversion builtins
Value ScriptRuntime::builtin_typeof(Value *argvals, size_t argc, const nari::CallExpr *) {
    return argc > 0 ? typeof_value(argvals[0]) : typeof_values[TY_NULL];
}

Value ScriptRuntime::builtin_toNumber(Value *argvals, size_t argc, const nari::CallExpr *) {
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
            bool is_float = (s.find('.') != std::string::npos) || (s.find('e') != std::string::npos) || (s.find('E') != std::string::npos);

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

Value ScriptRuntime::builtin_js_toNumber(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 0) {
        return Value::make_int(0);
    }
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
    if (v.is_none()) {
        return Value::make_int(0);
    }
    if (v.is_array()) {
        const auto &items = v.get_array();
        if (items.empty()) {
            return Value::make_int(0);
        }
        if (items.size() == 1) {
            Value elem = items[0];
            return builtin_js_toNumber(&elem, 1, nullptr);
        }
        return Value::make_float(std::numeric_limits<double>::quiet_NaN());
    }
    if (!v.is_string()) {
        return Value::make_float(std::numeric_limits<double>::quiet_NaN());
    }

    const std::string &raw = v.get_string();
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) {
        begin++;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
        end--;
    }
    if (begin == end) {
        return Value::make_int(0);
    }
    const std::string s = raw.substr(begin, end - begin);
    if (s == "Infinity" || s == "+Infinity") {
        return Value::make_float(std::numeric_limits<double>::infinity());
    }
    if (s == "-Infinity") {
        return Value::make_float(-std::numeric_limits<double>::infinity());
    }

    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint64_t value = 0;
        const char *first = s.data() + 2;
        const char *last = s.data() + s.size();
        auto [ptr, ec] = std::from_chars(first, last, value, 16);
        if (ec == std::errc() && ptr == last) {
            if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return Value::make_int(static_cast<int64_t>(value));
            }
            return Value::make_float(static_cast<double>(value));
        }
        return Value::make_float(std::numeric_limits<double>::quiet_NaN());
    }

    char *parsed_end = nullptr;
    errno = 0;
    double value = std::strtod(s.c_str(), &parsed_end);
    if (parsed_end == s.c_str() + s.size() && parsed_end != s.c_str() && errno != ERANGE && !std::isnan(value)) {
        if (s.find_first_of(".eE") == std::string::npos && value >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            value <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            return Value::make_int(static_cast<int64_t>(value));
        }
        return Value::make_float(value);
    }
    return Value::make_float(std::numeric_limits<double>::quiet_NaN());
}

Value ScriptRuntime::builtin_toString(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_string(argvals[0].to_string());
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_formatValue(Value *argvals, size_t argc, const nari::CallExpr *) {
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
    if (static_cast<size_t>(needed) < sizeof(stack_buf)) {
        return Value::make_string(std::string(stack_buf, static_cast<size_t>(needed)));
    }

    std::string out(static_cast<size_t>(needed), '\0');
    std::snprintf(out.data(), out.size() + 1, fmt, value);
    return Value::make_string(out);
}

Value ScriptRuntime::builtin_toBool(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].as_bool());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isNumber(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_int() || argvals[0].is_float());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isString(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_string());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isBool(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_bool());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isArray(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_array());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isObject(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_object());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_isFunction(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_function());
    }
    return Value::make_bool(false);
}

// Delegate(target, handler) -> a wrapper object to proxy object access.
// `handler` is an object whose optional "get"/"set"/"has"/"call" fields
// intercept the corresponding operations on the delegate; any absent trap falls through to `target`.
Value ScriptRuntime::builtin_delegate_new(Value *argvals, size_t argc, const nari::CallExpr *) {
    Value target = argc > 0 ? argvals[0] : Value::none();
    Value handler = argc > 1 ? argvals[1] : Value::none();
    return Value::make_delegate(target, handler);
}

Value ScriptRuntime::builtin_isDelegate(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_delegate());
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_delegateTarget(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        if (DelegateData *d = argvals[0].get_delegate()) {
            return d->target;
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_delegateHandler(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        if (DelegateData *d = argvals[0].get_delegate()) {
            return d->handler;
        }
    }
    return Value::none();
}
// Casting a double to int64_t is undefined behaviour unless the truncated value is
// representable, so NaN/+-inf/|x|>=2^63 must not reach the cast. UBSAN flagged this at
// Math.floor(1e21) and Math.floor(NaN) while running the npm fixtures. Out-of-range
// inputs keep their double value, which is also what JS Math.floor/ceil do.
static inline Value nari_int_or_float(double d) {
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) { // false for NaN too
        return Value::make_float(d);
    }
    return Value::make_int(static_cast<int64_t>(d));
}
Value ScriptRuntime::builtin_math_floor(Value *args, size_t argc, const CallExpr *) {
    if (argc > 0) {
        return nari_int_or_float(std::floor(args[0].as_number()));
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_math_ceil(Value *args, size_t argc, const CallExpr *) {
    if (argc > 0) {
        return nari_int_or_float(std::ceil(args[0].as_number()));
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_parse_int(Value *args, size_t argc, const CallExpr *) {
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

Value ScriptRuntime::builtin_parse_float(Value *args, size_t argc, const CallExpr *) {
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

Value ScriptRuntime::builtin_random(Value *, size_t, const CallExpr *) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    return Value::make_float(dis(gen));
}

Value ScriptRuntime::builtin_range(Value *args, size_t argc, const CallExpr *) {
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

Value ScriptRuntime::builtin_contains(Value *args, size_t argc, const CallExpr *) {
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
