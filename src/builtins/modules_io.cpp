#include "common.h"

Value ScriptRuntime::builtin_module_import_namespace(const Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
    if (argc != 1 || !argvals[0].is_string()) {
        runtime_fatal("__module_import_namespace expects a single module-path string argument", callExpr);
    }

    const std::string module_name = argvals[0].get_string();
    ensure_module_loaded(module_name, callExpr);
    const auto &exports = Parser::get_module_exports(module_name);
    Value result_ns = Value::make_object();
    ObjectObj *ns_oobj = result_ns.get_obj_ptr();

    for (const auto &binding : exports) {
        Value resolved = Value::none();
        bool found = false;

        std::string internal_name = Parser::get_module_function_internal_name(module_name, binding.local_name);
        if (!internal_name.empty()) {
            resolved = Value::make_function(internal_name);
            found = true;
        }

        if (!found) {
            auto mod_it = module_local_vars.find(module_name);
            if (mod_it != module_local_vars.end()) {
                auto val_it = mod_it->second.find(binding.local_name);
                if (val_it != mod_it->second.end()) {
                    resolved = val_it->second;
                    found = true;
                }
            }
        }

        if (!found) {
            auto global_it = globals.find(binding.local_name);
            if (global_it != globals.end()) {
                resolved = global_it->second;
                found = true;
            }
        }

        if (!found) {
            auto func_it = functions.find(binding.local_name);
            if (func_it != functions.end()) {
                resolved = Value::make_function(binding.local_name);
                found = true;
            }
        }

        if (!found) {
            runtime_fatal(
                "Module export '" + binding.export_name + "' was declared but no binding named '" + binding.local_name + "' exists in module '" + module_name + "'",
                callExpr);
        }

        ns_oobj->set_field(binding.export_name, resolved);
    }

    return result_ns;
}

Value ScriptRuntime::builtin_module_import_named(const Value *argvals, size_t argc, const nari::CallExpr *callExpr) {
    if (argc != 2 || !argvals[0].is_string() || !argvals[1].is_string()) {
        runtime_fatal("__module_import_named expects a module-path string and an export-name string", callExpr);
    }

    const std::string module_name = argvals[0].get_string();
    const std::string export_name = argvals[1].get_string();
    Value namespace_obj = builtin_module_import_namespace(argvals, 1, callExpr);
    const Value *exp_v = namespace_obj.get_obj_ptr()->get_field(export_name);
    if (!exp_v) {
        runtime_fatal("Module '" + module_name + "' does not export '" + export_name + "'", callExpr);
    }

    return *exp_v;
}

#ifndef NARI_ESP_IDF
// I/O builtins
Value ScriptRuntime::builtin_readLine(const Value *, size_t, const nari::CallExpr *) {
    char *line_buf = nullptr;
    size_t buf_size = 0;
    ssize_t len = getline(&line_buf, &buf_size, stdin);
    if (len > 0) {
        if (len > 0 && line_buf[len - 1] == '\n') {
            line_buf[len - 1] = '\0';
            len--;
        }
        std::string result(line_buf, len);
        free(line_buf);
        return Value::make_string(result);
    }
    free(line_buf);
    return Value::make_string("");
}

#ifdef _WIN32
#define NARI_ATTY_FUNC _isatty
#define NARI_FILENO_FUNC _fileno
#else
#define NARI_ATTY_FUNC isatty
#define NARI_FILENO_FUNC fileno
#endif

Value ScriptRuntime::builtin_readAll(const Value *, size_t, const nari::CallExpr *) {
    // when stdin is a terminal, reading until EOF requires Ctrl+D and blocks indefinitely after the user presses Enter.
    // this *should* detect that and fall back to reading a single line instead.
    bool is_tty = (NARI_ATTY_FUNC(NARI_FILENO_FUNC(stdin)) != 0);
    if (is_tty) {
        std::string line;
        char ch;
        while ((ch = static_cast<char>(fgetc(stdin))) != EOF && ch != '\n') {
            line += ch;
        }
        return Value::make_string(line);
    }
    std::string result;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        result.append(buf, n);
    }
    return Value::make_string(result);
}
#endif

Value ScriptRuntime::builtin_time(const Value *, size_t, const nari::CallExpr *) {
    using namespace chrono;

    auto now = high_resolution_clock::now();
    auto ns = duration_cast<nanoseconds>(now.time_since_epoch()).count();
    double ms = ns / 1'000'000.0;

    return Value::make_float(std::round(ms * 1000.0) / 1000.0);
}

// Date / time helpers
//
// Builtins exposed to script:
//  __time_now_ms() -> int ms since Unix epoch (UTC)
//  __time_components(ms, utc) -> object {year, month, day, hour,
//  minute, second, ms, weekday}
//  month: 1..12, day: 1..31,
//  weekday: 0=Sun..6=Sat
//  __time_from_components(o, utc) -> int ms since epoch
//  __time_format(ms, fmt, utc) -> string strftime-style; "%L" -> ms
//  (zero-padded to 3 digits)
//  __time_parse_iso(s) -> int ms since epoch from
//  ISO-8601 / RFC-3339 input
//
// "utc" is a bool: true -> UTC, false -> local time.
// On parse error, helpers return an Err result with a DateError message.
namespace {

// Cross-platform local-time conversion. Returns false on overflow.
bool ms_to_tm(int64_t ms, bool utc, std::tm &out, int &out_sub_ms) {
    // Split ms into seconds + sub-second, ensuring sub_ms is non-negative even for negative timestamps
    int64_t sec = ms / 1000;
    int sub_ms = static_cast<int>(ms - sec * 1000);
    if (sub_ms < 0) {
        sub_ms += 1000;
        sec -= 1;
    }
    out_sub_ms = sub_ms;
    std::time_t t = static_cast<std::time_t>(sec);
    if (static_cast<int64_t>(t) != sec) {
        return false;
    }
#ifdef _WIN32
    if (utc) {
        return gmtime_s(&out, &t) == 0;
    }
    return localtime_s(&out, &t) == 0;
#else
    if (utc) {
        return gmtime_r(&t, &out) != nullptr;
    }
    return localtime_r(&t, &out) != nullptr;
#endif
}

// Cross-platform tm -> time_t (UTC variant). timegm is non-standard but widely
// available on Linux/macOS; on Windows we use _mkgmtime.
std::time_t tm_to_time(std::tm &t, bool utc) {
    if (utc) {
#ifdef _WIN32
        return _mkgmtime(&t);
#else
        return timegm(&t);
#endif
    }
    return std::mktime(&t);
}

bool get_int_field(const ObjectObj *o, const std::string &name, int64_t &out, int64_t fallback) {
    const Value *vp = o->get_field(name);
    if (vp == nullptr || vp->is_none()) {
        out = fallback;
        return true;
    }
    if (vp->is_int()) {
        out = vp->get_int();
        return true;
    }
    if (vp->is_float()) {
        out = static_cast<int64_t>(vp->get_float());
        return true;
    }
    return false;
}

} // namespace

Value ScriptRuntime::builtin_time_now_ms(const Value *, size_t, const nari::CallExpr *) {
    using namespace chrono;
    auto now = system_clock::now();
    int64_t ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    return Value::make_int(ms);
}

Value ScriptRuntime::builtin_time_components(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1 || argc > 2) {
        runtime_fatal("DateError: __time_components(ms, utc=true) expects 1..2 args", call);
        return Value::none();
    }
    if (!argvals[0].is_int() && !argvals[0].is_float()) {
        runtime_fatal("DateError: ms must be a number", call);
        return Value::none();
    }
    int64_t ms = argvals[0].is_int() ? argvals[0].get_int()
                                     : static_cast<int64_t>(argvals[0].get_float());
    bool utc = (argc < 2) ? true : argvals[1].is_bool() ? argvals[1].get_bool()
                                                        : true;
    tm tm{};
    int sub_ms = 0;
    if (!ms_to_tm(ms, utc, tm, sub_ms)) {
        return make_err(Value::make_string("DateError: timestamp out of range"));
    }
    Value obj = Value::make_object();
    ObjectObj *o = obj.get_obj_ptr();
    o->set_field("year", Value::make_int(tm.tm_year + 1900));
    o->set_field("month", Value::make_int(tm.tm_mon + 1));
    o->set_field("day", Value::make_int(tm.tm_mday));
    o->set_field("hour", Value::make_int(tm.tm_hour));
    o->set_field("minute", Value::make_int(tm.tm_min));
    o->set_field("second", Value::make_int(tm.tm_sec));
    o->set_field("ms", Value::make_int(sub_ms));
    o->set_field("weekday", Value::make_int(tm.tm_wday));
    o->set_field("yearday", Value::make_int(tm.tm_yday + 1));
    o->set_field("utc", Value::make_bool(utc));
    return make_ok(obj);
}

Value ScriptRuntime::builtin_time_from_components(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1 || argc > 2) {
        runtime_fatal("DateError: __time_from_components expects 1..2 args", call);
        return Value::none();
    }
    if (!argvals[0].is_object()) {
        runtime_fatal("DateError: first arg must be an object with date fields", call);
        return Value::none();
    }
    const ObjectObj *o = argvals[0].get_obj_ptr();
    bool utc = (argc < 2) ? true : argvals[1].is_bool() ? argvals[1].get_bool()
                                                        : true;
    int64_t year, month, day, hour, minute, second, sub_ms;
    if (!get_int_field(o, "year", year, 1970) ||
        !get_int_field(o, "month", month, 1) ||
        !get_int_field(o, "day", day, 1) ||
        !get_int_field(o, "hour", hour, 0) ||
        !get_int_field(o, "minute", minute, 0) ||
        !get_int_field(o, "second", second, 0) ||
        !get_int_field(o, "ms", sub_ms, 0)) {
        runtime_fatal("DateError: date fields must be numbers", call);
        return Value::none();
    }
    std::tm tm{};
    tm.tm_year = static_cast<int>(year - 1900);
    tm.tm_mon = static_cast<int>(month - 1);
    tm.tm_mday = static_cast<int>(day);
    tm.tm_hour = static_cast<int>(hour);
    tm.tm_min = static_cast<int>(minute);
    tm.tm_sec = static_cast<int>(second);
    tm.tm_isdst = -1; // let libc decide
    std::time_t t = tm_to_time(tm, utc);
    if (t == static_cast<std::time_t>(-1)) {
        return make_err(Value::make_string("DateError: invalid date components"));
    }
    int64_t ms = static_cast<int64_t>(t) * 1000 + sub_ms;
    return make_ok(Value::make_int(ms));
}

Value ScriptRuntime::builtin_time_format(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2 || argc > 3) {
        runtime_fatal("DateError: __time_format(ms, fmt, utc=true) expects 2..3 args", call);
        return Value::none();
    }
    if (!argvals[0].is_int() && !argvals[0].is_float()) {
        runtime_fatal("DateError: ms must be a number", call);
        return Value::none();
    }
    if (!argvals[1].is_string()) {
        runtime_fatal("DateError: fmt must be a string", call);
        return Value::none();
    }
    int64_t ms = argvals[0].is_int() ? argvals[0].get_int()
                                     : static_cast<int64_t>(argvals[0].get_float());
    std::string fmt = argvals[1].get_string();
    bool utc = (argc < 3) ? true : argvals[2].is_bool() ? argvals[2].get_bool()
                                                        : true;
    std::tm tm{};
    int sub_ms = 0;
    if (!ms_to_tm(ms, utc, tm, sub_ms)) {
        return make_err(Value::make_string("DateError: timestamp out of range"));
    }
    // Custom token: %L -> 3-digit milliseconds. Substitute before strftime so
    // the format string passed to libc never contains %L.
    {
        std::string expanded;
        expanded.reserve(fmt.size() + 4);
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == 'L') {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%03d", sub_ms);
                expanded += buf;
                ++i;
            } else {
                expanded += fmt[i];
            }
        }
        fmt = std::move(expanded);
    }
    // strftime with a growable buffer: start small, double until it fits.
    std::string out;
    size_t cap = std::max<size_t>(64, fmt.size() * 2);
    for (int attempt = 0; attempt < 6; ++attempt) {
        out.resize(cap);
        size_t n = std::strftime(&out[0], cap, fmt.c_str(), &tm);
        if (n > 0) {
            out.resize(n);
            return make_ok(Value::make_string(out));
        }
        // strftime returns 0 when output doesn't fit OR when format produces an empty string, attempt to grow buffer.
        cap *= 2;
    }
    return make_ok(Value::make_string(""));
}

Value ScriptRuntime::builtin_time_parse_iso(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc != 1 || !argvals[0].is_string()) {
        runtime_fatal("DateError: __time_parse_iso(s) expects 1 string arg", call);
        return Value::none();
    }
    const std::string &s = argvals[0].get_string();
    // Accept:  YYYY-MM-DD
    //  YYYY-MM-DDTHH:MM:SS
    //  ...[.fff]
    //  ...[Z | +HH:MM | -HH:MM]
    // Space accepted in place of 'T'. Sub-second precision >3 digits is truncated (not rounded).
    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0, frac_ms = 0;
    int tz_sign = 0; // 0 = no tz / Z, 1 = +, -1 = -
    int tz_h = 0, tz_m = 0;
    bool saw_tz = false;
    size_t i = 0;
    auto fail = [&]() {
        return make_err(Value::make_string("DateError: invalid ISO-8601 string: '" + s + "'"));
    };
    auto read_int = [&](int width, int &out) -> bool {
        if (i + static_cast<size_t>(width) > s.size()) {
            return false;
        }
        int v = 0;
        for (int k = 0; k < width; ++k) {
            char c = s[i + k];
            if (c < '0' || c > '9') {
                return false;
            }
            v = v * 10 + (c - '0');
        }
        i += width;
        out = v;
        return true;
    };
    if (!read_int(4, year)) {
        return fail();
    }
    if (i >= s.size() || s[i] != '-') {
        return fail();
    }
    ++i;
    if (!read_int(2, month)) {
        return fail();
    }
    if (i >= s.size() || s[i] != '-') {
        return fail();
    }
    ++i;
    if (!read_int(2, day)) {
        return fail();
    }
    if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        ++i;
        if (!read_int(2, hour)) {
            return fail();
        }
        if (i >= s.size() || s[i] != ':') {
            return fail();
        }
        ++i;
        if (!read_int(2, minute)) {
            return fail();
        }
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!read_int(2, second)) {
                return fail();
            }
            if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
                ++i;
                int digits = 0;
                int v = 0;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                    if (digits < 3) {
                        v = v * 10 + (s[i] - '0');
                    }
                    ++digits;
                    ++i;
                }
                if (digits == 0) {
                    return fail();
                }
                // Pad to 3 digits (e.g. ".1" -> 100ms, ".12" -> 120ms).
                while (digits < 3) {
                    v *= 10;
                    ++digits;
                }
                frac_ms = v;
            }
        }
    }
    if (i < s.size()) {
        char c = s[i];
        if (c == 'Z' || c == 'z') {
            saw_tz = true;
            ++i;
        } else if (c == '+' || c == '-') {
            saw_tz = true;
            tz_sign = (c == '+') ? 1 : -1;
            ++i;
            if (!read_int(2, tz_h)) {
                return fail();
            }
            if (i < s.size() && s[i] == ':') {
                ++i;
            }
            if (i < s.size()) {
                if (!read_int(2, tz_m)) {
                    return fail();
                }
            }
        }
    }
    if (i != s.size()) {
        return fail();
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 60 /* leap second */) {
        return fail();
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;
    // No timezone marker -> treat as UTC.
    std::time_t t = tm_to_time(tm, /*utc=*/true);
    if (t == static_cast<std::time_t>(-1)) {
        return fail();
    }
    int64_t ms = static_cast<int64_t>(t) * 1000 + frac_ms;
    if (saw_tz && tz_sign != 0) {
        int64_t off_sec = (tz_h * 3600LL + tz_m * 60LL) * tz_sign;

        ms -= off_sec * 1000LL;
    }
    return make_ok(Value::make_int(ms));
}

// URL percent-encoding helpers
//
//  __url_encode(s, mode)  -> percent-encoded string
//  __url_decode(s, plus)  -> decoded string (throws on bad %xx)
//
// works like standard RFC 3986 percent-encoding (component mode)
//
// All operations are byte-oriented: input is treated as UTF-8 bytes (which is
// what Nari strings already are). No code-point reinterpretation happens.
namespace {

constexpr char URL_HEX[] = "0123456789ABCDEF";

bool url_is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

bool url_is_path_safe(unsigned char c) {
    return url_is_unreserved(c) || c == '/' || c == ':' || c == '@';
}

} // namespace

Value ScriptRuntime::builtin_url_encode(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1 || !argvals[0].is_string()) {
        runtime_fatal("URLError: __url_encode(s, mode='component') expects a string", call);
        return Value::none();
    }
    const std::string &s = argvals[0].get_string();
    bool path_mode = false;
    if (argc >= 2 && argvals[1].is_string() && argvals[1].get_string() == "path") {
        path_mode = true;
    }
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        bool safe = path_mode ? url_is_path_safe(c) : url_is_unreserved(c);
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(URL_HEX[(c >> 4) & 0x0F]);
            out.push_back(URL_HEX[c & 0x0F]);
        }
    }
    return Value::make_string(std::move(out));
}

Value ScriptRuntime::builtin_url_decode(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1 || !argvals[0].is_string()) {
        runtime_fatal("URLError: __url_decode(s, plus=false) expects a string", call);
        return Value::none();
    }
    const std::string &s = argvals[0].get_string();
    bool plus_mode = (argc >= 2 && argvals[1].is_bool() && argvals[1].get_bool());
    auto from_hex = [](char ch, bool &ok) -> int {
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
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%') {
            if (i + 2 >= s.size()) {
                return make_err(Value::make_string(
                    "URLError: truncated percent-escape at end of string"));
            }
            bool ok_hi = false, ok_lo = false;
            int hi = from_hex(s[i + 1], ok_hi);
            int lo = from_hex(s[i + 2], ok_lo);
            if (!ok_hi || !ok_lo) {
                return make_err(Value::make_string(
                    std::string("URLError: invalid hex digits in '%") +
                    s[i + 1] + s[i + 2] + "'"));
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else if (plus_mode && c == '+') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return make_ok(Value::make_string(std::move(out)));
}
