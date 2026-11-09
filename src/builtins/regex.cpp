// Regex builtins (split out of collections.cpp).
#include "common.h"

#define SRELL_NO_THROW
#include "../../thirdparty/srell.hpp"

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
Value ScriptRuntime::builtin_regex_new(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 1 || !argvals[0].is_string()) {
        return runtime_fatal("TypeError: 'Regex.new' expects a pattern string", call);
    }
    std::string pattern = argvals[0].get_string();
    std::string flags;
    if (argc >= 2 && argvals[1].is_string()) {
        flags = argvals[1].get_string();
    }
    // check up front so that we can easily return a RegexError
    srell::u8cregex re(pattern, srell_flags_from_string(flags));
    if (re.ecode() != 0) {
        return make_err(
            Value::make_string("RegexError: invalid pattern (srell error " + std::to_string(static_cast<int>(re.ecode())) + ")")
        );
    }
    return make_ok(Value::make_regex(std::move(pattern), std::move(flags)));
}

// Regex.test(string) -> bool
Value ScriptRuntime::builtin_regex_test(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2) {
        return runtime_fatal("TypeError: 'test' requires a string argument", call);
    }
    if (!argvals[0].is_regex()) {
        return runtime_fatal("TypeError: 'test' called on non-regex", call);
    }
    if (!argvals[1].is_string()) {
        return Value::make_bool(false);
    }
    const RegexObj *re_obj = argvals[0].get_regex();
    const std::string &subject = argvals[1].get_string();
    // srell is built with SRELL_NO_THROW and patterns are validated at
    // construction, so compile/search here cannot throw.
    srell::u8cregex re(re_obj->pattern, srell_flags_from_string(re_obj->flags));
    srell::u8csmatch m;
    return Value::make_bool(srell::regex_search(subject, m, re));
}

// Regex.exec(string) -> object {match, index, groups:[...]} or null
Value ScriptRuntime::builtin_regex_exec(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2) {
        return runtime_fatal("TypeError: 'exec' requires a string argument", call);
    }
    if (!argvals[0].is_regex()) {
        return runtime_fatal("TypeError: 'exec' called on non-regex", call);
    }
    if (!argvals[1].is_string()) {
        return Value::none(); // null
    }
    const RegexObj *re_obj = argvals[0].get_regex();
    const std::string &subject = argvals[1].get_string();
    // srell is built with SRELL_NO_THROW and patterns are validated at
    // construction, so compile/search here cannot throw.
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
}
