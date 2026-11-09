// VM instruction dispatch (split out of bytecode.cpp).
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

namespace {

// extract an int64_t from a Value for the bitwise/shift ops
// ints pass through directly, and floats route through safe_double_to_i64
static inline int64_t value_as_int_safe(const Value &v) noexcept {
    return v.is_int() ? v.get_int() : safe_double_to_i64(v.as_number());
}

#ifdef NARI_EXTENDED_JSRT
static inline int64_t js_signed32(uint32_t value) noexcept {
    return value >= 0x80000000U ? static_cast<int64_t>(value) - 0x100000000LL : static_cast<int64_t>(value);
}

static bool js_to_uint32(VM &vm, const Value &value, uint32_t &result) {
    ScriptRuntime::BuiltinFn to_number = vm.runtime->lookup_builtin_member("__js_to_number");
    Value value_copy = value;
    Value number = vm.call_builtin_member(to_number, &value_copy, 1);
    if (vm.has_error) {
        return false;
    }
    const double numeric = number.as_number();
    if (numeric == 0.0 || !std::isfinite(numeric)) {
        result = 0;
        return true;
    }
    double reduced = std::fmod(std::trunc(numeric), 4294967296.0);
    if (reduced < 0.0) {
        reduced += 4294967296.0;
    }
    result = static_cast<uint32_t>(reduced);
    return true;
}
#endif

;

} // namespace

bool VM::execute_instruction() {
    {
        if (NARI_UNLIKELY(Runtime::g_shutdown_requested.load())) {
            return false;
        }
        if (GarbageCollector::instance().should_collect()) {
            // collect only when the GC has flagged that enough has been allocated.
            // at an instruction boundary the operand stack is at a clean height
            gc_collect_roots();
        }
#ifndef DISABLE_JIT
        // save pointer to start of instruction (before any reads) and its PC offset.
        uint8_t *const insn_base_ptr = ip();
        uint8_t *const code_data_ptr = current_function()->code.data();
        const size_t insn_pc = static_cast<size_t>((insn_base_ptr - code_data_ptr));

        // flip from pending -> active if the target PC has been reached.
        if (trace_recorder.pending_record && insn_pc == trace_recorder.target_pc) {
            trace_recorder.pending_record = false;
            trace_recorder.recording = true;
            trace_recorder.type_vstack.clear();
            trace_recorder.steps.clear();
        }
        const bool trace_was_recording = trace_recorder.recording;
        const uint32_t trace_func_idx = trace_was_recording ? current_function()->self_idx : 0;
#endif

        // Debugger hook
        {
            auto &debug_controller = dbg::DebugController::instance();
            if (debug_controller.enabled()) {
                uint8_t *const dbg_ip = ip();
                FunctionMeta *const dbg_fn = current_function();
                const size_t dbg_pc = dbg_ip - dbg_fn->code.data();
                const int dbg_line = dbg_fn->resolve_line(dbg_pc);
                const std::string dbg_file = dbg::canonicalise_path(dbg_fn->source_file);
                const bool pending_entry_stop = debug_controller.pending_entry_stop();
                if (debug_controller.should_stop(*this, dbg_pc, frames.size(), dbg_line, dbg_file)) {
                    dbg::StopReason reason;
                    if (!debug_controller.has_fired_first_stop() && pending_entry_stop) {
                        reason = dbg::StopReason::Entry;
                    } else if (debug_controller.has_breakpoint(dbg_file, dbg_line)) {
                        reason = dbg::StopReason::Breakpoint;
                    } else {
                        reason = dbg::StopReason::Step;
                    }
                    debug_controller.mark_first_stop_fired();
                    debug_controller.publish_stop_and_wait(debug_controller.snapshot_frames(*this, reason));
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
                if (Value *cell = current_frame().find_open_upvalue(idx)) {
                    VM::push(*cell);
                    break;
                }
                VM::push(stack[current_frame().slot_base + idx]);
                break;
            }

            case OpCode::OP_STORE_VAR: {
                uint16_t idx = read_short();
                Value val = peek();
                stack[current_frame().slot_base + idx] = val;
                // Also update any open upvalue cell so closures see the change
                if (Value *cell = current_frame().find_open_upvalue(idx)) {
                    *cell = val;
                }
                break;
            }

            case OpCode::OP_LOAD_GLOBAL: {
                uint16_t name_idx = read_short();
                if (NARI_LIKELY(name_idx < global_cache_valid.size() && global_cache_valid[name_idx])) {
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
            case OpCode::OP_MODULE_ENTRY: {
                size_t marker_pc = static_cast<size_t>(ip() - current_function()->code.data() - 1);
                uint16_t name_idx = read_short();
                if (NARI_UNLIKELY(gdb_nari_active)) {
                    gdb_nari_note_module(*this, chunk->strings[name_idx], marker_pc);
                }
                break;
            }
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

            case OpCode::OP_ADD:
                jit_add(this); // shared body in jit_helpers.h
                break;

            case OpCode::OP_SUB:
                jit_sub(this); // shared body in jit_helpers.h
                break;

            case OpCode::OP_MUL:
                jit_mul(this); // shared body in jit_helpers.h
                break;

            case OpCode::OP_DIV:
                jit_div(this); // shared body in jit_helpers.h
                break;

            case OpCode::OP_MOD:
                jit_mod(this); // shared body in jit_helpers.h
                break;

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
                        a.set_int(result);
                    }
                } else {
                    a.set_float(std::pow(a.as_number(), b.as_number()));
                }
                stack.pop_back();
                break;
            }

            case OpCode::OP_NEG:
                jit_neg(this); // shared body in jit_helpers.h
                break;

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
                // shared with jit_format_value(); bool return is JIT-only
                (void)jit_format_value_body(this, spec_idx);
                break;
            }

            // in-place string append to a local variable: slot @= pop(), no copy of slot.
            case OpCode::OP_STR_APPEND_VAR: {
                uint16_t idx = read_short();
                Value rhs = pop(); // pop the right-hand side off the stack
                auto &frame = current_frame();
                Value *slot_ptr = &stack[frame.slot_base + idx];
                Value *cell = frame.find_open_upvalue(idx);
                if (cell) {
                    slot_ptr = cell;
                }
                Value &slot = *slot_ptr;
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
                        dst += ((const Value &)rhs).get_string();
                    } else {
                        slot = Value::make_string(slot.to_string() + rhs.to_string());
                    }
                } else {
                    slot = Value::make_string(slot.to_string() + rhs.to_string());
                }
                if (cell) {
                    stack[frame.slot_base + idx] = *cell;
                }
                // keep slot value on top of stack, copy first since push_back may reallocate the stack vector and
                // invalidate `slot`
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
                        dst += ((const Value &)rhs).get_string();
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

            case OpCode::OP_JS_TRUTHY: {
                bool r = is_js_truthy(peek(0));
                peek(0).set_bool(r);
                break;
            }

#ifdef NARI_EXTENDED_JSRT
            case OpCode::OP_JS_BIT_NOT: {
                uint32_t value;
                if (!js_to_uint32(*this, peek(0), value)) {
                    return false;
                }
                peek(0).set_int(js_signed32(~value));
                break;
            }

            case OpCode::OP_JS_BIT_AND:
            case OpCode::OP_JS_BIT_OR:
            case OpCode::OP_JS_BIT_XOR:
            case OpCode::OP_JS_SHL:
            case OpCode::OP_JS_SHR:
            case OpCode::OP_JS_USHR: {
                uint32_t left;
                uint32_t right;
                if (!js_to_uint32(*this, peek(1), left) || !js_to_uint32(*this, peek(0), right)) {
                    return false;
                }
                stack.pop_back();
                Value &result = peek(0);
                if (op == OpCode::OP_JS_BIT_AND) {
                    result.set_int(js_signed32(left & right));
                } else if (op == OpCode::OP_JS_BIT_OR) {
                    result.set_int(js_signed32(left | right));
                } else if (op == OpCode::OP_JS_BIT_XOR) {
                    result.set_int(js_signed32(left ^ right));
                } else if (op == OpCode::OP_JS_SHL) {
                    result.set_int(js_signed32(left << (right & 31U)));
                } else if (op == OpCode::OP_JS_USHR) {
                    result.set_int(static_cast<int64_t>(left >> (right & 31U)));
                } else {
                    const uint32_t shift = right & 31U;
                    const uint32_t shifted = (left & 0x80000000U) && shift ? ~(~left >> shift) : left >> shift;
                    result.set_int(js_signed32(shifted));
                }
                break;
            }
#endif

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

            case OpCode::OP_STRICT_EQ:
            case OpCode::OP_STRICT_NE: {
                Value &b = peek(0);
                Value &a = peek(1);
                bool r = Value::values_strict_equal(a, b);
                stack.pop_back();
                a.set_bool(op == OpCode::OP_STRICT_EQ ? r : !r);
                break;
            }

            case OpCode::OP_LT: {
                Value &b = peek(0);
                Value &a = peek(1);
                bool r = Value::values_lt(a, b);
                stack.pop_back();
                a.set_bool(r);
                break;
            }

            case OpCode::OP_LE: {
                Value &b = peek(0);
                Value &a = peek(1);
                bool r = Value::values_le(a, b);
                stack.pop_back();
                a.set_bool(r);
                break;
            }

            case OpCode::OP_GT: {
                Value &b = peek(0);
                Value &a = peek(1);
                bool r = Value::values_gt(a, b);
                stack.pop_back();
                a.set_bool(r);
                break;
            }

            case OpCode::OP_GE: {
                Value &b = peek(0);
                Value &a = peek(1);
                bool r = Value::values_ge(a, b);
                stack.pop_back();
                a.set_bool(r);
                break;
            }

            case OpCode::OP_JUMP: {
                int16_t offset = read_signed_short();
#ifndef DISABLE_JIT
                if (offset < 0 && jit_session &&
                    jit_session->on_backedge(*this, insn_pc, static_cast<size_t>((ip() + offset) - code_data_ptr),
                                             trace_was_recording)) {
                    break;
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
                uint16_t callee_label_idx = read_short();

                // args are already on the stack: stack[args_base .. top-1] (pushed left-to-right).
                // The function value sits just below them at stack[args_base-1].
                size_t args_base = stack.size() - argc;
                const Value &func_ref = stack[args_base - 1];

                if (!func_ref.is_function()) {
                    // check if callee is a class name string -> instantiate
                    if (func_ref.is_string()) {
                        std::string class_name = func_ref.get_string(); // copy, SSO buffer is shared
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
                        Value del = stack[args_base - 1];
                        Value result = runtime->delegate_call(del, stack.data() + args_base, argc);
                        stack.resize(args_base - 1);
                        VM::push(std::move(result));
                        break;
                    }
                    const char *actual_type = func_ref.is_none()     ? "null"
                                              : func_ref.is_string() ? "string"
                                              : func_ref.is_int()    ? "int"
                                              : func_ref.is_float()  ? "float"
                                              : func_ref.is_bool()   ? "bool"
                                              : func_ref.is_array()  ? "array"
                                              : func_ref.is_object() ? "object"
                                                                     : "other";
                    runtime_panic(
                        Value::make_string(
                            "called a non-function value: '" + chunk->strings[callee_label_idx] + "' (type " + actual_type + ", value " +
                            func_ref.to_string() + ")"
                        )
                    );
                    return false;
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
                    call_user_function_stack(fn.jit_func_idx, args_base, argc, fn.captures);
                    break;
                }

                const std::string &fname = fn.name;

                // medium path: user-defined bytecode function (first call, populate cache).
                auto fit = func_indices.find(fname);
                if (fit != func_indices.end()) {
                    // cache the index for future fast-path hits (mutable cache field).
                    fn.jit_func_idx = static_cast<int32_t>(fit->second);
                    call_user_function_stack(fit->second, args_base, argc, fn.captures);
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
                    arg_vec.assign(
                        std::make_move_iterator(stack.begin() + args_base), std::make_move_iterator(stack.begin() + args_base + argc)
                    );
                    args_ptr = arg_vec.data();
                }
                // copy func out before trimming (func_ref is a reference into stack[args_base-1]).
                Value func_copy = std::move(stack[args_base - 1]);
                stack.resize(args_base - 1);
                const auto &fn2 = func_copy.get_function();

                ScriptRuntime::BuiltinFn builtin_fn = nullptr;
                if (fn2.jit_builtin_id) {
                    builtin_fn = ScriptRuntime::jit_builtin_table()[fn2.jit_builtin_id];
                }
                auto bit = builtin_fn ? builtins.end() : builtins.find(fn2.name);
                if (builtin_fn || bit != builtins.end()) {
                    Value result = builtin_fn ? call_builtin_member(builtin_fn, args_ptr, argc) : call_builtin(fn2.name, args_ptr, argc);
                    if (!push_builtin_result(std::move(result))) {
                        return false;
                    }
                } else {
                    fprintf(stderr, "bytecode: unknown function '%s'\n", fn2.name.c_str());
                    VM::push(Value::none());
                }
                break;
            }

            case OpCode::OP_SELF_TAIL_CALL: {
                uint8_t argc = read_byte();
                CallFrame &frame = current_frame();
                FunctionMeta *func = frame.function;
                size_t slot_base = frame.slot_base;
                size_t locals_needed = func->var_names.size();
                size_t param_count = func->param_count;

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
                uint16_t capture_count = read_short();
                FunctionMeta *func = &chunk->functions[func_idx];
                CapturesList captures;
                CellRef single_cell;
                for (uint16_t i = 0; i < capture_count; i++) {
                    uint8_t source = read_byte();
                    uint16_t idx = read_short();
                    CellRef cell;
                    if (source == 0) {
                        // parent local: get or create cell from parent frame
                        cell = current_frame().get_or_create_cell(idx, stack[current_frame().slot_base + idx]);
                        // keep local in sync with cell
                        stack[current_frame().slot_base + idx] = *cell;
                    } else if (source == 1) {
                        // parent capture: share the same cell
                        auto &parent_caps = current_frame().captures;
                        if (parent_caps && idx < parent_caps->size()) {
                            cell = (*parent_caps)[idx];
                        } else {
                            cell = CellRef::make(Value::none());
                        }
                    } else {
                        // global
                        const std::string &name = chunk->strings[idx];
                        cell = CellRef::make(get_global(name));
                    }
                    if (capture_count == 1 && source != 2) {
                        single_cell = std::move(cell);
                    } else {
                        if (!captures) {
                            captures = std::make_shared<std::vector<CellRef>>(capture_count);
                        }
                        (*captures)[i] = std::move(cell);
                    }
                }
                if (capture_count == 1 && single_cell) {
                    captures = current_frame().single_capture_cache;
                    if (!captures || captures->size() != 1 || (*captures)[0] != single_cell) {
                        captures = std::make_shared<std::vector<CellRef>>();
                        captures->push_back(std::move(single_cell));
                        current_frame().single_capture_cache = captures;
                    }
                }
                // create a function value with captures
                Value closure = Value::make_function(func->name);
                closure.get_function().captures = std::move(captures);
                closure.get_function().cache_jit_captures();
                GarbageCollector::instance().track(&closure.get_function(), GarbageCollector::TrackedType::Function);
                closure.get_function().jit_func_idx = static_cast<int32_t>(func_idx);
                closure.get_function().jit_locals_count = static_cast<uint32_t>(chunk->functions[func_idx].var_names.size());
                closure.get_function().jit_meta = &chunk->functions[func_idx];
                {
                    const FunctionMeta &m_ = chunk->functions[func_idx];
                    closure.get_function().jit_param_count = static_cast<uint8_t>(m_.param_count);
                    closure.get_function().jit_rest_param_index = static_cast<int8_t>(m_.rest_param_index);
                    closure.get_function().jit_js_undefined_params = m_.js_undefined_params;
                }
                closure.get_function().jit_inline_kind = func->jit_inline_kind;
                closure.get_function().jit_inline_imm = func->jit_inline_imm;
                VM::push(closure);
                break;
            }

            case OpCode::OP_SPAWN: {
                // sync spawn: collector bookkeeping is not thread-safe, so a real thread would race on shared heap.
                // result is wrapped in a Handle still
                Value func_val = pop();
                auto handle = Value::make_handle_ptr();
                handle->state = HandleData::Running;
                // func_val and the handle are live across call_function_value_sync, which runs nested bytecode.
                // Root them so a collection there can't sweep them. handle_val owns the handle for marking.
                Value handle_val = Value::make_handle(handle);
                {
                    namespace chrono = std::chrono;

                    TempRootScope rs(*this);
                    rs.add(&func_val);
                    rs.add(&handle_val);
                    // No exceptions: every throwing std call reachable from nested
                    // execution was replaced with a non-throwing equivalent, and
                    // allocation failure aborts process-wide via the new_handler.
                    Value result = call_function_value_sync(func_val, {});
                    handle->result = result;
                    handle->end_time = chrono::steady_clock::now();
                    handle->state = HandleData::Completed;
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
                uint16_t callee_label_idx = read_short();
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
                    runtime_panic(
                        Value::make_string("called a non-function value with spread arguments: '" + chunk->strings[callee_label_idx] + "'")
                    );
                    return false;
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
                    call_user_function_stack(static_cast<uint32_t>(fn.jit_func_idx), args_base, argc, fn.captures);
                    break;
                }
                const std::string &fname = fn.name;
                auto fit = func_indices.find(fname);
                if (fit != func_indices.end()) {
                    fn.jit_func_idx = static_cast<int32_t>(fit->second);
                    call_user_function_stack(fit->second, args_base, argc, fn.captures);
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
                    arg_vec.assign(
                        std::make_move_iterator(stack.begin() + args_base), std::make_move_iterator(stack.begin() + args_base + argc)
                    );
                    args_ptr = arg_vec.data();
                }
                Value func_copy = std::move(stack[args_base - 1]);
                stack.resize(args_base - 1);
                const auto &fn2 = func_copy.get_function();
                ScriptRuntime::BuiltinFn builtin_fn = nullptr;
                if (fn2.jit_builtin_id) {
                    builtin_fn = ScriptRuntime::jit_builtin_table()[fn2.jit_builtin_id];
                }
                auto bit = builtin_fn ? builtins.end() : builtins.find(fn2.name);
                if (builtin_fn || bit != builtins.end()) {
                    Value result = builtin_fn ? call_builtin_member(builtin_fn, args_ptr, argc) : call_builtin(fn2.name, args_ptr, argc);
                    if (!push_builtin_result(std::move(result))) {
                        return false;
                    }
                } else {
                    fprintf(stderr, "bytecode: unknown function '%s'\n", fn2.name.c_str());
                    VM::push(Value::none());
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
                    auto *array_obj = static_cast<ArrayObj *>(obj.heap_ptr());
                    auto &arr = array_obj->v;
                    if (index.is_int()) {
                        int64_t idx = index.get_int();
                        if (idx >= 0 && idx < static_cast<int64_t>(arr.size())) {
                            VM::push(arr[idx]);
#ifndef DISABLE_JIT
                            // Trace-recordable if the loaded element is an int, or an int48-overflow float
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
                        const Value *v = array_obj->get_property(index.to_string());
                        VM::push(v ? *v : Value::none());
                    }
                } else if (obj.is_object()) {
                    const Value *v;
                    if (index.is_string()) {
                        auto *key = static_cast<StringObj *>(index.heap_ptr());
                        if (key->immutable && key->field_id == UINT32_MAX) {
                            key->field_id = intern_field(key->s);
                        }
                        v = key->immutable ? obj.get_obj_ptr()->get_field_by_id(key->field_id) : obj.get_obj_ptr()->get_field(key->s);
                    } else {
                        v = obj.get_obj_ptr()->get_field(index.to_string());
                    }
                    VM::push(v ? *v : Value::none());
                } else if (obj.is_function()) {
                    const Value *v = obj.get_function().get_property(index.to_string());
                    VM::push(v ? *v : Value::none());
                } else if (obj.is_delegate()) {
                    // Delegate get trap. After the object fast path so prop-IC is untouched.
                    VM::push(runtime->delegate_get(obj, index));
                } else if (obj.is_string()) {
                    // String character indexing: returns a 1-char string (auto-SSO).
                    if (index.is_int()) {
                        const std::string &s = obj.get_string();
                        int64_t idx = index.get_int();
                        if (idx >= 0 && idx < static_cast<int64_t>(s.size())) {
                            VM::push(Value::make_string(std::string(1, s[static_cast<size_t>(idx)])));
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
                    auto *array_obj = static_cast<ArrayObj *>(obj.heap_ptr());
                    auto &arr = array_obj->v;
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
                            if (trace_was_recording && orig_idx >= 0 && (val.is_int() || val.is_float())) {
                                trace_arr_recordable = true;
                                trace_arr_ptr = obj.heap_ptr();
                                trace_arr_size_bytes = arr.size() * sizeof(Value);
                            }
#endif
                        } else if (idx >= static_cast<int64_t>(arr.size()) && idx < 10000000) {
                            arr.resize(static_cast<size_t>(idx) + 1, Value::none());
                            arr[idx] = val;
                        }
                    } else {
                        array_obj->set_property(index.to_string(), val);
                    }
                } else if (obj.is_object()) {
                    if (index.is_string()) {
                        auto *key = static_cast<StringObj *>(index.heap_ptr());
                        if (key->immutable && key->field_id == UINT32_MAX) {
                            key->field_id = intern_field(key->s);
                        }
                        if (key->immutable) {
                            obj.get_obj_ptr()->set_field_by_id(key->field_id, val);
                        } else {
                            obj.get_obj_ptr()->set_field(key->s, val);
                        }
                    } else {
                        obj.get_obj_ptr()->set_field(index.to_string(), val);
                    }
                } else if (obj.is_function()) {
                    obj.get_function().set_property(index.to_string(), val);
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
                    auto &ic = prop_ic[(static_cast<unsigned>(name_idx)) & PROP_IC_MASK];
#ifndef DISABLE_JIT
                    // resolve the field slot for the trace recorder while obj is live
                    if (trace_was_recording) {
                        trace_prop_recordable = false;
                        if (!oobj->dict_mode) {
                            const uint32_t sit = oobj->shape->slot_of(field_id_for_name(name_idx));
                            if (sit != ObjectShape::NoSlot && sit < oobj->fields.size()) {
                                trace_prop_slot = static_cast<uint32_t>(sit);
                                trace_prop_recordable = true;
                            }
                        }
                    }
#endif
                    if (!oobj->dict_mode && ic.shape == oobj->shape && ic.name_idx == name_idx && ic.slot < oobj->fields.size()) {
                        VM::push(*oobj->materialize_lazy_field(ic.slot)); // IC hit
                    } else {
                        Value *v = oobj->get_field_by_id(field_id_for_name(name_idx));
                        if (v) {
                            if (!oobj->dict_mode) {
                                const uint32_t sit = oobj->shape->slot_of(field_id_for_name(name_idx));
                                if (sit != ObjectShape::NoSlot) {
                                    ic.shape = oobj->shape;
                                    ic.name_idx = name_idx;
                                    ic.slot = sit;
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
                } else if (obj.is_function()) {
                    const Value *v = obj.get_function().get_property(chunk->strings[name_idx]);
                    VM::push(v ? *v : Value::none());
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
                            const Value *sc = handle->result.is_object() ? handle->result.get_obj_ptr()->get_field("status_code") : nullptr;
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
                        } else if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
                            // compiler-internal speculative probes stay silent, as for objects below.
                            VM::push(Value::none());
                        } else {
                            bytecode_runtime_fatal(
                                "Unknown handle member '" + name + "' (valid: await, ready, failed, error, status_code, duration)", name
                            );
                            has_error = true;
                            return false;
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
                                has_error = true;
                                return false;
                            }
                        }
                    } else {
                        // Compiler-internal properties (__variant, __data) are used as
                        // speculative probes in match/pattern expressions, silently return none for non-objects.
                        //
                        // For all other properties, this is a fatal user error.
                        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
                            VM::push(Value::none());
                        } else {
                            bytecode_runtime_fatal(
                                "Cannot access property '" + name + "' on " + (obj.is_none() ? "null" : "non-object") + " value", name
                            );
                            has_error = true;
                            return false;
                        }
                    }
                }
                break;
            }

            case OpCode::OP_JS_GET_PROP_STATIC: {
                uint16_t name_idx = read_short();
                jit_js_get_prop_static(this, name_idx);
                break;
            }

            case OpCode::OP_JS_SET_PROP_STATIC: {
                uint16_t name_idx = read_short();
                jit_js_set_prop_static(this, name_idx);
                break;
            }

            case OpCode::OP_JS_POSTINC: {
                uint16_t name_idx = read_short();
                jit_js_postinc(this, name_idx);
                break;
            }

            case OpCode::OP_CLOSE_UPVALUES: {
                uint16_t first_slot = read_short();
                current_frame().close_upvalues_from(first_slot);
                break;
            }

            case OpCode::OP_SET_PROPERTY: {
                uint16_t name_idx = read_short();
                Value val = pop();
                Value obj = pop();

                if (obj.is_object()) {
                    ObjectObj *oobj = static_cast<ObjectObj *>(obj.heap_ptr());
                    auto &ic = prop_ic[(static_cast<unsigned>(name_idx)) & PROP_IC_MASK];
#ifndef DISABLE_JIT
                    // resolve the field slot for the trace recorder while obj is live
                    if (trace_was_recording) {
                        trace_prop_recordable = false;
                        if (!oobj->dict_mode && !oobj->frozen) {
                            const uint32_t sit = oobj->shape->slot_of(field_id_for_name(name_idx));
                            if (sit != ObjectShape::NoSlot && sit < oobj->fields.size()) {
                                trace_prop_slot = static_cast<uint32_t>(sit);
                                trace_prop_recordable = true;
                            }
                        }
                    }
#endif
                    if (!oobj->dict_mode && !oobj->frozen && ic.shape == oobj->shape && ic.name_idx == name_idx &&
                        ic.slot < oobj->fields.size()) {
                        oobj->clear_lazy_field(ic.slot);
                        oobj->fields[ic.slot] = val; // IC hit, direct write
                    } else {
                        oobj->set_field_by_id(field_id_for_name(name_idx), val);
                        if (!oobj->dict_mode) {
                            const uint32_t sit = oobj->shape->slot_of(field_id_for_name(name_idx));
                            if (sit != ObjectShape::NoSlot) {
                                ic.shape = oobj->shape;
                                ic.name_idx = name_idx;
                                ic.slot = sit;
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
                } else if (obj.is_function()) {
                    obj.get_function().set_property(chunk->strings[name_idx], val);
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
                    const ObjectObj *obj = iterable.get_obj_ptr();
                    for (const auto &name : obj->get_keys()) {
                        const Value *val = obj->get_field(name);
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
                    // done iterating, push iterator back (cleaned up by OP_POP)
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
                if (capture_native_throw(error)) {
                    error = Value::none();
                    if (overflow_jmp) {
                        std::longjmp(*overflow_jmp, 4);
                    }
                    return false;
                }
                bool caught = dispatch_throw(error);
                // Inside a JIT-compiled function call, returning normally from execute_instruction()
                // would resume the JIT caller's baked native code
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
                size_t instr_pc = static_cast<size_t>((ip() - current_function()->code.data())) - 1;
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
                if (!ok && (expected == "int" || expected == "float" || expected == "number" || expected == "string" ||
                            expected == "bool" || expected == "null" || expected == "array" || expected == "object" ||
                            expected == "function" || expected == "regex" || val.is_class_instance())) {
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
                    const char *ctx_str = ctx_byte == 0 ? "parameter" : ctx_byte == 1 ? "return value" : "variable";
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
                        std::string("TypeError: expected ") + ctx_str + " of type '" + expected + "', got '" + actual + "'" + loc;
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
                } else if (!current_frame().receiver.is_none()) {
                    VM::push(current_frame().receiver);
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
                const size_t args_base = stack_top - static_cast<size_t>(argc); // index of arg0
                const size_t obj_idx = args_base - 1;                           // index of obj

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
                            // static methods have no 'this'; current_class_name still scopes
                            // static-field and private-member resolution inside the body.
                            Value return_value = Value::none();
                            auto mit = chunk->method_func_idx.find(found);
                            if (mit != chunk->method_func_idx.end()) {
                                auto saved_instance = current_instance;
                                auto saved_class = current_class_name;
                                current_instance = nullptr;
                                current_class_name = class_name;
                                return_value = call_class_function_sync(mit->second, args.data(), args.size(), nullptr);
                                current_instance = saved_instance;
                                current_class_name = saved_class;
                            }
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
                    Value obj = std::move(stack[obj_idx]);
                    if (method_name == "has_key" && argc == 1) {
                        Value key = stack[args_base];
                        bool result = runtime->delegate_has(obj, key);
                        stack.resize(obj_idx);
                        VM::push(Value::make_bool(result));
                    } else {
                        std::vector<Value> args(stack.begin() + args_base, stack.end());
                        stack.resize(obj_idx);
                        VM::push(runtime->delegate_call_method(obj, method_name, std::move(args)));
                    }
                    break;
                }

                if (obj_ref.is_object() || obj_ref.is_function()) {
                    const Value *it_v = nullptr;
                    if (obj_ref.is_object()) {
                        ObjectObj *method_obj = obj_ref.get_obj_ptr();
                        const uint32_t method_slot = method_obj->shape->slot_of(intern_field(method_name));
                        Value lazy_result;
                        if (!method_obj->dict_mode && method_slot != ObjectShape::NoSlot &&
                            method_obj->invoke_lazy_field(method_slot, stack.data() + args_base, argc, lazy_result)) {
                            stack.resize(obj_idx);
                            VM::push(std::move(lazy_result));
                            break;
                        }
                        it_v = method_obj->get_field(method_name);
                    } else {
                        it_v = obj_ref.get_function().get_property(method_name);
                    }
                    if (it_v && it_v->is_function()) {
                        // Object property is a callable, dispatch it.
                        Value obj = obj_ref;
                        Value func = *it_v;
                        std::vector<Value> args(stack.begin() + args_base, stack.end());
                        stack.resize(obj_idx);
                        const std::string &fname = func.get_function().name;

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
                                    call_user_function(fit->second, args, nullptr, captures, &obj);
                                } else {
                                    call_user_function(fit->second, args, nullptr, {}, &obj);
                                }
                            } else {
                                bytecode_runtime_fatal("Method '" + method_name + "' is not callable or is otherwise unknown!", "");
                                has_error = true;
                                return false;
                            }
                        }
                        break;
                    }
                    // Not a callable property, fall through to builtin method dispatch.
                }

                // builtin method call (array, string, object, number, etc.)
                // stack-local buffer avoids heap alloc for argc <= 9.
                {
                    const size_t n = static_cast<size_t>(argc) + 1;
                    Value buf[10];
                    Value *argv;
                    std::vector<Value> overflow;
                    if (n <= 10) {
                        buf[0] = stack[obj_idx];
                        for (size_t i = 0; i < static_cast<size_t>(argc); i++) {
                            buf[i + 1] = stack[args_base + i];
                        }
                        argv = buf;
                    } else {
                        overflow.reserve(n);
                        overflow.push_back(stack[obj_idx]);
                        for (size_t i = 0; i < static_cast<size_t>(argc); i++) {
                            overflow.push_back(stack[args_base + i]);
                        }
                        argv = overflow.data();
                    }
                    stack.resize(obj_idx); // pop obj + args in one shot

                    // inline cache: once resolved, call the builtin member pointer directly,
                    // skipping builtins.find + runtime->call_builtin's own table lookups
                    if (name_idx < method_ic_state.size() && method_ic_state[name_idx] == 1) {
                        if (!push_builtin_result(call_builtin_member(method_ic_fn[name_idx], argv, n))) {
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
                        std::string type_str = ov.is_string()   ? "string"
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
                        has_error = true;
                        return false;
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
            trace_record_step(op, insn_base_ptr, insn_pc, trace_func_idx);
        }
#endif

        if (NARI_UNLIKELY(Runtime::g_runtime_error_occurred.load())) {
            // Native runtime failures are already diagnosed at their source.
            has_error = true;
            return false;
        }
        return true;
    }
}

} // namespace bytecode
} // namespace nari

bool VM::run(Chunk *compiled_chunk) {
    if (!compiled_chunk || compiled_chunk->functions.empty()) {
        return false;
    }

    chunk = compiled_chunk;
#ifndef DISABLE_JIT
    // The trace JIT has no chunk identity in its cache key (uses (func_idx, anchor_pc))
    // cached traces also bake absolute bytecode IPs from chunk.functions[i].code.data().
    // Invalidate cached traces before a new run because compiled exits embed bytecode addresses.
    if (jit_session.get()) {
        trace_recorder.reset();
        jit_session.get()->bind_chunk(*chunk);
    }
#endif

    register_chunk_functions(0);

    // set up FFI callback dispatch so native callbacks can re-enter the VM
    runtime->external_call_function_value = [&](const Value &func_val, const std::vector<Value> &args, const Value *receiver) -> Value {
        return call_function_value_sync(func_val, args, receiver);
    };
    // span form used by the hot delegate-trap path (no vector<Value> creation)
    runtime->external_call_function_value_span = [&](const Value &func_val, const Value *args, size_t argc,
                                                     const Value *receiver) -> Value {
        return call_function_value_span(func_val, args, argc, receiver);
    };
    runtime->external_catch_function_value = [&](const Value &func_val) -> Value { return call_function_value_catching(func_val); };
    runtime->external_global_lookup = [&](const std::string &name) -> Value { return get_global(name); };
    runtime->external_eval_source = [this](const FuncList &funcs, const std::string &entry_name) -> Value {
        return eval_compile_run(funcs, entry_name);
    };

    // Property and method opcodes can run during __stdlib_init__, so their
    // per-string-table caches must exist before entering that function.
    rebuild_global_cache();

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

    runtime->standard_ok_constructor = get_global("Ok");
    runtime->standard_err_constructor = get_global("Err");

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
    if (jit_session.get()) {
#ifndef NARI_DISABLE_GDB_JIT
        nari_jit_initial_load_complete();
#endif
    }
#endif

    // Set up longjmp recovery for fatal errors raised from JIT helpers
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
    if (!has_error && !Runtime::g_shutdown_requested.load()) {
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
            if (start_jmp_val == 3) {
                overflow_jmp = nullptr;
                return true;
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

    run_timer_loop();

    return !has_error;
}
