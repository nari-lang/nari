#include "common.h"
#include "int_overflow.h"
#include <algorithm>
#include <charconv>
#include <string_view>
#include <unordered_set>

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
            std::string_view text(v.get_string());
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
                text.remove_prefix(1);
            }
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
                text.remove_suffix(1);
            }
            if (text.empty()) {
                return Value::make_int(0);
            }
            const char *first = text.data();
            const char *last = first + text.size();
            if (text.size() > 1 && *first == '+' && first[1] >= '0' && first[1] <= '9') {
                ++first;
            }
            int64_t integer = 0;
            auto [ptr, ec] = std::from_chars(first, last, integer);
            if (ec == std::errc() && ptr == last) {
                return Value::make_int_checked(integer);
            }

            // strtod requires a terminator; check the actual string length so
            // an embedded NUL or trailing junk cannot become a valid number.
            const std::string number(text);
            char *end = nullptr;
            double value = std::strtod(number.c_str(), &end);
            return Value::make_float(end == number.c_str() + number.size() && end != number.c_str() ? value : 0.0);
        }
        return Value::make_int(0);
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_js_toNumber(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 0) {
        return Value::make_int(0);
    }
    // A single-element array chain needs no C++ recursion. A cycle stringifies
    // to an empty value for numeric conversion, and therefore converts to zero.
    const Value *current = &argvals[0];
    std::unordered_set<const void *> arrays;
    while (current->is_array()) {
        if (!arrays.insert(current->heap_ptr()).second) {
            return Value::make_int(0);
        }
        const auto &items = current->get_array();
        if (items.empty()) {
            return Value::make_int(0);
        }
        if (items.size() != 1) {
            return Value::make_float(std::numeric_limits<double>::quiet_NaN());
        }
        current = &items[0];
    }
    const Value &v = *current;
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
                return Value::make_int_checked(static_cast<int64_t>(value));
            }
            return Value::make_float(static_cast<double>(value));
        }
        return Value::make_float(std::numeric_limits<double>::quiet_NaN());
    }

    char *parsed_end = nullptr;
    errno = 0;
    double value = std::strtod(s.c_str(), &parsed_end);
    if (parsed_end == s.c_str() + s.size() && parsed_end != s.c_str() && errno != ERANGE && !std::isnan(value)) {
        int64_t integer;
        if (s.find_first_of(".eE") == std::string::npos && try_double_to_i64(value, &integer)) {
            return Value::make_int_checked(integer);
        }
        return Value::make_float(value);
    }
    return Value::make_float(std::numeric_limits<double>::quiet_NaN());
}

Value ScriptRuntime::builtin_toString(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        // to_string(bytes) decodes the raw bytes into a string; printing a
        // bytes value (Value::to_string) shows bytes([...]) instead.
        if (argvals[0].is_bytes()) {
            const auto &data = argvals[0].get_bytes();
            return Value::make_string(std::string(reinterpret_cast<const char *>(data.data()), data.size()));
        }
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
            precision = std::min(100, precision * 10 + (spec[pos] - '0'));
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
// Preserve doubles outside the integer range, including NaN and infinities.
// Representable int64 values still need checking against the 48-bit payload.
static inline Value nari_int_or_float(double d) {
    int64_t integer;
    return try_double_to_i64(d, &integer) ? Value::make_int_checked(integer) : Value::make_float(d);
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
        return nari_int_or_float(std::trunc(args[0].get_float()));
    }
    if (args[0].is_string()) {
        const std::string &text = args[0].get_string();
        char *end = nullptr;
        errno = 0;
        int64_t v = std::strtoll(text.c_str(), &end, 10);
        if (errno == ERANGE) {
            // Keep parse_int's decimal-prefix semantics while promoting an
            // integer too wide for int64, rather than using a saturated value.
            const std::string prefix(text.c_str(), static_cast<size_t>(end - text.c_str()));
            return nari_int_or_float(std::strtod(prefix.c_str(), nullptr));
        }
        return Value::make_int_checked(v);
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

Value ScriptRuntime::builtin_range(Value *args, size_t argc, const CallExpr *call) {
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) {
        if (!try_double_to_i64(args[0].as_number(), &stop)) {
            return runtime_fatal("range: invalid stop value", call);
        }
    } else if (argc >= 2) {
        if (!try_double_to_i64(args[0].as_number(), &start) || !try_double_to_i64(args[1].as_number(), &stop)) {
            return runtime_fatal("range: invalid start or stop value", call);
        }
    }
    if (argc >= 3) {
        if (!try_double_to_i64(args[2].as_number(), &step)) {
            return runtime_fatal("range: invalid step value", call);
        }
        if (step == 0) {
            step = 1;
        }
    }
    std::vector<Value> arr;
    for (int64_t i = start; step > 0 ? i < stop : i > stop;) {
        arr.push_back(Value::make_int_checked(i));
        int64_t next;
        if (add_overflow_i64(i, step, &next)) {
            break;
        }
        i = next;
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
