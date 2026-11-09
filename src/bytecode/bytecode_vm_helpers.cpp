// VM setup/registration/call helpers (split out of bytecode.cpp).
#include "ast.h"
#include "bytecode.h"
#include "compiler_support.h"
#include "debugger.h"
#include "gdb_bridge.h"
#include "int_overflow.h"
#include "parser_api.h"
#include "runtime.h"
#ifndef DISABLE_JIT
#include "jit_helpers.h"
#include "jit_tls.h"
#include "jit/runtime/session.h"
#endif
#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

// returns the number of nested FFI callback frames currently active on this thread.
extern "C" int ffi_reentry_depth();

namespace nari {
namespace bytecode {

// force a full precise mark-sweep using the complete root set (bytecode-VM roots via external_gc_roots_provider)
void VM::jit_safepoint() {
    if (GarbageCollector::instance().should_collect()) {
        gc_collect_roots();
    }
}

// cached object-property read used by the method-JIT LoadProperty helper.
// Mirrors the OP_GET_PROPERTY inline cache
uint32_t VM::field_id_for_name(uint16_t name_idx) {
    uint32_t &field_id = property_field_ids[name_idx];
    if (field_id == UINT32_MAX) {
        field_id = intern_field(chunk->strings[name_idx]);
    }
    return field_id;
}

uint32_t VM::getter_field_id_for_name(uint16_t name_idx) {
    uint32_t &field_id = getter_property_field_ids[name_idx];
    if (field_id == UINT32_MAX) {
        field_id = intern_field("__js_getter__" + chunk->strings[name_idx]);
    }
    return field_id;
}

uint32_t VM::setter_field_id_for_name(uint16_t name_idx) {
    uint32_t &field_id = setter_property_field_ids[name_idx];
    if (field_id == UINT32_MAX) {
        field_id = intern_field("__js_setter__" + chunk->strings[name_idx]);
    }
    return field_id;
}

Value VM::jit_lookup_object_property(ObjectObj *oobj, uint16_t name_idx) {
    auto &ic = prop_ic[(static_cast<unsigned>(name_idx)) & PROP_IC_MASK];
    if (!oobj->dict_mode && ic.shape == oobj->shape && ic.name_idx == name_idx && ic.slot < oobj->fields.size()) {
        return *oobj->materialize_lazy_field(ic.slot); // IC hit means no name/fid hashing
    }
    if (!oobj->dict_mode) {
        const uint32_t sit = oobj->shape->slot_of(field_id_for_name(name_idx));
        if (sit != ObjectShape::NoSlot) {
            ic.shape = oobj->shape;
            ic.name_idx = name_idx;
            ic.slot = sit;
            return *oobj->materialize_lazy_field(sit);
        }
        return Value::none();
    }
    Value *value = oobj->get_field_by_id(field_id_for_name(name_idx));
    return value ? *value : Value::none();
}

void VM::run_timer_loop() {
    while (runtime->has_pending_timers() && !has_error && !Runtime::g_shutdown_requested.load() &&
           !Runtime::g_runtime_error_occurred.load()) {
        runtime->process_completed_io(); // set_timeout completions
        runtime->fire_due_intervals();   // set_interval deadlines
        NARI_SLEEP_MILLIS(1);
    }
}

void VM::process_completed_io_for_jit() {
    runtime->process_completed_io();
}

void VM::ensure_static_fields_inited_for_jit(const std::string &class_name, const nari::ClassDecl *class_decl) {
    ensure_static_fields_inited(class_name, class_decl);
}

void VM::gc_collect_roots() {
    auto &gc = GarbageCollector::instance();
    if (!gc.is_enabled() || !runtime) {
        return;
    }
    auto roots = runtime->collect_gc_roots();
    // root the current 'this' receiver, which lives only as a raw ClassInstance* while a method body runs nested
    // statements.
    Value recv_root;
    if (current_instance) {
        recv_root = Value::from_class_instance(current_instance);
        roots.push_back(&recv_root);
    }
    gc.collect(roots);
}

Value VM::make_object_cached(const uint8_t *site, uint32_t size) {
    const size_t pairs_base = stack.size() - size * 2;

    // Fast path: compare keys in place, then move values directly from the VM stack.
    auto cit = make_object_shape_cache.find(site);
    if (cit != make_object_shape_cache.end()) {
        const ObjectShape *cached = cit->second;
        if (cached->field_ids.size() == size) {
            bool match = true;
            for (uint32_t i = 0; i < size; i++) {
                const Value &key = stack[pairs_base + i * 2];
                if (!key.is_string() || key.get_string() != cached->name_at(i)) {
                    match = false;
                    break;
                }
            }
            if (match) {
                Value obj_val = Value::make_object();
                ObjectObj *oobj = obj_val.get_obj_ptr();
                oobj->shape = cached;
                oobj->fields.resize(size);
                for (uint32_t i = 0; i < size; i++) {
                    oobj->fields[i] = std::move(stack[pairs_base + i * 2 + 1]);
                }
                oobj->shape_version = size;
                stack.resize(pairs_base);
                return obj_val;
            }
        }
    }

    // Slow path: materialize dynamic keys and establish the site's shape.
    std::vector<std::pair<std::string, Value>> pairs(size);
    for (int i = static_cast<int>(size) - 1; i >= 0; i--) {
        pairs[i].second = pop();
        pairs[i].first = pop().to_string();
    }
    Value obj_val = Value::make_object();
    ObjectObj *oobj = obj_val.get_obj_ptr();

    // slow path: build via set_field, then cache the resulting shape.
    if (size <= ObjectObj::kDictModeThreshold) {
        oobj->fields.reserve(size);
    }
    for (auto &[k, v] : pairs) {
        oobj->set_field(k, std::move(v));
    }
    if (!oobj->dict_mode && oobj->shape->field_ids.size() == size) {
        make_object_shape_cache[site] = oobj->shape;
    }
    return obj_val;
}

VM::~VM() {
#ifndef DISABLE_JIT
    jit_session.reset();
#endif
    // drop every VM-owned reference first so any still tracked
    // heap object is unreachable by roots then force a final sweep
    stack.clear();
    globals.clear();
    builtins.clear();
    frames.clear();
    try_stack.clear();
    global_cache.clear();
    global_cache_valid.clear();
    js_undefined_value = Value::none();
    GarbageCollector::instance().force_collect({});
}

void VM::set_global(const std::string &name, const Value &val) {
    globals[name] = val;
    if (name == "__js_undefined") {
        js_undefined_value = val;
    }
}

const Value &VM::js_this_cell() {
    if (NARI_LIKELY(js_this_cell_cached != nullptr)) {
        return *js_this_cell_cached;
    }
    auto it = globals.find("__js_this_cell");
    if (it == globals.end()) {
        return get_global("__js_this_cell");
    }
    js_this_cell_cached = &it->second;
    return *js_this_cell_cached;
}

const Value &VM::get_global(const std::string &name) {
    static const Value none_val;

    auto it = globals.find(name);
    if (it != globals.end()) {
        return it->second;
    }

    auto bit = builtins.find(name);
    if (bit != builtins.end()) {
        return bit->second;
    }

    if (Parser::get_registered_type(name)) {
        globals[name] = Value::make_string(name);
        return globals[name];
    }

    return none_val;
}

void VM::rebuild_global_cache() {
    if (!chunk) {
        return;
    }
    const size_t n = chunk->strings.size();
    global_cache.assign(n, Value{});
    global_cache_valid.assign(n, 0);
    js_get_prop_static_ic.assign(n, JsGetPropStaticIC{});
    method_ic_fn.assign(n, nullptr);
    method_ic_state.assign(n, 0);
    property_field_ids.assign(n, UINT32_MAX);
    getter_property_field_ids.assign(n, UINT32_MAX);
    setter_property_field_ids.assign(n, UINT32_MAX);
    method_field_ids.assign(n, UINT32_MAX);
    method_ic_shapes.assign(n, nullptr);
    method_ic_slots.assign(n, UINT32_MAX);
    method_ic_shapes2.assign(n, nullptr);
    method_ic_slots2.assign(n, UINT32_MAX);
    for (size_t i = 0; i < n; i++) {
        const std::string &name = chunk->strings[i];
        auto it = globals.find(name);
        if (it != globals.end()) {
            global_cache[i] = it->second;
            global_cache_valid[i] = 1;
            continue;
        }
        auto bit = builtins.find(name);
        if (bit != builtins.end()) {
            global_cache[i] = bit->second;
            global_cache_valid[i] = 1;
        }
    }
}

void VM::register_builtin(const std::string &name) {
    builtins[name] = Value::make_function(name);
    // pre-resolve the runtime builtin member-fn pointer so that JIT call
    // sites can skip the per-call hash lookups in builtins.find + runtime->call_builtin.
    if (runtime) {
        ScriptRuntime::BuiltinFn func = runtime->lookup_builtin_member(name);
        if (func) {
            auto &data = builtins[name].get_function();
            data.jit_builtin_id = ScriptRuntime::intern_jit_builtin(func);
        }
    }
}

void VM::ensure_static_fields_inited(const std::string &class_name, const nari::ClassDecl *class_decl) {
    auto &inited = Parser::get_static_inited_classes();
    if (inited.count(class_name)) {
        return;
    }
    inited.insert(class_name);
    auto &fields = Parser::get_static_fields();
    // seed every static field first: the compiled initializer only assigns the defaulted ones,
    // and a default expression may read another static field of the same class.
    for (const auto &field : class_decl->fields) {
        if (field.is_static) {
            fields[class_name + "." + field.name] = Value::none();
        }
    }

    auto it = chunk->class_static_init_idx.find(class_decl);
    if (it != chunk->class_static_init_idx.end()) {
        std::string saved_class = current_class_name;
        current_class_name = class_name;
        call_class_function_sync(it->second, nullptr, 0, nullptr);
        current_class_name = saved_class;
    }
}

Value VM::instantiate_class(const std::string &class_name, std::vector<Value> args) {
    const nari::ClassDecl *class_decl = Parser::get_registered_class(class_name);
    if (!class_decl) {
        fprintf(stderr, "bytecode: unknown class '%s'\n", class_name.c_str());
        return Value::none();
    }

    ClassInstance *instance = new ClassInstance(class_name);
    instance->type_tag = ValueTag::ClassInstance;
    GarbageCollector::instance().track(instance, GarbageCollector::TrackedType::ClassInstance);

    std::vector<const nari::ClassField *> all_fields;
    bc_collect_all_fields(class_decl, all_fields);

    // register class layout once (shared across all instances)
    auto &layout_reg = class_layout_registry();
    if (layout_reg.find(class_name) == layout_reg.end()) {
        ClassLayout layout;
        layout.names.reserve(all_fields.size());
        layout.index.reserve(all_fields.size());
        for (size_t i = 0; i < all_fields.size(); i++) {
            layout.names.push_back(all_fields[i]->name);
            layout.index[all_fields[i]->name] = i;
            if (all_fields[i]->visibility == nari::Visibility::Private) {
                layout.private_fields.insert(all_fields[i]->name);
            }
        }
        layout_reg.emplace(class_name, std::move(layout));
    }
    auto layout_it = layout_reg.find(class_name);
    if (layout_it == layout_reg.end()) {
        fprintf(stderr, "bytecode: missing layout for class '%s'\n", class_name.c_str());
        return Value::none();
    }
    instance->layout = &layout_it->second;

    // fields start empty; the compiled initializer assigns each declared default in MRO order
    instance->field_values.assign(all_fields.size(), Value::none());

    // arity warning parity with the previous AST constructor path
    const nari::ClassMethod *ctor = bc_find_method(class_decl, "init");
    if (ctor && ctor->is_constructor && args.size() != ctor->params.size()) {
        fprintf(stderr, "constructor '%s' expects %zu args but got %zu\n", class_name.c_str(), ctor->params.size(), args.size());
    }

    // run <Class>.__init__: field defaults, then a forward to the (possibly inherited) constructor
    auto init_it = chunk->class_init_idx.find(class_decl);
    if (init_it != chunk->class_init_idx.end()) {
        Value recv = Value::from_class_instance(instance);
        ClassInstancePtr saved_instance = current_instance;
        std::string saved_class = current_class_name;
        // keeps the half-built instance GC-rooted and lets defaults/ctor touch private fields
        current_instance = instance;
        current_class_name = class_name;
        call_class_function_sync(init_it->second, args.data(), args.size(), &recv);
        current_instance = saved_instance;
        current_class_name = saved_class;
    }

    return Value::from_class_instance(instance);
}

Value VM::call_class_method(ClassInstance *instance, const std::string &method_name, std::vector<Value> args) {
    const nari::ClassDecl *class_decl = Parser::get_registered_class(instance->class_name);
    if (!class_decl) {
        return bytecode_runtime_fatal("Unknown class '" + instance->class_name + "'!", "");
    }

    const nari::ClassMethod *method = bc_find_method(class_decl, method_name);
    if (!method) {
        return bytecode_runtime_fatal("Class '" + instance->class_name + "' has no method '" + method_name + "'!", "");
    }

    if (method->visibility == nari::Visibility::Private) {
        if (current_class_name != instance->class_name) {
            return bytecode_runtime_fatal("Cannot call private method '" + method_name + "' of " + "'" + instance->class_name + "'!", "");
        }
    }

    // bc_find_method may resolve to a parent's method; method_func_idx is keyed by the
    // ClassMethod pointer, so inherited dispatch lands on the parent's compiled function.
    auto it = chunk->method_func_idx.find(method);
    if (it == chunk->method_func_idx.end()) {
        return Value::none();
    }

    Value recv = Value::from_class_instance(instance);
    ClassInstancePtr saved_instance = current_instance;
    std::string saved_class = current_class_name;
    current_instance = instance;
    current_class_name = instance->class_name;

    Value return_value = call_class_function_sync(it->second, args.data(), args.size(), &recv);

    current_instance = saved_instance;
    current_class_name = saved_class;

    return return_value;
}

void VM::register_all_builtins() {
// any new builtin added to BUILTIN_FUNCTIONS or METHOD_ONLY_BUILTINS is automatically available
#define REGISTER_BUILTIN(bname, method) register_builtin(bname);
    BUILTIN_FUNCTIONS(REGISTER_BUILTIN)
    METHOD_ONLY_BUILTINS(REGISTER_BUILTIN)
#undef REGISTER_BUILTIN
    // also register any extension builtins added via ScriptRuntime::register_extension()
    for (auto &[name, _] : ScriptRuntime::get_extension_table()) {
        register_builtin(name);
    }
    // mark push as an inlineable native builtin (kind 1 = array_push)
    builtins["push"].get_function().jit_native_kind = 1;

    // TODO: I forgot what this is, still needed? "File I/O aliases" supposedly.
    register_builtin("read_file");
    register_builtin("write_file");
    register_builtin("append_file");
    register_builtin("delete_file");
    register_builtin("file_exists");
    register_builtin("list_dir");
}

Value VM::call_builtin(const std::string &name, Value *argv, size_t argc) {
    Value result = runtime->call_builtin(name, argv, argc, nullptr);
    if (NARI_UNLIKELY(Runtime::g_runtime_error_occurred.load())) {
        has_error = true;
        return Value::none();
    }
    return result;
}

Value VM::call_builtin_member(ScriptRuntime::BuiltinFn fn, Value *argv, size_t argc) {
    Value result = runtime->call_builtin_member(fn, argv, argc, nullptr);
    if (NARI_UNLIKELY(Runtime::g_runtime_error_occurred.load())) {
        has_error = true;
        return Value::none();
    }
    return result;
}

// push the result of a builtin call, but first check whether the builtin raised a script-catchable throw
bool VM::push_builtin_result(Value result) {
    if (has_error) {
        return false;
    }
    if (NARI_UNLIKELY(Runtime::g_runtime_error_occurred.load())) {
        has_error = true;
        return false;
    }
    if (runtime->flags.throw_flag) {
        Value err = runtime->flags.throw_value;
        runtime->flags.throw_flag = false;
        runtime->flags.throw_value = Value::none();
        if (capture_native_throw(err)) {
            if (overflow_jmp) {
                std::longjmp(*overflow_jmp, 4);
            }
            return false;
        }
        return dispatch_throw(err);
    }
    VM::push(std::move(result));
    return true;
}

void VM::note_jit_callee(uint32_t func_idx) {
#ifndef DISABLE_JIT
    // Once compiled, stop touching the call_counts hash map on every call, saves a map lookup
    if (jit_session.get() && !jit_session.get()->method_compiled(func_idx) && ++call_counts[func_idx] == JIT_THRESHOLD &&
        !jit_session->has_profitable_trace(func_idx)) {
        // jit_captures_raw currently points at func_idx's own closure captures
        // (see jit_call_value_impl), letting the compiler resolve LoadCapture callees.
        jit_session.get()->compile_method(*chunk, func_idx, this, true);
    }
#endif
}

void VM::call_user_function(
    uint32_t func_idx, const std::vector<Value> &args, const std::vector<Value> *captures, const CapturesList &cell_captures,
    const Value *receiver
) {
    if (frames.size() >= MAX_CALL_DEPTH) {
        fprintf(stderr, "Stack Overflow: maximum call-depth exceeded!\n");
        has_error = true;
        if (this->overflow_jmp) {
            std::longjmp(*this->overflow_jmp, 1);
        }
        return;
    }

    FunctionMeta *func = &chunk->functions[func_idx];

#ifndef DISABLE_JIT
    // track call count for JIT compilation
    if (jit_session.get() && !jit_session.get()->method_compiled(func_idx) && ++call_counts[func_idx] == JIT_THRESHOLD &&
        !jit_session->has_profitable_trace(func_idx)) {
        auto *saved_caps = jit_captures_raw;
        jit_captures_raw = cell_captures.get();
        jit_session.get()->compile_method(*chunk, func_idx, this, true);
        jit_captures_raw = saved_caps;
    }
#endif

    // save current stack position for restoration on return
    size_t slot_base = stack.size();

    // handle rest parameters: pack extra args into array at rest_param_index
    size_t param_count = func->param_count;
    size_t locals_needed = func->var_names.size();
    const Value missing_arg = func->js_undefined_params ? js_undefined_value : Value::none();

    // common case is that there are no rest params
    if (NARI_UNLIKELY(func->rest_param_index >= 0)) {
        size_t rest_idx = func->rest_param_index;
        for (size_t i = 0; i < locals_needed; i++) {
            if (i < rest_idx) {
                stack.push_back(i < args.size() ? args[i] : missing_arg);
            } else if (i == rest_idx) {
                std::vector<Value> rest_arr;
                for (size_t j = rest_idx; j < args.size(); j++) {
                    rest_arr.push_back(args[j]);
                }
                stack.push_back(Value::make_array(std::move(rest_arr)));
            } else {
                stack.push_back(Value::none());
            }
        }
    } else {
        // push args then fill remaining locals with none
        for (size_t i = 0; i < locals_needed; i++) {
            stack.push_back(i < args.size() ? args[i] : (i < param_count ? missing_arg : Value::none()));
        }
    }

    // push new call frame
    frames.emplace_back();
    CallFrame &frame = frames.back();
    frame.function = func;
    frame.ip = func->code.data();
    frame.slot_base = slot_base;
    frame.receiver = receiver ? *receiver : Value::none();
    // store captures reference on frame for OP_LOAD_CAPTURE/OP_STORE_CAPTURE
    if (cell_captures) {
        // pre-converted cell captures (from closure value), use directly
        frame.captures = cell_captures;
    } else if (captures && !captures->empty()) {
        // convert raw Value vector to cell vector
        auto cells = std::make_shared<std::vector<CellRef>>();
        cells->reserve(captures->size());
        for (const auto &v : *captures) {
            cells->push_back(CellRef::make(v));
        }
        frame.captures = cells;
    }

    if (NARI_UNLIKELY(gdb_nari_active)) {
        gdb_nari_note_function(*this, func_idx);
    }

#ifndef DISABLE_JIT
    // TODO: Skip JIT when we're being driven by an FFI callback.
    // JIT-compiled frames leak memory under reentry on at least Wine/x86_64 (see src/ffi.cpp).
    // The bytecode interpreter handles reentry correctly.
    bool jit_safe = ::ffi_reentry_depth() == 0;
    // try JIT execution after frame is fully set up
    if (jit_safe && jit_session.get() && jit_session.get()->method_compiled(func_idx)) {
        auto compiled = jit_session.get()->method_entry(func_idx);
        if (compiled) {
            // generated returns only move frames.storage_end, so keep ownership outside the frame while compiled code
            // runs.
            CapturesList compiled_captures = std::move(frame.captures);
            auto *prev_captures = jit_captures_raw;
            set_jit_captures_raw(compiled_captures.get());
            // JIT function executes the body and handles OP_RETURN
            compiled(this);
            set_jit_captures_raw(prev_captures);
            return;
        }
    }
#endif
}

// no alloc version of call_user_function
void VM::call_user_function_stack(
    uint32_t func_idx, size_t args_base, size_t argc, const CapturesList &cell_captures, const Value *receiver
) {
    if (frames.size() >= MAX_CALL_DEPTH) {
        fprintf(stderr, "Stack Overflow: maximum call-depth exceeded!\n");
        has_error = true;
        if (this->overflow_jmp) {
            std::longjmp(*this->overflow_jmp, 1);
        }
        return;
    }

    FunctionMeta *func = &chunk->functions[func_idx];

#ifndef DISABLE_JIT
    auto *session = jit_session.get();
    auto compiled = session ? session->method_entry(func_idx) : nullptr;
    if (session && !compiled && ++call_counts[func_idx] == JIT_THRESHOLD && !session->has_profitable_trace(func_idx)) {
        auto *saved_caps = jit_captures_raw;
        jit_captures_raw = cell_captures.get();
        session->compile_method(*chunk, func_idx, this, true);
        jit_captures_raw = saved_caps;
        compiled = session->method_entry(func_idx);
    }
#endif

    size_t locals_needed = func->var_names.size();
    const size_t param_count = func->param_count;
    const Value missing_arg = func->js_undefined_params ? js_undefined_value : Value::none();

    // slot_base: new frame's locals start right after the current stack top.
    // func_val is at args_base-1; args were args_base..args_base+argc-1
    size_t slot_base = args_base - 1;

    if (func->rest_param_index >= 0) {
        // Rest packing still needs the original arguments after the caller's
        // stack is trimmed, so preserve them outside stack storage.
        Value arg_buf[16];
        std::vector<Value> arg_vec;
        const Value *args_ptr;
        if (argc <= 16) {
            for (size_t i = 0; i < argc; i++) {
                arg_buf[i] = std::move(stack[args_base + i]);
            }
            args_ptr = arg_buf;
        } else {
            arg_vec.assign(std::make_move_iterator(stack.begin() + args_base), std::make_move_iterator(stack.begin() + args_base + argc));
            args_ptr = arg_vec.data();
        }
        stack.resize(slot_base);

        size_t rest_idx = func->rest_param_index;
        for (size_t i = 0; i < locals_needed; i++) {
            if (i < rest_idx) {
                stack.push_back(i < argc ? args_ptr[i] : missing_arg);
            } else if (i == rest_idx) {
                std::vector<Value> rest_arr;
                for (size_t j = rest_idx; j < argc; j++) {
                    rest_arr.push_back(args_ptr[j]);
                }
                stack.push_back(Value::make_array(std::move(rest_arr)));
            } else {
                stack.push_back(Value::none());
            }
        }
    } else {
        // Shift arguments over the function slot in place. The destination
        // precedes the source, so the one-slot overlap is safe going forward.
        const size_t arg_fill = std::min(argc, locals_needed);
        for (size_t i = 0; i < arg_fill; i++) {
            stack[slot_base + i] = std::move(stack[args_base + i]);
        }
        stack.resize(slot_base + locals_needed);
        // resize() initializes newly grown slots. Only the old top slot after
        // the shifted arguments can still contain a stale argument value.
        if (arg_fill < locals_needed) {
            stack[slot_base + arg_fill] = arg_fill < param_count ? missing_arg : Value::none();
            for (size_t i = arg_fill + 1; i < std::min(param_count, locals_needed); i++) {
                stack[slot_base + i] = missing_arg;
            }
        }
    }

    frames.emplace_back();
    CallFrame &frame = frames.back();
    frame.function = func;
    frame.ip = func->code.data();
    frame.slot_base = slot_base;
    frame.receiver = receiver ? *receiver : Value::none();
    if (cell_captures) {
        frame.captures = cell_captures;
    }

    if (NARI_UNLIKELY(gdb_nari_active)) {
        gdb_nari_note_function(*this, func_idx);
    }

#ifndef DISABLE_JIT
    if (compiled && ::ffi_reentry_depth() == 0) {
        CapturesList compiled_captures = std::move(frame.captures);
        auto *prev_captures = jit_captures_raw;
        set_jit_captures_raw(compiled_captures.get());
        compiled(this);
        set_jit_captures_raw(prev_captures);
        return;
    }
#endif
}

// Span variant of call_user_function for runtime re-entry, this avoids creating a Vector<Value>.
void VM::call_user_function_span(
    uint32_t func_idx, const Value *args, size_t argc, const CapturesList &cell_captures, const Value *receiver
) {
    if (frames.size() >= MAX_CALL_DEPTH) {
        fprintf(stderr, "Stack Overflow: maximum call-depth exceeded!\n");
        has_error = true;
        if (this->overflow_jmp) {
            std::longjmp(*this->overflow_jmp, 1);
        }
        return;
    }

    FunctionMeta *func = &chunk->functions[func_idx];

#ifndef DISABLE_JIT
    // single virtual lookup instead of the is_compiled + get_compiled pair
    auto compiled = jit_session.get() ? jit_session.get()->method_entry(func_idx) : nullptr;
    if (jit_session.get() && !compiled && ++call_counts[func_idx] == JIT_THRESHOLD && !jit_session->has_profitable_trace(func_idx)) {
        auto *saved_caps = jit_captures_raw;
        jit_captures_raw = cell_captures.get();
        jit_session.get()->compile_method(*chunk, func_idx, this, true);
        jit_captures_raw = saved_caps;
        compiled = jit_session.get()->method_entry(func_idx);
    }
#endif

    size_t slot_base = stack.size();
    size_t locals_needed = func->var_names.size();
    const size_t param_count = func->param_count;
    const Value missing_arg = func->js_undefined_params ? js_undefined_value : Value::none();

    if (NARI_UNLIKELY(func->rest_param_index >= 0)) {
        // pack extra args into the rest array (same as call_user_function)
        size_t rest_idx = func->rest_param_index;
        std::vector<Value> argv(args, args + argc);
        for (size_t i = 0; i < locals_needed; i++) {
            if (i < rest_idx) {
                stack.push_back(i < argv.size() ? argv[i] : missing_arg);
            } else if (i == rest_idx) {
                std::vector<Value> rest_arr;
                for (size_t j = rest_idx; j < argv.size(); j++) {
                    rest_arr.push_back(argv[j]);
                }
                stack.push_back(Value::make_array(std::move(rest_arr)));
            } else {
                stack.push_back(Value::none());
            }
        }
    } else {
        size_t need = slot_base + locals_needed;
        Value arg_buf[8];
        std::vector<Value> arg_vec;
        if (NARI_UNLIKELY(need > stack.capacity())) {
            // growing would invalidate a span that points into the stack
            if (argc <= 8) {
                for (size_t i = 0; i < argc; i++) {
                    arg_buf[i] = args[i];
                }
                args = arg_buf;
            } else {
                arg_vec.assign(args, args + argc);
                args = arg_vec.data();
            }
            stack.reserve(std::max(need, stack.capacity() * 2));
        }
        // assign through storage_end exactly like push_back, minus the per-push check
        Value *dst = stack.storage_end;
        size_t arg_fill = argc < locals_needed ? argc : locals_needed;
        for (size_t i = 0; i < arg_fill; i++) {
            dst[i] = args[i];
        }
        for (size_t i = arg_fill; i < locals_needed; i++) {
            dst[i] = i < param_count ? missing_arg : Value::none();
        }
        stack.storage_end = dst + locals_needed;
    }

    frames.emplace_back();
    CallFrame &frame = frames.back();
    frame.function = func;
    frame.ip = func->code.data();
    frame.slot_base = slot_base;
    frame.receiver = receiver ? *receiver : Value::none();
    if (cell_captures && !cell_captures->empty()) {
        frame.captures = cell_captures;
    }

    if (NARI_UNLIKELY(gdb_nari_active)) {
        gdb_nari_note_function(*this, func_idx);
    }

#ifndef DISABLE_JIT
    if (compiled && ::ffi_reentry_depth() == 0) {
        CapturesList compiled_captures = std::move(frame.captures);
        auto *prev_captures = jit_captures_raw;
        set_jit_captures_raw(compiled_captures.get());
        compiled(this);
        set_jit_captures_raw(prev_captures);
        return;
    }
#endif
}

} // namespace bytecode
} // namespace nari
