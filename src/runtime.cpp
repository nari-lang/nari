#include "runtime.h"
#include "ast.h"
#include "int_overflow.h"
#include "nari_ffi.h"
#include "parser_api.h"
#include "util.h"

#include <cmath>
#include <stdexcept>

namespace chrono = std::chrono;

static std::string format_interpolated_value(ScriptRuntime *, const Value &value, const nari::StringInterpolationExpr *expr, size_t index) {
    if (index >= expr->format_specs.size() || expr->format_specs[index].empty()) {
        return value.to_string();
    }

    const std::string &spec = expr->format_specs[index];
    size_t pos = 0;
    int precision = -1;
    if (pos < spec.size() && spec[pos] == '.') {
        ++pos;
        if (pos >= spec.size() || !std::isdigit((unsigned char)spec[pos])) {
            return value.to_string();
        }
        precision = 0;
        while (pos < spec.size() && std::isdigit((unsigned char)spec[pos])) {
            precision = precision * 10 + (spec[pos] - '0');
            ++pos;
        }
    }

    char presentation = pos < spec.size() ? spec[pos++] : '\0';
    if (pos != spec.size() || presentation != 'f' || (!value.is_int() && !value.is_float())) {
        return value.to_string();
    }

    if (precision > 100) {
        precision = 100;
    }

    char fmt[16];
    if (precision >= 0) {
        std::snprintf(fmt, sizeof(fmt), "%%.%df", precision);
    } else {
        std::snprintf(fmt, sizeof(fmt), "%%f");
    }

    char stack_buf[128];
    int needed = std::snprintf(stack_buf, sizeof(stack_buf), fmt, value.as_number());
    if (needed < 0) {
        return value.to_string();
    }
    if ((size_t)needed < sizeof(stack_buf)) {
        return std::string(stack_buf, (size_t)needed);
    }

    std::string out((size_t)needed, '\0');
    std::snprintf(out.data(), out.size() + 1, fmt, value.as_number());
    return out;
}

#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
// Windows doesn't have getline..
static ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) {
        return -1;
    }

    size_t pos = 0;
    int c;

    if (*lineptr == nullptr || *n == 0) {
        *n = 128;
        *lineptr = (char *)realloc(*lineptr, *n);
        if (!*lineptr) {
            return -1;
        }
    }

    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t new_size = *n * 2;
            char *new_ptr = (char *)realloc(*lineptr, new_size);
            if (!new_ptr) {
                return -1;
            }
            *lineptr = new_ptr;
            *n = new_size;
        }
        (*lineptr)[pos++] = c;
        if (c == '\n') {
            break;
        }
    }

    if (pos == 0 && c == EOF) {
        return -1;
    }
    (*lineptr)[pos] = '\0';
    return pos;
}
#endif

std::string nari_std_prelude_source();

// trace state
static bool g_trace_enabled = false;
static int g_trace_level = 0;

namespace Runtime {
bool runtime_trace_enabled() {
    return g_trace_enabled;
}
void set_runtime_trace_level(TraceLevel level) {
    g_trace_level = (int)level;
    g_trace_enabled = (g_trace_level > 0);
}
TraceLevel get_runtime_trace_level() {
    return (TraceLevel)g_trace_level;
}
void runtime_log(TraceLevel level, const std::string &msg) {
    if (g_trace_enabled && (int)level <= g_trace_level) {
        fprintf(stderr, "%s\n", msg.c_str());
    }
}
} // namespace Runtime

// report fatal runtime error with source location
void runtime_fatal(const std::string &msg, const nari::ASTNode *node) {
    std::string fname = "<runtime>";
    int l = 0, c = 0;
    if (node) {
        if (!node->filename.empty()) {
            fname = node->filename;
        }
        l = node->line;
        c = node->col;
    }

    fprintf(stderr, "Runtime error at %s:%d:%d\n", fname.c_str(), l, c);
    fprintf(stderr, "  %s\n", msg.c_str());

#ifndef NARI_ESP_IDF
    if (!fname.empty() && fname[0] != '<' && l > 0) {
        FILE *fp = fopen(fname.c_str(), "r");
        if (fp) {
            char *line_buffer = nullptr;
            size_t buffer_size = 0;
            std::string line_text;

            for (int i = 1; i <= l; ++i) {
                ssize_t line_length = getline(&line_buffer, &buffer_size, fp);
                if (line_length == -1) {
                    break;
                }
                if (i == l) {
                    line_text = line_buffer;
                    if (!line_text.empty() && line_text.back() == '\n') {
                        line_text.pop_back();
                    }
                }
            }
            free(line_buffer);
            fclose(fp);

            if (!line_text.empty()) {
                fprintf(stderr, "  %s\n", line_text.c_str());
                int caret_pos = c > 0 ? c - 1 : 0;
                std::string caret_line(caret_pos, ' ');
                caret_line.push_back('^');
                fprintf(stderr, "  %s\n", caret_line.c_str());
            }
        }
    }
#else
    // ESP-IDF does not support getline, just print the filename and line number
    fprintf(stderr, "  (source context not available on this platform!)\n");
#endif

    throw RuntimeError(msg, fname, l, c);
}

using namespace nari;
using namespace Runtime;

// Default get: behave like a normal property/index read on the wrapped target.
Value ScriptRuntime::delegate_default_get(const Value &target, const Value &key) {
    if (target.is_object()) {
        const Value *v = target.get_obj_ptr()->get_field(key.to_string());
        return v ? *v : Value::none();
    }
    if (target.is_array()) {
        const auto &arr = target.get_array();
        if (key.is_int()) {
            int64_t idx = key.get_int();
            if (idx >= 0 && idx < (int64_t)arr.size()) {
                return arr[idx];
            }
        }
        return Value::none();
    }
    if (target.is_delegate()) {
        // Chained delegates: forward to the inner delegate's get trap.
        return delegate_get(target, key);
    }
    return Value::none();
}

void ScriptRuntime::delegate_default_set(const Value &target, const Value &key, const Value &val) {
    if (target.is_object()) {
        const_cast<Value &>(target).get_obj_ptr()->set_field(key.to_string(), val);
        return;
    }
    if (target.is_array()) {
        if (key.is_int()) {
            auto &arr = const_cast<Value &>(target).get_array();
            int64_t idx = key.get_int();
            if (idx >= 0 && idx < (int64_t)arr.size()) {
                arr[idx] = val;
            }
        }
        return;
    }
    if (target.is_delegate()) {
        delegate_set(target, key, val);
    }
}

bool ScriptRuntime::delegate_default_has(const Value &target, const Value &key) {
    if (target.is_object()) {
        if (key.is_string()) {
            auto *string_key = static_cast<StringObj *>(key.heap_ptr());
            if (string_key->immutable) {
                if (string_key->field_id == UINT32_MAX) {
                    string_key->field_id = intern_field(string_key->s);
                }
                return target.get_obj_ptr()->has_field_by_id(string_key->field_id);
            }
        }
        return target.get_obj_ptr()->has_field(key.to_string());
    }
    if (target.is_array()) {
        if (key.is_int()) {
            int64_t idx = key.get_int();
            return idx >= 0 && idx < (int64_t)target.get_array().size();
        }
        return false;
    }
    if (target.is_delegate()) {
        return delegate_has(target, key);
    }
    return false;
}

// The four handler trap names, interned to their stable field ids once. The
// ids are process-global (field_intern_map never renumbers),
// so a Delegate handler's trap can be resolved via ObjectObj::get_field_by_id
enum class TrapId {
    Get,
    Set,
    Has,
    Call
};
static uint32_t trap_field_id(TrapId which) {
    static const uint32_t ids[4] = {
        intern_field("get"),
        intern_field("set"),
        intern_field("has"),
        intern_field("call"),
    };
    return ids[(int)which];
}

// Look up a trap on the handler object, returns a function Value or none.
// Semantically identical to reading handler[name] each call.
static Value delegate_trap(const Value &del, TrapId which) {
    DelegateData *d = del.get_delegate();
    if (!d || !d->handler.is_object()) {
        return Value::none();
    }
    ObjectObj *h = d->handler.get_obj_ptr();
    if (!h->dict_mode && h->lazy_field_mask == 0) {
        if (NARI_UNLIKELY(h->shape != d->trap_shape)) {
            // (re)resolve all four trap slots against this shape
            for (int i = 0; i < 4; i++) {
                const uint32_t slot = h->shape->slot_of(trap_field_id((TrapId)i));
                d->trap_slots[i] = slot != ObjectShape::kNoSlot ? (int32_t)slot : -1;
            }
            d->trap_shape = h->shape;
        }
        int32_t slot = d->trap_slots[(int)which];
        if (slot < 0 || (size_t)slot >= h->fields.size()) {
            return Value::none();
        }
        const Value &t = h->fields[(size_t)slot];
        return t.is_function() ? t : Value::none();
    }
    // dict-mode or pending-lazy handler: full id lookup
    const Value *t = h->get_field_by_id(trap_field_id(which));
    return (t && t->is_function()) ? *t : Value::none();
}

// Invoke a resolved trap function with a fixed-arity argument list
Value ScriptRuntime::invoke_trap(const Value &trap, const Value &a, const Value &b) {
    GcTempRoot gcRoot(*this);
    gcRoot.add(&trap);
    Value args[2] = { a, b };
    gcRoot.add(&args[0]);
    gcRoot.add(&args[1]);
    return call_function_value(trap, args, 2);
}

Value ScriptRuntime::invoke_trap(const Value &trap, const Value &a, const Value &b, const Value &c) {
    GcTempRoot gcRoot(*this);
    gcRoot.add(&trap);
    Value args[3] = { a, b, c };
    gcRoot.add(&args[0]);
    gcRoot.add(&args[1]);
    gcRoot.add(&args[2]);
    return call_function_value(trap, args, 3);
}

Value ScriptRuntime::delegate_get(const Value &del, const Value &key) {
    DelegateData *d = del.get_delegate();
    if (!d) {
        return Value::none();
    }
    Value target = d->target;
    Value trap = delegate_trap(del, TrapId::Get);
    if (trap.is_function()) {
        // trap(target, key)
        return invoke_trap(trap, target, key);
    }
    return delegate_default_get(target, key);
}

void ScriptRuntime::delegate_set(const Value &del, const Value &key, const Value &val) {
    DelegateData *d = del.get_delegate();
    if (!d) {
        return;
    }
    Value target = d->target;
    Value trap = delegate_trap(del, TrapId::Set);
    if (trap.is_function()) {
        // trap(target, key, value)
        invoke_trap(trap, target, key, val);
        return;
    }
    delegate_default_set(target, key, val);
}

bool ScriptRuntime::delegate_has(const Value &del, const Value &key) {
    DelegateData *d = del.get_delegate();
    if (!d) {
        return false;
    }
    Value target = d->target;
    Value trap = delegate_trap(del, TrapId::Has);
    if (trap.is_function()) {
        // trap(target, key) -> bool
        return invoke_trap(trap, target, key).as_bool();
    }
    return delegate_default_has(target, key);
}

Value ScriptRuntime::delegate_call(const Value &del, const std::vector<Value> &args) {
    return delegate_call(del, args.data(), args.size());
}

Value ScriptRuntime::delegate_call(const Value &del, const Value *args, size_t argc) {
    DelegateData *d = del.get_delegate();
    if (!d) {
        return Value::none();
    }
    Value target = d->target;
    Value trap = delegate_trap(del, TrapId::Call);
    if (trap.is_function()) {
        GcTempRoot gcRoot(*this);
        gcRoot.add(&target);
        gcRoot.add(&trap);
        // The trap receives the call args as a Nari array
        // that allocation is inherent. invoke_trap then avoids the extra {target, arr} vector.
        Value arr = Value::make_array(args, argc);
        gcRoot.add(&arr);
        // trap(target, argsArray)
        return invoke_trap(trap, target, arr);
    }
    // No call trap, forward to the target if it is itself callable.
    // The span passes through unchanged, its values stay rooted at the source until the VM copies them.
    if (target.is_function()) {
        GcTempRoot gcRoot(*this);
        gcRoot.add(&target);
        return call_function_value(target, args, argc);
    }
    if (target.is_delegate()) {
        return delegate_call(target, args, argc);
    }
    return Value::none();
}

// Method call on a delegate: resolve the member via the get trap, then invoke the resulting callable with the given
// args.
Value ScriptRuntime::delegate_call_method(const Value &del, const std::string &method, std::vector<Value> args) {
    Value callee = delegate_get(del, Value::make_string(method));
    GcTempRoot gcRoot(*this);
    gcRoot.add(&callee);
    gcRoot.add_vec(&args);
    if (callee.is_function()) {
        return call_function_value(callee, args);
    }
    if (callee.is_delegate()) {
        return delegate_call(callee, args);
    }
    return Value::none();
}

// pattern matching, this is for match, not regex :)
Value ScriptRuntime::construct_result_variant(const char *variant, const Value &payload, const Value &constructor) {
    ResultConstructorTmpl &cache = std::strcmp(variant, "Ok") == 0 ? ok_template : err_template;
    const Value &standard = std::strcmp(variant, "Ok") == 0 ? standard_ok_constructor : standard_err_constructor;
    if (constructor.raw_bits() != standard.raw_bits()) {
        std::vector<Value> args{ payload };
        return call_function_value(constructor, args);
    }
    if (cache.initialized && cache.constructor.raw_bits() == constructor.raw_bits() && cache.usable) {
        return instantiate_result_template(cache, payload);
    }

    std::vector<Value> args{ payload };
    Value result = call_function_value(constructor, args);
    if (!cache.initialized) {
        initialize_result_template(cache, constructor, result, payload);
    }
    return result;
}

bool ScriptRuntime::initialize_result_template(ResultConstructorTmpl &cache, const Value &constructor, const Value &result,
                                               const Value &payload) {
    cache.initialized = true;
    cache.constructor = constructor;
    if (!result.is_object()) {
        return false;
    }

    const ObjectObj *obj = result.get_obj_ptr();
    if (obj->dict_mode || !obj->shape || obj->shape->field_ids.size() != obj->fields.size()) {
        return false;
    }

    cache.shape = obj->shape;
    cache.fields.assign(obj->fields.begin(), obj->fields.end());
    cache.shape_version = obj->shape_version;
    if (const Value *variant = obj->get_field("__variant")) {
        cache.unwrap_returns_payload = variant->is_string() && variant->get_string() == "Ok";
    }
    bool found_data = false;
    CellRef payload_cell;
    for (size_t slot = 0; slot < obj->shape->field_ids.size(); ++slot) {
        if (obj->shape->name_at(slot) == "__data") {
            cache.data_slot = slot;
            found_data = true;
        }

        const Value &field = obj->fields[slot];
        if (!field.is_function() || !field.get_function().captures) {
            continue;
        }
        const FunctionData &fn = field.get_function();
        if (fn.captures->size() != 1 || !(*fn.captures)[0] || !Value::values_equal(*(*fn.captures)[0], payload)) {
            cache.fields.clear();
            cache.methods.clear();
            return false;
        }
        if (!payload_cell) {
            payload_cell = (*fn.captures)[0];
        } else if (payload_cell != (*fn.captures)[0]) {
            cache.fields.clear();
            cache.methods.clear();
            return false;
        }
        cache.methods.push_back(ResultMethodTmpl{ slot, fn.name, fn.jit_func_idx, fn.jit_locals_count, fn.jit_meta, fn.jit_inline_kind,
                                                  fn.jit_native_kind, fn.jit_inline_imm, fn.jit_param_count,
                                                  fn.jit_rest_param_index, fn.jit_js_undefined_params });
        cache.fields[slot] = Value::none();
    }
    cache.usable = found_data && !cache.methods.empty();
    return cache.usable;
}

Value ScriptRuntime::instantiate_result_template(const ResultConstructorTmpl &cache, const Value &payload) {
    Value result = Value::make_object();
    ObjectObj *obj = result.get_obj_ptr();
    obj->shape = cache.shape;
    obj->fields.resize(cache.fields.size());
    std::copy(cache.fields.begin(), cache.fields.end(), obj->fields.begin());
    obj->fields[cache.data_slot] = payload;
    obj->shape_version = cache.shape_version;

    for (const ResultMethodTmpl &method : cache.methods) {
        if (method.slot < 64) {
            obj->lazy_field_mask |= uint64_t{ 1 } << method.slot;
        }
    }
    obj->lazy_field_context = const_cast<ResultConstructorTmpl *>(&cache);
    obj->lazy_field_factory = &ScriptRuntime::make_result_method;
    obj->lazy_field_invoker = &ScriptRuntime::invoke_result_method;
    obj->lazy_payload = payload;
    return result;
}

Value ScriptRuntime::make_result_method(void *context, ObjectObj *obj, uint32_t slot) {
    const auto *cache = (const ResultConstructorTmpl *)context;
    const ResultMethodTmpl *method = nullptr;
    for (const ResultMethodTmpl &candidate : cache->methods) {
        if (candidate.slot == slot) {
            method = &candidate;
            break;
        }
    }
    if (!method) {
        return Value::none();
    }
    if (!obj->lazy_captures) {
        obj->lazy_captures = std::make_shared<std::vector<CellRef>>();
        obj->lazy_captures->push_back(CellRef::make(obj->lazy_payload));
    }
    Value closure = Value::make_function(method->name);
    FunctionData &fn = closure.get_function();
    fn.captures = obj->lazy_captures;
    fn.cache_jit_captures();
    fn.jit_func_idx = method->jit_func_idx;
    fn.jit_locals_count = method->jit_locals_count;
    fn.jit_meta = method->jit_meta;
    fn.jit_param_count = method->jit_param_count;
    fn.jit_rest_param_index = method->jit_rest_param_index;
    fn.jit_js_undefined_params = method->jit_js_undefined_params;
    fn.jit_inline_kind = method->jit_inline_kind;
    fn.jit_native_kind = method->jit_native_kind;
    fn.jit_inline_imm = method->jit_inline_imm;
    GarbageCollector::instance().track(&fn, GarbageCollector::TrackedType::Function);
    return closure;
}

bool ScriptRuntime::invoke_result_method(void *context, ObjectObj *obj, uint32_t slot, const Value *, size_t argc, Value &result) {
    if (argc != 0) {
        return false;
    }
    const auto *cache = (const ResultConstructorTmpl *)context;
    if (cache->unwrap_returns_payload && slot < cache->shape->field_ids.size() && cache->shape->name_at(slot) == "unwrap") {
        result = obj->lazy_payload;
        return true;
    }
    return false;
}

// collects from
//  globals call stack, block scopes, module-local vars, closure scope, task queue,
//  flags, external roots, async roots, builtin roots, persistent roots, and static class fields.
std::vector<const Value *> ScriptRuntime::collect_gc_roots() const {
    std::vector<const Value *> roots;

    for (const auto &[key, val] : globals) {
        roots.push_back(&val);
    }

    for (const auto *cache : { &ok_template, &err_template }) {
        if (cache->initialized) {
            roots.push_back(&cache->constructor);
            for (const Value &field : cache->fields) {
                roots.push_back(&field);
            }
        }
    }
    roots.push_back(&standard_ok_constructor);
    roots.push_back(&standard_err_constructor);

    for (const auto &[mod, vars] : module_local_vars) {
        for (const auto &[key, val] : vars) {
            roots.push_back(&val);
        }
    }

    // an armed interval owns its callback; set_timeout uses async_root_set instead
    for (const auto &[id, interval] : active_intervals) {
        roots.push_back(&interval.callback);
    }

    if (!flags.return_value.is_none()) {
        roots.push_back(&flags.return_value);
    }
    if (!flags.throw_value.is_none()) {
        roots.push_back(&flags.throw_value);
    }

    if (external_gc_roots_provider) {
        external_gc_roots_provider(roots);
    }

    for (const Value *v : gc_extra_value_roots) {
        if (v) {
            roots.push_back(v);
        }
    }
    for (const std::vector<Value> *c : gc_extra_vec_roots) {
        if (c) {
            for (const Value &e : *c) {
                roots.push_back(&e);
            }
        }
    }
    for (const std::map<std::string, Value> *m : gc_extra_map_roots) {
        if (m) {
            for (const auto &[key, value] : *m) {
                roots.push_back(&value);
            }
        }
    }

    for (const auto &[key, vals] : gc_async_roots) {
        for (const Value &v : vals) {
            roots.push_back(&v);
        }
    }

    for (const Value &v : gc_persistent_roots) {
        roots.push_back(&v);
    }
    for (const Value &v : this->typeof_values) {
        roots.push_back(&v);
    }

#ifndef DISABLE_FFI
    FFICallbackManager::instance().collect_roots(roots);
#endif

    // static class fields persist for the program's lifetime
    // and live in a global map outside the call stack / globals.
    for (const auto &[key, val] : Parser::get_static_fields()) {
        roots.push_back(&val);
    }

    return roots;
}

// lookup variable through scopes (block -> call -> module -> global)
// store variable through scopes (block -> call -> module -> global)
void ScriptRuntime::collect_garbage() {
    auto &gc = GarbageCollector::instance();

    if (!gc.is_enabled() || !gc.should_collect()) {
        return;
    }

    auto roots = collect_gc_roots();

    size_t collected = gc.collect(roots);

    if (Runtime::runtime_trace_enabled()) {
        Runtime::runtime_log(Runtime::TraceLevel::Debug, "GC: Collected " + std::to_string(collected) + " unreachable objects. " +
                                                             "Tracked objects: " + std::to_string(gc.get_tracked_count()));
    }
}

// recursively collect all fields from a class and its parents
static void collect_all_fields(const nari::ClassDecl *class_decl, std::vector<const nari::ClassField *> &all_fields) {
    if (!class_decl) {
        return;
    }

    // collect parent fields
    if (!class_decl->parent_name.empty()) {
        const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
        if (parent) {
            collect_all_fields(parent, all_fields);
        }
    }

    // then add this class's fields (skip static)
    for (const auto &field : class_decl->fields) {
        if (!field.is_static) {
            all_fields.push_back(&field);
        }
    }
}

// find method in class hierarchy, nullptr if not found.
static const nari::ClassMethod *find_method_in_hierarchy(const nari::ClassDecl *class_decl, const std::string &method_name) {
    if (!class_decl) {
        return nullptr;
    }

    for (const auto &m : class_decl->methods) {
        if (m.name == method_name) {
            return &m;
        }
    }

    if (!class_decl->parent_name.empty()) {
        const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
        if (parent) {
            return find_method_in_hierarchy(parent, method_name);
        }
    }

    return nullptr;
}

// find field declarations in class hierarchy, again, nullptr if not found.
static const nari::ClassField *find_field_in_hierarchy(const nari::ClassDecl *class_decl, const std::string &field_name) {
    if (!class_decl) {
        return nullptr;
    }

    for (const auto &field : class_decl->fields) {
        if (field.name == field_name) {
            return &field;
        }
    }

    if (!class_decl->parent_name.empty()) {
        const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
        if (parent) {
            return find_field_in_hierarchy(parent, field_name);
        }
    }

    return nullptr;
}

