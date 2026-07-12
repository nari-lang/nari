#include "bytecode.h"
#include "ast.h"
#include "compiler_support.h"
#include "debugger.h"
#include "int_overflow.h"
#include "parser_api.h"
#include "runtime.h"
#ifndef DISABLE_JIT
#include "jit_helpers.h"
#include "jit_tls.h"
#include "trace_jit.h"
#endif
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

namespace {

// extract an int64_t from a Value for the bitwise/shift ops
// ints pass through directly, and floats route through safe_double_to_i64
static inline int64_t value_as_int_safe(const Value &v) noexcept {
    return v.is_int() ? v.get_int() : safe_double_to_i64(v.as_number());
}

struct ScopedSyntheticDebugFrame {
    dbg::DebugController *controller = nullptr;

    ScopedSyntheticDebugFrame(const std::string &name, const std::string &source_file, int line,
                              size_t runtime_call_stack_index = static_cast<size_t>(-1),
                              Value this_value = Value::none()) {
        auto &dc = dbg::DebugController::instance();
        if (dc.enabled()) {
            controller = &dc;
            controller->push_synthetic_frame(name, source_file, line,
                                             runtime_call_stack_index,
                                             std::move(this_value));
        }
    }

    ~ScopedSyntheticDebugFrame() {
        if (controller) {
            controller->pop_synthetic_frame();
        }
    }
};

void maybe_stop_in_ast_body(VM &vm, const Stmt *stmt) {
    if (!stmt) {
        return;
    }

    auto &dc = dbg::DebugController::instance();
    if (!dc.enabled()) {
        return;
    }

    const int line = stmt->line;
    const std::string file = dbg::canonicalise_path(stmt->filename);
    const bool pending_entry_stop = dc.pending_entry_stop();
    dc.update_synthetic_frame_line(line);
    const size_t depth = vm.frames.size() + dc.synthetic_frame_depth();

    if (!dc.should_stop(vm, 0, depth, line, file)) {
        return;
    }

    dbg::StopReason reason;
    if (!dc.has_fired_first_stop() && pending_entry_stop) {
        reason = dbg::StopReason::Entry;
    } else if (dc.has_breakpoint(file, line)) {
        reason = dbg::StopReason::Breakpoint;
    } else {
        reason = dbg::StopReason::Step;
    }
    dc.mark_first_stop_fired();
    dc.publish_stop_and_wait(dc.snapshot_frames(vm, reason));
}

} // namespace

static const nari::ClassField *bc_find_field(const nari::ClassDecl *class_decl, const std::string &field_name) {
    if (!class_decl) {
        return nullptr;
    }
    for (const auto &field : class_decl->fields) {
        if (field.name == field_name) {
            return &field;
        }
    }
    if (!class_decl->parent_name.empty()) {
        const nari::ClassDecl *parent =
            Parser::get_registered_class(class_decl->parent_name);
        if (parent) {
            return bc_find_field(parent, field_name);
        }
    }
    return nullptr;
}

VM::VM(int argc, char **argv) : chunk(nullptr) {
#ifdef NARI_MCU
    // smaller reserve given less memory on MCUs
    stack.reserve(256);
    frames.reserve(16);
#else
    stack.reserve(1024);
    frames.reserve(128);
#endif

    FuncList empty_funcs;
    runtime = std::make_unique<ScriptRuntime>(empty_funcs, argc, argv);
    runtime->debug_stmt_hook = [this](const Stmt *stmt) {
        maybe_stop_in_ast_body(*this, stmt);
    };

    // register GC roots
    runtime->external_gc_roots_provider = [&](std::vector<const Value *> &roots) {
        for (const auto &[key, val] : globals) {
            roots.push_back(&val);
        }
        for (const auto &val : stack) {
            roots.push_back(&val);
        }
        for (const auto &[key, val] : builtins) {
            roots.push_back(&val);
        }
        if (chunk) {
            for (size_t i = 0; i < chunk->const_string_cache.size() && i < chunk->const_string_valid.size(); i++) {
                if (chunk->const_string_valid[i]) {
                    roots.push_back(&chunk->const_string_cache[i]);
                }
            }
        }
        // prevent cycle-collector from freeing objects the cache points to
        for (size_t i = 0; i < global_cache.size(); i++) {
            if (global_cache_valid[i]) {
                roots.push_back(&global_cache[i]);
            }
        }
        // root captures and open upvalues in all active call frames
        for (const auto &frame : frames) {
            if (frame.captures) {
                for (const auto &cell : *frame.captures) {
                    if (cell) {
                        roots.push_back(cell.get());
                    }
                }
            }
            if (frame.open_upvalues) {
                for (const auto &[idx, cell] : *frame.open_upvalues) {
                    if (cell) {
                        roots.push_back(cell.get());
                    }
                }
            }
        }
        // C++ temporaries held across nested execution
        for (const Value *r : gc_temp_roots) {
            if (r) {
                roots.push_back(r);
            }
        }
    };

    register_all_builtins();

    gc_stress = (getenv("NARI_GC_STRESS") != nullptr);
    // The precise sweep is the only reclaimer, so safe-points must run during
    // execution or memory grows unbounded. On by default; NARI_GC_NO_SAFEPOINT
    // can disable for debugging.
    gc_safepoints = (getenv("NARI_GC_NO_SAFEPOINT") == nullptr);

#ifdef NARI_MCU
    // collect more aggressively to keep heap pressure low
    GarbageCollector::instance().set_collection_threshold(100);
#endif
    // Tunable collection cadence for A/B testing (allocations between collections).
    if (const char *t = getenv("NARI_GC_THRESHOLD")) {
        long v = atol(t);
        if (v > 0) {
            GarbageCollector::instance().set_collection_threshold((size_t)v);
        }
    }
}

// force a full precise mark-sweep using the complete root set (bytecode-VM roots via external_gc_roots_provider)
void VM::jit_safepoint() {
    if (NARI_UNLIKELY(gc_safepoints) &&
        GarbageCollector::instance().should_collect()) {
        gc_collect_roots();
    }
}

// cached object-property read used by the method-JIT LoadProperty helper.
// Mirrors the OP_GET_PROPERTY inline cache
Value VM::jit_lookup_object_property(ObjectObj *oobj, uint16_t name_idx) {
    auto &ic = prop_ic[((unsigned)name_idx) & PROP_IC_MASK];
    if (!oobj->dict_mode && ic.shape == oobj->shape &&
        ic.name_idx == name_idx && ic.slot < oobj->fields.size()) {
        return oobj->fields[ic.slot]; // IC hit means no name/fid hashing
    }
    const std::string &name = chunk->strings[name_idx];
    Value *val = oobj->get_field(name);
    if (val) {
        if (!oobj->dict_mode) {
            auto sit = oobj->shape->index.find(intern_field(name));
            if (sit != oobj->shape->index.end()) {
                ic.shape = oobj->shape;
                ic.name_idx = name_idx;
                ic.slot = sit->second;
            }
        }
        return *val;
    }
    return Value::none();
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
    // root the current 'this' receiver, which lives only as a raw ClassInstance* while a method body runs nested statements.
    Value recv_root;
    if (current_instance) {
        recv_root = Value::from_class_instance(current_instance);
        roots.push_back(&recv_root);
    }
    gc.collect(roots);
}

Value VM::make_object_cached(const uint8_t *site, uint32_t size) {
    // collect pairs in insertion order
    std::vector<std::pair<std::string, Value>> pairs(size);
    for (int i = (int)size - 1; i >= 0; i--) {
        pairs[i].second = pop();
        pairs[i].first = pop().to_string();
    }
    Value obj_val = Value::make_object();
    ObjectObj *oobj = obj_val.get_obj_ptr();

    // Fast path: reuse this site's cached shape when the keys still match, filling fields by slot directly
    auto cit = make_object_shape_cache.find(site);
    if (cit != make_object_shape_cache.end()) {
        const ObjectShape *cached = cit->second;
        if (cached->names.size() == size) {
            bool match = true;
            for (uint32_t i = 0; i < size; i++) {
                if (pairs[i].first != cached->names[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                oobj->shape = cached;
                oobj->fields.resize(size);
                for (uint32_t i = 0; i < size; i++) {
                    oobj->fields[i] = std::move(pairs[i].second);
                }
                oobj->shape_version = size;
                return obj_val;
            }
        }
    }

    // slow path: build via set_field, then cache the resulting shape.
    if (size <= ObjectObj::kDictModeThreshold) {
        oobj->fields.reserve(size);
    }
    for (auto &[k, v] : pairs) {
        oobj->set_field(k, std::move(v));
    }
    if (!oobj->dict_mode && oobj->shape->names.size() == size) {
        make_object_shape_cache[site] = oobj->shape;
    }
    return obj_val;
}

VM::~VM() {
    // drop every VM-owned reference first so any still tracked
    // heap object is unreachable by roots then force a final sweep
    stack.clear();
    globals.clear();
    builtins.clear();
    frames.clear();
    try_stack.clear();
    global_cache.clear();
    global_cache_valid.clear();
    GarbageCollector::instance().force_collect({});
}

void VM::set_global(const std::string &name, const Value &val) {
    globals[name] = val;
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
    method_ic_fn.assign(n, nullptr);
    method_ic_state.assign(n, 0);
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
            // member-fn pointers are 16 bytes on Itanium ABI but 8 on MSVC x64. 
            // reader (jit_call_value) memcpys the same sizeof back out.
            static_assert(sizeof(func) <= sizeof(data.jit_builtin_fn), "pointer-to-member too large for jit_builtin_fn");
            std::memcpy(data.jit_builtin_fn, &func, sizeof(func));
            data.jit_builtin_fn_valid = true;
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
    for (const auto &field : class_decl->fields) {
        if (!field.is_static) {
            continue;
        }
        std::string key = class_name + "." + field.name;
        if (field.default_value) {
            fields[key] = runtime->eval_expr(field.default_value.get());
        } else {
            fields[key] = Value::none();
        }
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
            layout.index[all_fields[i]->name] = static_cast<uint32_t>(i);
            if (all_fields[i]->visibility == nari::Visibility::Private) {
                layout.private_fields.insert(all_fields[i]->name);
            }
        }
        layout_reg.emplace(class_name, std::move(layout));
    }
    instance->layout = &layout_reg.at(class_name);

    // initialize fields in flat vector (indexed by ClassLayout slot)
    instance->field_values.resize(all_fields.size());
    for (size_t i = 0; i < all_fields.size(); i++) {
        if (all_fields[i]->default_value) {
            instance->field_values[i] = runtime->eval_expr(all_fields[i]->default_value.get());
        } else {
            instance->field_values[i] = Value::none();
        }
    }

    // find and call constructor if it exists
    const nari::ClassMethod *ctor = bc_find_method(class_decl, "init");
    if (ctor && ctor->is_constructor) {
        if (args.size() != ctor->params.size()) {
            fprintf(stderr, "constructor '%s' expects %zu args but got %zu\n", class_name.c_str(), ctor->params.size(), args.size());
        }

        ClassInstancePtr saved_instance = current_instance;
        std::string saved_class = current_class_name;
        current_instance = instance;
        current_class_name = class_name;

        if (ctor->body) {
            runtime->current_instance = instance;
            runtime->current_class_name = class_name;

            runtime->call_stack.emplace_back();
            for (size_t i = 0; i < ctor->params.size() && i < args.size(); i++) {
                runtime->call_stack.back()[ctor->params[i].name] = args[i];
            }

            ScopedSyntheticDebugFrame debug_frame(
                class_name + ".init",
                ctor->filename.empty() ? class_decl->filename : ctor->filename,
                ctor->line,
                runtime->call_stack.size() - 1,
                Value::from_class_instance(instance));

            for (const auto &stmt : ctor->body->stmts) {
                runtime->exec_stmt(stmt.get());
                if (runtime->flags.any_flag()) {
                    break;
                }
            }

            runtime->call_stack.pop_back();
            if (runtime->flags.return_flag) {
                runtime->flags.return_flag = false;
            }
            runtime->current_instance = saved_instance;
            runtime->current_class_name = saved_class;
        }

        current_instance = saved_instance;
        current_class_name = saved_class;
    }

    return Value::from_class_instance(instance);
}

Value VM::call_class_method(ClassInstance *instance, const std::string &method_name, std::vector<Value> args) {
    const nari::ClassDecl *class_decl = Parser::get_registered_class(instance->class_name);
    if (!class_decl) {
        bytecode_runtime_fatal("Unknown class '" + instance->class_name + "'!", "");
    }

    const nari::ClassMethod *method = bc_find_method(class_decl, method_name);
    if (!method) {
        bytecode_runtime_fatal("Class '" + instance->class_name + "' has no method '" + method_name + "'!", "");
    }

    if (method->visibility == nari::Visibility::Private) {
        if (current_class_name != instance->class_name) {
            bytecode_runtime_fatal("Cannot call private method '" + method_name + "' of " + "'" + instance->class_name + "'!", "");
        }
    }

    ClassInstancePtr saved_instance = current_instance;
    std::string saved_class = current_class_name;
    current_instance = instance;
    current_class_name = instance->class_name;

    runtime->current_instance = instance;
    runtime->current_class_name = instance->class_name;

    runtime->call_stack.emplace_back();
    for (size_t i = 0; i < method->params.size() && i < args.size(); i++) {
        runtime->call_stack.back()[method->params[i].name] = args[i];
    }

    Value return_value = Value::none();
    if (method->body) {
        ScopedSyntheticDebugFrame debug_frame(
            instance->class_name + "." + method_name,
            method->filename.empty() ? class_decl->filename : method->filename,
            method->line,
            runtime->call_stack.size() - 1,
            Value::from_class_instance(instance));
        for (const auto &stmt : method->body->stmts) {
            runtime->exec_stmt(stmt.get());
            if (runtime->flags.return_flag) {
                return_value = runtime->flags.return_value;
                runtime->flags.return_flag = false;
                break;
            }
            if (runtime->flags.break_flag || runtime->flags.continue_flag ||
                runtime->flags.throw_flag) {
                break;
            }
        }
    }

    runtime->call_stack.pop_back();
    runtime->current_instance = saved_instance;
    runtime->current_class_name = saved_class;
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

    // TODO: I forgot what this is, still needed?
    //  "File I/O aliases" supposedly.
    register_builtin("read_file");
    register_builtin("write_file");
    register_builtin("append_file");
    register_builtin("delete_file");
    register_builtin("file_exists");
    register_builtin("list_dir");
}

Value VM::call_builtin(const std::string &name, const std::vector<Value> &args) {
    Value result = runtime->call_builtin(name, args, nullptr);
    return result;
}

Value VM::call_builtin(const std::string &name, const Value *argv, size_t argc) {
    return runtime->call_builtin(name, argv, argc, nullptr);
}

// push the result of a builtin call, but first check whether the builtin raised a script-catchable throw
bool VM::push_builtin_result(Value result) {
    if (runtime->flags.throw_flag) {
        Value err = runtime->flags.throw_value;
        runtime->flags.throw_flag = false;
        runtime->flags.throw_value = Value::none();
        return dispatch_throw(err);
    }
    VM::push(std::move(result));
    return true;
}

bool VM::has_profitable_trace(uint32_t func_idx) const {
#ifndef DISABLE_JIT
    // Tunable: iterations/entry to call a trace profitable, and minimum sampled
    // entries to trust the average. A long single loop runs thousands+ iters per
    // entry; a short re-entered loop runs tens.
    static const uint64_t kMinAvgIters = 1000;
    static const uint32_t kMinEntries = 4;
    for (const auto &kv : trace_iter_stats_) {
        if ((uint32_t)(kv.first >> 32) == func_idx && kv.second.second >= kMinEntries &&
            (kv.second.first / kv.second.second) >= kMinAvgIters) {
            return true;
        }
    }
#else
    (void)func_idx;
#endif
    return false;
}

void VM::note_jit_callee(uint32_t func_idx) {
#ifndef DISABLE_JIT
    // Once compiled, stop touching the call_counts hash map on every call, saves a map lookup
    if (jit::g_jit_compiler && !jit::g_jit_compiler->is_compiled(func_idx) &&
        ++call_counts[func_idx] == JIT_THRESHOLD &&
        !has_profitable_trace(func_idx)) {
        jit::g_jit_compiler->compile_chunk(*chunk, func_idx);
    }
#endif
}

void VM::call_user_function(uint32_t func_idx, const std::vector<Value> &args, const std::vector<Value> *captures, CapturesList cell_captures) {
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
    if (jit::g_jit_compiler && !jit::g_jit_compiler->is_compiled(func_idx) &&
        ++call_counts[func_idx] == JIT_THRESHOLD &&
        !has_profitable_trace(func_idx)) {
        jit::g_jit_compiler->compile_chunk(*chunk, func_idx);
    }
#endif

    // save current stack position for restoration on return
    size_t slot_base = stack.size();

    // handle rest parameters: pack extra args into array at rest_param_index
    size_t param_count = func->param_count;
    size_t locals_needed = func->var_names.size();

    // common case is that there are no rest params
    if (NARI_UNLIKELY(func->rest_param_index >= 0)) {
        size_t rest_idx = static_cast<size_t>(func->rest_param_index);
        for (size_t i = 0; i < locals_needed; i++) {
            if (i < rest_idx) {
                stack.push_back(i < args.size() ? args[i] : Value::none());
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
            stack.push_back(i < args.size() ? args[i] : Value::none());
        }
    }

    // push new call frame
    frames.emplace_back();
    CallFrame &frame = frames.back();
    frame.function = func;
    frame.ip = func->code.data();
    frame.slot_base = slot_base;
    // store captures reference on frame for OP_LOAD_CAPTURE/OP_STORE_CAPTURE
    if (cell_captures) {
        // pre-converted cell captures (from closure value), use directly
        frame.captures = cell_captures;
    } else if (captures && !captures->empty()) {
        // convert raw Value vector to cell vector
        auto cells = std::make_shared<std::vector<std::shared_ptr<Value>>>();
        cells->reserve(captures->size());
        for (const auto &v : *captures) {
            cells->push_back(std::make_shared<Value>(v));
        }
        frame.captures = cells;
    }

#ifndef DISABLE_JIT
    // TODO: Skip JIT when we're being driven by an FFI callback.
    // JIT-compiled frames leak memory under reentry on at least Wine/x86_64 (see src/ffi.cpp).
    // The bytecode interpreter handles reentry correctly.
    bool jit_safe = ::ffi_reentry_depth() == 0;
    // try JIT execution after frame is fully set up
    if (jit_safe && jit::g_jit_compiler && jit::g_jit_compiler->is_compiled(func_idx)) {
        auto compiled = jit::g_jit_compiler->get_compiled(func_idx);
        if (compiled) {
            // A JIT-compiled closure reads its captures from jit_captures_raw (frame.captures is only read by the interpreter).
            // Set it as the borrowed raw ptr for the callee, save/restore for correct nesting.
            auto *prev_captures = jit_captures_raw;
            jit_captures_raw = frame.captures.get();
            // JIT function executes the body and handles OP_RETURN
            compiled(this);
            jit_captures_raw = prev_captures;
            return;
        }
    }
#endif
    // execution continues in the main loop (interpreter dispatch)
}

// Allocation-free variant: stack[args_base..args_base+argc] are the args on entry.
// Pops args + func (at args_base-1), then sets up the new call frame.
void VM::call_user_function_stack(uint32_t func_idx, size_t args_base, size_t argc, CapturesList cell_captures) {
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
    if (jit::g_jit_compiler && !jit::g_jit_compiler->is_compiled(func_idx) &&
        ++call_counts[func_idx] == JIT_THRESHOLD &&
        !has_profitable_trace(func_idx)) {
        jit::g_jit_compiler->compile_chunk(*chunk, func_idx);
    }
#endif

    size_t locals_needed = func->var_names.size();

    // copy args out of the stack into a small fixed buffer (attempt to avoid slow heap for common small calls)
    Value arg_buf[16];
    std::vector<Value> arg_vec;
    const Value *args_ptr;
    if (argc <= 16) {
        for (size_t i = 0; i < argc; i++) {
            arg_buf[i] = std::move(stack[args_base + i]);
        }
        args_ptr = arg_buf;
    } else {
        arg_vec.assign(std::make_move_iterator(stack.begin() + args_base),
                       std::make_move_iterator(stack.begin() + args_base + argc));
        args_ptr = arg_vec.data();
    }

    // slot_base: new frame's locals start right after the current stack top.
    // func_val is at args_base-1; args were args_base..args_base+argc-1
    size_t slot_base = args_base - 1;
    stack.resize(slot_base);

    if (func->rest_param_index >= 0) {
        size_t rest_idx = static_cast<size_t>(func->rest_param_index);
        for (size_t i = 0; i < locals_needed; i++) {
            if (i < rest_idx) {
                stack.push_back(i < argc ? args_ptr[i] : Value::none());
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
        for (size_t i = 0; i < locals_needed; i++) {
            stack.push_back(i < argc ? args_ptr[i] : Value::none());
        }
    }

    frames.emplace_back();
    CallFrame &frame = frames.back();
    frame.function = func;
    frame.ip = func->code.data();
    frame.slot_base = slot_base;
    if (cell_captures) {
        frame.captures = cell_captures;
    }

#ifndef DISABLE_JIT
    bool jit_safe2 = ::ffi_reentry_depth() == 0;
    if (jit_safe2 && jit::g_jit_compiler && jit::g_jit_compiler->is_compiled(func_idx)) {
        auto compiled = jit::g_jit_compiler->get_compiled(func_idx);
        if (compiled) {
            // Borrow captures for the compiled callee via jit_captures_raw (read by JIT's OP_LOAD_CAPTURE lowering),
            // null when not a closure.
            auto *prev_captures = jit_captures_raw;
            jit_captures_raw = frame.captures.get();
            compiled(this);
            jit_captures_raw = prev_captures;
            return;
        }
    }
#endif
}

bool VM::execute_instruction() {
    // GC-stress validation safe-point: a full precise collection at every
    // instruction boundary (where all live Values are in scanned roots), no-op unless NARI_GC_STRESS is set.
    if (NARI_UNLIKELY(gc_stress)) {
        gc_collect_roots();
    } else if (NARI_UNLIKELY(gc_safepoints) && GarbageCollector::instance().should_collect()) {
        // collect only when the GC has flagged that enough has been allocated.
        // at an instruction boundary the operand stack is at a clean height
        gc_collect_roots();
    }
#ifndef DISABLE_JIT
    // save pointer to start of instruction (before any reads) and its PC offset.
    uint8_t *const insn_base_ptr = ip();
    uint8_t *const code_data_ptr = current_function()->code.data();
    const size_t insn_pc = (size_t)(insn_base_ptr - code_data_ptr);

    // flip from pending -> active if the target PC has been reached.
    if (trace_recorder.pending_record && insn_pc == trace_recorder.target_pc) {
        trace_recorder.pending_record = false;
        trace_recorder.recording = true;
        trace_recorder.type_vstack.clear();
        trace_recorder.steps.clear();
    }
    const bool trace_was_recording = trace_recorder.recording;
#endif

    // Debugger hook
    {
        auto &dc = dbg::DebugController::instance();
        if (dc.enabled()) {
            uint8_t *const dbg_ip = ip();
            FunctionMeta *const dbg_fn = current_function();
            const size_t dbg_pc = static_cast<size_t>(dbg_ip - dbg_fn->code.data());
            const int dbg_line = dbg_fn->resolve_line(dbg_pc);
            const std::string dbg_file = dbg::canonicalise_path(dbg_fn->source_file);
            const bool pending_entry_stop = dc.pending_entry_stop();
            if (dc.should_stop(*this, dbg_pc, frames.size(), dbg_line, dbg_file)) {
                dbg::StopReason reason;
                if (!dc.has_fired_first_stop() && pending_entry_stop) {
                    reason = dbg::StopReason::Entry;
                } else if (dc.has_breakpoint(dbg_file, dbg_line)) {
                    reason = dbg::StopReason::Breakpoint;
                } else {
                    reason = dbg::StopReason::Step;
                }
                dc.mark_first_stop_fired();
                dc.publish_stop_and_wait(dc.snapshot_frames(*this, reason));
            }
        }
    }

    OpCode op = static_cast<OpCode>(read_byte());

    switch (op) {
        // stack operations
        case OpCode::OP_LOAD_CONST: {
            uint16_t idx = read_short();
            Constant &c = current_function()->constants[idx];

            switch (c.type) {
                case Constant::Type::NONE:
                    VM::push(Value::none());
                    break;
                case Constant::Type::INT:
                    VM::push(Value::make_int(c.as_int));
                    break;
                case Constant::Type::FLOAT:
                    VM::push(Value::make_float(c.as_float));
                    break;
                case Constant::Type::STRING:
                    // shared immutable constant -- no per-load alloc/copy
                    VM::push(chunk->get_const_string(c.string_idx));
                    break;
                case Constant::Type::FUNCTION:
                    // TODO: create function value
                    VM::push(Value::none());
                    break;
            }
            break;
        }

        case OpCode::OP_LOAD_VAR: {
            uint16_t idx = read_short();
            // Check for open upvalue cell first (closure-shared variable)
            auto &upvals = current_frame().open_upvalues;
            if (upvals) {
                auto it = upvals->find(idx);
                if (it != upvals->end()) {
                    VM::push(*it->second);
                    break;
                }
            }
            VM::push(stack[current_frame().slot_base + idx]);
            break;
        }

        case OpCode::OP_STORE_VAR: {
            uint16_t idx = read_short();
            Value val = peek();
            stack[current_frame().slot_base + idx] = val;
            // Also update any open upvalue cell so closures see the change
            auto &upvals = current_frame().open_upvalues;
            if (upvals) {
                auto it = upvals->find(idx);
                if (it != upvals->end()) {
                    *it->second = val;
                }
            }
            break;
        }

        case OpCode::OP_LOAD_GLOBAL: {
            uint16_t name_idx = read_short();
            if (NARI_LIKELY(name_idx < global_cache_valid.size() &&
                            global_cache_valid[name_idx])) {
                VM::push(global_cache[name_idx]);
            } else {
                VM::push(get_global(chunk->strings[name_idx]));
            }
            break;
        }

        case OpCode::OP_STORE_GLOBAL: {
            uint16_t name_idx = read_short();
            const std::string &name = chunk->strings[name_idx];
            set_global(name, peek());
            if (name_idx < global_cache.size()) {
                global_cache[name_idx] = peek();
                global_cache_valid[name_idx] = 1;
            }
            break;
        }

        case OpCode::OP_LOAD_CAPTURE: {
            uint16_t idx = read_short();
            auto &captures = current_frame().captures;
            if (captures && idx < captures->size()) {
                VM::push(*(*captures)[idx]); // dereference cell
            } else {
                VM::push(Value::none());
            }
            break;
        }

        case OpCode::OP_STORE_CAPTURE: {
            uint16_t idx = read_short();
            auto &captures = current_frame().captures;
            if (captures && idx < captures->size()) {
                *(*captures)[idx] = peek(); // write through cell
            }
            break;
        }

        case OpCode::OP_POP:
            pop();
            break;

        case OpCode::OP_DUP:
            VM::push(peek());
            break;

        case OpCode::OP_LOAD_NONE:
            VM::push(Value::none());
            break;
        case OpCode::OP_LOAD_TRUE:
            VM::push(Value::make_bool(true));
            break;
        case OpCode::OP_LOAD_FALSE:
            VM::push(Value::make_bool(false));
            break;
        case OpCode::OP_LOAD_ZERO:
            VM::push(Value::make_int(0));
            break;
        case OpCode::OP_LOAD_ONE:
            VM::push(Value::make_int(1));
            break;

        case OpCode::OP_ADD: {
            Value &b = peek(0);
            Value &a = peek(1);
            if (a.is_int() && b.is_int()) {
                a.inplace_int_checked(a.get_int() + b.get_int());
            } else if (a.is_string() || b.is_string()) {
                a = Value::make_string(a.to_string() + b.to_string());
            } else {
                a.set_float(a.as_number() + b.as_number());
            }
            stack.pop_back();
            break;
        }

        case OpCode::OP_SUB: {
            Value &b = peek(0);
            Value &a = peek(1);
            if (a.is_int() && b.is_int()) {
                a.inplace_int_checked(a.get_int() - b.get_int());
            } else {
                a.set_float(a.as_number() - b.as_number());
            }
            stack.pop_back();
            break;
        }

        case OpCode::OP_MUL: {
            Value &b = peek(0);
            Value &a = peek(1);
            if (a.is_int() && b.is_int()) {
                // any product outside int48 promotes to float in one branch
                int64_t product;
                if (NARI_UNLIKELY(mul_overflow_i48(a.get_int(), b.get_int(), &product))) {
                    a.set_float(a.as_number() * b.as_number());
                } else {
                    a.inplace_int(product);
                }
            } else {
                a.set_float(a.as_number() * b.as_number());
            }
            stack.pop_back();
            break;
        }

        case OpCode::OP_DIV: {
            Value &b = peek(0);
            Value &a = peek(1);
            double bn = b.as_number();
            if (bn == 0.0) {
                a.set_float(std::nan(""));
            } else if (a.is_int() && b.is_int()) {
                int64_t av = a.get_int(), bv = b.get_int();
                if (av == INT64_MIN && bv == -1) {
                    a.set_float(-static_cast<double>(INT64_MIN));
                } else if (av % bv == 0) {
                    a.inplace_int(av / bv);
                } else {
                    a.set_float(a.as_number() / bn);
                }
            } else {
                a.set_float(a.as_number() / bn);
            }
            stack.pop_back();
            break;
        }

        case OpCode::OP_MOD: {
            Value &b = peek(0);
            Value &a = peek(1);
            if (a.is_int() && b.is_int()) {
                int64_t bv = b.get_int();
                if (bv == 0) {
                    a.set_float(std::nan(""));
                } else {
                    int64_t av = a.get_int();
                    if (av == INT64_MIN && bv == -1) {
                        a.set_float(0.0);
                    } else {
                        a.inplace_int(av % bv);
                    }
                }
            } else {
                a.set_float(std::fmod(a.as_number(), b.as_number()));
            }
            stack.pop_back();
            break;
        }

        case OpCode::OP_POW: {
            Value &b = peek(0);
            Value &a = peek(1);
            if (a.is_int() && b.is_int() && b.get_int() >= 0) {
                int64_t result = 1;
                int64_t base = a.get_int();
                int64_t exp = b.get_int();
                bool overflowed = false;
                while (exp > 0) {
                    if (exp & 1) {
                        if (mul_overflow_i64(result, base, &result)) {
                            overflowed = true;
                            break;
                        }
                    }
                    exp >>= 1;
                    if (exp > 0) {
                        if (mul_overflow_i64(base, base, &base)) {
                            overflowed = true;
                            break;
                        }
                    }
                }
                if (overflowed) {
                    a.set_float(std::pow(a.as_number(), b.as_number()));
                } else {
                    a.inplace_int(result);
                }
            } else {
                a.set_float(std::pow(a.as_number(), b.as_number()));
            }
            stack.pop_back();
            break;
        }

        case OpCode::OP_NEG: {
            Value &a = peek(0);
            if (a.is_int()) {
                // -INT48_MIN = 2^47 doesn't fit in int48 (max is 2^47 - 1),
                // inplace_int would truncate and sign-extend back to INT48_MIN,
                // so we use inplace_int_checked to promote to float when the result overflows int48.
                a.inplace_int_checked(-a.get_int());
            } else {
                a.set_float(-a.as_number());
            }
            break;
        }

        case OpCode::OP_STR_CONCAT: {
            Value &b = peek(0);
            Value &a = peek(1);
            // `@` must not mutate either operand. `LOAD_VAR` produces aliased `Value`
            // copies for heap strings, so in-place append here can corrupt caller
            // locals across nested function calls.
            a = Value::make_string(a.to_string() + b.to_string());
            stack.pop_back();
            break;
        }

        case OpCode::OP_FORMAT_VALUE: {
            uint16_t spec_idx = read_short();
            Value value = pop();
            if (spec_idx == 0xFFFF || spec_idx >= chunk->strings.size()) {
                VM::push(Value::make_string(value.to_string()));
            } else {
                Value args[2] = { value, Value::make_string(chunk->strings[spec_idx]) };
                VM::push(call_builtin("__format_value", args, 2));
            }
            break;
        }

        // in-place string append to a local variable: slot @= pop(), no copy of slot.
        case OpCode::OP_STR_APPEND_VAR: {
            uint16_t idx = read_short();
            Value rhs = pop(); // pop the right-hand side off the stack
            Value &slot = stack[current_frame().slot_base + idx];
            if (slot.is_mutable_heap_string()) {
                std::string &dst = slot.get_string();
                if (rhs.is_sso()) {
                    uint8_t len = rhs.sso_len();
                    if (len == 1) {
                        dst += rhs.sso_char(0);
                    } else if (len > 0) {
                        char buf[5];
                        for (uint8_t i = 0; i < len; i++) {
                            buf[i] = rhs.sso_char(i);
                        }
                        dst.append(buf, len);
                    }
                } else if (rhs.is_string()) {
                    dst += static_cast<const Value &>(rhs).get_string();
                } else {
                    slot = Value::make_string(slot.to_string() + rhs.to_string());
                }
            } else {
                slot = Value::make_string(slot.to_string() + rhs.to_string());
            }
            // keep slot value on top of stack, copy first since push_back may reallocate the stack vector and invalidate `slot`
            {
                Value to_push = slot;
                VM::push(std::move(to_push));
            }
            break;
        }

        // In-place string append to a global variable: globals[name] @= pop()
        case OpCode::OP_STR_APPEND_GLOBAL: {
            uint16_t name_idx = read_short();
            const std::string &name = chunk->strings[name_idx];
            Value rhs = pop();
            // Pop rhs before taking a reference into globals to avoid aliasing
            // if the same variable appears on both sides (e.g. x @= x).
            Value &slot = globals[name];
            if (slot.is_mutable_heap_string()) {
                std::string &dst = slot.get_string();
                if (rhs.is_sso()) {
                    uint8_t len = rhs.sso_len();
                    if (len == 1) {
                        dst += rhs.sso_char(0);
                    } else if (len > 0) {
                        char buf[5];
                        for (uint8_t i = 0; i < len; i++) {
                            buf[i] = rhs.sso_char(i);
                        }
                        dst.append(buf, len);
                    }
                } else if (rhs.is_string()) {
                    dst += static_cast<const Value &>(rhs).get_string();
                } else {
                    slot = Value::make_string(slot.to_string() + rhs.to_string());
                }
            } else {
                slot = Value::make_string(slot.to_string() + rhs.to_string());
            }
            VM::push(slot);
            if (name_idx < global_cache.size()) {
                global_cache[name_idx] = slot;
                global_cache_valid[name_idx] = 1;
            }
            break;
        }

        case OpCode::OP_BIT_AND: {
            int64_t val = value_as_int_safe(peek(0));
            stack.pop_back();
            Value &a = peek(0);
            int64_t av = value_as_int_safe(a);
            a.set_int(av & val);
            break;
        }

        case OpCode::OP_BIT_OR: {
            int64_t val = value_as_int_safe(peek(0));
            stack.pop_back();
            Value &a = peek(0);
            int64_t av = value_as_int_safe(a);
            a.set_int(av | val);
            break;
        }

        case OpCode::OP_BIT_XOR: {
            int64_t val = value_as_int_safe(peek(0));
            stack.pop_back();
            Value &a = peek(0);
            int64_t av = value_as_int_safe(a);
            a.set_int(av ^ val);
            break;
        }

        case OpCode::OP_BIT_NOT: {
            Value &a = peek(0);
            int64_t av = value_as_int_safe(a);
            a.set_int(~av);
            break;
        }

        case OpCode::OP_LSHIFT: {
            int64_t val = value_as_int_safe(peek(0));
            stack.pop_back();
            Value &a = peek(0);
            int64_t av = value_as_int_safe(a);
            // Mask the shift count to [0, 63]; shift counts >= 64 or < 0
            // would be UB on int64_t. Matches the JIT inline path.
            a.set_int(av << (val & 63));
            break;
        }

        case OpCode::OP_RSHIFT: {
            int64_t val = value_as_int_safe(peek(0));
            stack.pop_back();
            Value &a = peek(0);
            int64_t av = value_as_int_safe(a);
            a.set_int(av >> (val & 63));
            break;
        }

        case OpCode::OP_NOT: {
            bool r = !is_truthy(peek(0));
            peek(0).set_bool(r);
            break;
        }

        case OpCode::OP_EQ: {
            Value &b = peek(0);
            Value &a = peek(1);
            bool r = Value::values_equal(a, b);
            stack.pop_back();
            a.set_bool(r);
            break;
        }

        case OpCode::OP_NE: {
            Value &b = peek(0);
            Value &a = peek(1);
            bool r = !Value::values_equal(a, b);
            stack.pop_back();
            a.set_bool(r);
            break;
        }

        case OpCode::OP_LT: {
            Value &b = peek(0);
            Value &a = peek(1);
            bool r = (a.is_int() && b.is_int()) ? (a.get_int() < b.get_int())
                                                : (a.as_number() < b.as_number());
            stack.pop_back();
            a.set_bool(r);
            break;
        }

        case OpCode::OP_LE: {
            Value &b = peek(0);
            Value &a = peek(1);
            bool r = (a.is_int() && b.is_int()) ? (a.get_int() <= b.get_int())
                                                : (a.as_number() <= b.as_number());
            stack.pop_back();
            a.set_bool(r);
            break;
        }

        case OpCode::OP_GT: {
            Value &b = peek(0);
            Value &a = peek(1);
            bool r = (a.is_int() && b.is_int()) ? (a.get_int() > b.get_int())
                                                : (a.as_number() > b.as_number());
            stack.pop_back();
            a.set_bool(r);
            break;
        }

        case OpCode::OP_GE: {
            Value &b = peek(0);
            Value &a = peek(1);
            bool r = (a.is_int() && b.is_int()) ? (a.get_int() >= b.get_int())
                                                : (a.as_number() >= b.as_number());
            stack.pop_back();
            a.set_bool(r);
            break;
        }

        case OpCode::OP_JUMP: {
            int16_t offset = read_signed_short();
#ifndef DISABLE_JIT
            if (offset < 0) { // backward jump: potential loop back-edge
                uint32_t func_idx = (uint32_t)(current_function() - chunk->functions.data());
                size_t anchor_pc = insn_pc; // PC of this OP_JUMP

                if (trace_was_recording && trace_recorder.anchor_pc == anchor_pc) {
                    // Completed one full iteration; add LoopBack, finalize, compile.
                    trace_recorder.steps.push_back({ jit::TraceStep::Kind::LoopBack });
                    if (trace_recorder.exit_pc != 0 && !trace_recorder.aborted) {
                        trace_recorder.valid = true;
                        trace_recorder.recording = false;
                        if (jit::g_trace_jit) {
                            jit::jit_tls_prepare(*this);
                            jit::g_trace_jit->compile(trace_recorder, *chunk, func_idx);
                        }
                    } else {
                        trace_recorder.recording = false;
                        trace_recorder.aborted = true;
                        // Fallback: trace recording failed; compile with method JIT so the
                        // function doesn't stay in the interpreter forever.
                        if (jit::g_jit_compiler &&
                            !jit::g_jit_compiler->is_compiled(func_idx)) {
                            jit::g_jit_compiler->compile_chunk(*chunk, func_idx);
                        }
                    }
                    // Fall through to ip() += offset (complete this iteration normally)
                } else if (!trace_was_recording && !trace_recorder.pending_record && !trace_recorder.recording) {
                    // neither recording nor pending: check for cached trace, then do heat tracking.
                    if (jit::g_trace_jit) {
                        const jit::CompiledTrace *ct = jit::g_trace_jit->find(func_idx, anchor_pc);
                        if (ct && ct->valid) {
                            // Compiled traces assume vm->frames is non-empty,
                            // they load frames._M_finish and read &frames.back()
                            assert(!frames.empty() && "trace JIT entered with empty call frames!");
                            jit::jit_tls_prepare(*this);
                            trace_last_iters = 0;
                            ct->fn(this); // trace sets ip internally; skip normal jump

                            // Accumulate trace profitability stats
                            {
                                uint64_t pkey = ((uint64_t)func_idx << 32) | (uint64_t)anchor_pc;
                                auto &st = trace_iter_stats_[pkey];
                                st.first += trace_last_iters;
                                st.second++;
                                static const bool trace_stats = (getenv("NARI_TRACE_STATS") != nullptr);
                                if (trace_stats && (st.second & (st.second - 1)) == 0) {
                                    fprintf(stderr,
                                            "[trace-stats] func=%u anchor=%zu entries=%u "
                                            "total_iters=%llu avg=%llu\n",
                                            func_idx, anchor_pc, st.second,
                                            (unsigned long long)st.first,
                                            (unsigned long long)(st.first / st.second));
                                }
                            }
                            break;
                        }
                    } // Hot-edge detection: count backward jumps at this edge
                    uint64_t key = ((uint64_t)func_idx << 32) | (uint64_t)anchor_pc;
                    uint32_t heat = ++edge_heat_[key];
                    if (heat == TRACE_HOT_THRESHOLD) {
                        // target_pc = where this jump lands (the loop header)
                        size_t target = (size_t)((insn_base_ptr + 3 + offset) - code_data_ptr);
                        trace_recorder.reset();
                        trace_recorder.pending_record = true;
                        trace_recorder.anchor_pc = anchor_pc;
                        trace_recorder.target_pc = target;
                        trace_recorder.func_idx = func_idx;
                    }
                }
            }
#endif
            ip() += offset;
            break;
        }

        case OpCode::OP_JUMP_IF_FALSE: {
            int16_t offset = read_signed_short();
            if (!is_truthy(peek())) {
                ip() += offset;
            }
            pop(); // consume condition
            break;
        }

        case OpCode::OP_JUMP_IF_TRUE: {
            int16_t offset = read_signed_short();
            if (is_truthy(peek())) {
                ip() += offset;
            }
            pop();
            break;
        }

        case OpCode::OP_JUMP_IF_NONE: {
            int16_t offset = read_signed_short();
            if (peek().is_none()) {
                ip() += offset;
            }
            pop();
            break;
        }

        // functions
        case OpCode::OP_CALL: {
            uint8_t argc = read_byte();

            // args are already on the stack: stack[args_base .. top-1] (pushed left-to-right).
            // The function value sits just below them at stack[args_base-1].
            size_t args_base = stack.size() - argc;
            const Value &func_ref = stack[args_base - 1];

            if (!func_ref.is_function()) {
                // check if callee is a class name string -> instantiate
                if (func_ref.is_string()) {
                    std::string class_name =
                        func_ref.get_string(); // copy, SSO buffer is shared
                    if (Parser::get_registered_class(class_name)) {
                        std::vector<Value> args(stack.begin() + args_base, stack.begin() + args_base + argc);
                        stack.resize(args_base - 1);
                        VM::push(instantiate_class(class_name, std::move(args)));
                        break;
                    }
                }
                // Delegate call trap: d(args) -> handler.call(target, [args]),
                // after the is_function() fast path so ordinary calls are untouched.
                if (func_ref.is_delegate()) {
                    std::vector<Value> args(stack.begin() + args_base, stack.begin() + args_base + argc);
                    Value del = std::move(stack[args_base - 1]);
                    stack.resize(args_base - 1);
                    VM::push(runtime->delegate_call(del, args));
                    break;
                }
                fprintf(stderr, "bytecode: attempt to call non-function value: %s\n", func_ref.to_string().c_str());
                stack.resize(args_base - 1);
                VM::push(Value::none());
                break;
            }

            const auto &fn = func_ref.get_function();

            if (frames.size() >= MAX_CALL_DEPTH) {
                Value err = Value::make_string("Stack Overflow: maximum call depth exceeded!");
                if (!dispatch_throw(err)) {
                    return false;
                }
                break;
            }

            // fast path: cached function index avoids hash lookup on repeated calls.
            if (fn.jit_func_idx >= 0) {
                auto cell_captures = fn.captures;
                call_user_function_stack(static_cast<uint32_t>(fn.jit_func_idx), args_base, argc, cell_captures);
                break;
            }

            const std::string &fname = fn.name;

            // medium path: user-defined bytecode function (first call, populate cache).
            auto fit = func_indices.find(fname);
            if (fit != func_indices.end()) {
                // cache the index for future fast-path hits.
                const_cast<FunctionData &>(fn).jit_func_idx = static_cast<int32_t>(fit->second);
                auto cell_captures = fn.captures;
                call_user_function_stack(fit->second, args_base, argc, cell_captures);
                break;
            }

            // slower paths: builtins and AST-compiled functions.
            // move args out of the stack into a small local buffer, then trim.
            Value arg_buf[16];
            std::vector<Value> arg_vec;
            Value *args_ptr;
            if (argc <= 16) {
                for (int i = 0; i < argc; i++) {
                    arg_buf[i] = std::move(stack[args_base + i]);
                }
                args_ptr = arg_buf;
            } else {
                arg_vec.assign(std::make_move_iterator(stack.begin() + args_base),
                               std::make_move_iterator(stack.begin() + args_base + argc));
                args_ptr = arg_vec.data();
            }
            // copy func out before trimming (func_ref is a reference into stack[args_base-1]).
            Value func_copy = std::move(stack[args_base - 1]);
            stack.resize(args_base - 1);
            const auto &fn2 = func_copy.get_function();

            auto bit = builtins.find(fn2.name);
            if (bit != builtins.end()) {
                if (!push_builtin_result(call_builtin(fn2.name, args_ptr, argc))) {
                    return false;
                }
            } else if (fn2.func_ptr) {
                std::vector<Value> args_v(args_ptr, args_ptr + argc);
                VM::push(runtime->call_user_function(fn2.func_ptr.get(), args_v));
            } else {
                auto rit = runtime->functions.find(fn2.name);
                if (rit != runtime->functions.end()) {
                    std::vector<Value> args_v(args_ptr, args_ptr + argc);
                    VM::push(runtime->call_user_function(rit->second.get(), args_v));
                } else {
                    fprintf(stderr, "bytecode: unknown function '%s'\n", fn2.name.c_str());
                    VM::push(Value::none());
                }
            }
            break;
        }

        case OpCode::OP_SELF_TAIL_CALL: {
            uint8_t argc = read_byte();
            CallFrame &frame = current_frame();
            FunctionMeta *func = frame.function;
            size_t slot_base = frame.slot_base;
            size_t locals_needed = func->var_names.size();
            size_t param_count = static_cast<size_t>(func->param_count);

            // The N new argument values are on the top of the stack.
            // Read them into a local buffer before modifying any slot.
            size_t args_start = stack.size() - argc;
            Value arg_buf[16];
            std::vector<Value> vec_buf;
            Value *new_args;
            if (argc <= 16) {
                for (int i = 0; i < argc; i++) {
                    arg_buf[i] = std::move(stack[args_start + i]);
                }
                new_args = arg_buf;
            } else {
                vec_buf.assign(std::make_move_iterator(stack.begin() + args_start), std::make_move_iterator(stack.end()));
                new_args = vec_buf.data();
            }

            // trim stack back to the full locals frame.
            stack.resize(slot_base + locals_needed);

            // overwrite parameter slots with the new values.
            for (int i = 0; i < argc && static_cast<size_t>(i) < param_count; i++) {
                stack[slot_base + i] = std::move(new_args[i]);
            }
            // reset any params not passed to none
            for (size_t i = argc; i < param_count; i++) {
                stack[slot_base + i] = Value::none();
            }

            // jump back to function start, re-running the default-param preamble
            frame.ip = func->code.data();
            break;
        }

        case OpCode::OP_RETURN: {
            Value result = pop();

            // remove any try-handlers that were installed INSIDE this frame.
            // handler.frame_depth == frames.size() means the handler was pushed while
            // executing this frame, so it belongs to us and cannot outlive the return.
            while (!try_stack.empty() && try_stack.back().frame_depth >= frames.size()) {
                try_stack.pop_back();
            }

            // restore to pre-call state
            size_t slot_base = current_frame().slot_base;
            frames.pop_back();

            if (frames.empty()) {
                // returning from top-level - end execution
                VM::push(result);
                return false;
            }

            // trim stack back to before the call
            stack.resize(slot_base);
            VM::push(result);
            break;
        }

        case OpCode::OP_MAKE_CLOSURE: {
            uint16_t func_idx = read_short();
            uint8_t capture_count = read_byte();
            FunctionMeta *func = &chunk->functions[func_idx];
            // create captures vector with shared cells
            auto captures = std::make_shared<std::vector<std::shared_ptr<Value>>>();
            captures->resize(capture_count);
            for (int i = 0; i < capture_count; i++) {
                uint8_t source = read_byte();
                uint16_t idx = read_short();
                if (source == 0) {
                    // parent local: get or create cell from parent frame
                    auto cell = current_frame().get_or_create_cell(idx, stack[current_frame().slot_base + idx]);
                    // keep local in sync with cell
                    stack[current_frame().slot_base + idx] = *cell;
                    (*captures)[i] = cell;
                } else if (source == 1) {
                    // parent capture: share the same cell
                    auto &parent_caps = current_frame().captures;
                    if (parent_caps && idx < parent_caps->size()) {
                        (*captures)[i] = (*parent_caps)[idx];
                    } else {
                        (*captures)[i] = std::make_shared<Value>(Value::none());
                    }
                } else {
                    // global
                    const std::string &name = chunk->strings[idx];
                    (*captures)[i] = std::make_shared<Value>(get_global(name));
                }
            }
            // create a function value with captures
            Value closure = Value::make_function(func->name);
            closure.get_function().captures = captures;
            if (capture_count > 0) {
                closure.get_function().jit_capture0_raw = (*captures)[0].get();
            }
            GarbageCollector::instance().track(&closure.get_function(), GarbageCollector::TrackedType::Function);
            closure.get_function().jit_func_idx = (int32_t)func_idx;
            closure.get_function().jit_locals_count = (uint32_t)chunk->functions[func_idx].var_names.size();
            closure.get_function().jit_meta = &chunk->functions[func_idx];
            {
                auto cls = jit_classify_inline(chunk->functions[func_idx]);
                closure.get_function().jit_inline_kind = cls.kind;
                closure.get_function().jit_inline_imm = cls.imm;
            }
            func_indices[func->name] = func_idx;
            VM::push(closure);
            break;
        }

        case OpCode::OP_SPAWN: {
            // sync spawn: collector bookkeeping is not thread-safe, so a real thread would race on shared heap.
            // result is wrapped in a Handle still
            Value func_val = pop();
            auto handle = Value::make_handle_ptr();
            handle->state = HandleData::Running;
            // func_val and the handle are live across call_function_value_sync,
            // which runs nested bytecode (safe-points). Root them so a collection
            // there can't sweep them. handle_val owns the handle for marking.
            Value handle_val = Value::make_handle(handle);
            {
                TempRootScope rs(*this);
                rs.add(&func_val);
                rs.add(&handle_val);
                try {
                    Value result = call_function_value_sync(func_val, {});
                    handle->result = result;
                    handle->end_time = std::chrono::steady_clock::now();
                    handle->state = HandleData::Completed;
                } catch (const std::exception &e) {
                    handle->error = Value::make_string(e.what());
                    handle->end_time = std::chrono::steady_clock::now();
                    handle->state = HandleData::Failed;
                } catch (...) {
                    handle->error = Value::make_string("Unknown error in spawned task");
                    handle->end_time = std::chrono::steady_clock::now();
                    handle->state = HandleData::Failed;
                }
            }
            VM::push(std::move(handle_val));
            break;
        }

        // data structures
        case OpCode::OP_MAKE_ARRAY: {
            uint16_t size = read_short();
            if (stack.size() < size) {
                fprintf(stderr, "OP_MAKE_ARRAY: stack underflow\n");
                has_error = true;
                break;
            }
            std::vector<Value> elements(size);
            // pop in reverse order, fill from back to front
            for (int i = size - 1; i >= 0; i--) {
                elements[i] = pop();
            }
            VM::push(Value::make_array(std::move(elements)));
            break;
        }

        case OpCode::OP_MAKE_OBJECT: {
            const uint8_t *site = ip(); // stable per-literal key (before operand read)
            uint16_t size = read_short();
            VM::push(make_object_cached(site, size));
            break;
        }

        case OpCode::OP_ARRAY_PUSH: {
            // stack: [..., array, value] -> [..., array (with value appended)]
            Value val = pop();
            Value &arr_ref = stack[stack.size() - 1]; // peek the array
            if (!arr_ref.is_array()) {
                fprintf(stderr, "OP_ARRAY_PUSH: expected array\n");
                has_error = true;
                break;
            }
            arr_ref.get_array().push_back(std::move(val));
            break;
        }

        case OpCode::OP_ARRAY_SPREAD: {
            // stack: [..., array, iterable] -> [..., array (with iterable elements appended)]
            Value iterable = pop();
            Value &arr_ref = stack[stack.size() - 1];
            if (!arr_ref.is_array()) {
                fprintf(stderr, "OP_ARRAY_SPREAD: expected array\n");
                has_error = true;
                break;
            }
            auto &target = arr_ref.get_array();
            if (iterable.is_array()) {
                auto &src = iterable.get_array();
                target.insert(target.end(), src.begin(), src.end());
            } else {
                fprintf(stderr, "Spread operator requires an iterable (array)\n");
                has_error = true;
            }
            break;
        }

        case OpCode::OP_OBJECT_SPREAD: {
            // stack: [..., target_obj, source_obj] -> [..., target_obj (with source fields copied)]
            Value source = pop();
            Value &target_ref = stack[stack.size() - 1];
            if (!target_ref.is_object() || !source.is_object()) {
                fprintf(stderr, "OP_OBJECT_SPREAD: expected objects\n");
                has_error = true;
                break;
            }
            ObjectObj *src = source.get_obj_ptr();
            ObjectObj *tgt = target_ref.get_obj_ptr();
            for (const auto &name : src->get_keys()) {
                if (const Value *val = src->get_field(name)) {
                    tgt->set_field(name, *val);
                }
            }
            break;
        }

        case OpCode::OP_OBJECT_SET: {
            // stack: [..., obj, value] + 2-byte key string idx -> [..., obj (with key set)]
            uint16_t key_idx = read_short();
            Value val = pop();
            Value &obj_ref = stack[stack.size() - 1];
            if (!obj_ref.is_object()) {
                fprintf(stderr, "OP_OBJECT_SET: expected object\n");
                has_error = true;
                break;
            }
            const std::string &key = chunk->strings[key_idx];
            obj_ref.get_obj_ptr()->set_field(key, std::move(val));
            break;
        }

        case OpCode::OP_CALL_SPREAD: {
            // stack: [..., callee, args_array] -> [..., result]
            Value args_arr = pop();
            Value callee = pop();
            if (!args_arr.is_array()) {
                fprintf(stderr, "OP_CALL_SPREAD: expected args array\n");
                has_error = true;
                break;
            }
            auto &args = args_arr.get_array();
            // push callee back, then all args. This sets up the stack like OP_CALL expects
            VM::push(std::move(callee));
            for (auto &a : args) {
                VM::push(a);
            }
            uint8_t argc = static_cast<uint8_t>(args.size());
            // Now replicate OP_CALL logic
            size_t args_base = stack.size() - argc;
            const Value &func_ref = stack[args_base - 1];
            if (!func_ref.is_function()) {
                fprintf(stderr, "bytecode: attempt to call non-function value (spread): %s\n", func_ref.to_string().c_str());
                stack.resize(args_base - 1);
                VM::push(Value::none());
                break;
            }
            const auto &fn = func_ref.get_function();
            if (frames.size() >= MAX_CALL_DEPTH) {
                Value err = Value::make_string("Stack Overflow: maximum call-depth exceeded!");
                if (!dispatch_throw(err)) {
                    return false;
                }
                break;
            }
            if (fn.jit_func_idx >= 0) {
                auto cell_captures = fn.captures;
                call_user_function_stack(static_cast<uint32_t>(fn.jit_func_idx), args_base, argc, cell_captures);
                break;
            }
            const std::string &fname = fn.name;
            auto fit = func_indices.find(fname);
            if (fit != func_indices.end()) {
                const_cast<FunctionData &>(fn).jit_func_idx = static_cast<int32_t>(fit->second);
                auto cell_captures = fn.captures;
                call_user_function_stack(fit->second, args_base, argc, cell_captures);
                break;
            }
            Value arg_buf[16];
            std::vector<Value> arg_vec;
            Value *args_ptr;
            if (argc <= 16) {
                for (int i = 0; i < argc; i++) {
                    arg_buf[i] = std::move(stack[args_base + i]);
                }
                args_ptr = arg_buf;
            } else {
                arg_vec.assign(std::make_move_iterator(stack.begin() + args_base),
                               std::make_move_iterator(stack.begin() + args_base + argc));
                args_ptr = arg_vec.data();
            }
            Value func_copy = std::move(stack[args_base - 1]);
            stack.resize(args_base - 1);
            const auto &fn2 = func_copy.get_function();
            auto bit = builtins.find(fn2.name);
            if (bit != builtins.end()) {
                if (!push_builtin_result(call_builtin(fn2.name, args_ptr, argc))) {
                    return false;
                }
            } else if (fn2.func_ptr) {
                std::vector<Value> args_v(args_ptr, args_ptr + argc);
                VM::push(runtime->call_user_function(fn2.func_ptr.get(), args_v));
            } else {
                auto rit = runtime->functions.find(fn2.name);
                if (rit != runtime->functions.end()) {
                    std::vector<Value> args_v(args_ptr, args_ptr + argc);
                    VM::push(runtime->call_user_function(rit->second.get(), args_v));
                } else {
                    fprintf(stderr, "bytecode: unknown function '%s'\n", fn2.name.c_str());
                    VM::push(Value::none());
                }
            }
            break;
        }

        case OpCode::OP_MAKE_REGEX: {
            uint16_t pattern_idx = read_short();
            uint16_t flags_idx = read_short();
            const std::string &pattern = chunk->strings[pattern_idx];
            const std::string &flags = chunk->strings[flags_idx];
            VM::push(Value::make_regex(pattern, flags));
            break;
        }

        case OpCode::OP_GET_INDEX: {
#ifndef DISABLE_JIT
            trace_arr_recordable = false;
#endif
            Value index = pop();
            Value obj = pop();

            // Coerce whole-number floats to int for indexing (e.g. "1" - 1 == 0.0)
            if (index.is_float()) {
                double f = index.get_float();
                if (f == std::floor(f) && !std::isinf(f) && !std::isnan(f)) {
                    index = Value::make_int(static_cast<int64_t>(f));
                }
            }
            if (obj.is_array()) {
                auto &arr = obj.get_array();
                if (index.is_int()) {
                    int64_t idx = index.get_int();
                    if (idx >= 0 && idx < static_cast<int64_t>(arr.size())) {
                        VM::push(arr[idx]);
#ifndef DISABLE_JIT
                        // Trace-recordable if the loaded element is an int,
                        // or an int48-overflow float, which the trace decodes with
                        // the same dual-path as ObjGetProp
                        if (trace_was_recording) {
                            const Value &v = stack.back();
                            trace_arr_recordable = v.is_int() || v.is_float();
                            trace_arr_ptr = obj.heap_ptr();
                            trace_arr_size_bytes = arr.size() * sizeof(Value);
                        }
#endif
                    } else {
                        VM::push(Value::none());
                    }
                } else {
                    VM::push(Value::none());
                }
            } else if (obj.is_object()) {
                const std::string key = index.to_string();
                const Value *v = obj.get_obj_ptr()->get_field(key);
                VM::push(v ? *v : Value::none());
            } else if (obj.is_delegate()) {
                // Delegate get trap. After the object fast path so prop-IC is untouched.
                VM::push(runtime->delegate_get(obj, index));
            } else if (obj.is_string()) {
                // String character indexing: returns a 1-char string (auto-SSO).
                if (index.is_int()) {
                    const std::string &s = obj.get_string();
                    int64_t idx = index.get_int();
                    if (idx >= 0 && idx < (int64_t)s.size()) {
                        VM::push(Value::make_string(std::string(1, s[(size_t)idx])));
                    } else {
                        VM::push(Value::none());
                    }
                } else {
                    VM::push(Value::none());
                }
            } else {
                VM::push(Value::none());
            }
            break;
        }

        case OpCode::OP_SET_INDEX: {
#ifndef DISABLE_JIT
            trace_arr_recordable = false;
#endif
            Value val = pop();
            Value index = pop();
            Value obj = pop();

            // coerce whole-number floats to int for indexing
            if (index.is_float()) {
                double f = index.get_float();
                if (f == std::floor(f) && !std::isinf(f) && !std::isnan(f)) {
                    index = Value::make_int(static_cast<int64_t>(f));
                }
            }
            if (obj.is_array()) {
                auto &arr = obj.get_array();
                if (index.is_int()) {
                    int64_t idx = index.get_int();
                    int64_t orig_idx = idx;
                    if (idx < 0) {
                        idx += static_cast<int64_t>(arr.size());
                    }
                    if (idx >= 0 && idx < static_cast<int64_t>(arr.size())) {
                        arr[idx] = val;
#ifndef DISABLE_JIT
                        // in-bounds int-array int-store: trace-recordable.
                        if (trace_was_recording && orig_idx >= 0 &&
                            (val.is_int() || val.is_float())) {
                            trace_arr_recordable = true;
                            trace_arr_ptr = obj.heap_ptr();
                            trace_arr_size_bytes = arr.size() * sizeof(Value);
                        }
#endif
                    } else if (idx >= static_cast<int64_t>(arr.size()) && idx < 10000000) {
                        arr.resize(static_cast<size_t>(idx) + 1, Value::none());
                        arr[idx] = val;
                    }
                }
            } else if (obj.is_object()) {
                obj.get_obj_ptr()->set_field(index.to_string(), val);
            } else if (obj.is_delegate()) {
                // Delegate set trap. After the object fast path so prop-IC is untouched.
                runtime->delegate_set(obj, index, val);
            }
            VM::push(val); // assignment expressions return the value
            break;
        }

        case OpCode::OP_GET_PROPERTY: {
            uint16_t name_idx = read_short();
            Value obj = pop();

            // inline cache fast path for plain objects (dict_mode objects always take the slow path)
            if (obj.is_object()) {
                ObjectObj *oobj = static_cast<ObjectObj *>(obj.heap_ptr());
                auto &ic = prop_ic[((unsigned)name_idx) & PROP_IC_MASK];
#ifndef DISABLE_JIT
                // resolve the field slot for the trace recorder while obj is live
                if (trace_was_recording) {
                    trace_prop_recordable = false;
                    if (!oobj->dict_mode) {
                        auto sit = oobj->shape->index.find(intern_field(chunk->strings[name_idx]));
                        if (sit != oobj->shape->index.end() && sit->second < oobj->fields.size()) {
                            trace_prop_slot = (uint32_t)sit->second;
                            trace_prop_recordable = true;
                        }
                    }
                }
#endif
                if (!oobj->dict_mode && ic.shape == oobj->shape &&
                    ic.name_idx == name_idx &&
                    ic.slot < oobj->fields.size()) {
                    VM::push(oobj->fields[ic.slot]); // IC hit
                } else {
                    const std::string &name = chunk->strings[name_idx];
                    Value *v = oobj->get_field(name);
                    if (v) {
                        if (!oobj->dict_mode) {
                            auto sit = oobj->shape->index.find(intern_field(name));
                            if (sit != oobj->shape->index.end()) {
                                ic.shape = oobj->shape;
                                ic.name_idx = name_idx;
                                ic.slot = sit->second;
                            }
                        }
                        VM::push(*v);
                    } else {
                        VM::push(Value::none());
                    }
                }
            } else if (obj.is_class_instance()) {
                const std::string &name = chunk->strings[name_idx];
                const auto &instance = obj.get_class_instance();
                // check private field access
                if (instance->layout && instance->layout->private_fields.count(name) && current_class_name != instance->class_name) {
                    bytecode_runtime_fatal("Cannot access private field '" + name + "' of class '" + instance->class_name + "'!", "");
                    has_error = true;
                    return false;
                }
                const Value *fv = instance->get_field(name);
                VM::push(fv ? *fv : Value::none());
            } else if (obj.is_delegate()) {
                // Delegate get trap. After the object fast path so prop-IC is untouched.
                VM::push(runtime->delegate_get(obj, Value::make_string(chunk->strings[name_idx])));
            } else {
                const std::string &name = chunk->strings[name_idx];
                if (obj.is_array() && name == "length") {
                    VM::push(Value::make_int(static_cast<int64_t>(obj.get_array().size())));
                } else if (obj.is_string() && name == "length") {
                    VM::push(Value::make_int(static_cast<int64_t>(obj.get_string().size())));
                } else if (obj.is_handle()) {
                    // handle special members on async handles
                    const auto &handle = obj.get_handle();
                    if (!handle) {
                        VM::push(Value::none());
                    } else if (name == "await") {
                        // wait for handle to complete
                        while (handle->state == HandleData::Running) {
                            runtime->process_completed_io();
                            if (handle->state == HandleData::Running) {
                                NARI_SLEEP_MILLIS(1);
                            }
                        }
                        if (handle->state == HandleData::Failed) {
                            // throw the error
                            VM::push(handle->error);
                            // use throw to propagate
                            Value error = pop();
                            if (!try_stack.empty()) {
                                TryHandler th = try_stack.back();
                                try_stack.pop_back();
                                while (frames.size() > th.frame_depth) {
                                    size_t sb = current_frame().slot_base;
                                    frames.pop_back();
                                    if (!frames.empty()) {
                                        stack.resize(sb);
                                    }
                                }
                                stack.resize(th.stack_depth);
                                VM::push(error);
                                ip() = current_function()->code.data() + th.catch_ip;
                            } else {
                                fprintf(stderr, "panic: %s\n", error.to_string().c_str());
                            }
                        } else {
                            VM::push(handle->result);
                        }
                    } else if (name == "ready") {
                        runtime->process_completed_io();
                        VM::push(Value::make_bool(handle->state != HandleData::Running));
                    } else if (name == "failed") {
                        VM::push(Value::make_bool(handle->state == HandleData::Failed));
                    } else if (name == "error") {
                        VM::push(handle->error);
                    } else if (name == "status_code") {
                        // For HTTP response handles
                        const Value *sc =
                            handle->result.is_object()
                                ? handle->result.get_obj_ptr()->get_field("status_code")
                                : nullptr;
                        VM::push(sc ? *sc : Value::none());
                    } else if (name == "duration") {
                        if (handle->state == HandleData::Running) {
                            auto now = chrono::steady_clock::now();
                            auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - handle->start_time);
                            VM::push(Value::make_int(elapsed.count()));
                        } else {
                            auto elapsed = chrono::duration_cast<chrono::milliseconds>(handle->end_time - handle->start_time);
                            VM::push(Value::make_int(elapsed.count()));
                        }
                    } else {
                        VM::push(Value::none());
                    }
                } else if (obj.is_string()) {
                    std::string class_name = obj.get_string();
                    const nari::ClassDecl *class_decl = Parser::get_registered_class(class_name);
                    if (class_decl) {
                        ensure_static_fields_inited(class_name, class_decl);
                        std::string key = class_name + "." + name;
                        auto &sf = Parser::get_static_fields();
                        auto it = sf.find(key);
                        if (it != sf.end()) {
                            VM::push(it->second);
                        } else {
                            VM::push(Value::none());
                        }
                    } else {
                        // Not a class name, treat like other non-object types
                        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
                            VM::push(Value::none());
                        } else {
                            bytecode_runtime_fatal("Cannot access property '" + name + "' on string value", name);
                            VM::push(Value::none());
                        }
                    }
                } else {
                    // Compiler-internal properties (__variant, __data) are used as
                    // speculative probes in match/pattern expressions, silently return none for non-objects.
                    // For all other properties, this is a fatal user error.
                    if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
                        VM::push(Value::none());
                    } else {
                        bytecode_runtime_fatal(
                            "Cannot access property '" + name + "' on " +
                                (obj.is_none() ? "null" : "non-object") +
                                " value",
                            name);
                        VM::push(Value::none());
                    }
                }
            }
            break;
        }

        case OpCode::OP_SET_PROPERTY: {
            uint16_t name_idx = read_short();
            Value val = pop();
            Value obj = pop();

            if (obj.is_object()) {
                ObjectObj *oobj = static_cast<ObjectObj *>(obj.heap_ptr());
                auto &ic = prop_ic[((unsigned)name_idx) & PROP_IC_MASK];
#ifndef DISABLE_JIT
                // resolve the field slot for the trace recorder while obj is live
                if (trace_was_recording) {
                    trace_prop_recordable = false;
                    if (!oobj->dict_mode && !oobj->frozen) {
                        auto sit = oobj->shape->index.find(intern_field(chunk->strings[name_idx]));
                        if (sit != oobj->shape->index.end() && sit->second < oobj->fields.size()) {
                            trace_prop_slot = (uint32_t)sit->second;
                            trace_prop_recordable = true;
                        }
                    }
                }
#endif
                if (!oobj->dict_mode && !oobj->frozen &&
                    ic.shape == oobj->shape && ic.name_idx == name_idx &&
                    ic.slot < oobj->fields.size()) {
                    oobj->fields[ic.slot] = val; // IC hit, direct write
                } else {
                    const std::string &name = chunk->strings[name_idx];
                    oobj->set_field(name, val);
                    if (!oobj->dict_mode) {
                        auto sit = oobj->shape->index.find(intern_field(name));
                        if (sit != oobj->shape->index.end()) {
                            ic.shape = oobj->shape;
                            ic.name_idx = name_idx;
                            ic.slot = sit->second;
                        }
                    }
                }
            } else if (obj.is_class_instance()) {
                const std::string &name = chunk->strings[name_idx];
                auto instance = obj.get_class_instance();
                Value *fv = instance->get_field(name);
                if (fv) {
                    *fv = std::move(val);
                }
            } else if (obj.is_string()) {
                std::string cname = obj.get_string();
                if (Parser::get_registered_class(cname)) {
                    const std::string &name = chunk->strings[name_idx];
                    std::string key = cname + "." + name;
                    Parser::get_static_fields()[key] = val;
                }
            } else if (obj.is_delegate()) {
                // Delegate set trap
                runtime->delegate_set(obj, Value::make_string(chunk->strings[name_idx]), val);
            }
            VM::push(val);
            break;
        }

        // for-each iteration support
        case OpCode::OP_MAKE_ITERATOR: {
            // top of stack has the iterable (array or object)
            // push an iterator: [array_or_keys, index=0] on stack
            Value iterable = pop();
            if (iterable.is_array()) {
                VM::push(iterable);           // the array
                VM::push(Value::make_int(0)); // current index
            } else if (iterable.is_object()) {
                // create an array from the object's keys, then iterate that.
                // The loop variable receives each key, values are accessible via obj[key].
                const ObjectObj *oobj = iterable.get_obj_ptr();
                std::vector<Value> keys;
                for (const auto &name : oobj->get_keys()) {
                    keys.push_back(Value::make_string(name));
                }
                VM::push(Value::make_array(std::move(keys)));
                VM::push(Value::make_int(0));
            } else {
                fprintf(stderr, "bytecode: for-each requires an array or object\n");
                has_error = true;
                VM::push(Value::make_array()); // dummy empty array
                VM::push(Value::make_int(0));  // dummy index
            }
            break;
        }

        case OpCode::OP_ITER_ARRAY: {
            // Normalize an iterable to a plain array so the compiler can drive a desugared index-loop
            // mirrors the single-variable normalization of OP_MAKE_ITERATOR:
            //   array  -> itself
            //   object -> its keys as an array
            // anything else is an error, we just dump an empty array
            Value iterable = pop();
            if (iterable.is_array()) {
                VM::push(iterable);
            } else if (iterable.is_object()) {
                const ObjectObj *oobj = iterable.get_obj_ptr();
                std::vector<Value> keys;
                for (const auto &name : oobj->get_keys()) {
                    keys.push_back(Value::make_string(name));
                }
                VM::push(Value::make_array(std::move(keys)));
            } else {
                fprintf(stderr, "bytecode: for-each requires an array or object\n");
                has_error = true;
                VM::push(Value::make_array()); // dummy empty array
            }
            break;
        }

        case OpCode::OP_MAKE_ITERATOR_KV: {
            // key-value for-each: for (key, value in ...)
            // Builds a pairs array [[key0, val0], [key1, val1], ...] and pushes [pairs, index=0]
            Value iterable = pop();
            std::vector<Value> pairs;
            if (iterable.is_array()) {
                const auto &arr = iterable.get_array();
                pairs.reserve(arr.size());
                for (size_t _ki = 0; _ki < arr.size(); _ki++) {
                    std::vector<Value> pair;
                    pair.push_back(Value::make_int(static_cast<int64_t>(_ki)));
                    pair.push_back(arr[_ki]);
                    pairs.push_back(Value::make_array(std::move(pair)));
                }
            } else if (iterable.is_object()) {
                const ObjectObj *oobj = iterable.get_obj_ptr();
                for (const auto &name : oobj->get_keys()) {
                    const Value *val = oobj->get_field(name);
                    if (!val) {
                        continue;
                    }
                    std::vector<Value> pair;
                    pair.push_back(Value::make_string(name));
                    pair.push_back(*val);
                    pairs.push_back(Value::make_array(std::move(pair)));
                }
            } else {
                fprintf(stderr, "bytecode: for-each (key, value) requires an array or object\n");
                has_error = true;
            }
            VM::push(Value::make_array(std::move(pairs)));
            VM::push(Value::make_int(0));
            break;
        }

        case OpCode::OP_ITER_NEXT: {
            // stack has: [..., array, index]
            Value idx_val = pop(); // current index
            Value arr_val = pop(); // the array
            int64_t idx = idx_val.get_int();
            auto &arr = arr_val.get_array();
            if (idx < static_cast<int64_t>(arr.size())) {
                // push iterator state back for next iteration
                VM::push(arr_val);                  // array
                VM::push(Value::make_int(idx + 1)); // next index
                // push current element value and true
                VM::push(arr[idx]);               // value
                VM::push(Value::make_bool(true)); // still iterating
            } else {
                // done iterating, push iterator back (will be cleaned up by OP_POP)
                VM::push(arr_val);                 // array
                VM::push(Value::make_int(idx));    // index (unchanged)
                VM::push(Value::make_bool(false)); // done
            }
            break;
        }

        case OpCode::OP_ITER_NEXT_KV: {
            // key-value iteration: stack has [..., pairs_array, index]
            // pairs_array is [[key0, val0], [key1, val1], ...]
            // Pushes: pairs_array, index+1, key, value, true, or pairs_array, index, false
            Value idx_val = pop(); // current index
            Value arr_val = pop(); // the pairs array
            int64_t idx = idx_val.get_int();
            auto &pairs = arr_val.get_array();
            if (idx < static_cast<int64_t>(pairs.size())) {
                VM::push(arr_val);                  // pairs array
                VM::push(Value::make_int(idx + 1)); // next index
                // destructure pair [key, value]
                const auto &pair = pairs[idx].get_array();
                VM::push(pair[0]);                // key
                VM::push(pair[1]);                // value
                VM::push(Value::make_bool(true)); // still iterating
            } else {
                VM::push(arr_val);                 // pairs array
                VM::push(Value::make_int(idx));    // index (unchanged)
                VM::push(Value::make_bool(false)); // done
            }
            break;
        }

        case OpCode::OP_THROW: {
            Value error = pop();
            bool caught = dispatch_throw(error);
            // Inside a JIT-compiled function call, returning normally from execute_instruction()
            // would resume the JIT caller's baked native code, which has no way to honor
            // the rewound frame.ip (caught) or the unwound frames (uncaught).
            // Longjmp out to VM::run's setjmp instead.
            //
            // longjmp skips dtors, explicitly cleanup
            error = Value::none();
            if (jit_call_depth > 0 && this->overflow_jmp) {
                std::longjmp(*this->overflow_jmp, caught ? 2 : 1);
            }
            if (!caught) {
                return false;
            }
            break;
        }

        case OpCode::OP_CHECK_TYPE: {
            // Capture instruction pc *before* reading operands (opcode already consumed).
            size_t instr_pc = static_cast<size_t>(ip() - current_function()->code.data()) - 1;
            uint16_t type_str_idx = read_short();
            uint8_t ctx_byte = read_byte(); // 0=param, 1=return, 2=variable
            // peek TOS without consuming it
            const Value &val = stack.back();
            const std::string &expected = chunk->strings[type_str_idx];
            bool ok = false;
            if (expected == "int") {
                ok = val.is_int();
            } else if (expected == "float") {
                ok = val.is_float();
            } else if (expected == "number") {
                ok = val.is_numeric();
            } else if (expected == "string") {
                ok = val.is_string();
            } else if (expected == "bool") {
                ok = val.is_bool();
            } else if (expected == "null") {
                ok = val.is_none();
            } else if (expected == "array") {
                ok = val.is_array();
            } else if (expected == "object") {
                ok = val.is_object();
            } else if (expected == "function") {
                ok = val.is_function();
            } else if (expected == "regex") {
                ok = val.is_regex();
            } else if (val.is_class_instance()) {
                ok = (val.get_class_instance()->class_name == expected);
            }
            // unknown annotation name, skip check (forward-compat)
            if (!ok &&
                (expected == "int" || expected == "float" || expected == "number" ||
                 expected == "string" || expected == "bool" || expected == "null" ||
                 expected == "array" || expected == "object" ||
                 expected == "function" || expected == "regex" ||
                 val.is_class_instance())) {
                std::string actual;
                if (val.is_none()) {
                    actual = "null";
                } else if (val.is_int()) {
                    actual = "int";
                } else if (val.is_float()) {
                    actual = "float";
                } else if (val.is_string()) {
                    actual = "string";
                } else if (val.is_bool()) {
                    actual = "bool";
                } else if (val.is_array()) {
                    actual = "array";
                } else if (val.is_object()) {
                    actual = "object";
                } else if (val.is_function()) {
                    actual = "function";
                } else if (val.is_regex()) {
                    actual = "regex";
                } else if (val.is_class_instance()) {
                    actual = val.get_class_instance()->class_name;
                } else {
                    actual = "unknown";
                }
                const char *ctx_str = ctx_byte == 0   ? "parameter"
                                      : ctx_byte == 1 ? "return value"
                                                      : "variable";
                // Resolve source location from the line map.
                const FunctionMeta *fn = current_function();
                int src_line = fn ? fn->resolve_line(instr_pc) : 0;
                const std::string &fn_name = (fn && !fn->name.empty()) ? fn->name : "<anonymous>";
                const std::string &src_file = fn ? fn->source_file : "";
                std::string loc;
                if (!src_file.empty()) {
                    loc = " at '" + fn_name + "' (" + src_file;
                    if (src_line > 0) {
                        loc += ":" + std::to_string(src_line);
                    }
                    loc += ")";
                } else if (!fn_name.empty()) {
                    loc = " at '" + fn_name + "'";
                }
                std::string msg =
                    std::string("TypeError: expected ") + ctx_str +
                    " of type '" + expected + "', got '" + actual + "'" +
                    loc;
                Value err = Value::make_string(msg);
                if (!dispatch_throw(err)) {
                    return false;
                }
            }
            break;
        }

        case OpCode::OP_SETUP_TRY: {
            uint16_t catch_offset = read_short();
            uint16_t finally_offset = read_short();
            TryHandler handler;
            size_t current_pos = ip() - current_function()->code.data();
            handler.catch_ip = current_pos - 2 + static_cast<int16_t>(catch_offset);
            handler.finally_ip = current_pos + static_cast<int16_t>(finally_offset);
            handler.stack_depth = stack.size();
            handler.frame_depth = frames.size();
            try_stack.push_back(handler);
            break;
        }

        case OpCode::OP_POP_TRY: {
            if (!try_stack.empty()) {
                try_stack.pop_back();
            }
            break;
        }

        case OpCode::OP_BEGIN_CATCH: {
            // error value is already on stack (pushed by OP_THROW),
            // nothing extra needed
            break;
        }

        case OpCode::OP_BEGIN_FINALLY: {
            // nothing to do, finally block just runs
            break;
        }

        // class support
        case OpCode::OP_NEW_INSTANCE: {
            uint16_t name_idx = read_short();
            uint8_t argc = read_byte();
            const std::string &class_name = chunk->strings[name_idx];

            // collect args
            std::vector<Value> args;
            args.reserve(argc);
            for (int i = 0; i < argc; i++) {
                args.push_back(pop());
            }
            std::reverse(args.begin(), args.end());

            VM::push(instantiate_class(class_name, std::move(args)));
            break;
        }

        case OpCode::OP_LOAD_THIS: {
            if (current_instance) {
                VM::push(Value::from_class_instance(current_instance));
            } else {
                VM::push(Value::none());
            }
            break;
        }

        case OpCode::OP_CALL_METHOD: {
            uint16_t name_idx = read_short();
            uint8_t argc = read_byte();
            const std::string &method_name = chunk->strings[name_idx];

            // fast path: read obj and args directly from the stack
            // Stack: [..., obj, arg0, arg1, ..., argN-1]  (argN-1 on top)
            const size_t stack_top = stack.size();
            const size_t args_base = stack_top - (size_t)argc; // index of arg0
            const size_t obj_idx = args_base - 1;              // index of obj

            Value &obj_ref = stack[obj_idx]; // peek without popping

            if (obj_ref.is_class_instance()) {
                // class instance: needs args as a vector for the AST-based dispatch.
                std::vector<Value> args(stack.begin() + args_base, stack.end());
                Value obj = std::move(stack[obj_idx]);
                stack.resize(obj_idx);
                VM::push(call_class_method(obj.get_class_instance(), method_name, std::move(args)));
                break;
            }

            // Static method call: ClassName.method(args)
            // Gate on there being ANY registered class,
            // otherwise a string receiver is always a plain builtin-method call
            if (obj_ref.is_string() && !Parser::get_all_registered_classes().empty()) {
                std::string class_name = obj_ref.get_string(); // copy for SSO safety
                const nari::ClassDecl *class_decl = Parser::get_registered_class(class_name);
                if (class_decl) {
                    // look for a static method
                    const nari::ClassMethod *found = nullptr;
                    for (const auto &m : class_decl->methods) {
                        if (m.name == method_name && m.is_static) {
                            found = &m;
                            break;
                        }
                    }
                    if (found && found->body) {
                        std::vector<Value> args(stack.begin() + args_base, stack.end());
                        stack.resize(obj_idx);
                        // execute static method via AST interpreter (no 'this' binding)
                        auto saved_instance = current_instance;
                        auto saved_class = current_class_name;
                        current_instance = nullptr;
                        current_class_name = class_name;
                        runtime->current_instance = nullptr;
                        runtime->current_class_name = class_name;

                        runtime->call_stack.emplace_back();
                        for (size_t i = 0; i < found->params.size() && i < args.size(); i++) {
                            runtime->call_stack.back()[found->params[i].name] = args[i];
                        }

                        Value return_value = Value::none();
                        ScopedSyntheticDebugFrame debug_frame(
                            class_name + "." + method_name,
                            found->filename.empty() ? class_decl->filename : found->filename,
                            found->line,
                            runtime->call_stack.size() - 1);
                        for (const auto &stmt : found->body->stmts) {
                            runtime->exec_stmt(stmt.get());
                            if (runtime->flags.return_flag) {
                                return_value = runtime->flags.return_value;
                                runtime->flags.return_flag = false;
                                break;
                            }
                            if (runtime->flags.break_flag || runtime->flags.continue_flag ||
                                runtime->flags.throw_flag) {
                                break;
                            }
                        }

                        runtime->call_stack.pop_back();
                        runtime->current_instance = saved_instance;
                        runtime->current_class_name = saved_class;
                        current_instance = saved_instance;
                        current_class_name = saved_class;
                        VM::push(std::move(return_value));
                        break;
                    }
                    // Check if it's a static field that's callable
                    ensure_static_fields_inited(class_name, class_decl);
                    std::string key = class_name + "." + method_name;
                    auto &sf = Parser::get_static_fields();
                    auto sit = sf.find(key);
                    if (sit != sf.end() && sit->second.is_function()) {
                        std::vector<Value> args(stack.begin() + args_base, stack.end());
                        Value func = sit->second;
                        stack.resize(obj_idx);
                        const auto &fn = func.get_function();
                        auto fit = func_indices.find(fn.name);
                        if (fit != func_indices.end()) {
                            call_user_function(fit->second, args);
                        } else if (fn.func_ptr) {
                            VM::push(runtime->call_user_function(fn.func_ptr.get(), args));
                        } else {
                            VM::push(Value::none());
                        }
                        break;
                    }
                }
            }

            if (obj_ref.is_delegate()) {
                // has_key routes to the `has` trap,
                // every other method resolves through the get trap then is invoked
                std::vector<Value> args(stack.begin() + args_base, stack.end());
                Value obj = std::move(stack[obj_idx]);
                stack.resize(obj_idx);
                if (method_name == "has_key" && args.size() == 1) {
                    VM::push(Value::make_bool(runtime->delegate_has(obj, args[0])));
                } else {
                    VM::push(runtime->delegate_call_method(obj, method_name, std::move(args)));
                }
                break;
            }

            if (obj_ref.is_object()) {
                const Value *it_v = obj_ref.get_obj_ptr()->get_field(method_name);
                if (it_v && it_v->is_function()) {
                    // Object property is a callable, dispatch it.
                    Value func = *it_v;
                    std::vector<Value> args(stack.begin() + args_base, stack.end());
                    stack.resize(obj_idx);
                    const std::string &fname = func.get_function().name;
                    const auto &func_ptr = func.get_function().func_ptr;

                    auto bit = builtins.find(fname);
                    if (bit != builtins.end()) {
                        if (!push_builtin_result(call_builtin(fname, args.data(), args.size()))) {
                            return false;
                        }
                    } else {
                        auto fit = func_indices.find(fname);
                        if (fit != func_indices.end()) {
                            const auto &captures = func.get_function().captures;
                            if (captures && !captures->empty()) {
                                call_user_function(fit->second, args, nullptr, captures);
                            } else {
                                call_user_function(fit->second, args);
                            }
                        } else if (func_ptr) {
                            VM::push(runtime->call_user_function(func_ptr.get(), args));
                        } else {
                            auto rit = runtime->functions.find(fname);
                            if (rit != runtime->functions.end()) {
                                VM::push(runtime->call_user_function(rit->second.get(), args));
                            } else {
                                bytecode_runtime_fatal("Method '" + method_name + "' is not callable or is otherwise unknown!", "");
                                VM::push(Value::none());
                            }
                        }
                    }
                    break;
                }
                // Not a callable property, fall through to builtin method dispatch.
            }

            // builtin method call (array, string, object, number, etc.)
            // stack-local buffer avoids heap alloc for argc <= 9.
            {
                const size_t n = (size_t)argc + 1;
                Value buf[10];
                Value *argv;
                std::vector<Value> overflow;
                if (n <= 10) {
                    buf[0] = stack[obj_idx];
                    for (size_t i = 0; i < (size_t)argc; i++) {
                        buf[i + 1] = stack[args_base + i];
                    }
                    argv = buf;
                } else {
                    overflow.reserve(n);
                    overflow.push_back(stack[obj_idx]);
                    for (size_t i = 0; i < (size_t)argc; i++) {
                        overflow.push_back(stack[args_base + i]);
                    }
                    argv = overflow.data();
                }
                stack.resize(obj_idx); // pop obj + args in one shot

                // inline cache: once resolved, call the builtin member pointer directly,
                // skipping builtins.find + runtime->call_builtin's own table lookups
                if (name_idx < method_ic_state.size() && method_ic_state[name_idx] == 1) {
                    if (!push_builtin_result(runtime->call_builtin_member(method_ic_fn[name_idx], argv, n, nullptr))) {
                        return false;
                    }
                    break;
                }

                auto bit = builtins.find(method_name);
                if (bit != builtins.end()) {
                    if (name_idx < method_ic_state.size()) {
                        ScriptRuntime::BuiltinFn fn = runtime->lookup_builtin_member(method_name);
                        if (fn) {
                            method_ic_fn[name_idx] = fn;
                            method_ic_state[name_idx] = 1;
                        }
                    }
                    if (!push_builtin_result(call_builtin(method_name, argv, n))) {
                        return false;
                    }
                } else {
                    // Uncommon / error path: give a typed diagnostic.
                    const Value &ov = argv[0];
                    std::string type_str =
                        ov.is_string()   ? "string"
                        : ov.is_array()  ? "array"
                        : ov.is_object() ? "object"
                        : ov.is_int()    ? "number"
                        : ov.is_float()  ? "number"
                        : ov.is_bool()   ? "boolean"
                        : ov.is_none()   ? "null"
                        : ov.is_regex()  ? "regex"
                                         : "value";
                    if (runtime->is_builtin_name(method_name)) {
                        bytecode_runtime_fatal("Method '" + method_name + "' does not exist on type '" + type_str + "'!", "");
                    } else {
                        bytecode_runtime_fatal("'" + method_name + "' is not a method!", "");
                    }
                    VM::push(Value::none());
                }
            }
            break;
        }

        default:
            fprintf(stderr, "unknown opcode: %d\n", static_cast<int>(op));
            has_error = true;
            return false;
    }

#ifndef DISABLE_JIT
    // post-dispatch trace recording hook (already handled inline above for OP_JUMP)
    if (trace_was_recording && !trace_recorder.aborted && op != OpCode::OP_JUMP) {
        trace_record_step(op, insn_base_ptr);
    }
#endif

    return true;
}

#ifndef DISABLE_JIT
// trace recording: called once per instruction while recording is active
void VM::trace_record_step(OpCode op, uint8_t *insn_base) {
    using Kind = jit::TraceStep::Kind;
    using TraceType = jit::TraceType;
    auto &rec = trace_recorder;
    if (rec.aborted || !rec.recording) {
        return;
    }

    // Skip instructions from inlined function bodies.
    // When OP_CALL is successfully inlined (e.g. IntAdd), the interpreter still enters the called function,
    // so we track call depth and ignore all instructions until the matching RETURN pops us back to the recording function.
    if (rec.inline_depth > 0) {
        if (op == OpCode::OP_CALL || op == OpCode::OP_CALL_METHOD || op == OpCode::OP_NEW_INSTANCE) {
            ++rec.inline_depth;
        } else if (op == OpCode::OP_RETURN) {
            --rec.inline_depth;
        }
        return; // skip recording this instruction
    }

    // re-read uint16 operand from instruction bytes
    auto op16 = [&]() -> uint16_t {
        return (uint16_t(insn_base[1]) << 8) | uint16_t(insn_base[2]);
    };

    // determine TraceType from a live Value tag
    auto vtype = [](const Value &v) -> TraceType {
        if (v.is_int()) {
            return TraceType::Int;
        }
        if (v.is_float()) {
            return TraceType::Float;
        }
        if (v.is_bool()) {
            return TraceType::Bool;
        }
        return TraceType::Unknown;
    };

    auto abort_recording = [&]() {
        rec.aborted = true;
        rec.pending_inline_func = nullptr;
    };

    switch (op) {
        // loads
        case OpCode::OP_LOAD_VAR: {
            uint16_t slot = op16();
            TraceType t = vtype(peek());
            if (t == TraceType::Int) {
                jit::TraceStep s = { Kind::LoadIntVar };
                s.slot = slot;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Int);
            } else if (t == TraceType::Float) {
                jit::TraceStep s = { Kind::LoadFloatVar };
                s.slot = slot;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Float);
            } else if (peek().is_object()) {
                jit::TraceStep s = { Kind::LoadObjVar };
                s.slot = slot;
                ObjectObj *oobj = static_cast<ObjectObj *>(peek().heap_ptr());
                s.obj_ptr = oobj;
                s.shape_ptr = const_cast<ObjectShape *>(oobj->shape);
                s.shape_ver = oobj->shape_version;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Obj);
            } else if (peek().is_array()) {
                // Array trace: identity guard on ArrayObj* + size guard on (finish - start) at entry.
                // Trace body can access elements via ArrayGetIdx / ArraySetIdx.
                // Any op that could grow the array (push, spread, OOB store) aborts recording
                ArrayObj *aobj = static_cast<ArrayObj *>(peek().heap_ptr());
                jit::TraceStep s = { Kind::LoadArrayVar };
                s.slot = slot;
                s.obj_ptr = aobj;
                s.int_val = (int64_t)(aobj->v.size() * sizeof(Value));
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Array);
            } else if (peek().is_function()) {
                // deferred closure call, record the local slot, not the current capture cell
                auto &fd = peek().get_function();
                if ((fd.jit_inline_kind == JitInlineKind::ClosureInc ||
                     fd.jit_inline_kind == JitInlineKind::ClosureAddConst) &&
                    fd.jit_capture0_raw) {
                    rec.pending_closure_capture0 = fd.jit_capture0_raw;
                    rec.pending_closure_slot = slot;
                    rec.pending_closure_kind = fd.jit_inline_kind;
                    rec.pending_closure_imm = fd.jit_inline_imm;
                    rec.type_vstack.push_back(TraceType::Unknown);
                } else {
                    abort_recording();
                }
            } else {
                abort_recording();
            }
            break;
        }

        case OpCode::OP_STORE_VAR: {
            // STORE_VAR = peek, no pop: interpreter stack unchanged
            uint16_t slot = op16();
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            TraceType t =
                rec.type_vstack.back(); // type of stored value (remains on stack)
            if (t == TraceType::Int) {
                jit::TraceStep s = { Kind::StoreIntVar };
                s.slot = slot;
                rec.steps.push_back(s);
            } else if (t == TraceType::Float) {
                jit::TraceStep s = { Kind::StoreFloatVar };
                s.slot = slot;
                rec.steps.push_back(s);
            } else {
                abort_recording();
            }
            // type_vstack unchanged (value still on interpreter stack)
            break;
        }

        case OpCode::OP_POP:
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back();
            rec.steps.push_back({ Kind::Pop });
            break;

        case OpCode::OP_DUP:
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            rec.type_vstack.push_back(rec.type_vstack.back());
            rec.steps.push_back({ Kind::Dup });
            break;

        case OpCode::OP_LOAD_CONST: {
            uint16_t idx = op16();
            const auto &c = current_function()->constants[idx];
            if (c.type == Constant::Type::INT) {
                jit::TraceStep s{ Kind::LoadIntConst };
                s.int_val = c.as_int;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Int);
            } else if (c.type == Constant::Type::FLOAT) {
                jit::TraceStep s{ Kind::LoadFloatConst };
                s.dbl_val = c.as_float;
                rec.steps.push_back(s);
                rec.type_vstack.push_back(TraceType::Float);
            } else {
                abort_recording();
            }
            break;
        }

        case OpCode::OP_LOAD_ZERO:
            rec.steps.push_back({ Kind::LoadZeroConst });
            rec.type_vstack.push_back(TraceType::Int);
            break;

        case OpCode::OP_LOAD_ONE:
            rec.steps.push_back({ Kind::LoadOneConst });
            rec.type_vstack.push_back(TraceType::Int);
            break;

        case OpCode::OP_LOAD_GLOBAL: {
            uint16_t name_idx = op16();
            const std::string &name = chunk->strings[name_idx];
            if (name == "math") {
                rec.steps.push_back({ Kind::LoadMathGlobal });
                rec.type_vstack.push_back(TraceType::MathObject);
            } else if (rec.pending_inline_func == nullptr) {
                // try to defer as an inline call target for a subsequent OP_CALL,
                // only save if the global is a JIT-compiled function with metadata
                auto git = globals.find(name);
                if (git != globals.end() && git->second.is_function()) {
                    FunctionData &fd = git->second.get_function();
                    if (fd.jit_meta != nullptr) {
                        rec.pending_inline_func = fd.jit_meta;
                        // Don't push to type_vstack; func value is deferred.
                        break;
                    }
                }
                abort_recording();
            } else {
                // second LOAD_GLOBAL while one is pending; too complex to inline.
                abort_recording();
            }
            break;
        }

        // LOAD_GLOBAL + CALL peephole
        // when a prior LOAD_GLOBAL saved a pending_inline_func, analyse the callee's
        // bytecode for a simple 2-arg math pattern and emit the net op
        case OpCode::OP_CALL: {
            // deferred local closure call
            if (rec.pending_closure_capture0 != nullptr) {
                uint8_t argc = insn_base[1];
                if (argc == 0 && !rec.type_vstack.empty()) {
                    rec.type_vstack.pop_back(); // pop the function placeholder
                    Kind kind = (rec.pending_closure_kind == JitInlineKind::ClosureAddConst)
                                    ? Kind::ClosureAddConst
                                    : Kind::ClosureInc;
                    jit::TraceStep s{ kind };
                    s.capture_ptr = rec.pending_closure_capture0;
                    s.closure_slot = rec.pending_closure_slot;
                    s.int_val = rec.pending_closure_imm;
                    rec.steps.push_back(s);
                    rec.type_vstack.push_back(TraceType::Int); // result is always Int
                    rec.pending_closure_capture0 = nullptr;
                    rec.pending_closure_slot = 0;
                    rec.pending_closure_kind = JitInlineKind::None;
                    rec.pending_closure_imm = 0;
                    rec.inline_depth = 1; // skip the inlined closure body
                    break;
                }
                rec.pending_closure_capture0 = nullptr;
                rec.pending_closure_slot = 0;
                rec.pending_closure_kind = JitInlineKind::None;
                abort_recording();
                break;
            }
            // --- global function inline (LOAD_GLOBAL + CALL peephole) ---
            if (rec.pending_inline_func != nullptr) {
                FunctionMeta *f = rec.pending_inline_func;
                rec.pending_inline_func = nullptr;
                uint8_t argc = insn_base[1];
                if (argc == 2 && rec.type_vstack.size() >= 2) {
                    TraceType ta = rec.type_vstack[rec.type_vstack.size() - 2]; // first arg
                    TraceType tb =
                        rec.type_vstack[rec.type_vstack.size() - 1]; // second arg
                    const auto &fc = f->code;
                    // Compute the starting offset of the function body, skipping any
                    // strict-mode CHECK_TYPE parameter preamble.
                    // Each preamble entry: LOAD_VAR(3) + CHECK_TYPE(4, ctx=0) +
                    // STORE_VAR(3) + POP(1) = 11 bytes.
                    size_t bo = 0;
                    if (f->strict_mode) {
                        while (bo + 11 <= fc.size()) {
                            if ((OpCode)fc[bo] != OpCode::OP_LOAD_VAR) {
                                break;
                            }
                            if ((OpCode)fc[bo + 3] != OpCode::OP_CHECK_TYPE) {
                                break;
                            }
                            if (fc[bo + 6] != 0) {
                                break; // ctx=0
                            }
                            if ((OpCode)fc[bo + 7] != OpCode::OP_STORE_VAR) {
                                break;
                            }
                            if ((OpCode)fc[bo + 10] != OpCode::OP_POP) {
                                break;
                            }
                            bo += 11;
                        }
                    }
                    // Helper: check if the position ends with [CHECK_TYPE(ctx=1)] RETURN
                    auto is_rt_end = [&](size_t pos) -> bool {
                        if (pos >= fc.size()) {
                            return false;
                        }
                        if ((OpCode)fc[pos] == OpCode::OP_RETURN) {
                            return true;
                        }
                        if (pos + 4 < fc.size() && (OpCode)fc[pos] == OpCode::OP_CHECK_TYPE &&
                            fc[pos + 3] == 1 && (OpCode)fc[pos + 4] == OpCode::OP_RETURN) {
                            return true;
                        }
                        return false;
                    };
                    // Match: LOAD_VAR(0), LOAD_VAR(1), <op>, [CHECK_TYPE] RETURN  (7+ bytes
                    // from body offset)
                    if (bo + 7 <= fc.size() && (OpCode)fc[bo + 0] == OpCode::OP_LOAD_VAR &&
                        fc[bo + 1] == 0 && fc[bo + 2] == 0 &&
                        (OpCode)fc[bo + 3] == OpCode::OP_LOAD_VAR && fc[bo + 4] == 0 &&
                        fc[bo + 5] == 1 && is_rt_end(bo + 7)) {
                        auto op2 = (OpCode)fc[bo + 6];
                        Kind kind;
                        TraceType result_type = TraceType::Int;
                        bool ok = false;
                        if (ta == TraceType::Int && tb == TraceType::Int) {
                            switch (op2) {
                                case OpCode::OP_ADD:
                                    kind = Kind::IntAdd;
                                    ok = true;
                                    break;
                                case OpCode::OP_SUB:
                                    kind = Kind::IntSub;
                                    ok = true;
                                    break;
                                case OpCode::OP_MUL:
                                    kind = Kind::IntMul;
                                    ok = true;
                                    break;
                                case OpCode::OP_DIV:
                                    kind = Kind::IntDiv;
                                    ok = true;
                                    break;
                                case OpCode::OP_MOD:
                                    kind = Kind::IntMod;
                                    ok = true;
                                    break;
                                default:
                                    break;
                            }
                        } else if (ta == TraceType::Float && tb == TraceType::Float) {
                            result_type = TraceType::Float;
                            switch (op2) {
                                case OpCode::OP_ADD:
                                    kind = Kind::FloatAdd;
                                    ok = true;
                                    break;
                                case OpCode::OP_SUB:
                                    kind = Kind::FloatSub;
                                    ok = true;
                                    break;
                                case OpCode::OP_MUL:
                                    kind = Kind::FloatMul;
                                    ok = true;
                                    break;
                                case OpCode::OP_DIV:
                                    kind = Kind::FloatDiv;
                                    ok = true;
                                    break;
                                default:
                                    break;
                            }
                        }
                        if (ok) {
                            rec.type_vstack.pop_back();
                            rec.type_vstack.pop_back();
                            rec.steps.push_back({ kind });
                            rec.type_vstack.push_back(result_type);
                            rec.inline_depth = 1; // skip the inlined function's body
                            break;                // success
                        }
                    }
                }
                abort_recording();
            } else {
                abort_recording(); // CALL without a deferred inline target
            }
            break;
        }

        // arithmetic ops
        case OpCode::OP_ADD:
        case OpCode::OP_SUB:
        case OpCode::OP_MUL:
        case OpCode::OP_DIV:
        case OpCode::OP_MOD: {
            if (rec.type_vstack.size() < 2) {
                abort_recording();
                break;
            }
            TraceType tb = rec.type_vstack.back();
            rec.type_vstack.pop_back();
            TraceType ta = rec.type_vstack.back();
            rec.type_vstack.pop_back();
            TraceType tr = vtype(peek()); // result is now on interpreter stack

            // Promote Int operands to Float for mixed arithmetic
            if (ta == TraceType::Int && tb == TraceType::Float) {
                // ta (TOS-1 at time of op) needs promotion; at compile time it's second
                // from top
                jit::TraceStep coerce = { Kind::IntToFloat };
                coerce.int_val = 1; // TOS-1
                rec.steps.push_back(coerce);
                ta = TraceType::Float;
            } else if (ta == TraceType::Float && tb == TraceType::Int) {
                jit::TraceStep coerce = { Kind::IntToFloat };
                coerce.int_val = 0; // TOS
                rec.steps.push_back(coerce);
                tb = TraceType::Float;
            }

            Kind kind;
            if (ta == TraceType::Int && tb == TraceType::Int && tr == TraceType::Int) {
                if (op == OpCode::OP_ADD) {
                    kind = Kind::IntAdd;
                } else if (op == OpCode::OP_SUB) {
                    kind = Kind::IntSub;
                } else if (op == OpCode::OP_MUL) {
                    kind = Kind::IntMul;
                } else if (op == OpCode::OP_DIV) {
                    kind = Kind::IntDiv;
                } else {
                    kind = Kind::IntMod;
                }
            } else if (ta == TraceType::Float && tb == TraceType::Float &&
                       tr == TraceType::Float) {
                if (op == OpCode::OP_ADD) {
                    kind = Kind::FloatAdd;
                } else if (op == OpCode::OP_SUB) {
                    kind = Kind::FloatSub;
                } else if (op == OpCode::OP_MUL) {
                    kind = Kind::FloatMul;
                } else if (op == OpCode::OP_DIV) {
                    kind = Kind::FloatDiv;
                } else {
                    abort_recording();
                    break;
                } // FloatMod not supported
            } else {
                abort_recording();
                break;
            }
            rec.steps.push_back({ kind });
            rec.type_vstack.push_back(tr);
            break;
        }

        // comparisons
        case OpCode::OP_LT:
        case OpCode::OP_LE:
        case OpCode::OP_GT:
        case OpCode::OP_GE:
        case OpCode::OP_EQ:
        case OpCode::OP_NE: {
            if (rec.type_vstack.size() < 2) {
                abort_recording();
                break;
            }
            TraceType tb = rec.type_vstack.back();
            rec.type_vstack.pop_back();
            TraceType ta = rec.type_vstack.back();
            rec.type_vstack.pop_back();
            // Promote mixed Int/Float operands before comparison
            if (ta == TraceType::Int && tb == TraceType::Float) {
                jit::TraceStep coerce = { Kind::IntToFloat };
                coerce.int_val = 1; // TOS-1
                rec.steps.push_back(coerce);
                ta = TraceType::Float;
            } else if (ta == TraceType::Float && tb == TraceType::Int) {
                jit::TraceStep coerce = { Kind::IntToFloat };
                coerce.int_val = 0; // TOS
                rec.steps.push_back(coerce);
                tb = TraceType::Float;
            }
            Kind kind;
            if (ta == TraceType::Int && tb == TraceType::Int) {
                if (op == OpCode::OP_LT) {
                    kind = Kind::IntLt;
                } else if (op == OpCode::OP_LE) {
                    kind = Kind::IntLe;
                } else if (op == OpCode::OP_GT) {
                    kind = Kind::IntGt;
                } else if (op == OpCode::OP_GE) {
                    kind = Kind::IntGe;
                } else if (op == OpCode::OP_EQ) {
                    kind = Kind::IntEq;
                } else {
                    kind = Kind::IntNe;
                }
            } else if (ta == TraceType::Float && tb == TraceType::Float) {
                if (op == OpCode::OP_LT) {
                    kind = Kind::FloatLt;
                } else if (op == OpCode::OP_LE) {
                    kind = Kind::FloatLe;
                } else if (op == OpCode::OP_GT) {
                    kind = Kind::FloatGt;
                } else if (op == OpCode::OP_GE) {
                    kind = Kind::FloatGe;
                } else if (op == OpCode::OP_EQ) {
                    kind = Kind::FloatEq;
                } else {
                    kind = Kind::FloatNe;
                }
            } else {
                abort_recording();
                break;
            }
            rec.steps.push_back({ kind });
            rec.type_vstack.push_back(TraceType::Bool);
            break;
        }

        case OpCode::OP_NOT: {
            // Logical NOT of a Bool/Int value
            // typically we just emit NOT, but on a lazy comparison the compiler just inverts condition codes
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            TraceType t = rec.type_vstack.back();
            if (t != TraceType::Bool && t != TraceType::Int) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back();
            rec.steps.push_back({ Kind::Not });
            rec.type_vstack.push_back(TraceType::Bool);
            break;
        }

        // loop exit / body branch
        case OpCode::OP_JUMP_IF_FALSE: {
            // Condition was already popped by the instruction.
            // Compute exit_pc = where we'd jump when condition is false.
            int16_t offset =
                (int16_t)((uint16_t(insn_base[1]) << 8) | uint16_t(insn_base[2]));
            uint8_t *code_base = current_function()->code.data();
            size_t this_exit = (size_t)((insn_base + 3 + offset) - code_base);
            if (rec.type_vstack.empty()) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // condition was consumed
            if (rec.exit_pc == 0) {
                // First JUMP_IF_FALSE -> this is the loop-exit condition.
                rec.exit_pc = this_exit;
                rec.steps.push_back({ Kind::CondExitIfFalse });
            } else if (rec.exit_pc == this_exit) {
                // Same exit target (duplicate condition): record normally.
                rec.steps.push_back({ Kind::CondExitIfFalse });
            } else {
                // Body branch (if/else inside loop body).
                // Determine which path was taken during recording.
                size_t next_pc = (size_t)(ip() - code_base);
                size_t no_jump_pc = (size_t)((insn_base + 3) - code_base);
                if (next_pc == no_jump_pc) {
                    // Jump NOT taken -> condition was TRUE.
                    // Guard: exit if FALSE at runtime.
                    jit::TraceStep s = { Kind::SideExitIfFalse };
                    s.fallback_pc = this_exit;
                    rec.steps.push_back(s);
                } else {
                    // Jump taken -> condition was FALSE.
                    // Guard: exit if TRUE at runtime.
                    jit::TraceStep s = { Kind::SideExitIfTrue };
                    s.fallback_pc = no_jump_pc;
                    rec.steps.push_back(s);
                }
            }
            break;
        }

        // Forward OP_JUMP inside a loop body (e.g. skip-over-else after an if body).
        // The forward jump just advances the IP past instructions that were never recorded.
        // Backward OP_JUMP is handled separately in execute_instruction() as LoopBack.
        case OpCode::OP_JUMP: {
            int16_t offset =
                (int16_t)((uint16_t(insn_base[1]) << 8) | uint16_t(insn_base[2]));
            if (offset >= 0) {
                // forward jump: silently skip (no step emitted)
                break;
            }
            // backward jump: handled by the main loop as LoopBack (should not reach here)
            abort_recording();
            break;
        }

        // math method calls
        case OpCode::OP_CALL_METHOD: {
            uint16_t name_idx = (uint16_t(insn_base[1]) << 8) | uint16_t(insn_base[2]);
            uint8_t argc = insn_base[3];
            const std::string &mname = chunk->strings[name_idx];
            if (rec.type_vstack.size() < (size_t)(argc + 1)) {
                abort_recording();
                break;
            }
            for (int i = 0; i < argc; i++) {
                rec.type_vstack.pop_back(); // arg types
            }
            TraceType t_obj = rec.type_vstack.back();
            rec.type_vstack.pop_back();
            if (t_obj != TraceType::MathObject || argc != 1) {
                abort_recording();
                break;
            }
            Kind kind;
            if (mname == "sqrt") {
                kind = Kind::MathSqrt;
            } else if (mname == "sin") {
                kind = Kind::MathSin;
            } else if (mname == "cos") {
                kind = Kind::MathCos;
            } else if (mname == "tan") {
                kind = Kind::MathTan;
            } else if (mname == "floor") {
                kind = Kind::MathFloor;
            } else if (mname == "ceil") {
                kind = Kind::MathCeil;
            } else {
                abort_recording();
                break;
            }
            rec.steps.push_back({ kind });
            rec.type_vstack.push_back(TraceType::Float);
            break;
        }

        // array element access, same idea as object property access below
        //
        // the ArrayObj was popped before the recorder runs
        // so trace_arr_recordable / trace_arr_ptr / trace_arr_size_bytes are resolved
        // while the array is still live
        case OpCode::OP_GET_INDEX: {
            // stack: [..., Array, Int] -> [..., Int]
            if (rec.type_vstack.size() < 2 || !trace_arr_recordable) {
                abort_recording();
                break;
            }
            TraceType t_idx = rec.type_vstack.back();
            TraceType t_arr = rec.type_vstack[rec.type_vstack.size() - 2];
            if (t_arr != TraceType::Array || t_idx != TraceType::Int) {
                abort_recording();
                break;
            }
            TraceType vt = vtype(peek()); // result on top of stack post-execute
            if (vt != TraceType::Int) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // index
            rec.type_vstack.pop_back(); // array
            jit::TraceStep s = { Kind::ArrayGetIdx };
            s.prop_val_type = TraceType::Int;
            // fallback_pc = this instruction's PC. On OOB side-exit we re-push
            // [array, index] and let the interpreter re-execute OP_GET_INDEX
            s.fallback_pc = (size_t)(insn_base - current_function()->code.data());
            rec.steps.push_back(s);
            rec.type_vstack.push_back(TraceType::Int);
            break;
        }

        case OpCode::OP_SET_INDEX: {
            // stack: [..., Array, Int, val] -> [..., val]
            if (rec.type_vstack.size() < 3 || !trace_arr_recordable) {
                abort_recording();
                break;
            }
            TraceType t_val = rec.type_vstack.back();
            TraceType t_idx = rec.type_vstack[rec.type_vstack.size() - 2];
            TraceType t_arr = rec.type_vstack[rec.type_vstack.size() - 3];
            if (t_arr != TraceType::Array || t_idx != TraceType::Int ||
                t_val != TraceType::Int) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // val
            rec.type_vstack.pop_back(); // index
            rec.type_vstack.pop_back(); // array
            jit::TraceStep s = { Kind::ArraySetIdx };
            s.prop_val_type = TraceType::Int;
            // fallback_pc = this instruction's PC. On OOB side-exit we re-push
            // [array, index, val] and let the interpreter re-execute OP_SET_INDEX
            s.fallback_pc = (size_t)(insn_base - current_function()->code.data());
            rec.steps.push_back(s);
            rec.type_vstack.push_back(TraceType::Int); // assignment result
            break;
        }

            // object property access (shape-pointer + slot-index discipline)
            //
            // we record the field's slot index within the shape,
            // the trace lowering recomputes &fields[slot] from the live object on every access.
            // A recorded slot can only ever be used against an object with the exact shape it was recorded for.

        case OpCode::OP_GET_PROPERTY: {
            // stack: [..., Obj] -> [..., value]
            if (rec.type_vstack.empty() ||
                rec.type_vstack.back() != TraceType::Obj ||
                !trace_prop_recordable) {
                abort_recording();
                break;
            }
            TraceType vt = vtype(peek()); // result value now on top of stack
            if (vt != TraceType::Int && vt != TraceType::Float) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // pop Obj
            jit::TraceStep s = { Kind::ObjGetProp };
            s.prop_slot_index = trace_prop_slot;
            s.prop_val_type = vt;
            rec.steps.push_back(s);
            rec.type_vstack.push_back(vt);
            break;
        }

        case OpCode::OP_SET_PROPERTY: {
            // stack: [..., Obj, value] -> [..., value]
            if (rec.type_vstack.size() < 2 || !trace_prop_recordable) {
                abort_recording();
                break;
            }
            TraceType vt = rec.type_vstack.back();                      // value being stored
            TraceType ot = rec.type_vstack[rec.type_vstack.size() - 2]; // object
            if (ot != TraceType::Obj ||
                (vt != TraceType::Int && vt != TraceType::Float)) {
                abort_recording();
                break;
            }
            rec.type_vstack.pop_back(); // value
            rec.type_vstack.pop_back(); // object

            jit::TraceStep s = { Kind::ObjSetProp };
            s.prop_slot_index = trace_prop_slot;
            s.prop_val_type = vt;
            rec.steps.push_back(s);
            rec.type_vstack.push_back(vt); // value pushed back as result
            break;
        }

        // abort trace if there's an unrecognized opcode
        default:
            abort_recording();
            break;
    }
}
#endif // !DISABLE_JIT

Value VM::call_function_value_sync(const Value &func_val, const std::vector<Value> &args) {
    if (!func_val.is_function()) {
        return Value::none();
    }
    const auto &fn = func_val.get_function();

    // Resolve the bytecode function index,
    // this runs on every trap invocation for Delegate handlers
    // cache the resolved index on the FunctionData to avoid re-hashing the function name on each call
    uint32_t func_idx;
    if (fn.jit_func_idx >= 0) {
        func_idx = static_cast<uint32_t>(fn.jit_func_idx);
    } else {
        auto it = func_indices.find(fn.name);
        if (it == func_indices.end()) {
            return Value::none();
        }
        const_cast<FunctionData &>(fn).jit_func_idx = static_cast<int32_t>(it->second);
        func_idx = it->second;
    }

    size_t saved_frame_depth = frames.size();
    size_t saved_stack_size = stack.size();

    // Push a new call frame for this function.
    // Pass the closure's captures as the 4th arg so the frame owns them BEFORE the body runs.
    if (fn.captures && !fn.captures->empty()) {
        call_user_function(func_idx, args, nullptr, fn.captures);
    } else {
        call_user_function(func_idx, args);
    }

    // Execute instructions until this function returns
    while (frames.size() > saved_frame_depth) {
        if (!execute_instruction()) {
            break;
        }
    }

    Value result = Value::none();
    if (stack.size() > saved_stack_size) {
        result = pop();
    }

    return result;
}

bool VM::dispatch_throw(Value error) {
    // capture the call stack now, before any frame unwinding, so it can be printed if the exception turns out to be uncaught.
    std::vector<std::string> trace;
    trace.reserve(frames.size());
    for (int fi = (int)frames.size() - 1; fi >= 0; fi--) {
        const CallFrame &frame = frames[fi];
        if (!frame.function) {
            continue;
        }
        const std::string &fn_name = (frame.function->name.empty() || frame.function->name == "<main>") ? "<top level>" : frame.function->name;
        size_t pc_offset = (size_t)(frame.ip - frame.function->code.data());
        // if ip points one byte past the last consumed byte, back off by 1.
        if (pc_offset > 0) {
            pc_offset--;
        }
        int src_line = frame.function->resolve_line(pc_offset);
        std::string entry = "  at " + fn_name;
        if (!frame.function->source_file.empty()) {
            entry += " (" + frame.function->source_file;
            if (src_line > 0) {
                entry += ":" + std::to_string(src_line);
            }
            entry += ")";
        }
        trace.push_back(std::move(entry));
    }
    while (!try_stack.empty()) {
        TryHandler handler = try_stack.back();
        try_stack.pop_back();

        // basic check for a stale handler, we obviously can't target a frame that no longer exists
        if (handler.frame_depth > frames.size()) {
            fprintf(stderr, "warning: discarding stale try handler (frame_depth=%zu, size=%zu)\n", handler.frame_depth, frames.size());
            continue;
        }

        // unwind call frames back to the frame that installed the handler.
        while (frames.size() > handler.frame_depth) {
            size_t slot_base = current_frame().slot_base;
            frames.pop_back();
            if (!frames.empty()) {
                stack.resize(slot_base);
            }
        }

        // after unwinding, we must have at least one frame to land in.
        if (frames.empty()) {
            fprintf(stderr, "warning: try handler left no call frame; treating as uncaught!\n");
            break;
        }

        // stack_depth must not exceed the current unwound stack size, and we can't grow it here
        if (handler.stack_depth > stack.size()) {
            fprintf(stderr, "warning: try handler stack_depth=%zu > stack.size()=%zu; clamping\n", handler.stack_depth, stack.size());
            handler.stack_depth = stack.size();
        }
        stack.resize(handler.stack_depth);

        // catch_ip must be a valid offset inside the current function
        FunctionMeta *fn = current_function();
        if (!fn || handler.catch_ip >= fn->code.size()) {
            fprintf(stderr, "warning: try handler catch_ip=%zu out of range (code.size()=%zu)\n", handler.catch_ip, fn ? fn->code.size() : 0);
            break;
        }

        // push error value for the catch block and then jump to the handler.
        VM::push(error);
        ip() = fn->code.data() + handler.catch_ip;
        return true; // caught
    }

    // flush any remaining stale handlers so they cannot cause issues for a top level panic
    try_stack.clear();

    fprintf(stderr, "panic: %s\n", error.to_string().c_str());
    for (const auto &entry : trace) {
        fprintf(stderr, "%s\n", entry.c_str());
    }
    has_error = true;
    return false;
}

void VM::poll_io() {
    runtime->process_completed_io();
}

bool VM::run(Chunk *compiled_chunk) {
    if (!compiled_chunk || compiled_chunk->functions.empty()) {
        return false;
    }

    chunk = compiled_chunk;

#ifndef DISABLE_JIT
    // The trace JIT has no chunk identity in its cache key (uses (func_idx, anchor_pc))
    // cached traces also bake absolute bytecode IPs from chunk.functions[i].code.data().
    // When reusing g_trace_jit across distinct chunks, invalidate everything before executing the new chunk.
    if (jit::g_trace_jit) {
        jit::g_trace_jit->reset();
    }
#endif

    // register user-defined functions by name -> index
    for (size_t i = 0; i < chunk->functions.size(); i++) {
        FunctionMeta &fmeta = chunk->functions[i];
        const std::string &name = fmeta.name;
        if (name != "<main>" && !name.empty()) {
            func_indices[name] = i;
            // also register as global so OP_LOAD_GLOBAL can find them
            globals[name] = Value::make_function(name);
            // Populate JIT metadata now so the trace JIT can inline calls to these functions without aborting the trace
            FunctionData &fd = globals[name].get_function();
            fd.jit_func_idx = i;
            fd.jit_locals_count = fmeta.var_names.size();
            fd.jit_meta = &fmeta;

            auto cls = jit_classify_inline(fmeta);
            fd.jit_inline_kind = cls.kind;
            fd.jit_inline_imm = cls.imm;

            std::string local_alias = Parser::get_exported_function_local_name(name);
            if (!local_alias.empty()) {
                globals[local_alias] = globals[name]; // copy with all metadata intact
            }
        }
    }

    // set up FFI callback dispatch so native callbacks can re-enter the VM
    runtime->external_call_function_value = [&](const Value &func_val, const std::vector<Value> &args) -> Value {
        return call_function_value_sync(func_val, args);
    };

    runtime->external_global_lookup = [&](const std::string &name) -> Value {
        return get_global(name);
    };

    // call __stdlib_init__() if it exists to populate the system object
    auto stdlib_init_it = func_indices.find("__stdlib_init__");
    if (stdlib_init_it != func_indices.end()) {
        std::vector<Value> empty_args;
        call_user_function(stdlib_init_it->second, empty_args);

        // execute __stdlib_init__() function
        while (!frames.empty()) {
            if (!execute_instruction()) {
                break;
            }

            // check for end of current function
            size_t code_offset = ip() - current_function()->code.data();
            if (code_offset >= current_function()->code.size()) {
                break;
            }
        }
    }

    // build indexed global cache now that all globals (builtins + user funcs + __stdlib_init__) are registered
    rebuild_global_cache();

    // set up main function frame
    FunctionMeta *main_func = &chunk->functions[chunk->main_func_idx];
    frames.emplace_back();
    CallFrame &frame = frames.back();
    frame.function = main_func;
    frame.ip = main_func->code.data();
    frame.slot_base = 0;

    // allocate space for local variables
    size_t locals_needed = main_func->var_names.size();
    for (size_t i = 0; i < locals_needed; i++) {
        stack.push_back(Value::none());
    }

#ifndef DISABLE_JIT
    // attempt to eagerly compile user functions that contain backward jumps (loops) but no method calls AND at least one OP_CALL
    if (jit::g_jit_compiler) {
        // Simple per-opcode size table for scanning.
        // Returns the total byte footprint of an instruction (opcode + operands).
        auto opcode_size = [](OpCode op) -> size_t {
            switch (op) {
                // 1-byte (no operands)
                case OpCode::OP_POP:
                case OpCode::OP_DUP:
                case OpCode::OP_LOAD_NONE:
                case OpCode::OP_LOAD_TRUE:
                case OpCode::OP_LOAD_FALSE:
                case OpCode::OP_LOAD_ZERO:
                case OpCode::OP_LOAD_ONE:
                case OpCode::OP_ADD:
                case OpCode::OP_SUB:
                case OpCode::OP_MUL:
                case OpCode::OP_DIV:
                case OpCode::OP_MOD:
                case OpCode::OP_POW:
                case OpCode::OP_NEG:
                case OpCode::OP_STR_CONCAT:
                case OpCode::OP_BIT_AND:
                case OpCode::OP_BIT_OR:
                case OpCode::OP_BIT_XOR:
                case OpCode::OP_BIT_NOT:
                case OpCode::OP_LSHIFT:
                case OpCode::OP_RSHIFT:
                case OpCode::OP_NOT:
                case OpCode::OP_EQ:
                case OpCode::OP_NE:
                case OpCode::OP_LT:
                case OpCode::OP_LE:
                case OpCode::OP_GT:
                case OpCode::OP_GE:
                case OpCode::OP_RETURN:
                case OpCode::OP_GET_INDEX:
                case OpCode::OP_SET_INDEX:
                case OpCode::OP_MAKE_ITERATOR:
                case OpCode::OP_ITER_NEXT:
                case OpCode::OP_MAKE_ITERATOR_KV:
                case OpCode::OP_ITER_NEXT_KV:
                case OpCode::OP_ITER_ARRAY:
                case OpCode::OP_THROW:
                case OpCode::OP_POP_TRY:
                case OpCode::OP_BEGIN_CATCH:
                case OpCode::OP_BEGIN_FINALLY:
                case OpCode::OP_LOAD_THIS:
                case OpCode::OP_SPAWN:
                case OpCode::OP_ARRAY_PUSH:
                case OpCode::OP_ARRAY_SPREAD:
                case OpCode::OP_OBJECT_SPREAD:
                case OpCode::OP_CALL_SPREAD:
                    return 1;
                // 2-byte (1-byte operand)
                case OpCode::OP_CALL:
                case OpCode::OP_SELF_TAIL_CALL:
                    return 2;
                // 3-byte (uint16 operand)
                case OpCode::OP_LOAD_CONST:
                case OpCode::OP_LOAD_VAR:
                case OpCode::OP_STORE_VAR:
                case OpCode::OP_LOAD_GLOBAL:
                case OpCode::OP_STORE_GLOBAL:
                case OpCode::OP_FORMAT_VALUE:
                case OpCode::OP_STR_APPEND_VAR:
                case OpCode::OP_JUMP:
                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE:
                case OpCode::OP_JUMP_IF_NONE:
                case OpCode::OP_MAKE_ARRAY:
                case OpCode::OP_MAKE_OBJECT:
                case OpCode::OP_OBJECT_SET:
                case OpCode::OP_GET_PROPERTY:
                case OpCode::OP_SET_PROPERTY:
                case OpCode::OP_LOAD_CAPTURE:
                case OpCode::OP_STORE_CAPTURE:
                case OpCode::OP_STR_APPEND_GLOBAL:
                    return 3;
                // 4-byte
                case OpCode::OP_CALL_METHOD:  // uint16 name_idx + uint8 argc
                case OpCode::OP_NEW_INSTANCE: // uint16 class_name_idx + uint8 argc
                case OpCode::OP_CHECK_TYPE:   // uint16 type_str_idx + uint8 context
                    return 4;
                case OpCode::OP_MAKE_CLOSURE:
                    return 4;
                // 5-byte: uint16 catch_offset + uint16 finally_offset
                case OpCode::OP_SETUP_TRY:
                // 5-byte: uint16 pattern_idx + uint16 flags_idx
                case OpCode::OP_MAKE_REGEX:
                    return 5;
                default:
                    return 1; // unknown: assume 1-byte to avoid skipping too far
            }
        };

        for (size_t i = 0; i < chunk->functions.size(); i++) {
            const FunctionMeta &fm = chunk->functions[i];
            if (fm.name.empty() || fm.name == "<main>") {
                continue;
            }

            bool has_make_iterator = false;
            int backward_jump_count = 0;
            const auto &code = fm.code;
            size_t pc = 0;
            // Track the last LOAD_GLOBAL seen (for LOAD_GLOBAL + args + CALL detection).
            // Reset only on non-argument-loading instructions (not LOAD_VAR/LOAD_CONST etc.).
            bool pending_global = false;
            uint16_t pending_global_name_idx = 0;
            bool pending_load_var = false; // tracks LOAD_VAR preceding a CALL (potential closure)
            // collect positions of non-inlineable calls, method calls, and backward jumps to determine which are inside loop bodies.
            std::vector<size_t> non_inlineable_call_pcs;
            std::vector<size_t> call_method_pcs;
            std::vector<size_t> string_op_pcs; // string ops the trace JIT can't handle
            struct BwJump {
                size_t jump_pc;
                size_t target_pc;
            };
            std::vector<BwJump> bw_jumps;
            while (pc < code.size()) {
                OpCode op2 = static_cast<OpCode>(code[pc]);
                if (op2 == OpCode::OP_CALL) {
                    uint8_t argc = (pc + 1 < code.size()) ? code[pc + 1] : 255;
                    bool inlineable = false;
                    if (pending_global) {
                        const std::string &gname = chunk->strings[pending_global_name_idx];
                        auto it = globals.find(gname);
                        if (it != globals.end() && it->second.is_function()) {
                            FunctionData &fd = it->second.get_function();
                            if (fd.jit_meta != nullptr &&
                                fd.jit_inline_kind != JitInlineKind::None) {
                                inlineable = true;
                            }
                        }
                    }
                    // LOAD_VAR + CALL 0: potential 0-arg closure call (ClosureInc etc)
                    if (!inlineable && pending_load_var && argc == 0) {
                        inlineable = true;
                    }
                    if (!inlineable) {
                        non_inlineable_call_pcs.push_back(pc);
                    }
                    pending_global = false;
                    pending_load_var = false;
                } else if (op2 == OpCode::OP_CALL_METHOD) {
                    call_method_pcs.push_back(pc);
                    pending_global = false;
                    pending_load_var = false;
                } else if (op2 == OpCode::OP_LOAD_GLOBAL && pc + 2 < code.size()) {
                    if (pending_global) {
                        pending_global = false;
                        non_inlineable_call_pcs.push_back(pc); // double LOAD_GLOBAL
                    } else {
                        pending_global = true;
                        pending_global_name_idx =
                            (uint16_t(code[pc + 1]) << 8) | uint16_t(code[pc + 2]);
                    }
                } else if (
                    op2 != OpCode::OP_LOAD_VAR && op2 != OpCode::OP_LOAD_CONST &&
                    op2 != OpCode::OP_LOAD_ONE && op2 != OpCode::OP_LOAD_ZERO &&
                    op2 != OpCode::OP_LOAD_NONE && op2 != OpCode::OP_LOAD_TRUE &&
                    op2 != OpCode::OP_LOAD_FALSE &&
                    op2 != OpCode::OP_LOAD_CAPTURE) {
                    pending_global = false;
                    pending_load_var = false;
                }
                if (op2 == OpCode::OP_LOAD_VAR) {
                    pending_load_var = true;
                }
                if (op2 == OpCode::OP_MAKE_ITERATOR || op2 == OpCode::OP_MAKE_ITERATOR_KV) {
                    has_make_iterator = true;
                }
                if (op2 == OpCode::OP_STR_APPEND_VAR || op2 == OpCode::OP_STR_APPEND_GLOBAL || op2 == OpCode::OP_STR_CONCAT) {
                    string_op_pcs.push_back(pc);
                }
                if (op2 == OpCode::OP_JUMP && pc + 2 < code.size()) {
                    int16_t off = static_cast<int16_t>((uint16_t(code[pc + 1]) << 8) | uint16_t(code[pc + 2]));
                    if (off < 0) {
                        backward_jump_count++;
                        size_t target_pc = pc + 3 + off; // target of backwards jump
                        bw_jumps.push_back({ pc, target_pc });
                    }
                }
                pc += opcode_size(op2);
            }

            // determine which calls are inside loop bodies
            auto in_any_loop = [&](size_t cpc) -> bool {
                for (const auto &bj : bw_jumps) {
                    if (cpc >= bj.target_pc && cpc <= bj.jump_pc) {
                        return true;
                    }
                }
                return false;
            };

            bool has_non_inlineable_call_in_loop = false;
            bool has_non_inlineable_call_outside_loop = false;
            for (size_t cpc : non_inlineable_call_pcs) {
                if (in_any_loop(cpc)) {
                    has_non_inlineable_call_in_loop = true;
                } else {
                    has_non_inlineable_call_outside_loop = true;
                }
            }

            bool has_call_method_in_loop = false;
            for (size_t cpc : call_method_pcs) {
                if (in_any_loop(cpc)) {
                    has_call_method_in_loop = true;
                    break;
                }
            }

            bool has_string_op_in_loop = false;
            for (size_t cpc : string_op_pcs) {
                if (in_any_loop(cpc)) {
                    has_string_op_in_loop = true;
                    break;
                }
            }
            /*
                eager-compile when the trace JIT won't be sufficient:
                    - >=2 backward jumps: nested loops need the method JIT
                    - non-inlineable OP_CALL inside the loop: trace JIT will abort
                    - same with OP_CALL outside loop + CALL_METHOD in loop, trace JIT aborts on CALL_METHOD
                    - string operations in a loop: trace JIT aborts on string LOAD_CONST

                    TODO: we exclude functions with OP_MAKE_ITERATOR because
                          the method JIT has a known stack-layout bug when 'continue' is used
                          inside a for-in body (multiple backward jumps to OP_ITER_NEXT).
            */

            const bool needs_eager =
                !has_make_iterator && (backward_jump_count >= 2 ||
                                       (backward_jump_count >= 1 &&
                                        (has_non_inlineable_call_in_loop || has_string_op_in_loop ||
                                         (has_non_inlineable_call_outside_loop && has_call_method_in_loop))));
            if (needs_eager) {
                jit::g_jit_compiler->compile_chunk(*chunk, static_cast<uint32_t>(i));
            }
        }
#ifdef NARI_ENABLE_GDB_JIT
        nari_jit_initial_load_complete();
#endif
    }
#endif

    // Set up longjmp recovery point for stack-overflow detection.
    // C++ exceptions cannot unwind through JIT-generated machine code, so
    // call_user_function_stack/jit_check_call_depth longjmp here instead.
    // longjmp value 1 = uncaught; 2 = caught by Nari try-catch (resume loop).
    std::jmp_buf overflow_jmp_buf;
    overflow_jmp = &overflow_jmp_buf;
    {
        int jmp_val = setjmp(overflow_jmp_buf);
        if (jmp_val != 0) {
            // longjmp skipped any decrement of jit_call_depth on the unwound C++ frames, reset it before continuing
            jit_call_depth = 0;
        }
        if (jmp_val == 1) {
            overflow_jmp = nullptr;
            return false;
        }
        // jmp_val == 0 (first entry) or 2
    }

    // execute instructions until done
    while (true) {
        if (!execute_instruction()) {
            break;
        }

        // check for end of function
        size_t code_offset = ip() - current_function()->code.data();
        if (code_offset >= current_function()->code.size()) {
            break;
        }
    }

    overflow_jmp = nullptr;

    // auto-call start() if it exists
    if (!has_error) {
        auto start_it = func_indices.find("start");
        if (start_it != func_indices.end()) {
            std::jmp_buf start_jmp;
            overflow_jmp = &start_jmp;
            bool start_called = false;
            int start_jmp_val = setjmp(start_jmp);
            if (start_jmp_val != 0) {
                jit_call_depth = 0;
            }
            if (start_jmp_val == 1) {
                overflow_jmp = nullptr;
                return false;
            }
            // start_jmp_val == 0 (first entry) or 2 (caught; resume loop)
            if (!start_called) {
                start_called = true;
                std::vector<Value> empty_args;
                call_user_function(start_it->second, empty_args);
            }
            while (!frames.empty()) {
                if (!execute_instruction()) {
                    break;
                }
                size_t code_offset = ip() - current_function()->code.data();
                if (code_offset >= current_function()->code.size()) {
                    break;
                }
            }
            overflow_jmp = nullptr;
        }
    }

    return !has_error;
}

} // namespace bytecode
} // namespace nari
