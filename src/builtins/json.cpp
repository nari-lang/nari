// JSON builtins (split out of collections.cpp).
#include "common.h"
#include "json_dom.h"

#include <charconv>

// JSON builtins

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

// Nari Value -> JsonValue DOM (used for indented stringify). On cyclic or
// too-deeply-nested input, sets ok=false and leaves `out` partial.
static bool value_to_jdom(const Value &v, JsonValue &out, std::vector<const void *> &seen, bool &ok) {
    if (v.is_none()) {
        out = JsonValue();
        return true;
    }
    if (v.is_bool()) {
        out = JsonValue(v.get_bool());
        return true;
    }
    if (v.is_int()) {
        out = JsonValue(v.get_int());
        return true;
    }
    if (v.is_float()) {
        out = JsonValue(v.get_float());
        return true;
    }
    if (v.is_string()) {
        out = JsonValue(v.get_string());
        return true;
    }
    if (v.is_array()) {
        if (!json_enter(seen, v.heap_ptr())) {
            ok = false;
            return false;
        }
        out = JsonValue::array();
        for (const auto &el : v.get_array()) {
            JsonValue child;
            if (!value_to_jdom(el, child, seen, ok)) {
                return false;
            }
            out.tp_array.push_back(std::move(child));
        }
        seen.pop_back();
        return true;
    }
    if (v.is_object()) {
        if (!json_enter(seen, v.heap_ptr())) {
            ok = false;
            return false;
        }
        out = JsonValue::object();
        const ObjectObj *oobj = v.get_obj_ptr();
        for (const auto &name : oobj->get_keys()) {
            if (const Value *val = oobj->get_field(name)) {
                JsonValue child;
                if (!value_to_jdom(*val, child, seen, ok)) {
                    return false;
                }
                out.tp_object[name] = std::move(child);
            }
        }
        seen.pop_back();
        return true;
    }
    // class instances: serialize fields like an object
    if (v.is_class_instance()) {
        if (!json_enter(seen, v.heap_ptr())) {
            ok = false;
            return false;
        }
        out = JsonValue::object();
        const ClassInstance *ci = v.get_class_instance();
        if (ci->layout) {
            for (size_t i = 0; i < ci->field_values.size(); i++) {
                JsonValue child;
                if (!value_to_jdom(ci->field_values[i], child, seen, ok)) {
                    return false;
                }
                out.tp_object[ci->layout->names[i]] = std::move(child);
            }
        }
        seen.pop_back();
        return true;
    }
    out = JsonValue();
    return true;
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
                v |= static_cast<unsigned>((c - '0'));
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned>((c - 'a' + 10));
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned>((c - 'A' + 10));
            } else {
                fail("invalid hex digit in \\u escape");
                return 0;
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
                out.append(run, static_cast<size_t>(p - run));
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
        if (static_cast<size_t>(end - p) < size + 2 || p[size + 1] != '"' || std::memcmp(p + 1, expected.data(), size) != 0) {
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
        // Fast path: a plain integer (no '.'/'e'/'E'/'+').
        // The scanner already delimited [start, p), std::from_chars parses it in place with no string allocation and no
        // locale.
        if (!is_float && have_digit && !int_overflow) {
            if (negative) {
                if (magnitude == uint64_t(INT64_MAX) + 1) {
                    return Value::make_int(INT64_MIN);
                }
                return Value::make_int(-static_cast<int64_t>(magnitude));
            }
            return Value::make_int(static_cast<int64_t>(magnitude));
        }
        if (!is_float) {
            int64_t iv = 0;
            auto [ptr, ec] = std::from_chars(start, p, iv);
            if (ec == std::errc() && ptr == p) {
                return Value::make_int(iv);
            }
        }
        std::string num(start, static_cast<size_t>(p - start));
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
        return Value::make_int(static_cast<int64_t>(iv));
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
            if (!shape_matches || count >= expected_shape->field_ids.size() || !match_shape_key(expected_shape->name_at(count))) {
                if (shape_matches) {
                    for (size_t i = 0; i < count; i++) {
                        if (i < kInline) {
                            ik[i] = expected_shape->name_at(i);
                        } else {
                            overflow[i - kInline].first = expected_shape->name_at(i);
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

        if (shape_matches && expected_shape->field_ids.size() != count) {
            for (size_t i = 0; i < count; i++) {
                if (i < kInline) {
                    ik[i] = expected_shape->name_at(i);
                } else {
                    overflow[i - kInline].first = expected_shape->name_at(i);
                }
            }
            shape_matches = false;
        }

        Value obj = Value::make_object();
        ObjectObj *oobj = obj.get_obj_ptr();
        auto key_at = [&](size_t i) -> std::string & { return i < kInline ? ik[i] : overflow[i - kInline].first; };
        auto val_at = [&](size_t i) -> Value & { return i < kInline ? iv[i] : overflow[i - kInline].second; };

        // Fast path: same keys (in order) as the previous object, we reuse its
        // shape, fill fields by slot, skips interning/transition hashing.
        if (shape_matches && count > 0 && expected_shape->field_ids.size() == count) {
            oobj->shape = expected_shape;
            oobj->fields.resize(count);
            for (size_t i = 0; i < count; i++) {
                oobj->fields[i] = std::move(val_at(i));
            }
            oobj->shape_version = static_cast<uint32_t>(count);
            spec_shape = expected_shape;
            return obj;
        }
        // slow path: build via set_field, then remember the resulting shape.
        for (size_t i = 0; i < count; i++) {
            oobj->set_field(key_at(i), std::move(val_at(i)));
        }
        if (!oobj->dict_mode && !oobj->shape->field_ids.empty()) {
            spec_shape = oobj->shape;
        }
        return obj;
    }

    bool match_lit(const char *lit) {
        size_t n = std::strlen(lit);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, lit, n) != 0) {
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
                if (depth >= static_cast<int>(kJsonMaxDepth)) {
                    fail("nesting too deep");
                    return Value::none();
                }
                depth++;
                Value v = parse_object();
                depth--;
                return v;
            }
            case '[': {
                if (depth >= static_cast<int>(kJsonMaxDepth)) {
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
Value ScriptRuntime::builtin_json_parse(Value *argvals, size_t argc, const CallExpr *call) {
    if (argc < 1 || !argvals[0].is_string()) {
        return runtime_fatal("TypeError: JSON.parse requires a string argument", call);
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
        unsigned char c = static_cast<unsigned char>(s[clean]);
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
        unsigned char c = static_cast<unsigned char>(s[i]);
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
                    out += static_cast<char>(c);
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
        append_json_float(out, v.get_float());
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
            // shape mode: fields[i] is the value for shape->name_at(i).
            // Iterate by slot to avoid hashing each field name (get_keys + get_field).
            const size_t n_named = o->shape->field_ids.size();
            for (size_t i = 0; i < n_named && i < o->fields.size(); i++) {
                if (i) {
                    out += ',';
                }
                out += '"';
                json_escape_into(o->shape->name_at(i), out);
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
Value ScriptRuntime::builtin_json_stringify(Value *argvals, size_t argc, const CallExpr *) {
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
    // compact output serializes directly from the Value, no DOM in between
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
    // Nari strings can carry arbitrary bytes; the pretty-printer replaces
    // invalid UTF-8 with U+FFFD. Allocation failure aborts process-wide via
    // the new_handler installed in main.
    JsonValue j;
    if (!value_to_jdom(argvals[0], j, seen, ok) || !ok) {
        return make_err(Value::make_string("TypeError: JSON.stringify: cyclic or too deeply nested structure"));
    }
    return make_ok(Value::make_string(j.dump(indent)));
}
