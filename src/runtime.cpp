#include "runtime.h"
#include "ast.h"
#include "int_overflow.h"
#include "nari_ffi.h"
#include "parser_api.h"
#include "util.h"

#include <cmath>
#include <stdexcept>

namespace chrono = std::chrono;

static std::string format_interpolated_value(ScriptRuntime *, const Value &value,
                                             const nari::StringInterpolationExpr *expr, size_t index) {
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

// Interpreter
void ScriptRuntime::run_start(bool found_toplevel) {
    auto it = functions.find("start");
    if (it == functions.end()) {
        if (!found_toplevel) {
            fprintf(stderr, "No start() function found and no top level code found. Nothing to run!\n");
        }
        return;
    }
    call_user_function(it->second.get(), {});
    if (flags.throw_flag) {
        runtime_fatal("panic: " + flags.throw_value.to_string(), nullptr);
    }
}

void ScriptRuntime::execute_toplevel_function(Function *f) {
    if (!f) {
        return;
    }

    if (!f->filename.empty()) {
        if (executed_toplevel_modules.count(f->filename) > 0) {
            return;
        }
        module_stack.push_back(f->filename);
    }

    if (f->function_expr && f->function_expr->body) {
        for (const auto &st : f->function_expr->body->stmts) {
            if (!st) {
                continue;
            }
            exec_stmt(st.get());
            if (flags.return_flag || flags.throw_flag) {
                break;
            }
        }
    } else if (f->body) {
        for (const auto &st : f->body->stmts) {
            if (!st) {
                continue;
            }
            exec_stmt(st.get());
            if (flags.return_flag || flags.throw_flag) {
                break;
            }
        }
    }

    if (!f->filename.empty()) {
        executed_toplevel_modules.insert(f->filename);
        if (!module_stack.empty()) {
            module_stack.pop_back();
        }
    }

    if (flags.throw_flag) {
        runtime_fatal("panic: " + flags.throw_value.to_string(), nullptr);
    }
}

void ScriptRuntime::ensure_module_loaded(const std::string &module_name, const ASTNode *site) {
    if (module_name.empty() || executed_toplevel_modules.count(module_name) > 0) {
        return;
    }

    for (const auto &funcname : toplevel_order) {
        auto it = functions.find(funcname);
        if (it == functions.end()) {
            continue;
        }

        Function *f = it->second.get();
        if (!f || f->filename.empty()) {
            continue;
        }
        if (executed_toplevel_modules.count(f->filename) > 0) {
            continue;
        }

        execute_toplevel_function(f);
        if (executed_toplevel_modules.count(module_name) > 0) {
            return;
        }
    }

    if (executed_toplevel_modules.count(module_name) == 0) {
        runtime_fatal("Failed to load module '" + module_name + "'", site);
    }
}

void ScriptRuntime::run_top_level() {
    bool found_toplevel = false;

    // use preserved insertion order so imports run before the importing module
    const auto &toplevel_funcs = toplevel_order;

    for (const auto &funcname : toplevel_funcs) {
        auto it = functions.find(funcname);
        if (it == functions.end()) {
            continue;
        }

        found_toplevel = true;
        execute_toplevel_function(it->second.get());
        if (Runtime::g_shutdown_requested.load()) {
            return;
        }
    }

    run_event_loop();
    if (Runtime::g_shutdown_requested.load()) {
        return;
    }
    run_start(found_toplevel);
    run_event_loop();
}

void ScriptRuntime::step_task(HandlePtr handle) {
    if (!handle || !handle->task) {
        return;
    }

    Task *task = handle->task.get();
    if (task->state != Task::Running && task->state != Task::Yielded) {
        return;
    }

    auto saved_block_scopes = block_scope_stack;
    auto saved_block_const_scopes = block_const_scope_stack;
    block_scope_stack = task->block_scopes;
    block_const_scope_stack = task->block_const_scopes;
    call_stack.push_back(task->locals);
    call_const_stack.push_back(task->const_locals);

    Flags saved_flags = flags;
    flags = task->flags;

    if (task->body && task->current_stmt < task->body->stmts.size()) {
        for (; task->current_stmt < task->body->stmts.size(); ++task->current_stmt) {
            const auto &stmt = task->body->stmts[task->current_stmt];
            if (stmt) {
                exec_stmt(stmt.get());
            }
            // continue/break inside a spawn body have no outer loop to act on,
            // treat them as an implicit return (stop this task's execution).
            if (flags.return_flag || flags.throw_flag || flags.continue_flag || flags.break_flag) {
                flags.continue_flag = false;
                flags.break_flag = false;
                break;
            }
        }
    }

    if (task->current_stmt >= task->body->stmts.size() || flags.return_flag) {
        handle->end_time = chrono::steady_clock::now();
        if (flags.throw_flag) {
            handle->state = HandleData::Failed;
            handle->error = flags.throw_value;
            task->state = Task::Failed;
        } else {
            handle->state = HandleData::Completed;
            handle->result = flags.return_flag ? flags.return_value : Value::none();
            task->state = Task::Completed;
        }
    } else {
        task->state = Task::Yielded;
    }

    task->locals = call_stack.back();
    task->const_locals = call_const_stack.back();
    task->block_scopes = block_scope_stack;
    task->block_const_scopes = block_const_scope_stack;
    task->flags = flags;

    call_stack.pop_back();
    call_const_stack.pop_back();
    block_scope_stack = saved_block_scopes;
    block_const_scope_stack = saved_block_const_scopes;
    flags = saved_flags;
}

void ScriptRuntime::run_event_loop() {
    while ((!task_queue.empty() || has_pending_io()) && !Runtime::g_shutdown_requested.load() &&
           !Runtime::g_runtime_error_occurred.load()) {
        process_completed_io();

        collect_garbage();

        auto now = chrono::steady_clock::now();
        for (auto &[id, interval] : active_intervals) {
            if (now >= interval.next_fire) {
                if (interval.callback.is_function()) {
                    call_function_value(interval.callback, {});
                }
                interval.next_fire = now + chrono::milliseconds(interval.interval_ms);
            }
        }

        if (!task_queue.empty()) {
            HandlePtr handle = task_queue.front();
            task_queue.pop();
            step_task(handle);
            if (handle->state == HandleData::Running) {
                task_queue.push(handle);
            }
        } else if (has_pending_io()) {
            NARI_SLEEP_MILLIS(10);
        }
    }

    // final GC pass after event loop completes
    collect_garbage();
}

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
enum class TrapId { Get, Set, Has, Call };
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
            const auto &index = h->shape->index;
            for (int i = 0; i < 4; i++) {
                auto it = index.find(trap_field_id((TrapId)i));
                d->trap_slots[i] = it != index.end() ? (int32_t)it->second : -1;
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
bool ScriptRuntime::match_pattern(const Pattern *pattern, const Value &value, Value &bindings) {
    if (!pattern) {
        return false;
    }

    if (!bindings.is_object()) {
        bindings = Value::make_object();
    }

    switch (pattern->pattern_kind) {
        case PatternKind::Wildcard:
            return true;

        case PatternKind::Binding: {
            const auto *bp = (const BindingPattern *)pattern;
            bindings.get_obj_ptr()->set_field(bp->name, value);
            return true;
        }

        case PatternKind::Literal: {
            const auto *lp = (const LiteralPattern *)pattern;
            Value pattern_value = eval_expr(lp->value.get());

            if (value.is_int() && pattern_value.is_int()) {
                return value.get_int() == pattern_value.get_int();
            }
            if ((value.is_int() || value.is_float()) && (pattern_value.is_int() || pattern_value.is_float())) {
                return std::fabs(value.as_number() - pattern_value.as_number()) < 1e-12;
            }
            return value.to_string() == pattern_value.to_string();
        }

        case PatternKind::Variant: {
            const auto *vp = (const VariantPattern *)pattern;
            // for now, check if value is an object with __variant field
            if (!value.is_object()) {
                return false;
            }

            auto *obj = value.get_obj_ptr();

            const Value *var_it = obj->get_field("__variant");
            if (!var_it) {
                return false;
            }

            std::string variant_name = var_it->to_string();
            if (variant_name != vp->variant_name) {
                return false;
            }

            // if pattern has fields, match them
            if (!vp->fields.empty()) {
                const Value *data_it = obj->get_field("__data");
                if (!data_it) {
                    return false;
                }

                const Value &data = *data_it;

                if (data.is_array()) {
                    // tuple variant
                    auto &data_arr = data.get_array();
                    if (vp->fields.size() != data_arr.size()) {
                        return false;
                    }

                    for (size_t i = 0; i < vp->fields.size(); i++) {
                        if (!match_pattern(vp->fields[i].get(), data_arr[i], bindings)) {
                            return false;
                        }
                    }
                } else {
                    // single value variant
                    if (vp->fields.size() == 1) {
                        if (!match_pattern(vp->fields[0].get(), data, bindings)) {
                            return false;
                        }
                    } else {
                        return false;
                    }
                }
            }

            return true;
        }

        default:
            break;
    }

    return false;
}

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

bool ScriptRuntime::initialize_result_template(ResultConstructorTmpl &cache, const Value &constructor,
                                               const Value &result, const Value &payload) {
    cache.initialized = true;
    cache.constructor = constructor;
    if (!result.is_object()) {
        return false;
    }

    const ObjectObj *obj = result.get_obj_ptr();
    if (obj->dict_mode || !obj->shape || obj->shape->names.size() != obj->fields.size()) {
        return false;
    }

    cache.shape = obj->shape;
    cache.fields.assign(obj->fields.begin(), obj->fields.end());
    cache.shape_version = obj->shape_version;
    if (const Value *variant = obj->get_field("__variant")) {
        cache.unwrap_returns_payload = variant->is_string() && variant->get_string() == "Ok";
    }
    bool found_data = false;
    std::shared_ptr<Value> payload_cell;
    for (size_t slot = 0; slot < obj->shape->names.size(); ++slot) {
        if (obj->shape->names[slot] == "__data") {
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
        cache.methods.push_back(ResultMethodTmpl{ slot, fn.name, fn.jit_func_idx, fn.jit_locals_count, fn.jit_meta,
                                                  fn.jit_inline_kind, fn.jit_native_kind, fn.jit_inline_imm });
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
        obj->lazy_captures = std::make_shared<std::vector<std::shared_ptr<Value>>>();
        obj->lazy_captures->push_back(std::make_shared<Value>(obj->lazy_payload));
    }
    Value closure = Value::make_function(method->name);
    FunctionData &fn = closure.get_function();
    fn.captures = obj->lazy_captures;
    fn.jit_capture0_raw = (*fn.captures)[0].get();
    fn.jit_func_idx = method->jit_func_idx;
    fn.jit_locals_count = method->jit_locals_count;
    fn.jit_meta = method->jit_meta;
    fn.jit_inline_kind = method->jit_inline_kind;
    fn.jit_native_kind = method->jit_native_kind;
    fn.jit_inline_imm = method->jit_inline_imm;
    GarbageCollector::instance().track(&fn, GarbageCollector::TrackedType::Function);
    return closure;
}

bool ScriptRuntime::invoke_result_method(void *context, ObjectObj *obj, uint32_t slot, const Value *, size_t argc,
                                         Value &result) {
    if (argc != 0) {
        return false;
    }
    const auto *cache = (const ResultConstructorTmpl *)context;
    if (cache->unwrap_returns_payload && slot < cache->shape->names.size() && cache->shape->names[slot] == "unwrap") {
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

    for (const auto &frame : call_stack) {
        for (const auto &[key, val] : frame) {
            roots.push_back(&val);
        }
    }

    for (const auto &scope : block_scope_stack) {
        for (const auto &[key, val] : scope) {
            roots.push_back(&val);
        }
    }

    for (const auto &[mod, vars] : module_local_vars) {
        for (const auto &[key, val] : vars) {
            roots.push_back(&val);
        }
    }

    if (current_scope_closure) {
        for (const auto &[key, val] : *current_scope_closure) {
            roots.push_back(&val);
        }
    }

    std::queue<HandlePtr> queue_copy = task_queue;
    while (!queue_copy.empty()) {
        const HandlePtr &handle = queue_copy.front();
        if (handle) {
            // the handle itself is tracked, but this ensures that we mark the Value wrapper as well
            static thread_local Value temp_handle;
            temp_handle = Value::make_handle(handle);
            roots.push_back(&temp_handle);
        }
        queue_copy.pop();
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

    for (const auto &[key, vals] : gc_async_roots) {
        for (const Value &v : vals) {
            roots.push_back(&v);
        }
    }

    for (const Value &v : gc_persistent_roots) {
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
Value ScriptRuntime::lookup_variable(const std::string &name, const std::string &filename, bool &found) {
    found = false;

    // check block scopes (innermost to outermost)
    for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
        auto &block_scope = block_scope_stack[i];
        auto it = block_scope.find(name);
        if (it != block_scope.end()) {
            found = true;
            return it->second;
        }
    }

    // check function call stack (locals)
    if (!call_stack.empty()) {
        auto &locals = call_stack.back();
        auto it = locals.find(name);
        if (it != locals.end()) {
            found = true;
            // prefer current_scope_closure when it holds this var, it tracks mutations made by inner closures after the
            // local frame was set up.
            if (current_scope_closure) {
                auto cit = current_scope_closure->find(name);
                if (cit != current_scope_closure->end()) {
                    return cit->second;
                }
            }
            return it->second;
        }
    }

    // check module-local vars (current module on stack)
    if (!module_stack.empty()) {
        auto it = module_local_vars.find(module_stack.back());
        if (it != module_local_vars.end()) {
            auto mlocal = it->second.find(name);
            if (mlocal != it->second.end()) {
                found = true;
                return mlocal->second;
            }
        }
    }

    // check globals
    auto it = globals.find(name);
    if (it != globals.end()) {
        found = true;
        return it->second;
    }

    return Value::make_int(0);
}

// store variable through scopes (block -> call -> module -> global)
void ScriptRuntime::store_variable(const std::string &name, const std::string &filename, const Value &value) {
    if (is_const_binding(name, filename)) {
        runtime_fatal("TypeError: Assignment to constant variable '" + name + "'");
    }

    // try block scopes first
    for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
        auto &block_scope = block_scope_stack[i];
        auto it = block_scope.find(name);
        if (it != block_scope.end()) {
            it->second = value;
            return;
        }
    }

    // try call stack
    if (!call_stack.empty()) {
        auto &locals = call_stack.back();
        if (locals.find(name) != locals.end()) {
            locals[name] = value;
            if (current_scope_closure) {
                auto cit = current_scope_closure->find(name);
                if (cit != current_scope_closure->end()) {
                    cit->second = value;
                }
            }
            return;
        }
    }

    // try module-local vars (current module)
    if (!module_stack.empty()) {
        auto it = module_local_vars.find(module_stack.back());
        if (it != module_local_vars.end() && it->second.find(name) != it->second.end()) {
            it->second[name] = value;
            return;
        }
    }

    // try module-local vars by filename
    if (!filename.empty()) {
        auto it = module_local_vars.find(filename);
        if (it != module_local_vars.end() && it->second.find(name) != it->second.end()) {
            it->second[name] = value;
            return;
        }
    }

    // fall back to globals
    globals[name] = value;
}

bool ScriptRuntime::is_const_binding(const std::string &name, const std::string &filename) const {
    for (int i = (int)block_scope_stack.size() - 1; i >= 0; --i) {
        if (block_scope_stack[i].count(name)) {
            return block_const_scope_stack[i].count(name) != 0;
        }
    }

    if (!call_stack.empty() && call_stack.back().count(name)) {
        return call_const_stack.back().count(name) != 0;
    }

    if (!module_stack.empty()) {
        auto vars = module_local_vars.find(module_stack.back());
        if (vars != module_local_vars.end() && vars->second.count(name)) {
            auto consts = module_local_consts.find(module_stack.back());
            return consts != module_local_consts.end() && consts->second.count(name);
        }
    }

    if (!filename.empty()) {
        auto vars = module_local_vars.find(filename);
        if (vars != module_local_vars.end() && vars->second.count(name)) {
            auto consts = module_local_consts.find(filename);
            return consts != module_local_consts.end() && consts->second.count(name);
        }
    }

    return globals.count(name) && global_consts.count(name);
}

void ScriptRuntime::collect_garbage() {
    auto &gc = GarbageCollector::instance();

    if (!gc.is_enabled() || !gc.should_collect()) {
        return;
    }

    auto roots = collect_gc_roots();

    size_t collected = gc.collect(roots);

    if (Runtime::runtime_trace_enabled()) {
        Runtime::runtime_log(Runtime::TraceLevel::Debug,
                             "GC: Collected " + std::to_string(collected) + " unreachable objects. " +
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
static const nari::ClassMethod *find_method_in_hierarchy(const nari::ClassDecl *class_decl,
                                                         const std::string &method_name) {
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
static const nari::ClassField *find_field_in_hierarchy(const nari::ClassDecl *class_decl,
                                                       const std::string &field_name) {
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

Value ScriptRuntime::eval_expr(const Expr *e) {
    if (!e) {
        return Value::none();
    }

    // dispatch via type tag instead of dynamic_cast
    switch (e->kind) {
        case ExprKind::Ident: {
            const auto *identExpr = (const IdentExpr *)e;
            if (identExpr->name == "true") {
                return Value::make_bool(true);
            }
            if (identExpr->name == "false") {
                return Value::make_bool(false);
            }

            for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
                auto &block_scope = block_scope_stack[i];
                auto it = block_scope.find(identExpr->name);
                if (it != block_scope.end()) {
                    return it->second;
                }
            }
            // function scope (locals)
            if (!call_stack.empty()) {
                auto &locals = call_stack.back();
                auto it = locals.find(identExpr->name);
                if (it != locals.end()) {
                    if (current_scope_closure) {
                        auto cit = current_scope_closure->find(identExpr->name);
                        if (cit != current_scope_closure->end()) {
                            return cit->second;
                        }
                    }
                    return it->second;
                }
            }
            // module-local lookup, prefer the current module on the module stack
            if (!module_stack.empty()) {
                const std::string &mod = module_stack.back();
                auto it = module_local_vars.find(mod);
                if (it != module_local_vars.end()) {
                    auto mlocal = it->second.find(identExpr->name);
                    if (mlocal != it->second.end()) {
                        return mlocal->second;
                    }
                }

                std::string internal_name = Parser::get_module_function_internal_name(mod, identExpr->name);
                if (!internal_name.empty()) {
                    return Value::make_function(internal_name);
                }
            }
            if (!identExpr->filename.empty()) {
                auto it = module_local_vars.find(identExpr->filename);
                if (it != module_local_vars.end()) {
                    auto mlocal = it->second.find(identExpr->name);
                    if (mlocal != it->second.end()) {
                        return mlocal->second;
                    }
                }

                std::string internal_name =
                    Parser::get_module_function_internal_name(identExpr->filename, identExpr->name);
                if (!internal_name.empty()) {
                    return Value::make_function(internal_name);
                }
            }
            auto global_it = globals.find(identExpr->name);
            if (global_it != globals.end()) {
                return global_it->second;
            }
            // when running under the bytecode VM, resolve through its globals table
            if (external_global_lookup) {
                Value ext_global = external_global_lookup(identExpr->name);
                if (!ext_global.is_none()) {
                    return ext_global;
                }
            }
            // check if it's a function name (for first-class function support)
            auto func_it = functions.find(identExpr->name);
            if (func_it != functions.end()) {
                return Value::make_function(identExpr->name);
            }
            // only allow global builtins as identifiers, not method-only builtins
            if (is_global_builtin(identExpr->name)) {
                return Value::make_function(identExpr->name);
            }

            // check if it's a registered type name
            if (Parser::get_registered_type(identExpr->name) || Parser::is_registered_class(identExpr->name)) {
                return Value::make_string(identExpr->name);
            }

            // undefined identifier: this is a fatal runtime error!
            std::string msg = "Undefined identifier '" + identExpr->name + "'";
            runtime_fatal(msg, identExpr);
            unreachable();
        }

        case ExprKind::String: {
            const auto *stringExpr = (const StringExpr *)e;
            return Value::make_string(stringExpr->value);
        }

        case ExprKind::Number: {
            const auto *numberExpr = (const NumberExpr *)e;
            if (numberExpr->is_float) {
                return Value::make_float(numberExpr->f);
            }
            return Value::make_int(numberExpr->i);
        }

        case ExprKind::Bool: {
            const auto *boolExpr = (const BoolExpr *)e;
            return Value::make_bool(boolExpr->value);
        }

        case ExprKind::Null:
            return Value::none();

        case ExprKind::Regex: {
            const auto *regexExpr = (const RegexLiteralExpr *)e;
            return Value::make_regex(regexExpr->pattern, regexExpr->flags);
        }

        case ExprKind::Unary: {
            const auto *unaryExpr = (const UnaryExpr *)e;
            if (!unaryExpr->operand) {
                return Value::none();
            }
            const std::string &op = unaryExpr->op;

            if (op == "++" || op == "--") {
                if (!unaryExpr->operand) {
                    runtime_fatal("Increment/decrement operand is null", unaryExpr);
                }
                const IdentExpr *ie =
                    unaryExpr->operand->kind == ExprKind::Ident ? (const IdentExpr *)unaryExpr->operand.get() : nullptr;
                if (!ie) {
                    if (unaryExpr->operand->kind == ExprKind::Unary) {
                        const auto *nested = (const UnaryExpr *)unaryExpr->operand.get();
                        std::string msg = "Prefix ++ has nested UnaryExpr (op=" + nested->op + ")";
                        runtime_fatal(msg, unaryExpr);
                    }
                    std::string msg = "Prefix ++ requires a variable";
                    runtime_fatal(msg, unaryExpr);
                }

                bool found = false;
                Value current = lookup_variable(ie->name, ie->filename, found);

                Value newval;
                if (current.is_float()) {
                    double val = current.get_float() + ((op == "++") ? 1.0 : -1.0);
                    newval = Value::make_float(val);
                } else if (current.is_int() || current.is_none()) {
                    int64_t val = current.is_int() ? current.get_int() : 0;
                    val += (op == "++") ? 1 : -1;
                    newval = Value::make_int(val);
                } else {
                    runtime_fatal("Increment/decrement requires int or float", unaryExpr);
                }

                store_variable(ie->name, ie->filename, newval);
                return newval;
            }

            if (op == "post++" || op == "post--") {
                const IdentExpr *ie =
                    unaryExpr->operand->kind == ExprKind::Ident ? (const IdentExpr *)unaryExpr->operand.get() : nullptr;
                if (!ie) {
                    runtime_fatal("Increment/decrement requires a variable", unaryExpr);
                }

                bool found = false;
                Value current = lookup_variable(ie->name, ie->filename, found);
                Value oldval = current;

                Value newval;
                if (current.is_float()) {
                    double val = current.get_float() + ((op == "post++") ? 1.0 : -1.0);
                    newval = Value::make_float(val);
                } else if (current.is_int() || current.is_none()) {
                    int64_t val = current.is_int() ? current.get_int() : 0;
                    val += (op == "post++") ? 1 : -1;
                    newval = Value::make_int(val);
                } else {
                    runtime_fatal("Increment/decrement requires int or float", unaryExpr);
                }

                store_variable(ie->name, ie->filename, newval);
                return oldval;
            }

            Value v = eval_expr(unaryExpr->operand.get());
            if (op == "neg") {
                if (v.is_float()) {
                    return Value::make_float(-v.get_float());
                }
                if (v.is_int()) {
                    // -INT48_MIN doesn't fit in int48
                    // make_int_checked promotes to float instead of silently wrapping
                    return Value::make_int_checked(-v.get_int());
                }
                return Value::make_float(-v.as_number());
            } else if (op == "!") {
                return Value::make_bool(!v.as_bool());
            } else if (op == "~") {
                if (v.is_int()) {
                    return Value::make_int(~v.get_int());
                }
                // convert to int if not already (safe for NaN/Inf/huge)
                return Value::make_int(~safe_double_to_i64(v.as_number()));
            } else {
                return v;
            }
        }

        case ExprKind::Binary: {
            const auto *binaryExpr = (const BinaryExpr *)e;
            const std::string &op = binaryExpr->op;
            if (op == "&&") {
                if (!binaryExpr->left || !binaryExpr->right) {
                    return Value::none();
                }
                Value a = eval_expr(binaryExpr->left.get());
                if (!a.as_bool()) {
                    return Value::make_bool(false);
                }
                Value b = eval_expr(binaryExpr->right.get());
                return Value::make_bool(b.as_bool());
            }
            if (op == "||") {
                if (!binaryExpr->left || !binaryExpr->right) {
                    return Value::none();
                }
                Value a = eval_expr(binaryExpr->left.get());
                if (a.as_bool()) {
                    return Value::make_bool(true);
                }
                Value b = eval_expr(binaryExpr->right.get());
                return Value::make_bool(b.as_bool());
            }
            if (op == "??") {
                if (!binaryExpr->left || !binaryExpr->right) {
                    return Value::none();
                }
                Value a = eval_expr(binaryExpr->left.get());
                if (!a.is_none()) {
                    return a;
                }
                return eval_expr(binaryExpr->right.get());
            }

            // arithmetic and comparison
            if (!binaryExpr->left || !binaryExpr->right) {
                return Value::none();
            }
            Value left = eval_expr(binaryExpr->left.get());
            Value right = eval_expr(binaryExpr->right.get());

            if (op == "+") {
                if (left.is_string() || right.is_string()) {
                    return Value::make_string(left.to_string() + right.to_string());
                }
                if (left.is_int() && right.is_int()) {
                    return Value::make_int_checked(left.get_int() + right.get_int());
                }
                return Value::make_float(left.as_number() + right.as_number());
            }
            if (op == "@") {
                return Value::make_string(left.to_string() + right.to_string());
            }
            if (op == "-") {
                if (left.is_int() && right.is_int()) {
                    return Value::make_int_checked(left.get_int() - right.get_int());
                }
                return Value::make_float(left.as_number() - right.as_number());
            }
            if (op == "*") {
                if (left.is_int() && right.is_int()) {
                    // fused check: product outside int48 falls back to float
                    int64_t product;
                    if (NARI_UNLIKELY(mul_overflow_i48(left.get_int(), right.get_int(), &product))) {
                        return Value::make_float(left.as_number() * right.as_number());
                    }
                    return Value::make_int(product);
                }
                return Value::make_float(left.as_number() * right.as_number());
            }
            if (op == "/") {
                double rn = right.as_number();
                if (rn == 0.0) {
                    return Value::make_float(std::nan(""));
                }
                return Value::make_float(left.as_number() / rn);
            }
            if (op == "%") {
                if (left.is_int() && right.is_int()) {
                    if (right.get_int() == 0) {
                        return Value::make_float(std::nan(""));
                    }
                    return Value::make_int(left.get_int() % right.get_int());
                }
                double rn = right.as_number();
                if (rn == 0.0) {
                    return Value::make_float(std::nan(""));
                }
                return Value::make_float(std::fmod(left.as_number(), rn));
            }
            if (op == "**") {
                if (left.is_int() && right.is_int() && right.get_int() >= 0) {
                    if (right.get_int() > std::numeric_limits<uint64_t>::max()) {
                        return Value::make_float(std::pow(left.as_number(), right.as_number()));
                    }
                    uint64_t exp = (uint64_t)right.get_int();
                    int64_t base = left.get_int();
                    int64_t result = 1;
                    while (exp > 0) {
                        if (exp & 1ULL) {
                            if (mul_overflow_i64(result, base, &result)) {
                                return Value::make_float(std::pow(left.as_number(), right.as_number()));
                            }
                        }
                        if (exp > 1) {
                            if (mul_overflow_i64(base, base, &base)) {
                                return Value::make_float(std::pow(left.as_number(), right.as_number()));
                            }
                        }
                        exp >>= 1ULL;
                    }
                    return Value::make_int(result);
                }
                return Value::make_float(std::pow(left.as_number(), right.as_number()));
            }

            if (op == "==") {
                if (left.is_int() && right.is_int()) {
                    return Value::make_bool(left.get_int() == right.get_int());
                }
                if ((left.is_int() || left.is_float()) && (right.is_int() || right.is_float())) {
                    return Value::make_bool(std::fabs(left.as_number() - right.as_number()) < 1e-12);
                }
                return Value::make_bool(left.to_string() == right.to_string());
            }
            if (op == "!=") {
                if (left.is_int() && right.is_int()) {
                    return Value::make_bool(left.get_int() != right.get_int());
                }
                if ((left.is_int() || left.is_float()) && (right.is_int() || right.is_float())) {
                    return Value::make_bool(!(std::fabs(left.as_number() - right.as_number()) < 1e-12));
                }
                return Value::make_bool(left.to_string() != right.to_string());
            }
            if (op == "<") {
                if (left.is_int() && right.is_int()) {
                    return Value::make_bool(left.get_int() < right.get_int());
                }
                return Value::make_bool(left.as_number() < right.as_number());
            }
            if (op == ">") {
                if (left.is_int() && right.is_int()) {
                    return Value::make_bool(left.get_int() > right.get_int());
                }
                return Value::make_bool(left.as_number() > right.as_number());
            }
            if (op == "<=") {
                if (left.is_int() && right.is_int()) {
                    return Value::make_bool(left.get_int() <= right.get_int());
                }
                return Value::make_bool(left.as_number() <= right.as_number());
            }
            if (op == ">=") {
                if (left.is_int() && right.is_int()) {
                    return Value::make_bool(left.get_int() >= right.get_int());
                }
                return Value::make_bool(left.as_number() >= right.as_number());
            }

            // bitwise operations only work on integers
            // for non-int operands, safe_double_to_i64 avoids UB for NaN/Inf/out-of-range.
            auto to_i64 = [](const Value &v) -> int64_t {
                return v.is_int() ? v.get_int() : safe_double_to_i64(v.as_number());
            };
            if (op == "&") {
                return Value::make_int(to_i64(left) & to_i64(right));
            }
            if (op == "|") {
                return Value::make_int(to_i64(left) | to_i64(right));
            }
            if (op == "^") {
                return Value::make_int(to_i64(left) ^ to_i64(right));
            }
            if (op == "<<") {
                int64_t l = to_i64(left);
                int64_t r = to_i64(right);
                // Mask shift count to [0, 63]; matches bytecode VM + JIT.
                return Value::make_int(l << (r & 63));
            }
            if (op == ">>") {
                int64_t l = to_i64(left);
                int64_t r = to_i64(right);
                return Value::make_int(l >> (r & 63));
            }

            return Value::none();
        }

        case ExprKind::Call: {
            const auto *callExpr = (const CallExpr *)e;
            // check if this is a method call (callee is MemberExpr like obj.method())
            if (callExpr->callee->kind == ExprKind::Member) {
                const auto *memberExpr = (const MemberExpr *)callExpr->callee.get();
                Value obj = eval_expr(memberExpr->object.get());
                std::string method_name = memberExpr->member;

                // handle class instance method calls
                if (obj.is_class_instance()) {
                    const auto &instance = obj.get_class_instance();
                    const nari::ClassDecl *class_decl = Parser::get_registered_class(instance->class_name);

                    if (!class_decl) {
                        runtime_fatal("Unknown class: " + instance->class_name, callExpr);
                    }

                    const nari::ClassMethod *method = find_method_in_hierarchy(class_decl, method_name);

                    if (!method) {
                        runtime_fatal("Class " + instance->class_name + " has no method '" + method_name + "'",
                                      callExpr);
                    }

                    if (method->visibility == nari::Visibility::Private) {
                        if (current_class_name != instance->class_name) {
                            runtime_fatal("Cannot call private method '" + method_name + "' of class " +
                                              instance->class_name,
                                          callExpr);
                        }
                    }

                    std::vector<Value> arg_values;
                    for (const auto &arg_expr : callExpr->args) {
                        arg_values.push_back(eval_expr(arg_expr.get()));
                    }

                    if (arg_values.size() != method->params.size()) {
                        runtime_fatal("Method '" + method_name + "' expects " + std::to_string(method->params.size()) +
                                          " arguments but got " + std::to_string(arg_values.size()),
                                      callExpr);
                    }

                    ClassInstancePtr saved_instance = current_instance;
                    std::string saved_class = current_class_name;
                    current_instance = instance;
                    current_class_name = instance->class_name;

                    call_stack.emplace_back();
                    call_const_stack.emplace_back();

                    for (size_t i = 0; i < method->params.size(); ++i) {
                        call_stack.back()[method->params[i].name] = arg_values[i];
                    }

                    Value return_value = Value::none();
                    if (method->body) {
                        for (const auto &stmt : method->body->stmts) {
                            exec_stmt(stmt.get());
                            if (flags.return_flag) {
                                return_value = flags.return_value;
                                flags.return_flag = false;
                                break;
                            }
                            if (flags.break_flag || flags.continue_flag || flags.throw_flag) {
                                break;
                            }
                        }
                    }

                    call_stack.pop_back();
                    call_const_stack.pop_back();
                    current_instance = saved_instance;
                    current_class_name = saved_class;

                    return return_value;
                }

                // static method call: ClassName.method(args)
                if (obj.is_string()) {
                    const std::string &class_name = obj.get_string();
                    const nari::ClassDecl *class_decl = Parser::get_registered_class(class_name);
                    if (class_decl) {
                        const nari::ClassMethod *method = nullptr;
                        for (const auto &m : class_decl->methods) {
                            if (m.name == method_name && m.is_static) {
                                method = &m;
                                break;
                            }
                        }
                        if (method && method->body) {
                            std::vector<Value> arg_values;
                            for (const auto &arg_expr : callExpr->args) {
                                arg_values.push_back(eval_expr(arg_expr.get()));
                            }

                            ClassInstancePtr saved_instance = current_instance;
                            std::string saved_class = current_class_name;
                            current_instance = nullptr;
                            current_class_name = class_name;

                            call_stack.emplace_back();
                            call_const_stack.emplace_back();
                            for (size_t i = 0; i < method->params.size() && i < arg_values.size(); ++i) {
                                call_stack.back()[method->params[i].name] = arg_values[i];
                            }

                            Value return_value = Value::none();
                            for (const auto &stmt : method->body->stmts) {
                                exec_stmt(stmt.get());
                                if (flags.return_flag) {
                                    return_value = flags.return_value;
                                    flags.return_flag = false;
                                    break;
                                }
                                if (flags.break_flag || flags.continue_flag || flags.throw_flag) {
                                    break;
                                }
                            }

                            call_stack.pop_back();
                            call_const_stack.pop_back();
                            current_instance = saved_instance;
                            current_class_name = saved_class;
                            return return_value;
                        }
                    }
                }

                if (obj.is_object()) {
                    const auto &objMap = obj.get_obj_ptr();
                    auto it = objMap->get_field(method_name);
                    if (it) {
                        if (it->is_function()) {
                            std::vector<Value> argvals;
                            for (const auto &a : callExpr->args) {
                                argvals.push_back(eval_expr(a.get()));
                            }

                            const auto &func_val = it->get_function();

                            if (is_builtin_name(func_val.name)) {
                                return call_builtin(func_val.name, argvals, callExpr);
                            }

                            // try direct pointer first (for lambdas)
                            if (func_val.func_ptr) {
                                return call_user_function(func_val.func_ptr.get(), argvals);
                            }

                            // otherwise look it up as a named user function
                            auto func_it = functions.find(func_val.name);
                            if (func_it != functions.end()) {
                                return call_user_function(func_it->second.get(), argvals);
                            }

                            // otherwise dispatch to the bytecode VM
                            if (external_call_function_value) {
                                return external_call_function_value(*it, argvals);
                            }
                        }
                        // member exists but is not a function, fall through to error
                    }
                }

                // Delegate method call: obj.method(args) resolves `method` via the get trap, then invokes the result
                if (obj.is_delegate()) {
                    std::vector<Value> argvals;
                    argvals.reserve(callExpr->args.size());
                    for (const auto &a : callExpr->args) {
                        argvals.push_back(eval_expr(a.get()));
                    }
                    if (method_name == "has_key" && argvals.size() == 1) {
                        return Value::make_bool(delegate_has(obj, argvals[0]));
                    }
                    return delegate_call_method(obj, method_name, std::move(argvals));
                }

                if (is_builtin_name(method_name)) {
                    if (!is_method_valid_for_type(method_name, obj)) {
                        std::string type_str = obj.is_string()   ? "string"
                                               : obj.is_array()  ? "array"
                                               : obj.is_object() ? "object"
                                               : obj.is_int()    ? "number"
                                               : obj.is_float()  ? "number"
                                               : obj.is_bool()   ? "boolean"
                                               : obj.is_none()   ? "null"
                                                                 : "value";
                        runtime_fatal("Method '" + method_name + "' is not available on type '" + type_str + "'",
                                      callExpr);
                    }

                    std::vector<Value> method_args;
                    method_args.reserve(callExpr->args.size() + 1);
                    method_args.push_back(obj); // object becomes first argument
                    for (const auto &a : callExpr->args) {
                        method_args.push_back(eval_expr(a.get()));
                    }

                    return call_builtin(method_name, method_args, callExpr);
                }

                std::string type_str = obj.is_string()   ? "string"
                                       : obj.is_array()  ? "array"
                                       : obj.is_object() ? "object"
                                       : obj.is_int()    ? "number"
                                       : obj.is_float()  ? "number"
                                       : obj.is_bool()   ? "boolean"
                                                         : "value";
                runtime_fatal("Unknown method '" + method_name + "' on " + type_str, callExpr);
            }

            // regular function call (not a method)
            std::vector<Value> argvals;
            argvals.reserve(callExpr->args.size());
            for (const auto &a : callExpr->args) {
                argvals.push_back(eval_expr(a.get()));
            }

            std::string op;
            if (callExpr->callee->kind == ExprKind::Ident) {
                op = ((const IdentExpr *)callExpr->callee.get())->name;
            }

            Value calleeVal;
            bool haveCalleeVal = false;
            if (op.empty()) {
                calleeVal = eval_expr(callExpr->callee.get());
                haveCalleeVal = true;
                if (calleeVal.is_function()) {
                    op = calleeVal.get_function().name;
                }
            }

            // only allow calling global builtins directly, not method-only builtins
            if (is_global_builtin(op)) {
                return call_builtin(op, argvals, callExpr);
            }

            // try to evaluate the callee as an expression
            if (!haveCalleeVal) {
                calleeVal = eval_expr(callExpr->callee.get());
                haveCalleeVal = true;
            }
            if (calleeVal.is_function()) {
                const auto &func_val = calleeVal.get_function();

                // first, check if we have a direct pointer to the function (for lambdas)
                if (func_val.func_ptr) {
                    return call_user_function(func_val.func_ptr.get(), argvals);
                }

                // otherwise, look it up in the global functions map (for named functions)
                auto it = functions.find(func_val.name);
                if (it != functions.end()) {
                    return call_user_function(it->second.get(), argvals);
                }

                // otherwise dispatch to the bytecode VM
                if (external_call_function_value) {
                    return external_call_function_value(calleeVal, argvals);
                }
            }

            // Delegate call trap: d(args) -> handler.call(target, [args]).
            // After the is_function() fast path so ordinary calls are untouched.
            if (haveCalleeVal && calleeVal.is_delegate()) {
                return delegate_call(calleeVal, argvals);
            }

            // class instantiation: ClassName(args) where callee is a string matching a class
            if (haveCalleeVal && calleeVal.is_string()) {
                const std::string &class_name = calleeVal.get_string();
                const nari::ClassDecl *class_decl = Parser::get_registered_class(class_name);
                if (class_decl) {
                    ClassInstance *instance = new ClassInstance(class_name);
                    instance->type_tag = ValueTag::ClassInstance;
                    GarbageCollector::instance().track(instance, GarbageCollector::TrackedType::ClassInstance);

                    std::vector<const nari::ClassField *> all_fields;
                    collect_all_fields(class_decl, all_fields);

                    auto &layout_reg = class_layout_registry();
                    if (layout_reg.find(class_name) == layout_reg.end()) {
                        ClassLayout layout;
                        layout.names.reserve(all_fields.size());
                        layout.index.reserve(all_fields.size());
                        for (size_t i = 0; i < all_fields.size(); i++) {
                            layout.names.push_back(all_fields[i]->name);
                            layout.index[all_fields[i]->name] = (uint32_t)i;
                        }
                        layout_reg.emplace(class_name, std::move(layout));
                    }
                    instance->layout = &layout_reg.at(class_name);

                    instance->field_values.resize(all_fields.size());
                    for (size_t i = 0; i < all_fields.size(); i++) {
                        if (all_fields[i]->default_value) {
                            instance->field_values[i] = eval_expr(all_fields[i]->default_value.get());
                        } else {
                            instance->field_values[i] = Value::none();
                        }
                    }

                    const nari::ClassMethod *ctor = find_method_in_hierarchy(class_decl, "init");
                    if (ctor && ctor->is_constructor) {
                        ClassInstancePtr saved_instance = current_instance;
                        std::string saved_class = current_class_name;
                        current_instance = instance;
                        current_class_name = class_name;
                        call_stack.emplace_back();
                        call_const_stack.emplace_back();
                        for (size_t i = 0; i < ctor->params.size() && i < argvals.size(); ++i) {
                            call_stack.back()[ctor->params[i].name] = argvals[i];
                        }
                        if (ctor->body) {
                            for (const auto &stmt : ctor->body->stmts) {
                                exec_stmt(stmt.get());
                                if (flags.return_flag) {
                                    flags.return_flag = false;
                                    break;
                                }
                                if (flags.break_flag || flags.continue_flag || flags.throw_flag) {
                                    break;
                                }
                            }
                        }
                        call_stack.pop_back();
                        call_const_stack.pop_back();
                        current_instance = saved_instance;
                        current_class_name = saved_class;
                    }

                    return Value::from_class_instance(instance);
                }
            }

            // Unknown call: print and return none

            printf("[call] %s(", (op.empty() ? "<expr>" : op.c_str()));
            for (size_t i = 0; i < argvals.size(); ++i) {
                if (i) {
                    printf(", ");
                }
                printf("%s", argvals[i].to_string().c_str());
            }
            printf(")\n");
            return Value::none();
        }

        case ExprKind::Ternary: {
            const auto *ternary = (const TernaryExpr *)e;
            Value cond = eval_expr(ternary->condition.get());
            if (cond.as_bool()) {
                return eval_expr(ternary->true_expr.get());
            } else {
                return eval_expr(ternary->false_expr.get());
            }
        }

        case ExprKind::Match: {
            const auto *match_expr = (const MatchExpr *)e;
            Value scrutinee = eval_expr(match_expr->scrutinee.get());

            for (const auto &arm : match_expr->arms) {
                Value bindings;
                if (match_pattern(arm.pattern.get(), scrutinee, bindings)) {
                    block_scope_stack.push_back({});
                    block_const_scope_stack.push_back({});
                    auto &scope = block_scope_stack.back();

                    if (bindings.is_object()) {
                        const ObjectObj *binding_oobj = bindings.get_obj_ptr();
                        for (const auto &name : binding_oobj->get_keys()) {
                            if (const Value *val = binding_oobj->get_field(name)) {
                                scope[name] = *val;
                            }
                        }
                    }

                    Value result = eval_expr(arm.body.get());

                    block_scope_stack.pop_back();
                    block_const_scope_stack.pop_back();

                    return result;
                }
            }

            runtime_fatal("No pattern matched in match expression", match_expr);
        }

        // spawn expression - creates a handle for cooperative async execution
        case ExprKind::Spawn: {
            const auto *spawnExpr = (const SpawnExpr *)e;
            if (!spawnExpr->body) {
                return Value::make_handle(nullptr);
            }
            auto handle = Value::make_handle_ptr();
            auto task = std::make_unique<Task>(spawnExpr->body.get());

            if (!call_stack.empty()) {
                task->locals = call_stack.back();
                task->const_locals = call_const_stack.back();
            }
            task->block_scopes = block_scope_stack;
            task->block_const_scopes = block_const_scope_stack;

            handle->task = std::move(task);
            handle->state = HandleData::Running;

            task_queue.push(handle);
            return Value::make_handle(handle);
        }

        case ExprKind::StringInterpolation: {
            const auto *stringInterpExpr = (const StringInterpolationExpr *)e;
            std::string result;

            for (size_t i = 0; i < stringInterpExpr->parts.size(); ++i) {
                result += stringInterpExpr->parts[i];
                if (i < stringInterpExpr->expr_sources.size()) {
#ifndef DISABLE_PARSER
                    // fast path: use the pre-parsed expression AST stored at parse time.
                    if (i < stringInterpExpr->exprs.size() && stringInterpExpr->exprs[i]) {
                        Value expr_val = eval_expr(stringInterpExpr->exprs[i].get());
                        result += format_interpolated_value(this, expr_val, stringInterpExpr, i);
                    } else {
                        // slow path (fallback): re-parse the expression source.
                        std::string saved_filename =
                            !stringInterpExpr->filename.empty() ? stringInterpExpr->filename : "<interpolation>";
                        Parser::set_source_filename(saved_filename);

                        auto expr_funcs = Parser::parse_program_from_source(stringInterpExpr->expr_sources[i]);
                        Parser::set_source_filename(saved_filename);

                        if (expr_funcs.size() < 2 || !expr_funcs[1] || !expr_funcs[1]->body ||
                            expr_funcs[1]->body->stmts.empty()) {
                            runtime_fatal("Failed to parse interpolated expression: " +
                                              stringInterpExpr->expr_sources[i],
                                          stringInterpExpr);
                        }

                        auto *first_stmt = expr_funcs[1]->body->stmts[0].get();
                        auto *expr_stmt =
                            first_stmt->stmt_kind == StmtKind::Expr ? (nari::ExprStmt *)first_stmt : nullptr;
                        if (!expr_stmt || !expr_stmt->expr) {
                            runtime_fatal("String interpolation must contain expressions: " +
                                              stringInterpExpr->expr_sources[i],
                                          stringInterpExpr);
                        }

                        Value expr_val = eval_expr(expr_stmt->expr.get());
                        result += format_interpolated_value(this, expr_val, stringInterpExpr, i);
                    }
#else
                    runtime_fatal("String interpolation unavailable: DISABLE_PARSER build", stringInterpExpr);
#endif
                }
            }

            return Value::make_string(result);
        }

        case ExprKind::ArrayLiteral: {
            const auto *arrayLitExpr = (const ArrayLiteralExpr *)e;
            std::vector<Value> elements;
            for (const auto &elem : arrayLitExpr->elements) {
                elements.push_back(eval_expr(elem.get()));
            }
            return Value::make_array(std::move(elements));
        }

        case ExprKind::ObjectLiteral: {
            const auto *objectLitExpr = (const ObjectLiteralExpr *)e;
            Value result = Value::make_object();
            ObjectObj *oobj = result.get_obj_ptr();
            // only pre-reserve the shape-mode vector when the literal is small enough to stay in shape mode
            if (objectLitExpr->entries.size() <= ObjectObj::kDictModeThreshold) {
                oobj->fields.reserve(objectLitExpr->entries.size());
            }
            for (const auto &[key, val] : objectLitExpr->entries) {
                oobj->set_field(key, eval_expr(val.get()));
            }
            return result;
        }

        // func(params) { ... }
        case ExprKind::Function: {
            const auto *funcExpr = (const FunctionExpr *)e;
            static size_t lambda_counter = 0;
            std::string func_name = "<lambda_" + std::to_string(lambda_counter++) + ">";

            // use shared_ptr for lambdas so they can be cleaned up automatically
            auto func = std::make_shared<nari::Function>();
            func->name = func_name;
            func->line = funcExpr->line;
            func->col = funcExpr->col;
            func->filename = funcExpr->filename;
            func->function_expr = funcExpr;

            if (!call_stack.empty()) {
                if (!current_scope_closure) {
                    current_scope_closure = std::make_shared<std::map<std::string, Value>>(call_stack.back());
                }
                if (!current_scope_closure_consts) {
                    current_scope_closure_consts =
                        std::make_shared<std::unordered_set<std::string>>(call_const_stack.back());
                }
                func->closure_env_ptr = new std::shared_ptr<std::map<std::string, Value>>(current_scope_closure);
                func->closure_deleter = [](void *ptr) { delete (std::shared_ptr<std::map<std::string, Value>> *)ptr; };
                func->closure_const_env_ptr =
                    new std::shared_ptr<std::unordered_set<std::string>>(current_scope_closure_consts);
                func->closure_const_deleter = [](void *ptr) {
                    delete (std::shared_ptr<std::unordered_set<std::string>> *)ptr;
                };
            }

            for (const auto &param : funcExpr->params) {
                func->params.emplace_back(param.name,
                                          nullptr, // we'll handle defaults during function calls
                                          param.is_rest);
            }
            func->body = std::make_unique<BlockStmt>();

            // don't add lambdas to global functions map, store them directly in the Value.
            // This allows them to be garbage collected
            return Value::make_function(func_name, func);
        }

        case ExprKind::Index: {
            const auto *idxExpr = (const IndexExpr *)e;
            Value obj = eval_expr(idxExpr->object.get());
            Value index = eval_expr(idxExpr->index.get());

            if (obj.is_array()) {
                const auto &arr = obj.get_array();
                // coerce whole-number floats to int (e.g. from string arithmetic: "1" - 1 = 0.0)
                if (index.is_float()) {
                    double f = index.get_float();
                    if (f == std::floor(f) && !std::isinf(f) && !std::isnan(f)) {
                        index = Value::make_int((int64_t)f);
                    }
                }
                if (!index.is_int()) {
                    runtime_fatal("Array index must be int", idxExpr);
                }
                int64_t idx = index.get_int();
                if (idx < 0 || idx >= (int64_t)arr.size()) {
                    std::string error_msg = "Array index out of bounds: " + std::to_string(idx) +
                                            " (size: " + std::to_string(arr.size()) + ")";
                    runtime_fatal(error_msg, idxExpr);
                }
                return arr[idx];
            } else if (obj.is_object()) {
                const auto &objMap = obj.get_obj_ptr();
                const Value *v = objMap->get_field(index.to_string());
                return v ? *v : Value::none();
            } else if (obj.is_delegate()) {
                // Delegate get trap: obj[index] -> handler.get(target, index).
                // After the object fast path so shape prop-IC is untouched.
                return delegate_get(obj, index);
            } else if (obj.is_string()) {
                if (index.is_float()) {
                    double f = index.get_float();
                    if (f == std::floor(f) && !std::isinf(f) && !std::isnan(f)) {
                        index = Value::make_int((int64_t)f);
                    }
                }
                if (!index.is_int()) {
                    runtime_fatal("String index must be int", idxExpr);
                }
                int64_t idx = index.get_int();
                const auto &str = obj.get_string();
                if (idx < 0 || idx >= (int64_t)str.size()) {
                    return Value::none();
                }
                return Value::make_string(std::string(1, str[idx]));
            } else {
                runtime_fatal("Index access requires array, object, or string", idxExpr);
            }
        }

        case ExprKind::Member: {
            const auto *memExpr = (const MemberExpr *)e;
            Value obj = eval_expr(memExpr->object.get());

            if (obj.is_class_instance()) {
                const auto &instance = obj.get_class_instance();
                const nari::ClassDecl *class_decl = Parser::get_registered_class(instance->class_name);

                if (!class_decl) {
                    runtime_fatal("Unknown class: " + instance->class_name, memExpr);
                }

                const nari::ClassField *field = find_field_in_hierarchy(class_decl, memExpr->member);
                if (field) {
                    if (field->visibility == nari::Visibility::Private) {
                        if (current_class_name != instance->class_name) {
                            runtime_fatal("Cannot access private field '" + memExpr->member + "' of class " +
                                              instance->class_name,
                                          memExpr);
                        }
                    }

                    const Value *fv = instance->get_field(memExpr->member);
                    return fv ? *fv : Value::none();
                }

                return Value::none();
            }

            if (obj.is_object()) {
                const auto &objMap = obj.get_obj_ptr();
                const Value *v = objMap->get_field(memExpr->member);
                return v ? *v : Value::none();
            } else if (obj.is_delegate()) {
                // Delegate get trap: obj.member -> handler.get(target, "member").
                // After the object fast path so shape prop-IC is untouched.
                return delegate_get(obj, Value::make_string(memExpr->member));
            } else if (obj.is_handle()) {
                const auto &handle = obj.get_handle();
                if (!handle) {
                    runtime_fatal("Member access on null handle", memExpr);
                }

                if (memExpr->member == "await") {
                    // process tasks and IO cooperatively until this handle completes
                    while (handle->state == HandleData::Running) {
                        bool did_work = false;

                        process_completed_io();

                        if (!task_queue.empty()) {
                            HandlePtr next_task = task_queue.front();
                            task_queue.pop();
                            step_task(next_task);
                            if (next_task->state == HandleData::Running) {
                                task_queue.push(next_task);
                            }
                            did_work = true;
                        }

                        if (has_pending_io()) {
                            did_work = true;
                        }

                        // if no work was done and handle is still running, yield briefly
                        if (!did_work && handle->state == HandleData::Running) {
                            NARI_SLEEP_MILLIS(1);
                        }
                    }

                    if (handle->state == HandleData::Failed) {
                        flags.throw_flag = true;
                        flags.throw_value = handle->error;
                        return Value::none();
                    }

                    return handle->result;
                } else if (memExpr->member == "ready") {
                    process_completed_io();

                    // if still not ready, give IO threads a tiny bit of time
                    if (handle->state == HandleData::Running && has_pending_io()) {
                        NARI_SLEEP_MICROS(100);
                        process_completed_io();
                    }

                    return Value::make_bool(handle->state != HandleData::Running);
                } else if (memExpr->member == "failed") {
                    return Value::make_bool(handle->state == HandleData::Failed);
                } else if (memExpr->member == "error") {
                    return handle->error;
                } else if (memExpr->member == "duration") {
                    if (handle->state == HandleData::Running) {
                        // still running - return time elapsed so far
                        auto now = chrono::steady_clock::now();
                        auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - handle->start_time);
                        return Value::make_int(elapsed.count());
                    } else {
                        // completed or failed - return total time
                        auto elapsed =
                            chrono::duration_cast<chrono::milliseconds>(handle->end_time - handle->start_time);
                        return Value::make_int(elapsed.count());
                    }
                }
            }

            // static field access: ClassName.field
            if (obj.is_string()) {
                const std::string &class_name = obj.get_string();
                const nari::ClassDecl *class_decl = Parser::get_registered_class(class_name);
                if (class_decl) {
                    // ensure static fields initialized
                    auto &inited = Parser::get_static_inited_classes();
                    if (!inited.count(class_name)) {
                        inited.insert(class_name);
                        auto &fields = Parser::get_static_fields();
                        for (const auto &f : class_decl->fields) {
                            if (!f.is_static) {
                                continue;
                            }
                            std::string k = class_name + "." + f.name;
                            if (f.default_value) {
                                fields[k] = eval_expr(f.default_value.get());
                            } else {
                                fields[k] = Value::none();
                            }
                        }
                    }
                    std::string key = class_name + "." + memExpr->member;
                    auto &static_fields = Parser::get_static_fields();
                    auto it = static_fields.find(key);
                    if (it != static_fields.end()) {
                        return it->second;
                    }
                    return Value::none();
                }
            }

            return Value::none();
        }

        case ExprKind::This: {
            if (!current_instance) {
                const auto *thisExpr = (const ThisExpr *)e;
                runtime_fatal("'this' can only be used inside class methods", thisExpr);
            }
            Value v;
            v = Value::from_class_instance(current_instance);
            return v;
        }

        // new ClassName(args)
        case ExprKind::New: {
            const auto *newExpr = (const NewExpr *)e;
            const nari::ClassDecl *class_decl = Parser::get_registered_class(newExpr->class_name);

            if (!class_decl) {
                runtime_fatal("Unknown class: " + newExpr->class_name, newExpr);
            }

            ClassInstance *instance = new ClassInstance(newExpr->class_name);
            instance->type_tag = ValueTag::ClassInstance;
            GarbageCollector::instance().track(instance, GarbageCollector::TrackedType::ClassInstance);

            std::vector<const nari::ClassField *> all_fields;
            collect_all_fields(class_decl, all_fields);

            // register class layout once (shared across all instances)
            auto &layout_reg = class_layout_registry();
            if (layout_reg.find(newExpr->class_name) == layout_reg.end()) {
                ClassLayout layout;
                layout.names.reserve(all_fields.size());
                layout.index.reserve(all_fields.size());
                for (size_t i = 0; i < all_fields.size(); i++) {
                    layout.names.push_back(all_fields[i]->name);
                    layout.index[all_fields[i]->name] = i;
                }
                layout_reg.emplace(newExpr->class_name, std::move(layout));
            }
            instance->layout = &layout_reg.at(newExpr->class_name);

            // initialize fields in flat vector (indexed by ClassLayout slot)
            instance->field_values.resize(all_fields.size());
            for (size_t i = 0; i < all_fields.size(); i++) {
                if (all_fields[i]->default_value) {
                    instance->field_values[i] = eval_expr(all_fields[i]->default_value.get());
                } else {
                    instance->field_values[i] = Value::none();
                }
            }

            const nari::ClassMethod *constructor = find_method_in_hierarchy(class_decl, "init");
            if (constructor && constructor->is_constructor) {
                std::vector<Value> arg_values;
                for (const auto &arg_expr : newExpr->args) {
                    arg_values.push_back(eval_expr(arg_expr.get()));
                }

                if (arg_values.size() != constructor->params.size()) {
                    runtime_fatal("Constructor expects " + std::to_string(constructor->params.size()) +
                                      " arguments but got " + std::to_string(arg_values.size()),
                                  newExpr);
                }

                ClassInstancePtr saved_instance = current_instance;
                std::string saved_class = current_class_name;
                current_instance = instance;
                current_class_name = newExpr->class_name;

                call_stack.emplace_back();
                call_const_stack.emplace_back();

                for (size_t i = 0; i < constructor->params.size(); ++i) {
                    call_stack.back()[constructor->params[i].name] = arg_values[i];
                }

                if (constructor->body) {
                    for (const auto &stmt : constructor->body->stmts) {
                        exec_stmt(stmt.get());
                        if (flags.any_flag()) {
                            break;
                        }
                    }
                }

                call_stack.pop_back();
                call_const_stack.pop_back();
                current_instance = saved_instance;
                current_class_name = saved_class;

                // clear flags except throw
                if (flags.return_flag) {
                    flags.return_flag = false;
                }
            } else if (!newExpr->args.empty()) {
                runtime_fatal("Class " + newExpr->class_name + " has no constructor but arguments were provided",
                              newExpr);
            }

            return Value::from_class_instance(instance);
        }

        default:
            break;
    } // end switch

    return Value::none();
}

void ScriptRuntime::exec_stmt(const Stmt *s) {
    if (!s) {
        fprintf(stderr, "Runtime trace: exec_stmt received null statement\n");
        return;
    }
    if (Runtime::g_shutdown_requested.load()) {
        flags.shutdown_flag = true;
        return;
    }

    if (debug_stmt_hook) {
        debug_stmt_hook(s);
    }

    // basic trace info for debugging: file:line:col and a small type hint.
    std::string fn = s->filename.empty() ? std::string("<unknown>") : s->filename;
    if (Runtime::runtime_trace_enabled()) {
        std::string trace_msg = "exec_stmt: " + fn + ":" + std::to_string(s->line) + ":" + std::to_string(s->col);
        Runtime::runtime_log(Runtime::TraceLevel::Debug, trace_msg);
    }

    // dispatch via type tag instead of dynamic_cast
    switch (s->stmt_kind) {
        // variable declaration: `let name = expr` or `global name = expr`
        // or destructuring: `let [a, b] = expr` or `let {x, y} = expr`
        case StmtKind::VarDecl: {
            const auto *varDecl = (const VarDeclStmt *)s;

            if (varDecl->destructure_kind == nari::DestructureKind::Array) {
                // array destructuring: let [a, b, c] = value
                if (!varDecl->initializerExpr) {
                    runtime_fatal("Array destructuring requires initialization", varDecl);
                }

                Value val = eval_expr(varDecl->initializerExpr.get());
                if (!val.is_array()) {
                    runtime_fatal("Array destructuring requires an array value", varDecl);
                }

                const auto &arr = val.get_array();
                for (size_t i = 0; i < varDecl->array_names.size(); i++) {
                    Value element = Value::none();
                    if (i < arr.size()) {
                        element = arr[i];
                    }

                    const std::string &name = varDecl->array_names[i];
                    if (varDecl->is_global) {
                        globals[name] = element;
                        if (varDecl->is_const) {
                            global_consts.insert(name);
                        }
                    } else if (!block_scope_stack.empty()) {
                        auto &block_scope = block_scope_stack.back();
                        if (block_scope.find(name) != block_scope.end()) {
                            runtime_fatal("Variable already declared in current block: '" + name + "'", varDecl);
                        }
                        block_scope[name] = element;
                        if (varDecl->is_const) {
                            block_const_scope_stack.back().insert(name);
                        }
                    } else if (!call_stack.empty()) {
                        auto &locals = call_stack.back();
                        if (locals.find(name) != locals.end()) {
                            runtime_fatal("Variable already declared: '" + name + "'", varDecl);
                        }
                        locals[name] = element;
                        if (varDecl->is_const) {
                            call_const_stack.back().insert(name);
                            if (current_scope_closure_consts) {
                                current_scope_closure_consts->insert(name);
                            }
                        }
                    } else {
                        if (!module_stack.empty()) {
                            const std::string &modfn = module_stack.back();
                            auto &module = module_local_vars[modfn];
                            if (module.find(name) != module.end()) {
                                runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
                            }
                            module[name] = element;
                            if (varDecl->is_const) {
                                module_local_consts[modfn].insert(name);
                            }
                        } else if (!varDecl->filename.empty()) {
                            auto &module = module_local_vars[varDecl->filename];
                            if (module.find(name) != module.end()) {
                                runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
                            }
                            module[name] = element;
                            if (varDecl->is_const) {
                                module_local_consts[varDecl->filename].insert(name);
                            }
                        } else {
                            if (globals.find(name) != globals.end()) {
                                runtime_fatal("Global already declared: '" + name + "'", varDecl);
                            }
                            globals[name] = element;
                            if (varDecl->is_const) {
                                global_consts.insert(name);
                            }
                        }
                    }
                }
                return;
            }

            if (varDecl->destructure_kind == nari::DestructureKind::Object) {
                // object destructuring: let {a, b: c} = value
                if (!varDecl->initializerExpr) {
                    runtime_fatal("Object destructuring requires initialization", varDecl);
                }

                Value val = eval_expr(varDecl->initializerExpr.get());
                if (!val.is_object()) {
                    runtime_fatal("Object destructuring requires an object value", varDecl);
                }

                const auto *obj_data = val.get_obj_ptr();
                for (const auto &binding : varDecl->object_bindings) {
                    const std::string &key = binding.first;
                    const std::string &name = binding.second;

                    const Value *fv = obj_data->get_field(key);
                    Value element = fv ? *fv : Value::none();

                    if (varDecl->is_global) {
                        globals[name] = element;
                        if (varDecl->is_const) {
                            global_consts.insert(name);
                        }
                    } else if (!block_scope_stack.empty()) {
                        auto &block_scope = block_scope_stack.back();
                        if (block_scope.find(name) != block_scope.end()) {
                            runtime_fatal("Variable already declared in current block: '" + name + "'", varDecl);
                        }
                        block_scope[name] = element;
                        if (varDecl->is_const) {
                            block_const_scope_stack.back().insert(name);
                        }
                    } else if (!call_stack.empty()) {
                        auto &locals = call_stack.back();
                        if (locals.find(name) != locals.end()) {
                            runtime_fatal("Variable already declared: '" + name + "'", varDecl);
                        }
                        locals[name] = element;
                        if (varDecl->is_const) {
                            call_const_stack.back().insert(name);
                            if (current_scope_closure_consts) {
                                current_scope_closure_consts->insert(name);
                            }
                        }
                    } else {
                        if (!module_stack.empty()) {
                            const std::string &modfn = module_stack.back();
                            auto &module = module_local_vars[modfn];
                            if (module.find(name) != module.end()) {
                                runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
                            }
                            module[name] = element;
                            if (varDecl->is_const) {
                                module_local_consts[modfn].insert(name);
                            }
                        } else if (!varDecl->filename.empty()) {
                            auto &module = module_local_vars[varDecl->filename];
                            if (module.find(name) != module.end()) {
                                runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
                            }
                            module[name] = element;
                            if (varDecl->is_const) {
                                module_local_consts[varDecl->filename].insert(name);
                            }
                        } else {
                            if (globals.find(name) != globals.end()) {
                                runtime_fatal("Global already declared: '" + name + "'", varDecl);
                            }
                            globals[name] = element;
                            if (varDecl->is_const) {
                                global_consts.insert(name);
                            }
                        }
                    }
                }
                return;
            }

            Value val = Value::none();
            if (varDecl->initializerExpr) {
                val = eval_expr(varDecl->initializerExpr.get());
            }

            if (varDecl->is_global) {
                // declare or overwrite a global explicitly requested by `global`
                globals[varDecl->name] = val;
                if (varDecl->is_const) {
                    global_consts.insert(varDecl->name);
                }
            } else {
                if (!block_scope_stack.empty()) {
                    auto &block_scope = block_scope_stack.back();
                    if (block_scope.find(varDecl->name) != block_scope.end()) {
                        std::string error_msg = "Variable already declared in current block: '" + varDecl->name + "'";
                        runtime_fatal(error_msg, varDecl);
                    }
                    block_scope[varDecl->name] = val;
                    if (varDecl->is_const) {
                        block_const_scope_stack.back().insert(varDecl->name);
                    }
                } else if (!call_stack.empty()) {
                    auto &locals = call_stack.back();
                    if (locals.find(varDecl->name) != locals.end()) {
                        std::string error_msg = "Variable already declared: '" + varDecl->name + "'";
                        runtime_fatal(error_msg, varDecl);
                    }
                    locals[varDecl->name] = val;
                    if (varDecl->is_const) {
                        call_const_stack.back().insert(varDecl->name);
                        if (current_scope_closure_consts) {
                            current_scope_closure_consts->insert(varDecl->name);
                        }
                    }
                    if (current_scope_closure) {
                        (*current_scope_closure)[varDecl->name] = val;
                    }
                } else {
                    // top-level let: store as module-local keyed by current module,
                    // else by the declaration's filename, else fall back to globals.
                    if (!module_stack.empty()) {
                        const std::string &modfn = module_stack.back();
                        auto &module = module_local_vars[modfn];
                        if (module.find(varDecl->name) != module.end()) {
                            std::string error_msg = "Module-local already declared: '" + varDecl->name + "'";
                            runtime_fatal(error_msg, varDecl);
                        }
                        module[varDecl->name] = val;
                        if (varDecl->is_const) {
                            module_local_consts[modfn].insert(varDecl->name);
                        }
                    } else if (!varDecl->filename.empty()) {
                        auto &module = module_local_vars[varDecl->filename];
                        if (module.find(varDecl->name) != module.end()) {
                            std::string error_msg = "Module-local already declared: '" + varDecl->name + "'";
                            runtime_fatal(error_msg, varDecl);
                        }
                        module[varDecl->name] = val;
                        if (varDecl->is_const) {
                            module_local_consts[varDecl->filename].insert(varDecl->name);
                        }
                    } else {
                        // fallback to global if no module context or filename is available.
                        if (globals.find(varDecl->name) != globals.end()) {
                            std::string error_msg = "Global already declared: '" + varDecl->name + "'";
                            runtime_fatal(error_msg, varDecl);
                        }
                        globals[varDecl->name] = val;
                        if (varDecl->is_const) {
                            global_consts.insert(varDecl->name);
                        }
                    }
                }
            }
            return;
        }

        case StmtKind::Expr: {
            const auto *exprStmt = (const ExprStmt *)s;
            eval_expr(exprStmt->expr.get());
            return;
        }
        case StmtKind::Assign: {
            const auto *as = (const AssignStmt *)s;
            if (!as->value) {
                std::string error_msg = "Attempt to assign from null expression for target '" + as->target + "'";
                runtime_fatal(error_msg, as);
            }
            if (is_const_binding(as->target, as->filename)) {
                runtime_fatal("TypeError: Assignment to constant variable '" + as->target + "'", as);
            }
            Value v = eval_expr(as->value.get());
            // check block scopes first (innermost out), then locals, then module/globals
            for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
                auto &block_scope = block_scope_stack[i];
                auto blkTarget = block_scope.find(as->target);
                if (blkTarget != block_scope.end()) {
                    blkTarget->second = std::move(v);
                    return;
                }
            }
            if (!call_stack.empty()) {
                call_stack.back()[as->target] = v;
                if (current_scope_closure) {
                    auto cit = current_scope_closure->find(as->target);
                    if (cit != current_scope_closure->end()) {
                        cit->second = v;
                    }
                }
            } else {
                if (!module_stack.empty()) {
                    auto &mmap = module_local_vars[module_stack.back()];
                    mmap[as->target] = std::move(v);
                } else if (!as->filename.empty()) {
                    auto &mmap = module_local_vars[as->filename];
                    mmap[as->target] = std::move(v);
                } else {
                    globals[as->target] = std::move(v);
                }
            }
            return;
        }

        // indexed assignment: arr[i] = val or obj.key = val
        case StmtKind::IndexAssign: {
            const auto *indexAssignStmt = (const IndexAssignStmt *)s;
            if (!indexAssignStmt->target || !indexAssignStmt->value) {
                runtime_fatal("Indexed assignment has null target or value", indexAssignStmt);
            }

            Value val = eval_expr(indexAssignStmt->value.get());

            if (indexAssignStmt->target->kind == ExprKind::Index) {
                const auto *indexExpr = (const IndexExpr *)indexAssignStmt->target.get();
                Value obj = eval_expr(indexExpr->object.get());
                Value index = eval_expr(indexExpr->index.get());

                if (obj.is_array()) {
                    auto &arr = obj.get_array();
                    if (!index.is_int()) {
                        runtime_fatal("Array index must be int", indexAssignStmt);
                    }
                    int64_t idx = index.get_int();
                    if (idx < 0 || idx >= (int64_t)arr.size()) {
                        std::string error_msg = "Array index out of bounds: " + std::to_string(idx) +
                                                " (size: " + std::to_string(arr.size()) + ")";
                        runtime_fatal(error_msg, indexAssignStmt);
                    }
                    arr[idx] = val;
                } else if (obj.is_object()) {
                    obj.get_obj_ptr()->set_field(index.to_string(), val);
                } else if (obj.is_delegate()) {
                    // Delegate set trap: obj[index] = val -> handler.set(target, index, val).
                    delegate_set(obj, index, val);
                } else {
                    runtime_fatal("Indexed assignment requires array or object", indexAssignStmt);
                }
            }
            // handle MemberExpr target: obj.member = val
            else if (indexAssignStmt->target->kind == ExprKind::Member) {
                const auto *memberExpr = (const MemberExpr *)indexAssignStmt->target.get();
                Value obj = eval_expr(memberExpr->object.get());

                if (obj.is_class_instance()) {
                    const auto &instance = obj.get_class_instance();
                    const nari::ClassDecl *class_decl = Parser::get_registered_class(instance->class_name);

                    if (!class_decl) {
                        runtime_fatal("Unknown class: " + instance->class_name, indexAssignStmt);
                    }

                    const nari::ClassField *field = find_field_in_hierarchy(class_decl, memberExpr->member);

                    if (!field) {
                        runtime_fatal("Class " + instance->class_name + " has no field '" + memberExpr->member + "'",
                                      indexAssignStmt);
                    }

                    if (field->visibility == nari::Visibility::Private) {
                        if (current_class_name != instance->class_name) {
                            runtime_fatal("Cannot assign to private field '" + memberExpr->member + "' of class " +
                                              instance->class_name,
                                          indexAssignStmt);
                        }
                    }

                    Value *field_val = instance->get_field(memberExpr->member);
                    if (field_val) {
                        *field_val = std::move(val);
                    } else {
                        runtime_fatal("Class " + instance->class_name + " has no field '" + memberExpr->member + "'",
                                      indexAssignStmt);
                    }
                } else if (obj.is_object()) {
                    obj.get_obj_ptr()->set_field(memberExpr->member, std::move(val));
                } else if (obj.is_delegate()) {
                    // Delegate set trap: obj.member = val -> handler.set(target, "member", val).
                    delegate_set(obj, Value::make_string(memberExpr->member), val);
                } else if (obj.is_string() && Parser::get_registered_class(obj.get_string())) {
                    // static field assignment: ClassName.field = val
                    std::string key = obj.get_string() + "." + memberExpr->member;
                    Parser::get_static_fields()[key] = std::move(val);
                } else {
                    runtime_fatal("Member assignment requires object or class instance", indexAssignStmt);
                }
            } else {
                runtime_fatal("Indexed assignment target must be IndexExpr or MemberExpr", indexAssignStmt);
            }
            return;
        }

        case StmtKind::Block: {
            const auto *blockStmt = (const BlockStmt *)s;
            push_block_scope();

            for (const auto &stmt : blockStmt->stmts) {
                exec_stmt(stmt.get());

                if (flags.any_flag()) {
                    break;
                }
            }

            pop_block_scope();
            return;
        }
        case StmtKind::If: {
            const auto *ifStmt = (const IfStmt *)s;
            bool take = false;
            if (ifStmt->cond) {
                take = eval_expr(ifStmt->cond.get()).as_bool();
            }
            if (take) {
                if (ifStmt->then_branch) {
                    exec_stmt(ifStmt->then_branch.get());
                }
            } else {
                if (ifStmt->else_branch) {
                    exec_stmt(ifStmt->else_branch.get());
                }
            }
            return;
        }
        case StmtKind::While: {
            const auto *whileStmt = (const WhileStmt *)s;

            while (true) {
                bool ok = true;
                if (whileStmt->cond) {
                    ok = eval_expr(whileStmt->cond.get()).as_bool();
                }
                if (!ok) {
                    break;
                }
                flags.continue_flag = false;
                exec_stmt(whileStmt->body.get());
                if (flags.return_flag) {
                    return;
                }
                if (flags.throw_flag) {
                    return;
                }
                if (flags.shutdown_flag) {
                    return;
                }
                if (Runtime::g_shutdown_requested.load()) {
                    break;
                }
                if (Runtime::g_runtime_error_occurred.load()) {
                    break;
                }
                if (flags.break_flag) {
                    flags.break_flag = false;
                    break;
                }
                if (flags.continue_flag) {
                    flags.continue_flag = false;
                    continue;
                }
            }
            return;
        }
        case StmtKind::ForEach: {
            const auto *forEachStmt = (const ForEachStmt *)s;
            Value iterable = eval_expr(forEachStmt->iterable.get());
            if (!iterable.is_array() && !iterable.is_object()) {
                runtime_fatal("for-each requires an array or object", forEachStmt);
            }

            bool is_kv = !forEachStmt->val_var.empty();

            // for objects, iterate over the keys (string values)
            Array obj_keys;
            if (iterable.is_object()) {
                const ObjectObj *oobj = iterable.get_obj_ptr();
                for (const auto &name : oobj->get_keys()) {
                    obj_keys.push_back(Value::make_string(name));
                }
            }
            const Array &items = iterable.is_object() ? obj_keys : iterable.get_array();

            auto assign_var = [&](const std::string &name, Value val) {
                if (!call_stack.empty()) {
                    call_stack.back()[name] = std::move(val);
                } else if (!module_stack.empty()) {
                    module_local_vars[module_stack.back()][name] = std::move(val);
                } else if (!forEachStmt->filename.empty()) {
                    module_local_vars[forEachStmt->filename][name] = std::move(val);
                } else {
                    globals[name] = std::move(val);
                }
            };

            for (size_t _fi = 0; _fi < items.size(); _fi++) {
                const Value &item = items[_fi];
                if (is_kv && iterable.is_array()) {
                    // two-var array: var=index, val_var=element
                    assign_var(forEachStmt->var, Value::make_int(_fi));
                    assign_var(forEachStmt->val_var, item);
                } else {
                    assign_var(forEachStmt->var, item);
                    if (is_kv) {
                        // two-var form for objects: key is item (string), value is obj[key]
                        const ObjectObj *oobj = iterable.get_obj_ptr();
                        const Value *fv = oobj->get_field(item.to_string());
                        assign_var(forEachStmt->val_var, fv ? *fv : Value());
                    }
                }
                flags.continue_flag = false;
                exec_stmt(forEachStmt->body.get());
                if (flags.return_flag) {
                    return;
                }
                if (flags.throw_flag) {
                    return;
                }
                if (flags.shutdown_flag) {
                    return;
                }
                if (Runtime::g_shutdown_requested.load()) {
                    break;
                }
                if (Runtime::g_runtime_error_occurred.load()) {
                    break;
                }
                if (flags.break_flag) {
                    flags.break_flag = false;
                    break;
                }
                if (flags.continue_flag) {
                    flags.continue_flag = false;
                    continue;
                }
            }
            return;
        }
        case StmtKind::Switch: {
            const auto *switchStmt = (const SwitchStmt *)s;
            Value target = eval_expr(switchStmt->value.get());

            auto is_empty_body = [](const BlockPtr &body) -> bool {
                return !body || (dynamic_cast<const BlockStmt *>(body.get()) &&
                                 dynamic_cast<const BlockStmt *>(body.get())->stmts.empty());
            };

            for (size_t i = 0; i < switchStmt->cases.size(); i++) {
                Value match = eval_expr(switchStmt->cases[i].match.get());
                if (Value::values_equal(target, match)) {
                    // fall through: from the matched case, run the first non-empty body
                    for (size_t j = i; j < switchStmt->cases.size(); j++) {
                        if (!is_empty_body(switchStmt->cases[j].body)) {
                            exec_stmt(switchStmt->cases[j].body.get());
                            if (flags.break_flag) {
                                flags.break_flag = false;
                            }
                            return;
                        }
                    }
                    // all remaining cases are empty, try default
                    if (switchStmt->default_body && !is_empty_body(switchStmt->default_body)) {
                        exec_stmt(switchStmt->default_body.get());
                        if (flags.break_flag) {
                            flags.break_flag = false;
                        }
                    }
                    return;
                }
            }

            if (switchStmt->default_body) {
                exec_stmt(switchStmt->default_body.get());
                if (flags.break_flag) {
                    flags.break_flag = false;
                }
            }
            return;
        }
        case StmtKind::For: {
            const auto *forStmt = (const ForStmt *)s;
            push_block_scope();

            if (forStmt->init) {
                exec_stmt(forStmt->init.get());
            }

            while (true) {
                bool ok = true;
                if (forStmt->cond) {
                    ok = eval_expr(forStmt->cond.get()).as_bool();
                }
                if (!ok) {
                    break;
                }
                flags.continue_flag = false;
                exec_stmt(forStmt->body.get());
                if (flags.return_flag) {
                    return;
                }
                if (flags.throw_flag) {
                    break;
                }
                if (Runtime::g_runtime_error_occurred.load()) {
                    break;
                }
                if (flags.break_flag) {
                    flags.break_flag = false;
                    break;
                }
                if (flags.continue_flag) {
                    flags.continue_flag = false;

                    if (forStmt->post) {
                        exec_stmt(forStmt->post.get());
                    }
                    continue;
                }

                if (forStmt->post) {
                    exec_stmt(forStmt->post.get());
                }
            }
            pop_block_scope();
            return;
        }

        case StmtKind::Break:
            flags.break_flag = true;
            return;

        case StmtKind::Continue:
            flags.continue_flag = true;
            return;

        case StmtKind::Return: {
            const auto *returnStmt = (const nari::ReturnStmt *)s;
            // self tail-call elimination
            // detects `return current_func(args...)` when not a lambda
            if (returnStmt->value && !func_stack.empty()) {
                const std::string &cur_name = func_stack.back();
                if (!cur_name.empty() && cur_name[0] != '<') {
                    if (auto *tcall = dynamic_cast<const nari::CallExpr *>(returnStmt->value.get())) {
                        if (auto *callee_id = dynamic_cast<const nari::IdentExpr *>(tcall->callee.get())) {
                            if (callee_id->name == cur_name) {
                                std::vector<Value> new_args;
                                new_args.reserve(tcall->args.size());
                                for (const auto &a : tcall->args) {
                                    new_args.push_back(eval_expr(a.get()));
                                }
                                flags.tailcall_flag = true;
                                flags.tailcall_args = std::move(new_args);
                                flags.return_flag = true;
                                return;
                            }
                        }
                    }
                }
            }

            flags.return_flag = true;
            if (returnStmt->value) {
                flags.return_value = eval_expr(returnStmt->value.get());
            } else {
                flags.return_value = Value::none();
            }
            return;
        }

        default:
            break;
    }

    fprintf(stderr, "Unhandled statement type in exec_stmt %s! This is a bug!\n", demangle(typeid(*s).name()).c_str());
}

Value ScriptRuntime::call_user_function(Function *func, const std::vector<Value> &args) {
    if (!func) {
        fprintf(stderr, "Runtime trace: call_user_function called with null Function*\n");
        return Value::none();
    }

    if (call_stack.size() >= MAX_CALL_DEPTH) {
        runtime_fatal("StackOverflowError: maximum call depth exceeded");
    }

    if (Runtime::runtime_trace_enabled()) {
        Runtime::runtime_log(Runtime::TraceLevel::Debug, "enter function: " + func->name + " @ " + func->filename +
                                                             ":" + std::to_string(func->line) + ":" +
                                                             std::to_string(func->col));
    }

    auto saved_scope_closure = current_scope_closure;
    auto saved_scope_closure_consts = current_scope_closure_consts;
    current_scope_closure = nullptr;
    current_scope_closure_consts = nullptr;

    // every function gets a clean block scope stack
    auto saved_block_scopes = block_scope_stack;
    auto saved_block_const_scopes = block_const_scope_stack;
    block_scope_stack.clear();
    block_const_scope_stack.clear();

    // push module context
    module_stack.push_back(func->filename.empty() ? std::string("<unknown>") : func->filename);
    func_stack.push_back(func->name);

    // create locals frame, pushed once per call and not once per tail-call restart.
    call_stack.emplace_back();
    call_const_stack.emplace_back();

    // current_args is mutable: replaced with new args on each tail-call restart.
    std::vector<Value> current_args(args);

    // closure env ref is stable across restarts (same func, same env ptr).
    std::shared_ptr<std::map<std::string, Value>> closure_env_ref;
    if (func->closure_env_ptr) {
        closure_env_ref = *(std::shared_ptr<std::map<std::string, Value>> *)func->closure_env_ptr;
    }
    std::shared_ptr<std::unordered_set<std::string>> closure_const_env_ref;
    if (func->closure_const_env_ptr) {
        closure_const_env_ref = *(std::shared_ptr<std::unordered_set<std::string>> *)func->closure_const_env_ptr;
    }

    // restart loop for TCE
    while (true) {
        auto &locals = call_stack.back();
        locals.clear();
        auto &const_locals = call_const_stack.back();
        const_locals.clear();

        // copy captured environment to local scope
        if (closure_env_ref) {
            for (const auto &[var_name, var_value] : *closure_env_ref) {
                locals[var_name] = var_value;
            }
        }
        if (closure_const_env_ref) {
            const_locals = *closure_const_env_ref;
        }

        size_t arg_index = 0;
        for (size_t i = 0; i < func->params.size(); ++i) {
            const auto &param = func->params[i];
            if (param.is_rest) {
                std::vector<Value> rest_values;
                for (size_t j = arg_index; j < current_args.size(); ++j) {
                    rest_values.push_back(current_args[j]);
                }
                locals[param.name] = Value::make_array(std::move(rest_values));
                arg_index = current_args.size();
            } else if (arg_index < current_args.size()) {
                locals[param.name] = current_args[arg_index++];
            } else if (param.default_value) {
                locals[param.name] = eval_expr(param.default_value.get());
            } else {
                locals[param.name] = Value::none();
            }
        }

        flags.return_flag = false;
        flags.return_value = Value::none();
        flags.tailcall_flag = false;

        if (func->function_expr && func->function_expr->body) {
            // lambda function created from FunctionExpr
            for (const auto &st : func->function_expr->body->stmts) {
                if (!st) {
                    fprintf(stderr, "Runtime trace: skipping null statement in lambda %s\n", func->name.c_str());
                    continue;
                }
                exec_stmt(st.get());
                if (flags.return_flag) {
                    break;
                }
                if (flags.throw_flag) {
                    break;
                }
                if (flags.break_flag) {
                    flags.break_flag = false;
                }
                if (flags.continue_flag) {
                    flags.continue_flag = false;
                }
            }
        } else if (func->body) {
            // regular function with copied/cloned body
            for (const auto &st : func->body->stmts) {
                if (!st) {
                    fprintf(stderr, "Runtime trace: skipping null statement in function %s\n", func->name.c_str());
                    continue;
                }
                exec_stmt(st.get());
                if (flags.return_flag) {
                    break;
                }
                if (flags.throw_flag) {
                    break;
                }
                if (flags.break_flag) {
                    flags.break_flag = false;
                }
                if (flags.continue_flag) {
                    flags.continue_flag = false;
                }
            }
        } else {
            fprintf(stderr, "Runtime trace: function %s has no body\n", func->name.c_str());
        }

        // Tail-call restart: write closure vars back, update args, loop.
        if (flags.tailcall_flag) {
            if (closure_env_ref) {
                auto &lf = call_stack.back();
                for (auto &[k, dest] : *closure_env_ref) {
                    if (current_scope_closure) {
                        auto cit = current_scope_closure->find(k);
                        if (cit != current_scope_closure->end()) {
                            dest = cit->second;
                            continue;
                        }
                    }
                    auto lit = lf.find(k);
                    if (lit != lf.end()) {
                        dest = lit->second;
                    }
                }
            }
            current_args = std::move(flags.tailcall_args);
            flags.tailcall_flag = false;
            flags.return_flag = false;
            flags.return_value = Value::none();
            continue;
        }
        break; // normal return
    }

    Value result = flags.return_value;
    flags.return_flag = false;
    flags.return_value = Value::none();

    if (closure_env_ref) {
        auto &locals_final = call_stack.back();
        for (auto &[var_name, dest] : *closure_env_ref) {
            // Prefer current_scope_closure: it reflects mutations made by any
            // inner closures that ran during this call.
            if (current_scope_closure) {
                auto cit = current_scope_closure->find(var_name);
                if (cit != current_scope_closure->end()) {
                    dest = cit->second;
                    continue;
                }
            }
            auto lit = locals_final.find(var_name);
            if (lit != locals_final.end()) {
                dest = lit->second;
            }
        }
    }

    call_stack.pop_back();
    call_const_stack.pop_back();
    if (!func_stack.empty()) {
        func_stack.pop_back();
    }
    if (!module_stack.empty()) {
        module_stack.pop_back();
    }

    block_scope_stack = saved_block_scopes;
    block_const_scope_stack = saved_block_const_scopes;
    current_scope_closure = saved_scope_closure;
    current_scope_closure_consts = saved_scope_closure_consts;

    if (Runtime::runtime_trace_enabled()) {
        std::string trace_msg = "exit function: " + func->name;
        Runtime::runtime_log(Runtime::TraceLevel::Debug, trace_msg);
    }

    return result;
}

namespace Runtime {

void run_program_with_runtime(FuncList &funcs, int argc, char **argv) {
#ifndef DISABLE_PARSER
    Parser::set_source_filename("<embedded_stdlib>");
    std::string embedded = nari_std_prelude_source();
    auto stdlib_funcs = Parser::parse_program_from_source(embedded, false);

    // stdlib functions must come first so user code can resolve/override them
    FuncList combined;
    combined.reserve(stdlib_funcs.size() + funcs.size());
    for (auto &f : stdlib_funcs) {
        combined.push_back(std::move(f));
    }
    for (auto &f : funcs) {
        combined.push_back(std::move(f));
    }

    try {
        ScriptRuntime rt(combined, argc, argv);
        rt.run_top_level();
    } catch (const RuntimeError &err) {
        // runtime error occurred, at this point error details have already been
        // printed to stderr just rethrow to let caller decide what to do
        throw;
    }
#else
    // DISABLE_PARSER builds: tree-walking interpreter is unavailable.
    fprintf(stderr, "Error: tree-walk runtime unavailable in DISABLE_PARSER builds!\n");
#endif
}

} // namespace Runtime
