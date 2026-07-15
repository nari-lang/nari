#ifndef DISABLE_JIT
#include "jit_helpers.h"
#include "compiler_support.h"
#include "int_overflow.h"
#include "io.h"
#include "parser_api.h"
#include "stl_layout.h"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace nari::bytecode;

// Helper functions called from JIT-compiled code.
// Same operations as execute_instruction() but as standalone functions
// so the JIT can call them directly.

template <typename Call>
static auto jit_runtime_call(VM *vm, Call &&call) -> decltype(call()) {
    try {
        auto result = call();
        if (NARI_UNLIKELY(vm->has_error) && vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
        if (NARI_UNLIKELY(vm->runtime->has_pending_throw())) {
            Value err = vm->runtime->take_pending_throw();
            bool caught = vm->dispatch_throw(err);
            vm->has_error = !caught;
            if (vm->overflow_jmp) {
                std::longjmp(*vm->overflow_jmp, caught ? 2 : 1);
            }
        }
        return result;
    } catch (const RuntimeError &) {
        vm->has_error = true;
        if (vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
        return {};
    }
}

template <typename Call>
static void jit_runtime_call_void(VM *vm, Call &&call) {
    try {
        call();
        if (NARI_UNLIKELY(vm->has_error) && vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
        if (NARI_UNLIKELY(vm->runtime->has_pending_throw())) {
            Value err = vm->runtime->take_pending_throw();
            bool caught = vm->dispatch_throw(err);
            vm->has_error = !caught;
            if (vm->overflow_jmp) {
                std::longjmp(*vm->overflow_jmp, caught ? 2 : 1);
            }
        }
    } catch (const RuntimeError &) {
        vm->has_error = true;
        if (vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
    }
}

static inline void jit_abort_on_runtime_error(VM *vm);

extern "C" {

void jit_poll_shutdown(VM *vm) {
    if (NARI_UNLIKELY(Runtime::g_shutdown_requested.load()) && vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, 3);
    }
}

// Push an immediate int/float. Used by the optimizing-IR lowering, whose IConst/
// FConst carry the value directly (not a constant-pool index). make_int_checked
// keeps Nari's int48-overflow-promotes-to-float semantics.
void jit_push_int(VM *vm, int64_t v) {
    vm->push(Value::make_int_checked(v));
}
// Takes the f64 *bit pattern* as int64 (avoids a double-typed call arg in codegen).
void jit_push_float(VM *vm, int64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    vm->push(Value::make_float(d));
}
void jit_push_bool(VM *vm, int64_t v) {
    vm->push(Value::make_bool(v != 0));
}

void jit_load_const(VM *vm, uint32_t idx) {
    auto *func = vm->current_function();
    Constant &c = func->constants[idx];
    switch (c.type) {
        case Constant::Type::NONE:
            vm->push(Value::none());
            break;
        case Constant::Type::INT:
            vm->push(Value::make_int(c.as_int));
            break;
        case Constant::Type::FLOAT:
            vm->push(Value::make_float(c.as_float));
            break;
        case Constant::Type::STRING:
            // shared immutable constant -- no per-load alloc/copy
            vm->push(vm->chunk->get_const_string(c.string_idx));
            break;
        case Constant::Type::FUNCTION:
            vm->push(Value::none());
            break;
    }
}

void jit_load_var(VM *vm, uint32_t idx) {
    auto &frame = vm->current_frame();
    if (frame.open_upvalues) {
        auto it = frame.open_upvalues->find(idx);
        if (it != frame.open_upvalues->end()) {
            Value to_push = *it->second;
            vm->push(std::move(to_push));
            return;
        }
    }
    // push_back may reallocate vm->stack and invalidate the reference, so copy to a local first
    Value to_push = vm->stack[frame.slot_base + idx];
    vm->push(std::move(to_push));
}

void jit_store_var(VM *vm, uint32_t idx) {
    Value val = vm->peek();
    auto &frame = vm->current_frame();
    vm->stack[frame.slot_base + idx] = val;
    if (frame.open_upvalues) {
        auto it = frame.open_upvalues->find(idx);
        if (it != frame.open_upvalues->end()) {
            *it->second = val;
        }
    }
}

// slow path: write directly to slot with no operand-stack traffic.
void jit_slot_store_raw(VM *vm, uint32_t idx, uint64_t raw) {
    Value val = Value::from_raw(raw);
    auto &frame = vm->current_frame();
    vm->stack[frame.slot_base + idx] = val;
    if (frame.open_upvalues) {
        auto it = frame.open_upvalues->find(idx);
        if (it != frame.open_upvalues->end()) {
            *it->second = val;
        }
    }
}
void jit_slot_copy(VM *vm, uint32_t src_idx, uint32_t dst_idx) {
    auto &frame = vm->current_frame();
    Value val = vm->stack[frame.slot_base + src_idx];
    if (frame.open_upvalues) {
        auto it_s = frame.open_upvalues->find(src_idx);
        if (it_s != frame.open_upvalues->end()) {
            val = *it_s->second;
        }
    }
    vm->stack[frame.slot_base + dst_idx] = val;
    if (frame.open_upvalues) {
        auto it_d = frame.open_upvalues->find(dst_idx);
        if (it_d != frame.open_upvalues->end()) {
            *it_d->second = val;
        }
    }
}

void jit_load_global(VM *vm, uint32_t name_idx) {
    // Fast path: the indexed global cache (same one the interpreter's
    // OP_LOAD_GLOBAL uses) skips the get_global name-hash lookup. Hot for globals
    // referenced in loops (e.g. MOD / mix in the benchmark).
    if (NARI_LIKELY(name_idx < vm->global_cache_valid.size() &&
                    vm->global_cache_valid[name_idx])) {
        vm->push(vm->global_cache[name_idx]);
        return;
    }
    vm->push(vm->get_global(vm->chunk->strings[name_idx]));
}

void jit_store_global(VM *vm, uint32_t name_idx) {
    const std::string &name = vm->chunk->strings[name_idx];
    vm->set_global(name, vm->peek());
    // Keep the indexed global cache in sync, exactly like the interpreter's
    // OP_STORE_GLOBAL. jit_load_global now reads this cache, so a stale entry
    // here would make a subsequent load (JIT or interpreter) return the old value.
    if (name_idx < vm->global_cache.size()) {
        vm->global_cache[name_idx] = vm->peek();
        vm->global_cache_valid[name_idx] = 1;
    }
}

void jit_load_capture(VM *vm, uint32_t idx) {
    // Mirror the interpreter's OP_LOAD_CAPTURE (bytecode.cpp), but a JIT-compiled
    // closure carries its captures on vm->jit_captures_raw (a borrowed raw ptr set
    // by every compiled-code entry point), not frame.captures which is left null.
    auto *captures = vm->jit_captures_raw;
    if (captures && idx < captures->size()) {
        vm->push(*(*captures)[idx]); // dereference cell
    } else {
        vm->push(Value::none());
    }
}

void jit_store_capture(VM *vm, uint32_t idx) {
    // Mirror the interpreter's OP_STORE_CAPTURE: write through the cell, leaving the
    // stored value on the stack (peek, no pop). Reads vm->jit_captures_raw.
    auto *captures = vm->jit_captures_raw;
    if (captures && idx < captures->size()) {
        *(*captures)[idx] = vm->peek(); // write through cell
    }
}

void jit_pop(VM *vm) {
    vm->pop();
}
void jit_dup(VM *vm) {
    vm->push(vm->peek());
}

void jit_load_none(VM *vm) {
    vm->push(Value::none());
}
void jit_load_true(VM *vm) {
    vm->push(Value::make_bool(true));
}
void jit_load_false(VM *vm) {
    vm->push(Value::make_bool(false));
}
void jit_load_zero(VM *vm) {
    vm->push(Value::make_int(0));
}
void jit_load_one(VM *vm) {
    vm->push(Value::make_int(1));
}

void jit_add(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        a.inplace_int_checked(a.get_int() + b.get_int());
    } else if (a.is_string() || b.is_string()) {
        a = Value::make_string(a.to_string() + b.to_string());
    } else {
        a.inplace_float(a.as_number() + b.as_number());
    }
    vm->stack.pop_back();
}

void jit_sub(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        a.inplace_int_checked(a.get_int() - b.get_int());
    } else {
        a.inplace_float(a.as_number() - b.as_number());
    }
    vm->stack.pop_back();
}

void jit_mul(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        // fused check: product outside int48 promotes to float in one branch
        int64_t product;
        if (NARI_UNLIKELY(mul_overflow_i48(a.get_int(), b.get_int(), &product))) {
            a.inplace_float(a.as_number() * b.as_number());
        } else {
            a.inplace_int(product);
        }
    } else {
        a.inplace_float(a.as_number() * b.as_number());
    }
    vm->stack.pop_back();
}

void jit_div(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    double bn = b.as_number();
    if (bn == 0.0) {
        a.inplace_float(std::nan(""));
    } else if (a.is_int() && b.is_int()) {
        int64_t av = a.get_int(), bv = b.get_int();
        if (av == INT64_MIN && bv == -1) {
            a.inplace_float(-static_cast<double>(INT64_MIN));
        } else if (av % bv == 0) {
            a.inplace_int(av / bv);
        } else {
            a.inplace_float(a.as_number() / bn);
        }
    } else {
        a.inplace_float(a.as_number() / bn);
    }
    vm->stack.pop_back();
}

void jit_mod(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        int64_t bv = b.get_int();
        if (bv == 0) {
            a.inplace_float(std::nan(""));
        } else {
            int64_t av = a.get_int();
            if (av == INT64_MIN && bv == -1) {
                a.inplace_float(0.0);
            } else {
                a.inplace_int(av % bv);
            }
        }
    } else {
        a.inplace_float(std::fmod(a.as_number(), b.as_number()));
    }
    vm->stack.pop_back();
}

void jit_neg(VM *vm) {
    Value &a = vm->peek(0);
    if (a.is_int()) {
        // -INT48_MIN doesn't fit in int48; inplace_int_checked promotes to
        // float instead of silently wrapping. -get_int() itself is in-range
        // because get_int() returns an int48 (so its negation fits int64).
        a.inplace_int_checked(-a.get_int());
    } else {
        a.inplace_float(-a.as_number());
    }
}

// Append `b`'s string form onto the std::string `out`. Shared by the copying
// and in-place concat helpers so both produce byte-identical results.
static inline void append_concat_rhs(std::string &out, const Value &b) {
    if (b.is_sso()) {
        uint8_t len = b.sso_len();
        out.reserve(out.size() + len);
        for (uint8_t i = 0; i < len; i++) {
            out.push_back(b.sso_char(i));
        }
    } else if (b.is_string()) {
        const std::string &rhs = b.get_string();
        out.reserve(out.size() + rhs.size());
        out.append(rhs);
    } else {
        std::string rhs = b.to_string();
        out.reserve(out.size() + rhs.size());
        out.append(rhs);
    }
}

void jit_str_concat(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    std::string out = a.to_string();
    append_concat_rhs(out, b);
    a = Value::make_string(std::move(out));
    vm->stack.pop_back();
    vm->jit_safepoint(); // concat result is on the stack (rooted) before any collect
}

// In-place variant: the IR pass mark_inplace_concat() proved that `a` (peek(1))
// is the freshly-allocated, single-use result of a preceding StrConcat, so its
// StringObj buffer is uniquely owned and safe to append into. This avoids the
// O(n) prefix copy AND the fresh StringObj allocation that jit_str_concat does
// every call, turning a left-associative concat chain from O(n^2)+n-allocs into
// O(n)+1-alloc. Falls back to the copying path if `a` is unexpectedly not a
// mutable heap string (immutable/interned/SSO/non-string) so it stays correct
// even if an assumption is ever violated.
void jit_str_concat_inplace(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (NARI_LIKELY(a.is_mutable_heap_string())) {
        append_concat_rhs(a.get_string(), b);
        // `a` keeps its existing StringObj (already rooted on the stack); no new
        // allocation, so no safepoint needed here.
        vm->stack.pop_back();
        return;
    }
    // Fallback: identical to jit_str_concat.
    std::string out = a.to_string();
    append_concat_rhs(out, b);
    a = Value::make_string(std::move(out));
    vm->stack.pop_back();
    vm->jit_safepoint();
}

void jit_format_value(VM *vm, uint32_t spec_idx) {
    Value value = vm->pop();
    if (spec_idx == 0xFFFF || spec_idx >= vm->chunk->strings.size()) {
        vm->push(Value::make_string(value.to_string()));
    } else {
        Value args[2] = { value, Value::make_string(vm->chunk->strings[spec_idx]) };
        vm->push(vm->call_builtin("__format_value", args, 2));
        jit_abort_on_runtime_error(vm);
    }
}

void jit_bit_and(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    vm->peek(0).inplace_int(vm->peek(0).get_int() & val);
}

void jit_bit_or(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    vm->peek(0).inplace_int(vm->peek(0).get_int() | val);
}

void jit_bit_xor(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    vm->peek(0).inplace_int(vm->peek(0).get_int() ^ val);
}

void jit_bit_not(VM *vm) {
    vm->peek(0).inplace_int(~vm->peek(0).get_int());
}

void jit_lshift(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    // mask the shift count to [0, 63]
    vm->peek(0).inplace_int(vm->peek(0).get_int() << (val & 63));
}

void jit_rshift(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    // mask the shift count to [0, 63]
    vm->peek(0).inplace_int(vm->peek(0).get_int() >> (val & 63));
}

void jit_not(VM *vm) {
    bool r = !is_truthy(vm->peek(0));
    vm->peek(0).set_bool(r);
}

void jit_eq(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = (a.is_int() && b.is_int()) ? (a.get_int() == b.get_int()) : Value::values_equal(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_ne(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = (a.is_int() && b.is_int()) ? (a.get_int() != b.get_int()) : !Value::values_equal(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_lt(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = (a.is_int() && b.is_int()) ? (a.get_int() < b.get_int()) : (a.as_number() < b.as_number());
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_le(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = (a.is_int() && b.is_int()) ? (a.get_int() <= b.get_int()) : (a.as_number() <= b.as_number());
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_gt(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = (a.is_int() && b.is_int()) ? (a.get_int() > b.get_int()) : (a.as_number() > b.as_number());
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_ge(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = (a.is_int() && b.is_int()) ? (a.get_int() >= b.get_int()) : (a.as_number() >= b.as_number());
    vm->stack.pop_back();
    a.set_bool(r);
}

// returns 1 if top of stack is truthy, then pops
int64_t jit_check_truthy(VM *vm) {
    int64_t result = is_truthy(vm->peek()) ? 1 : 0;
    vm->pop();
    return result;
}

// returns 1 if top of stack is none, then pops
int64_t jit_check_none(VM *vm) {
    int64_t result = vm->peek().is_none() ? 1 : 0;
    vm->pop();
    return result;
}

static inline void jit_check_call_depth(VM *vm) {
    if (vm->frames.size() >= MAX_CALL_DEPTH) {
        Value err = Value::make_string("Stack Overflow: maximum call-depth exceeded!");
        bool caught = vm->dispatch_throw(err);
        vm->has_error = !caught;
        if (vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, caught ? 2 : 1);
        }
    }
}

static inline void jit_deliver_pending_throw(VM *vm) {
    if (NARI_LIKELY(!vm->runtime->has_pending_throw())) {
        return;
    }
    Value err = vm->runtime->take_pending_throw();
    bool caught = vm->dispatch_throw(err);
    vm->has_error = !caught;
    if (vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, caught ? 2 : 1);
    }
}

static inline void jit_abort_on_runtime_error(VM *vm) {
    if (NARI_UNLIKELY(vm->has_error) && vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, 1);
    }
}

// Forward declaration (defined below jit_call_value)
void jit_call(VM *vm, uint32_t argc);

// fast-dispatch call when caller loaded the function from a variable (not a statically-resolved global)
static void jit_call_value_impl(VM *vm, uint32_t argc, uint32_t callee_label_idx) {
    jit_check_call_depth(vm);
    const size_t args_base = vm->stack.size() - argc;
    const size_t slot_base = args_base - 1; // func_val slot

    Value &func_val = vm->stack[slot_base];
    if (!func_val.is_function()) {
        // Delegate call trap: route to jit_call, which handles the delegate
        // call trap (and other non-function callees) uniformly. Stack is still
        // in the pre-call layout, so jit_call reads args/func correctly. After
        // the is_function() fast path so ordinary calls are untouched.
        if (func_val.is_delegate()) {
            jit_call(vm, argc);
            return;
        }
        std::string attempted = func_val.to_string();
        vm->stack.resize(slot_base);
        vm->runtime_panic(Value::make_string("called a non-function value '" + vm->chunk->strings[callee_label_idx] + "'"));
        if (vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
        return;
    }

    FunctionData &fd = func_val.get_function();
    const int32_t fidx = fd.jit_func_idx;

    if (fidx >= 0) {
        FunctionMeta *func = &vm->chunk->functions[(uint32_t)fidx];
        const size_t locals_needed = func->var_names.size();

        // Borrow captures as raw pointer to avoid atomic shared_ptr ownership traffic.
        // should be safe because fd (FunctionData) owns the shared_ptr and stays alive during the callee's execution
        auto *captures_raw = fd.captures.get();

        // in-place arg move: shift args down over the func_val slot.
        for (uint32_t i = 0; i < argc; i++) {
            vm->stack[slot_base + i] = std::move(vm->stack[args_base + i]);
        }

        // resize to exactly locals_needed slots from slot_base.
        vm->stack.resize(slot_base + locals_needed);

        // initialize unfilled param/local slots to None.
        for (size_t i = argc; i < locals_needed; i++) {
            vm->stack[slot_base + i] = Value::none();
        }

        // push call frame; captures left as null shared_ptr (no atomic ops).
        vm->frames.emplace_back();
        CallFrame &frame = vm->frames.back();
        frame.function = func;
        frame.ip = func->code.data();
        frame.slot_base = slot_base;
        // frame.captures stays default (null); we use jit_captures_raw instead.

        // save/restore jit_captures_raw for correct nesting.
        auto *prev_captures = vm->jit_captures_raw;
        vm->jit_captures_raw = captures_raw;

        // dispatch to JIT
        vm->note_jit_callee((uint32_t)fidx); // count + compile hot callees reached only via JIT
        auto compiled = nari::jit::g_jit_compiler->get_compiled_fast((uint32_t)fidx);
        if (compiled) {
            vm->jit_call_depth++;
            compiled(vm);
            vm->jit_call_depth--;
            vm->jit_captures_raw = prev_captures;
            return;
        }

        // not yet compiled, need frame.captures set for interpreter.
        if (captures_raw) {
            // copies shared_ptr for interpreter path
            frame.captures = fd.captures;
        }
        vm->jit_captures_raw = prev_captures;

        // run interpreter loop.
        const size_t target_depth = vm->frames.size() - 1;
        vm->jit_call_depth++;
        while (vm->frames.size() > target_depth) {
            if (!vm->execute_instruction()) {
                break;
            }
        }
        vm->jit_call_depth--;
        jit_abort_on_runtime_error(vm);
        return;
    }

    // unknown func_idx (builtin, AST lambda, non-closure user func without jit_func_idx).
    // fall back to full jit_call; stack hasn't been modified yet.
    jit_call(vm, argc);
}

// Public wrapper: a called value may be an allocating builtin/lambda that never
// hits the interpreter safe-point; poll here on return (result on stack).
void jit_call_value(VM *vm, uint32_t argc, uint32_t callee_label_idx) {
    jit_call_value_impl(vm, argc, callee_label_idx);
    vm->jit_safepoint();
}

// handles the full OP_CALL: pop args, pop func, dispatch, run callee to completion
static void jit_call_impl(VM *vm, uint32_t argc) {
    jit_check_call_depth(vm);
    // read args directly from the VM stack top, no heap alloc
    size_t stack_top = vm->stack.size();
    size_t args_base = stack_top - argc;   // index of arg0 in vm->stack
    Value func = vm->stack[args_base - 1]; // func_val sits just below the args

    if (!func.is_function()) {
        // Delegate call trap: d(args) -> handler.call(target, [args]).
        // After the is_function() fast path so ordinary calls are untouched
        if (func.is_delegate()) {
            Value result = jit_runtime_call(vm, [&] {
                return vm->runtime->delegate_call(func, vm->stack.data() + args_base, argc);
            });
            vm->stack.resize(args_base - 1); // pop args + func
            vm->push(std::move(result));
            return;
        }
        vm->stack.resize(args_base - 1); // pop args + func
        vm->runtime_panic(Value::make_string("attempt to call non-function value: " + func.to_string()));
        if (vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
        return;
    }

    auto &fdata = func.get_function();
    const std::string &fname = fdata.name;
    const auto &func_ptr = fdata.func_ptr;

    // fast path: builtin's member-fn pointer is pre-resolved on FunctionData, skips a name-hash lookup
    if (fdata.jit_builtin_fn_valid) {
        ScriptRuntime::BuiltinFn fn;
        std::memcpy(&fn, fdata.jit_builtin_fn, sizeof(fn));
        Value *argv = (argc > 0) ? &vm->stack[args_base] : nullptr;
        Value result = vm->call_builtin_member(fn, argv, argc);
        vm->stack.resize(args_base - 1); // pop args + func
        vm->push(result);
        jit_abort_on_runtime_error(vm);
        jit_deliver_pending_throw(vm);
        return;
    }

    // user function call cache: `jit_func_idx` is pre-resolved on the FunctionData
    // at registration (VM::run, top-level fns) and at OP_CLOSURE creation
    // so that a repeat call to a user bytecode function can skip builtins.find(fname) and func_indices.find(fname).
    int32_t user_idx = fdata.jit_func_idx;
    if (user_idx < 0) {
        // extension builtins (registered via ScriptRuntime::register_extension)
        // aren't in the member-fn table, so they have no cached pointer.
        auto bit = vm->builtins.find(fname);
        if (bit != vm->builtins.end()) {
            Value *argv = (argc > 0) ? &vm->stack[args_base] : nullptr;
            Value result = vm->call_builtin(fname, argv, argc);
            vm->stack.resize(args_base - 1); // pop args + func
            vm->push(result);
            jit_abort_on_runtime_error(vm);
            jit_deliver_pending_throw(vm);
            return;
        }

        auto fit = vm->func_indices.find(fname);
        if (fit == vm->func_indices.end()) {
            user_idx = -1; // not a user bytecode function; fall to func_ptr path
        } else {
            user_idx = static_cast<int32_t>(fit->second);
            // Populate the IC for future calls.
            const_cast<FunctionData &>(fdata).jit_func_idx = user_idx;
        }
    }

    if (user_idx >= 0) {
        size_t saved_frame_depth = vm->frames.size();
        const auto &captures = func.get_function().captures;

        if (captures && !captures->empty()) {
            // closures need args as vector for existing API.
            std::vector<Value> args(vm->stack.begin() + args_base, vm->stack.end());
            vm->stack.resize(args_base - 1);
            vm->call_user_function(static_cast<uint32_t>(user_idx), args, nullptr,
                                   captures);
        } else {
            // no captures
            // pass args_base and argc; call_user_function_stack reads them and pops func+args itself.
            vm->call_user_function_stack((uint32_t)user_idx, args_base, argc);
        }

        // run the callee to completion (interpreter loop)
        vm->jit_call_depth++;
        while (vm->frames.size() > saved_frame_depth) {
            if (!vm->execute_instruction()) {
                break;
            }
        }
        vm->jit_call_depth--;
        jit_abort_on_runtime_error(vm);
        return;
    }

    // AST lambda / runtime function.
    std::vector<Value> args(vm->stack.begin() + args_base, vm->stack.end());
    vm->stack.resize(args_base - 1);

    if (func_ptr) {
        Value result = jit_runtime_call(vm, [&] { return vm->runtime->call_user_function(func_ptr.get(), args); });
        vm->push(result);
    } else {
        Value func_val_copy = func;
        Value result = jit_runtime_call(vm, [&] { return vm->runtime->call_function_value(func_val_copy, args); });
        vm->push(result);
    }
}

// the called global may be an allocating builtin (to_string, etc.)
// that never re-enters the interpreter safe-point, poll here on return.
void jit_call(VM *vm, uint32_t argc) {
    jit_call_impl(vm, argc);
    vm->jit_safepoint();
}

void jit_call_method(VM *vm, uint32_t method_name_idx, uint32_t argc) {
    const std::string &method_name = vm->chunk->strings[method_name_idx];
    const size_t stack_top = vm->stack.size();
    const size_t args_base = stack_top - argc;
    const size_t obj_idx = args_base - 1;
    Value obj = vm->stack[obj_idx];

    // an object's / class instance's own callable field shadows any builtin member of the same name.
    const Value *method = nullptr;
    if (obj.is_object()) {
        ObjectObj *method_obj = obj.get_obj_ptr();
        if (!method_obj->dict_mode) {
            auto method_slot = method_obj->shape->index.find(intern_field(method_name));
            if (method_slot != method_obj->shape->index.end()) {
                Value lazy_result;
                if (method_obj->invoke_lazy_field(method_slot->second, vm->stack.data() + args_base, argc, lazy_result)) {
                    vm->stack.resize(obj_idx);
                    vm->push(std::move(lazy_result));
                    return;
                }
                // reuse the resolved slot instead of a second get_field lookup
                method = method_obj->materialize_lazy_field(method_slot->second);
            }
        } else {
            method = method_obj->get_field(method_name);
        }
    } else if (obj.is_class_instance()) {
        method = obj.get_class_instance()->get_field(method_name);
    }
    if (method && method->is_function()) {
        vm->stack[obj_idx] = *method;
        jit_call(vm, argc);
        return;
    }

    if (obj.is_delegate()) {
        // delegate method call, comes after the object fast path so shape prop-IC is untouched.
        if (method_name == "has_key" && argc == 1) {
            Value key = vm->stack[args_base];
            bool result = jit_runtime_call(vm, [&] { return vm->runtime->delegate_has(obj, key); });
            vm->stack.resize(obj_idx);
            vm->push(Value::make_bool(result));
        } else {
            std::vector<Value> args(vm->stack.begin() + args_base, vm->stack.end());
            vm->stack.resize(obj_idx);
            vm->push(jit_runtime_call(vm, [&] { return vm->runtime->delegate_call_method(obj, method_name, std::move(args)); }));
        }
        vm->jit_safepoint();
        return;
    }

    ScriptRuntime::BuiltinFn fn = nullptr;
    if (method_name_idx < vm->method_ic_state.size() && vm->method_ic_state[method_name_idx]) {
        fn = vm->method_ic_fn[method_name_idx];
    } else {
        fn = vm->runtime->lookup_builtin_member(method_name);
        if (fn && method_name_idx < vm->method_ic_state.size()) {
            vm->method_ic_fn[method_name_idx] = fn;
            vm->method_ic_state[method_name_idx] = 1;
        }
    }
    if (fn) {
        // small fixed buffer for {receiver, args...}: no heap vector per call
        Value arg_buf[9];
        std::vector<Value> arg_vec;
        const Value *args_ptr;
        if (argc < 9) {
            arg_buf[0] = obj;
            for (uint32_t i = 0; i < argc; i++) {
                arg_buf[1 + i] = vm->stack[args_base + i];
            }
            args_ptr = arg_buf;
        } else {
            arg_vec.reserve((size_t)argc + 1);
            arg_vec.push_back(obj);
            for (uint32_t i = 0; i < argc; i++) {
                arg_vec.push_back(vm->stack[args_base + i]);
            }
            args_ptr = arg_vec.data();
        }
        vm->stack.resize(obj_idx);
        vm->push(vm->call_builtin_member(fn, args_ptr, (size_t)argc + 1));
        jit_abort_on_runtime_error(vm);
        jit_deliver_pending_throw(vm);
        vm->jit_safepoint();
        return;
    }

    vm->stack.resize(obj_idx);
    vm->runtime_panic(Value::make_string("'" + method_name + "' is not a method!"));
    if (vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, 1);
    }
}

// shared shadow-precedence guard for the inline method fast paths below.
static inline bool jit_try_shadow_method(VM *vm, const char *name, uint32_t argc) {
    const size_t obj_idx = vm->stack.size() - argc - 1;
    const Value &obj = vm->stack[obj_idx];
    // Only user objects / class instances can carry a shadowing field.
    const ValueTag t = obj.heap_tag();
    const Value *method = nullptr;
    if (t == ValueTag::Object) {
        method = obj.get_obj_ptr()->get_field(name);
    } else if (t == ValueTag::ClassInstance) {
        method = obj.get_class_instance()->get_field(name);
    } else if (t == ValueTag::Delegate) {
        // A delegate receiver reached a name-dispatched inline method helper (length/char_code_at/starts_with/substr).
        const size_t args_base = obj_idx + 1;
        Value del = obj; // copy before resizing invalidates the stack ref
        if (vm->stack.size() - args_base == 1 && std::strcmp(name, "has_key") == 0) {
            Value key = vm->stack[args_base];
            vm->stack.resize(obj_idx);
            vm->push(Value::make_bool(jit_runtime_call(vm, [&] { return vm->runtime->delegate_has(del, key); })));
            return true;
        }
        std::vector<Value> args(vm->stack.begin() + args_base, vm->stack.end());
        vm->stack.resize(obj_idx);
        vm->push(jit_runtime_call(vm, [&] { return vm->runtime->delegate_call_method(del, name, std::move(args)); }));
        return true;
    } else {
        return false;
    }
    if (method && method->is_function()) {
        vm->stack[obj_idx] = *method;
        jit_call(vm, argc);
        return true;
    }
    return false;
}

void jit_method_length(VM *vm) {
    if (jit_try_shadow_method(vm, "length", 0)) {
        return;
    }
    Value obj = vm->pop();
    if (obj.is_array()) {
        vm->push(Value::make_int((int64_t)obj.get_array().size()));
    } else if (obj.is_string()) {
        vm->push(Value::make_int((int64_t)obj.get_string().size()));
    } else if (obj.is_object()) {
        vm->push(Value::make_int((int64_t)obj.get_obj_ptr()->field_count()));
    } else {
        vm->push(Value::make_int(0));
    }
}

// Per-helper cached builtin member-fn pointer
// Caching here removes the two hash lookups per call (builtins.find + globals_tbl.find)
static ScriptRuntime::BuiltinFn s_fn_char_code_at = nullptr;
static ScriptRuntime::BuiltinFn s_fn_starts_with = nullptr;
static ScriptRuntime::BuiltinFn s_fn_substr = nullptr;

static inline ScriptRuntime::BuiltinFn resolve_method_fn(VM *vm, ScriptRuntime::BuiltinFn &slot, const char *name) {
    if (!slot) {
        slot = vm->runtime->lookup_builtin_member(name);
    }
    return slot;
}

void jit_method_char_code_at(VM *vm) {
    if (jit_try_shadow_method(vm, "char_code_at", 1)) {
        return;
    }
    auto fn = resolve_method_fn(vm, s_fn_char_code_at, "char_code_at");
    Value args[2] = { vm->peek(1), vm->peek(0) };
    vm->pop();
    vm->pop();
    vm->push(vm->call_builtin_member(fn, args, 2));
    jit_abort_on_runtime_error(vm);
}

void jit_method_starts_with(VM *vm) {
    // fast path: plain string receiver + plain string arg. starts_with cannot be shadowed on a string receiver
    {
        const Value &recv = vm->peek(1);
        const Value &arg = vm->peek(0);
        if (recv.is_string() && arg.is_string()) {
            const std::string &str = recv.get_string();
            const std::string &prefix = arg.get_string();
            bool r = str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
            vm->pop();
            vm->pop();
            vm->push(Value::make_bool(r));
            return;
        }
    }
    if (jit_try_shadow_method(vm, "starts_with", 1)) {
        return;
    }
    auto fn = resolve_method_fn(vm, s_fn_starts_with, "starts_with");
    Value args[2] = { vm->peek(1), vm->peek(0) };
    vm->pop();
    vm->pop();
    vm->push(vm->call_builtin_member(fn, args, 2));
    jit_abort_on_runtime_error(vm);
}

// accepts ints and floats that can be coerced without loss, rejects fractional floats and non-numerics.
static inline bool jit_coerce_index(const Value &v, int &out) {
    if (v.is_int()) {
        out = static_cast<int>(v.get_int());
        return true;
    }
    if (v.is_float()) {
        double f = v.get_float();
        if (std::floor(f) != f) {
            return false;
        }
        out = static_cast<int>(f);
        return true;
    }
    return false;
}

void jit_method_substr(VM *vm, uint32_t argc) {
    // plain string receiver with the arg count the asmjit call site allows (0..2).
    // Avoids BOTH the 4-layer dispatch AND the per-call std::vector<Value> heap alloc/free
    if (argc <= 2) {
        const Value &recv = vm->peek(argc); // receiver is below the args
        if (recv.is_string()) {
            // Compute the whole result while the receiver is still rooted on the stack, THEN pop.
            // This keeps the source string alive across the read.
            const std::string &str = recv.get_string();
            const int slen = static_cast<int>(str.size());
            int start = 0;
            int len = slen;
            bool ok = true;
            if (argc >= 1) {
                ok = jit_coerce_index(vm->peek(argc - 1), start);
                if (ok) {
                    len = slen - start;
                }
            }
            if (ok && argc >= 2) {
                ok = jit_coerce_index(vm->peek(argc - 2), len);
            }
            std::string result; // empty == builtin_substr's bad-arg / OOB result
            if (ok) {
                if (start < 0) {
                    start = 0;
                }
                if (start < slen) {
                    if (len < 0) {
                        len = 0;
                    }
                    if (start + len > slen) {
                        len = slen - start;
                    }
                    result = str.substr((size_t)start, (size_t)len);
                }
            }
            // `result` is an independent std::string copy of the bytes,
            // so the receiver can be popped before allocating the new string
            for (uint32_t i = 0; i <= argc; i++) {
                vm->pop();
            }
            vm->push(Value::make_string(std::move(result)));
            return;
        }
    }
    if (jit_try_shadow_method(vm, "substr", argc)) {
        return;
    }
    auto fn = resolve_method_fn(vm, s_fn_substr, "substr");
    std::vector<Value> args((size_t)argc + 1);
    for (int64_t i = (int64_t)argc; i >= 0; i--) {
        args[(size_t)i] = vm->pop();
    }
    vm->push(vm->call_builtin_member(fn, args.data(), args.size()));
    jit_abort_on_runtime_error(vm);
}

// handles OP_RETURN: pop result, pop frame, restore stack, push result
void jit_return(VM *vm) {
    Value result = vm->pop();
    size_t slot_base = vm->current_frame().slot_base;
    vm->frames.pop_back();

    if (!vm->frames.empty()) {
        vm->stack.resize(slot_base);
        vm->push(result);
    } else {
        vm->push(result);
    }
}

void jit_make_array(VM *vm, uint32_t size) {
    // pop in reverse so elements end up in correct order
    std::vector<Value> elements(size);
    for (int32_t i = static_cast<int32_t>(size) - 1; i >= 0; i--) {
        elements[i] = vm->pop();
    }
    vm->push(Value::make_array(std::move(elements)));
    vm->jit_safepoint(); // result is on the stack (rooted) before any collect
}

void jit_make_object(VM *vm, uint32_t size) {
    // collect pairs in insertion order (stack: key0,val0,...,keyN,valN with valN on top)
    std::vector<std::pair<std::string, Value>> pairs(size);
    for (int i = (int)size - 1; i >= 0; i--) {
        pairs[i].second = vm->pop();
        pairs[i].first = vm->pop().to_string();
    }
    Value obj_val = Value::make_object();
    ObjectObj *oobj = obj_val.get_obj_ptr();
    if (size <= ObjectObj::kDictModeThreshold) {
        oobj->fields.reserve(size);
    }
    for (auto &[k, v] : pairs) {
        oobj->set_field(k, std::move(v));
    }
    vm->push(std::move(obj_val));
    vm->jit_safepoint(); // result is on the stack (rooted) before any collect
}

// normalize an iterable to a plain array for the desugared for-in index loop.
//   array  -> itself (no allocation)
//   object -> its keys materialized as an array
void jit_iter_array(VM *vm) {
    Value iterable = vm->pop();
    if (iterable.is_array()) {
        vm->push(std::move(iterable));
        return; // no allocation, so no safepoint needed
    }
    if (iterable.is_object()) {
        const ObjectObj *oobj = iterable.get_obj_ptr();
        std::vector<Value> keys;
        for (const auto &name : oobj->get_keys()) {
            keys.push_back(Value::make_string(name));
        }
        vm->push(Value::make_array(std::move(keys)));
        vm->jit_safepoint(); // result is on the stack (rooted) before any collect
        return;
    }
    fprintf(stderr, "bytecode: for-each requires an array or object\n");
    vm->has_error = true;          // match the interpreter's OP_ITER_ARRAY path
    vm->push(Value::make_array()); // dummy empty array keeps the VM sane
}

// similar to jit_make_object but uses the per-site shape cache keyed by the literal's bytecode address
void jit_make_object_site(VM *vm, uint32_t size, void *site) {
    vm->push(vm->make_object_cached(static_cast<const uint8_t *>(site), size));
    vm->jit_safepoint(); // result is on the stack (rooted) before any collect
}

// ensure the VM value stack has headroom for `n` more entries before JIT code manually advances _M_finish to batch-store
void jit_reserve(VM *vm, uint32_t n) {
    vm->stack.reserve(vm->stack.size() + (size_t)n);
}

void jit_get_index(VM *vm) {
    Value key = vm->pop();
    Value obj = vm->pop();

    if (obj.is_array()) {
        auto &arr = obj.get_array();
        int64_t idx = key.get_int();
        if (idx < 0) {
            idx += arr.size();
        }
        if (idx >= 0 && idx < (int64_t)arr.size()) {
            vm->push(arr[idx]);
        } else {
            vm->push(Value::none());
        }
    } else if (obj.is_object()) {
        const Value *v = obj.get_obj_ptr()->get_field(key.to_string());
        vm->push(v ? *v : Value::none());
    } else if (obj.is_delegate()) {
        // Delegate get trap. After the object fast path so shape prop-IC is untouched.
        vm->push(jit_runtime_call(vm, [&] { return vm->runtime->delegate_get(obj, key); }));
    } else if (obj.is_string()) {
        int64_t idx = key.get_int();
        const std::string &s = obj.get_string();
        if (idx < 0) {
            idx += s.size();
        }
        if (idx >= 0 && idx < (int64_t)s.size()) {
            vm->push(Value::make_string(std::string(1, s[idx])));
        } else {
            vm->push(Value::none());
        }
    } else {
        vm->push(Value::none());
    }
}

void jit_set_index(VM *vm) {
    Value val = vm->pop();
    Value key = vm->pop();
    Value obj = vm->pop();

    if (obj.is_array()) {
        auto &arr = obj.get_array();
        int64_t idx = key.get_int();
        if (idx < 0) {
            idx += arr.size();
        }
        if (idx >= 0 && idx < (int64_t)arr.size()) {
            arr[idx] = val;
        } else if (idx >= (int64_t)arr.size() && idx < 10000000) {
            arr.resize(static_cast<size_t>(idx) + 1, Value::none());
            arr[idx] = val;
        }
    } else if (obj.is_object()) {
        obj.get_obj_ptr()->set_field(key.to_string(), val);
    } else if (obj.is_delegate()) {
        // delegate set trap, after the object fast path so shape prop-IC is untouched.
        jit_runtime_call_void(vm, [&] { vm->runtime->delegate_set(obj, key, val); });
    }
    vm->push(val);
}

// non-IC property helpers (used by non-JIT-compiled call sites)
void jit_get_property(VM *vm, uint32_t name_idx) {
    const std::string &name = vm->chunk->strings[name_idx];
    Value obj = vm->pop();

    if (obj.is_object()) {
        vm->push(vm->jit_lookup_object_property(obj.get_obj_ptr(), (uint16_t)name_idx));
    } else if (obj.is_class_instance()) {
        const auto &instance = obj.get_class_instance();
        if (instance->layout && instance->layout->private_fields.count(name) && vm->current_class_name != instance->class_name) {
            fprintf(stderr, "Cannot access private field '%s' of class %s\n", name.c_str(), instance->class_name.c_str());
            vm->has_error = true;
            vm->push(Value::none());
        } else {
            const Value *fv = instance->get_field(name);
            vm->push(fv ? *fv : Value::none());
        }
    } else if (obj.is_delegate()) {
        // delegate get trap, after the object fast path so shape prop-IC is untouched.
        // a delegate (tag 11) misses the inline caches emitted by the method JIT and lowers to this helper.
        vm->push(jit_runtime_call(vm, [&] { return vm->runtime->delegate_get(obj, Value::make_string(name)); }));
    } else if (obj.is_array() && name == "length") {
        vm->push(Value::make_int(static_cast<int64_t>(obj.get_array().size())));
    } else if (obj.is_string() && name == "length") {
        vm->push(Value::make_int(static_cast<int64_t>(obj.get_string().size())));
    } else if (obj.is_handle()) {
        const auto &handle = obj.get_handle();
        if (!handle) {
            vm->push(Value::none());
        } else if (name == "await") {
            while (handle->state == HandleData::Running) {
                vm->process_completed_io_for_jit();
                if (handle->state == HandleData::Running) {
                    NARI_SLEEP_MILLIS(1);
                }
            }
            if (handle->state == HandleData::Failed) {
                bool caught = vm->dispatch_throw(handle->error);
                vm->has_error = !caught;
                if (vm->overflow_jmp) {
                    std::longjmp(*vm->overflow_jmp, caught ? 2 : 1);
                }
            } else {
                vm->push(handle->result);
            }
        } else if (name == "ready") {
            vm->process_completed_io_for_jit();
            vm->push(Value::make_bool(handle->state != HandleData::Running));
        } else if (name == "failed") {
            vm->push(Value::make_bool(handle->state == HandleData::Failed));
        } else if (name == "error") {
            vm->push(handle->error);
        } else if (name == "status_code") {
            const Value *sc = handle->result.is_object()
                                  ? handle->result.get_obj_ptr()->get_field("status_code")
                                  : nullptr;
            vm->push(sc ? *sc : Value::none());
        } else if (name == "duration") {
            if (handle->state == HandleData::Running) {
                auto now = chrono::steady_clock::now();
                auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                    now - handle->start_time);
                vm->push(Value::make_int(elapsed.count()));
            } else {
                auto elapsed = chrono::duration_cast<chrono::milliseconds>(handle->end_time - handle->start_time);
                vm->push(Value::make_int(elapsed.count()));
            }
        } else {
            vm->push(Value::none());
        }
    } else if (obj.is_string()) {
        std::string class_name = obj.get_string();
        const nari::ClassDecl *class_decl = Parser::get_registered_class(class_name);
        if (class_decl) {
            vm->ensure_static_fields_inited_for_jit(class_name, class_decl);
            std::string key = class_name + "." + name;
            auto &sf = Parser::get_static_fields();
            auto it = sf.find(key);
            vm->push(it != sf.end() ? it->second : Value::none());
        } else if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
            vm->push(Value::none());
        } else {
            fprintf(stderr, "RuntimeError: cannot access property '%s' on string value\n", name.c_str());
            vm->has_error = true;
            vm->push(Value::none());
        }
    } else {
        // compiler-internal properties (like __variant, __data) are used as speculative probes in match/pattern expressions
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
            vm->push(Value::none());
        } else {
            // JIT helpers cannot throw C++ exceptions through JIT-compiled frames; use the has_error flag pattern instead.
            fprintf(
                stderr,
                "RuntimeError: cannot access property '%s' on %s value\n",
                name.c_str(),
                obj.is_none() ? "null" : "non-object");
            vm->has_error = true;
            vm->push(Value::none());
        }
    }
}

void jit_set_property(VM *vm, uint32_t name_idx) {
    const std::string &name = vm->chunk->strings[name_idx];
    Value val = vm->pop();
    Value obj = vm->pop();

    if (obj.is_object()) {
        obj.get_obj_ptr()->set_field(name, std::move(val));
    } else if (obj.is_class_instance()) {
        Value *fv = obj.get_class_instance()->get_field(name);
        if (fv) {
            *fv = std::move(val);
        }
    } else if (obj.is_string()) {
        std::string cname = obj.get_string();
        if (Parser::get_registered_class(cname)) {
            Parser::get_static_fields()[cname + "." + name] = val;
        }
    } else if (obj.is_delegate()) {
        // delegate set trap, after the object fast path so shape prop-IC is untouched.
        jit_runtime_call_void(vm, [&] { vm->runtime->delegate_set(obj, Value::make_string(name), val); });
    }
    vm->push(val);
}

} // extern "C"

// called by OP_CHECK_TYPE in strict mode.
// Peeks TOS, verifies it matches type_name, throws TypeError on mismatch.
extern "C" {
void jit_check_type(VM *vm, uint32_t type_str_idx, uint32_t packed) {
    uint8_t context = (uint8_t)(packed & 0xFF);
    int src_line = (int)((packed >> 8) & 0xFFFFFF);
    // Peek the TOS value.
    if (vm->stack.empty()) {
        return; // should never happen! (TODO: add unreachable() here?)
    }
    const Value &val = vm->stack.back();

    const std::string &expected = vm->chunk->strings[type_str_idx];

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
    // else: unknown annotation, skip, this should only be possible if we add new types in the future, and the interpreter doesn't support it yet

    if (ok) {
        return; // fast path: type matches
    }

    // build the error message.
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

    const char *ctx_str = context == 0 ? "parameter" : (context == 1 ? "return value" : "variable");

    // resolve source location from the active call frame
    std::string fn_name = "<anonymous>";
    std::string src_file;
    if (!vm->frames.empty()) {
        const FunctionMeta *fn = vm->frames.back().function;
        if (fn) {
            if (!fn->name.empty()) {
                fn_name = fn->name;
            }
            src_file = fn->source_file;
        }
    }
    std::string loc;
    if (!src_file.empty()) {
        loc = " at '" + fn_name + "' (" + src_file;
        if (src_line > 0) {
            loc += ":" + std::to_string(src_line);
        }
        loc += ")";
    }

    std::string msg = std::string("TypeError: expected ") + ctx_str + " of type '" + expected + "', got '" + actual + "'" + loc;

    Value err = Value::make_string(msg);
    if (!vm->dispatch_throw(err)) {
        // uncaught, ensure stack non-empty for JIT safety
        if (vm->stack.empty()) {
            vm->push(Value::none());
        }
    }
}
} // extern "C"

// global JIT instance

namespace nari {
namespace jit {

// generated frame setup clears smart-pointer slots with zero stores
// we require that std::shared_ptr<Value> is all-zero when null, and that CapturesList is a pair of pointers without padding.
bool stl_layouts_ok() {
    static const bool ok = [] {
        bool good = stl::null_is_all_zero<std::shared_ptr<Value>>() &&
                    stl::null_is_all_zero<CapturesList>() &&
                    sizeof(CapturesList) == 2 * sizeof(void *);
        if (!good) {
            fprintf(stderr, "nari: smart-pointer ABI check failed; JIT disabled (interpreter only)\n");
        }
        return good;
    }();
    return ok;
}

MethodJITBase *g_jit_compiler = nullptr;

void init_jit() {
    if (getenv("NARI_DISABLE_JIT")) {
        return;
    }
    if (!stl_layouts_ok()) {
        return;
    }
    if (!g_jit_compiler) {
        g_jit_compiler = new AsmJITMethodCompiler();
    }
}

void shutdown_jit() {
    unregister_all_gdb_jit_functions();
    perf_jitdump_close();
    if (g_jit_compiler) {
        delete g_jit_compiler;
        g_jit_compiler = nullptr;
    }
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
