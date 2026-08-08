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
#include <deque>
#include <unordered_set>
#include <vector>

using namespace nari::bytecode;

// Helper functions called from JIT-compiled code.
// Same operations as execute_instruction() but as standalone functions
// so the JIT can call them directly.

static void jit_dispatch_script_throw(VM *vm, Value error) {
    if (vm->capture_native_throw(error)) {
        vm->has_error = false;
        if (vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 4);
        }
        return;
    }
    bool caught = vm->dispatch_throw(std::move(error));
    vm->has_error = !caught;
    if (vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, caught ? 2 : 1);
    }
}

template <typename Call> static auto jit_runtime_call(VM *vm, Call &&call) -> decltype(call()) {
    try {
        auto result = call();
        if (NARI_UNLIKELY(vm->has_error) && vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
        if (NARI_UNLIKELY(vm->runtime->has_pending_throw())) {
            Value err = vm->runtime->take_pending_throw();
            jit_dispatch_script_throw(vm, std::move(err));
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

template <typename Call> static void jit_runtime_call_void(VM *vm, Call &&call) {
    try {
        call();
        if (NARI_UNLIKELY(vm->has_error) && vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
        if (NARI_UNLIKELY(vm->runtime->has_pending_throw())) {
            Value err = vm->runtime->take_pending_throw();
            jit_dispatch_script_throw(vm, std::move(err));
        }
    } catch (const RuntimeError &) {
        vm->has_error = true;
        if (vm->overflow_jmp) {
            std::longjmp(*vm->overflow_jmp, 1);
        }
    }
}

static inline void jit_abort_on_runtime_error(VM *vm) {
    if (NARI_UNLIKELY(vm->has_error) && vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, 1);
    }
}

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
    if (Value *cell = frame.find_open_upvalue(idx)) {
        Value to_push = *cell;
        vm->push(std::move(to_push));
        return;
    }
    // push_back may reallocate vm->stack and invalidate the reference, so copy to a local first
    Value to_push = vm->stack[frame.slot_base + idx];
    vm->push(std::move(to_push));
}

void jit_store_var(VM *vm, uint32_t idx) {
    Value val = vm->peek();
    auto &frame = vm->current_frame();
    vm->stack[frame.slot_base + idx] = val;
    if (Value *cell = frame.find_open_upvalue(idx)) {
        *cell = val;
    }
}

// Ends a loop iteration: drops this frame's upvalue cells for locals at or above
// first_slot so the next iteration's closures get fresh cells instead of aliasing.
void jit_close_upvalues(VM *vm, uint32_t first_slot) {
    vm->current_frame().close_upvalues_from((uint16_t)first_slot);
}

// slow path: write directly to slot with no operand-stack traffic.
void jit_slot_store_raw(VM *vm, uint32_t idx, uint64_t raw) {
    Value val = Value::from_raw(raw);
    auto &frame = vm->current_frame();
    vm->stack[frame.slot_base + idx] = val;
    if (Value *cell = frame.find_open_upvalue(idx)) {
        *cell = val;
    }
}
void jit_slot_copy(VM *vm, uint32_t src_idx, uint32_t dst_idx) {
    auto &frame = vm->current_frame();
    Value val = vm->stack[frame.slot_base + src_idx];
    if (Value *cell = frame.find_open_upvalue(src_idx)) {
        val = *cell;
    }
    vm->stack[frame.slot_base + dst_idx] = val;
    if (Value *cell = frame.find_open_upvalue(dst_idx)) {
        *cell = val;
    }
}

void jit_load_global(VM *vm, uint32_t name_idx) {
    // Fast path: the indexed global cache (same one the interpreter's
    // OP_LOAD_GLOBAL uses) skips the get_global name-hash lookup. Hot for globals
    // referenced in loops (e.g. MOD / mix in the benchmark).
    if (NARI_LIKELY(name_idx < vm->global_cache_valid.size() && vm->global_cache_valid[name_idx])) {
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
    // by every compiled-code entry point) to avoid shared_ptr work on each load.
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

void jit_pow(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int() && b.get_int() >= 0) {
        int64_t result = 1;
        int64_t base = a.get_int();
        int64_t exp = b.get_int();
        bool overflowed = false;
        while (exp > 0) {
            if ((exp & 1) && mul_overflow_i64(result, base, &result)) {
                overflowed = true;
                break;
            }
            exp >>= 1;
            if (exp > 0 && mul_overflow_i64(base, base, &base)) {
                overflowed = true;
                break;
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
    vm->stack.pop_back();
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
    if (NARI_LIKELY(a.is_string() && b.is_string()) && static_cast<StringObj *>(a.heap_ptr())->js_getter_prefix) {
        auto *rhs = static_cast<StringObj *>(b.heap_ptr());
        if (rhs->immutable) {
            bool allocated = false;
            if (rhs->getter_key_cache.is_none()) {
                rhs->getter_key_cache = Value::make_const_string("__js_getter__" + rhs->s);
                allocated = true;
            }
            a = rhs->getter_key_cache;
            vm->stack.pop_back();
            if (allocated) {
                vm->jit_safepoint();
            }
            return;
        }
    }
    std::string out;
    if (NARI_LIKELY(a.is_string() && b.is_string())) {
        const std::string &lhs = a.get_string();
        const std::string &rhs = b.get_string();
        out.reserve(lhs.size() + rhs.size());
        out.append(lhs);
        out.append(rhs);
    } else {
        out = a.to_string();
        append_concat_rhs(out, b);
    }
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

void jit_str_append_slot(VM *vm, uint32_t idx) {
    Value rhs = vm->pop();
    auto &frame = vm->current_frame();
    Value *slot_ptr = &vm->stack[frame.slot_base + idx];
    Value *cell = frame.find_open_upvalue(idx);
    if (cell) {
        slot_ptr = cell;
    }
    Value &slot = *slot_ptr;
    if (slot.is_mutable_heap_string()) {
        std::string &dst = slot.get_string();
        if (rhs.is_sso()) {
            uint8_t len = rhs.sso_len();
            char buf[5];
            for (uint8_t i = 0; i < len; i++) {
                buf[i] = rhs.sso_char(i);
            }
            dst.append(buf, len);
        } else if (rhs.is_string()) {
            dst += ((const Value &)rhs).get_string();
        } else {
            slot = Value::make_string(slot.to_string() + rhs.to_string());
        }
    } else {
        slot = Value::make_string(slot.to_string() + rhs.to_string());
    }
    if (cell) {
        vm->stack[frame.slot_base + idx] = *cell;
    }
    Value result = slot;
    vm->push(std::move(result));
}

void jit_format_value(VM *vm, uint32_t spec_idx) {
    if (jit_format_value_body(vm, spec_idx)) {
        jit_abort_on_runtime_error(vm);
    }
}

void jit_bit_and(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    vm->peek(0).set_int(vm->peek(0).get_int() & val);
}

void jit_bit_or(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    vm->peek(0).set_int(vm->peek(0).get_int() | val);
}

void jit_bit_xor(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    vm->peek(0).set_int(vm->peek(0).get_int() ^ val);
}

void jit_bit_not(VM *vm) {
    vm->peek(0).set_int(~vm->peek(0).get_int());
}

void jit_lshift(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    // mask the shift count to [0, 63]
    vm->peek(0).set_int(vm->peek(0).get_int() << (val & 63));
}

void jit_rshift(VM *vm) {
    int64_t val = vm->peek(0).get_int();
    vm->stack.pop_back();
    // mask the shift count to [0, 63]
    vm->peek(0).set_int(vm->peek(0).get_int() >> (val & 63));
}

static uint32_t jit_js_to_uint32(VM *vm, const Value &value) {
    if (value.is_int()) {
        return static_cast<uint32_t>(value.get_int());
    }
    double numeric;
    if (value.is_float()) {
        numeric = value.get_float();
    } else {
        ScriptRuntime::BuiltinFn to_number = vm->runtime->lookup_builtin_member("__js_to_number");
        Value number = vm->call_builtin_member(to_number, &value, 1);
        jit_abort_on_runtime_error(vm);
        numeric = number.as_number();
    }
    if (numeric == 0.0 || !std::isfinite(numeric)) {
        return 0;
    }
    double reduced = std::fmod(std::trunc(numeric), 4294967296.0);
    if (reduced < 0.0) reduced += 4294967296.0;
    return static_cast<uint32_t>(reduced);
}

static int64_t jit_js_signed32(uint32_t value) {
    return value >= 0x80000000U ? static_cast<int64_t>(value) - 0x100000000LL : static_cast<int64_t>(value);
}

void jit_js_bit_binary(VM *vm, uint32_t raw_op) {
    const uint32_t left = jit_js_to_uint32(vm, vm->peek(1));
    const uint32_t right = jit_js_to_uint32(vm, vm->peek(0));
    vm->stack.pop_back();
    Value &result = vm->peek(0);
    const OpCode op = static_cast<OpCode>(raw_op);
    if (op == OpCode::OP_JS_BIT_AND) result.set_int(jit_js_signed32(left & right));
    else if (op == OpCode::OP_JS_BIT_OR) result.set_int(jit_js_signed32(left | right));
    else if (op == OpCode::OP_JS_BIT_XOR) result.set_int(jit_js_signed32(left ^ right));
    else if (op == OpCode::OP_JS_SHL) result.set_int(jit_js_signed32(left << (right & 31U)));
    else if (op == OpCode::OP_JS_USHR) result.set_int(static_cast<int64_t>(left >> (right & 31U)));
    else {
        const uint32_t shift = right & 31U;
        const uint32_t shifted = (left & 0x80000000U) && shift ? ~(~left >> shift) : left >> shift;
        result.set_int(jit_js_signed32(shifted));
    }
}

void jit_js_bit_not(VM *vm) {
    vm->peek(0).set_int(jit_js_signed32(~jit_js_to_uint32(vm, vm->peek(0))));
}

void jit_not(VM *vm) {
    bool r = !is_truthy(vm->peek(0));
    vm->peek(0).set_bool(r);
}

void jit_js_truthy(VM *vm) {
    bool r = is_js_truthy(vm->peek(0));
    vm->peek(0).set_bool(r);
}

static NARI_ALWAYS_INLINE bool jit_values_equal(const Value &a, const Value &b) {
    const uint16_t a_tag = a.tag_word();
    const uint16_t b_tag = b.tag_word();

    if (a._raw == b._raw && a_tag != Value::TAG_INT && a_tag != Value::TAG_HEAP && a_tag != Value::TAG_BOOL &&
        a_tag != Value::TAG_NONE) {
        return std::fabs(a.as_number() - b.as_number()) < 1e-12;
    }
    if (a._raw == b._raw) {
        return true;
    }
    if (NARI_LIKELY(a_tag == Value::TAG_HEAP)) {
        if (NARI_UNLIKELY(b_tag != Value::TAG_HEAP)) {
            return false;
        }
        HeapHeader *a_heap = reinterpret_cast<HeapHeader *>(a._raw & Value::PTR_MASK);
        HeapHeader *b_heap = reinterpret_cast<HeapHeader *>(b._raw & Value::PTR_MASK);
        if (NARI_LIKELY(a_heap->type_tag == ValueTag::String)) {
            if (NARI_UNLIKELY(b_heap->type_tag != ValueTag::String)) {
                return false;
            }
            auto *a_string = static_cast<StringObj *>(a_heap);
            auto *b_string = static_cast<StringObj *>(b_heap);
            if (NARI_LIKELY(a_string->immutable && b_string->immutable)) {
                if (NARI_UNLIKELY(a_string->field_id == UINT32_MAX)) {
                    a_string->field_id = intern_field(a_string->s);
                }
                if (NARI_UNLIKELY(b_string->field_id == UINT32_MAX)) {
                    b_string->field_id = intern_field(b_string->s);
                }
                return a_string->field_id == b_string->field_id;
            }
            return a_string->s == b_string->s;
        }
        return false;
    }
    if (a_tag == Value::TAG_INT && b_tag == Value::TAG_INT) {
        return false;
    }
    if (a.is_numeric() && b.is_numeric()) {
        return std::fabs(a.as_number() - b.as_number()) < 1e-12;
    }
    return false;
}

static bool jit_strict_strings_equal(StringObj *a, StringObj *b) {
    if (NARI_LIKELY(a->immutable && b->immutable)) {
        if (NARI_UNLIKELY(a->field_id == UINT32_MAX)) {
            a->field_id = intern_field(a->s);
        }
        if (NARI_UNLIKELY(b->field_id == UINT32_MAX)) {
            b->field_id = intern_field(b->s);
        }
        return a->field_id == b->field_id;
    }
    return a->s == b->s;
}

static NARI_ALWAYS_INLINE bool jit_values_strict_equal(const Value &a, const Value &b) {
    const uint16_t a_tag = a.tag_word();

    if (a._raw == b._raw) {
        return a_tag == Value::TAG_HEAP || a_tag == Value::TAG_INT || a_tag == Value::TAG_BOOL ||
               a_tag == Value::TAG_NONE || !std::isnan(a.as_number());
    }

    const uint16_t b_tag = b.tag_word();

    if (a_tag != b_tag &&
        (a_tag == Value::TAG_HEAP || a_tag == Value::TAG_BOOL || a_tag == Value::TAG_NONE ||
         b_tag == Value::TAG_HEAP || b_tag == Value::TAG_BOOL || b_tag == Value::TAG_NONE)) {
        return false;
    }

    if (a_tag == Value::TAG_HEAP && b_tag == Value::TAG_HEAP) {
        HeapHeader *a_heap = reinterpret_cast<HeapHeader *>(a._raw & Value::PTR_MASK);
        HeapHeader *b_heap = reinterpret_cast<HeapHeader *>(b._raw & Value::PTR_MASK);
        if (a_heap->type_tag != ValueTag::String || b_heap->type_tag != ValueTag::String) {
            return false;
        }
        return jit_strict_strings_equal(static_cast<StringObj *>(a_heap), static_cast<StringObj *>(b_heap));
    }
    if (a_tag == Value::TAG_INT && b_tag == Value::TAG_INT) {
        return a._raw == b._raw;
    }
    if (a.is_numeric() && b.is_numeric()) {
        return a.as_number() == b.as_number();
    }
    return a_tag == b_tag && a._raw == b._raw;
}

void jit_eq(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = jit_values_equal(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_ne(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = !jit_values_equal(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_strict_eq(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = jit_values_strict_equal(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_strict_ne(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = !jit_values_strict_equal(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_lt(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = Value::values_lt(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_le(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = Value::values_le(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_gt(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = Value::values_gt(a, b);
    vm->stack.pop_back();
    a.set_bool(r);
}

void jit_ge(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    bool r = Value::values_ge(a, b);
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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static void jit_call_depth_overflow(VM *vm) {
    Value err = Value::make_string("Stack Overflow: maximum call-depth exceeded!");
    bool caught = vm->dispatch_throw(err);
    vm->has_error = !caught;
    if (vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, caught ? 2 : 1);
    }
}

// Debug-census switches. Deliberately at file scope: as function-local statics
// inside jit_call_value_impl these cost a guard-variable load + branch each, i.e.
// 10 instructions of dead weight on all 15.3M calls per tsc run.
static const bool g_call_census = getenv("NARI_CALL_CENSUS") != nullptr;
static const bool g_dc_census = getenv("NARI_DC_CENSUS") != nullptr;

static NARI_ALWAYS_INLINE void jit_check_call_depth(VM *vm) {
    // frames.size() divides the byte span by sizeof(CallFrame) (104, not a power of
    // two), so it compiled to a shift plus a magic-reciprocal imul on every call.
    // Comparing pointers folds the bound into a single constant displacement.
    if (NARI_UNLIKELY(vm->frames.storage_end >= vm->frames.storage_begin + MAX_CALL_DEPTH)) {
        jit_call_depth_overflow(vm);
    }
}

static inline void jit_deliver_pending_throw(VM *vm) {
    if (NARI_LIKELY(!vm->runtime->has_pending_throw())) {
        return;
    }
    Value err = vm->runtime->take_pending_throw();
    jit_dispatch_script_throw(vm, std::move(err));
}

// Forward declaration (defined below jit_call_value)
void jit_call(VM *vm, uint32_t argc);
static void jit_call_value_impl(VM *vm, uint32_t argc, uint32_t callee_label_idx);


// __js_to_string(v) returns `v` unchanged when it is already a string: the
// helper's leading `v === __js_undefined` test can never match a string, and the
// next branch is `typeof(v) == "string" -> return v`.
//
// Kept out of line on purpose. Inlining this into jit_try_native_call measurably
// regressed the whole call path (+2%) even though it removes ~1.07B instructions,
// so that dispatcher is sensitive to its own size/layout; here it costs it only a
// compare and a tail call.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static bool jit_native_to_string(VM *vm, uint32_t argc, size_t args_base, size_t slot_base) {
    if (argc != 1) {
        return false;
    }
    const Value &value = vm->stack[args_base];
    if (!value.is_string()) {
        return false;
    }
    vm->stack[slot_base] = value;
    vm->stack.resize(slot_base + 1);
    return true;
}

static bool jit_try_native_call(VM *vm, FunctionData &fd, uint32_t argc, uint32_t callee_label_idx,
                                size_t args_base, size_t slot_base) {
    if (fd.jit_native_kind == 14) {
        return jit_native_to_string(vm, argc, args_base, slot_base);
    }

    if (argc == 2 && fd.jit_native_kind == 12) {
        const Value &a = vm->stack[args_base];
        const Value &b = vm->stack[args_base + 1];
        if (a.is_none() || b.is_none()) {
            vm->stack[slot_base].set_bool((a.is_none() && b.is_none()) ||
                                          (a.is_none() && b.raw_bits() == vm->js_undefined_value.raw_bits()) ||
                                          (b.is_none() && a.raw_bits() == vm->js_undefined_value.raw_bits()));
            vm->stack.resize(slot_base + 1);
            return true;
        }
    }

    // __js_str_code_point_at(s, i = 0). char_code_at is byte-based, so every code
    // unit is 0..255 and the helper's surrogate-pair branches are unreachable:
    // the result is the indexed byte, or undefined when out of range.
    if (argc >= 1 && argc <= 2 && fd.jit_native_kind == 15) {
        const Value &receiver = vm->stack[args_base];
        int64_t index = 0;
        bool index_ok = true;
        if (argc == 2) {
            const Value &index_value = vm->stack[args_base + 1];
            if (index_value.is_int()) {
                index = index_value.get_int();
            } else {
                index_ok = false;
            }
        }
        if (index_ok && receiver.is_string()) {
            const std::string &text = receiver.get_string();
            if (index < 0 || index >= static_cast<int64_t>(text.size())) {
                vm->stack[slot_base] = vm->js_undefined_value;
            } else {
                vm->stack[slot_base].set_int(static_cast<unsigned char>(text[static_cast<size_t>(index)]));
            }
            vm->stack.resize(slot_base + 1);
            return true;
        }
    }

    // __js_add(a, b): both-integer case is plain integer addition (the helper's
    // string and mixed-numeric branches need the scripted path). Kept to the
    // int48-safe range so the sum boxes exactly like the interpreter's `a + b`.
    if (argc == 2 && fd.jit_native_kind == 16) {
        const Value &a = vm->stack[args_base];
        const Value &b = vm->stack[args_base + 1];
        if (a.is_int() && b.is_int()) {
            constexpr int64_t kSafeIntMax = (int64_t{1} << 46);
            const int64_t x = a.get_int();
            const int64_t y = b.get_int();
            if (x > -kSafeIntMax && x < kSafeIntMax && y > -kSafeIntMax && y < kSafeIntMax) {
                vm->stack[slot_base].set_int(x + y);
                vm->stack.resize(slot_base + 1);
                return true;
            }
        }
    }

    // __js_postinc(o, k): `let old = __js_to_number(o[k]); o[k] = old + 1; return old;`
    // The helper uses raw index access, so there is no getter/prototype lookup to
    // reproduce here. Handle the integer counter case (o[k] already an int, whose
    // successor stays in int48 range) and let anything else take the scripted path.
    if (argc == 2 && fd.jit_native_kind == 13) {
        Value &object = vm->stack[args_base];
        const Value &key = vm->stack[args_base + 1];
        constexpr int64_t kSafeIntMax = (int64_t{1} << 46);
        if (object.is_object() && key.is_string() && !key.is_sso()) {
            auto *string_key = static_cast<StringObj *>(key.heap_ptr());
            if (string_key->immutable) {
                if (NARI_UNLIKELY(string_key->field_id == UINT32_MAX)) {
                    string_key->field_id = intern_field(string_key->s);
                }
                ObjectObj *obj = object.get_obj_ptr();
                const Value *field = obj->get_field_by_id(string_key->field_id);
                if (field && field->is_int()) {
                    const int64_t old = field->get_int();
                    if (old > -kSafeIntMax && old < kSafeIntMax) {
                        obj->set_field_by_id(string_key->field_id, Value::make_int(old + 1));
                        vm->stack[slot_base].set_int(old);
                        vm->stack.resize(slot_base + 1);
                        return true;
                    }
                }
            }
        } else if (object.is_array() && key.is_int()) {
            const int64_t index = key.get_int();
            auto &array = object.get_array();
            if (index >= 0 && index < static_cast<int64_t>(array.size()) && array[static_cast<size_t>(index)].is_int()) {
                const int64_t old = array[static_cast<size_t>(index)].get_int();
                if (old > -kSafeIntMax && old < kSafeIntMax) {
                    array[static_cast<size_t>(index)] = Value::make_int(old + 1);
                    vm->stack[slot_base].set_int(old);
                    vm->stack.resize(slot_base + 1);
                    return true;
                }
            }
        }
    }

    if (argc == 2 && fd.jit_native_kind >= 2 && fd.jit_native_kind <= 5) {
        Value &a = vm->stack[args_base];
        Value &b = vm->stack[args_base + 1];
        bool handled = false;
        bool result = false;
        if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
            const double left = a.as_number();
            const double right = b.as_number();
            if (fd.jit_native_kind == 2) result = left < right;
            else if (fd.jit_native_kind == 3) result = left > right;
            else if (fd.jit_native_kind == 4) result = left <= right;
            else result = left >= right;
            handled = true;
        } else if (a.is_string() && b.is_string()) {
            const std::string &left = a.get_string();
            const std::string &right = b.get_string();
            if (fd.jit_native_kind == 2) result = left < right;
            else if (fd.jit_native_kind == 3) result = left > right;
            else if (fd.jit_native_kind == 4) result = left <= right;
            else result = left >= right;
            handled = true;
        }
        if (handled) {
            vm->stack[slot_base].set_bool(result);
            vm->stack.resize(slot_base + 1);
            return true;
        }
    }

    if (argc == 2 && fd.jit_native_kind == 6) {
        const Value &object = vm->stack[args_base];
        const Value &key = vm->stack[args_base + 1];
        if (object.is_array() && key.is_int()) {
            const int64_t index = key.get_int();
            const auto &array = object.get_array();
            if (index >= 0 && index < static_cast<int64_t>(array.size())) {
                vm->stack[slot_base] = array[static_cast<size_t>(index)];
                vm->stack.resize(slot_base + 1);
                return true;
            }
        } else if (object.is_object() && key.is_string() && !key.is_sso()) {
            auto *string_key = static_cast<StringObj *>(key.heap_ptr());
            if (string_key->immutable) {
                if (NARI_UNLIKELY(string_key->field_id == UINT32_MAX)) {
                    string_key->field_id = intern_field(string_key->s);
                }
                if (NARI_UNLIKELY(string_key->getter_field_id == UINT32_MAX)) {
                    string_key->getter_field_id = intern_field("__js_getter__" + string_key->s);
                }
                const ObjectObj *obj = object.get_obj_ptr();
                const Value *getter = obj->get_field_by_id(string_key->getter_field_id);
                if (!getter || !getter->is_function()) {
                    const Value *field = obj->get_field_by_id(string_key->field_id);
                    if (field) {
                        vm->stack[slot_base] = *field;
                        vm->stack.resize(slot_base + 1);
                        return true;
                    }
                }
            }
        }
    }

    if (argc == 3 && fd.jit_native_kind == 7) {
        Value &object = vm->stack[args_base];
        const Value &key = vm->stack[args_base + 1];
        if (object.is_array() && key.is_int()) {
            const int64_t index = key.get_int();
            auto &array = object.get_array();
            if (index >= 0 && index < static_cast<int64_t>(array.size())) {
                array[static_cast<size_t>(index)] = vm->stack[args_base + 2];
                vm->stack[slot_base] = array[static_cast<size_t>(index)];
                vm->stack.resize(slot_base + 1);
                return true;
            }
        }
    }

    if (argc == 3 && fd.jit_native_kind == 8) {
        Value &object = vm->stack[args_base];
        const Value &key = vm->stack[args_base + 1];
        if (object.is_object() && key.is_string()) {
            auto *obj = object.get_obj_ptr();
            static const uint32_t proto_id = intern_field("__proto__");
            StringObj *string_key = !key.is_sso() ? static_cast<StringObj *>(key.heap_ptr()) : nullptr;
            uint32_t setter_id;
            if (string_key && string_key->immutable) {
                if (NARI_UNLIKELY(string_key->setter_field_id == UINT32_MAX)) {
                    string_key->setter_field_id = intern_field("__js_setter__" + string_key->s);
                }
                setter_id = string_key->setter_field_id;
            } else {
                setter_id = intern_field("__js_setter__" + key.get_string());
            }
            const bool needs_shim = obj->dict_mode || obj->has_field_by_id(proto_id) || obj->has_field_by_id(setter_id);
            if (!needs_shim) {
                if (string_key && string_key->immutable) {
                    if (string_key->field_id == UINT32_MAX) {
                        string_key->field_id = intern_field(string_key->s);
                    }
                    obj->set_field_by_id(string_key->field_id, vm->stack[args_base + 2]);
                } else {
                    obj->set_field(key.get_string(), vm->stack[args_base + 2]);
                }
                vm->stack[slot_base] = vm->stack[args_base + 2];
                vm->stack.resize(slot_base + 1);
                return true;
            }
        }
    }

    if (argc == 1 && fd.jit_native_kind == 9) {
        const Value &object = vm->stack[args_base];
        if (object.is_array()) {
            vm->stack[slot_base].set_int(static_cast<int64_t>(object.get_array().size()));
        } else if (object.is_string()) {
            vm->stack[slot_base].set_int(static_cast<int64_t>(object.get_string().size()));
        } else if (object.is_object()) {
            static const uint32_t length_id = intern_field("length");
            const Value *length = object.get_obj_ptr()->get_field_by_id(length_id);
            vm->stack[slot_base] = length ? *length : vm->js_undefined_value;
        } else if (object.is_function()) {
            // fn.length is kept out of the property bag; see FunctionData::int_length.
            static const uint32_t length_id = intern_field("length");
            const Value *length = object.get_function().get_property_by_id(length_id);
            vm->stack[slot_base] = length ? *length : vm->js_undefined_value;
        } else {
            vm->stack[slot_base] = vm->js_undefined_value;
        }
        vm->stack.resize(slot_base + 1);
        return true;
    }

    if (argc == 2 && fd.jit_native_kind == 10) {
        const Value &receiver = vm->stack[args_base];
        const Value &index_value = vm->stack[args_base + 1];
        if (receiver.is_string() && index_value.is_int()) {
            const int64_t index = index_value.get_int();
            if (index >= std::numeric_limits<int>::min() && index <= std::numeric_limits<int>::max()) {
                const std::string &text = receiver.get_string();
                const int64_t code = index >= 0 && index < static_cast<int64_t>(text.size())
                                         ? static_cast<unsigned char>(text[static_cast<size_t>(index)])
                                         : -1;
                vm->stack[slot_base].set_int(code);
                vm->stack.resize(slot_base + 1);
                return true;
            }
        }
    }

    if (fd.jit_native_kind == 11 && argc >= 3) {
        Value receiver = vm->stack[args_base];
        const Value &key = vm->stack[args_base + 1];
        Value args = vm->stack[args_base + 2];
        if (receiver.is_object() && key.is_string() && args.is_array()) {
            ObjectObj *object = receiver.get_obj_ptr();
            uint32_t field_id;
            uint32_t getter_field_id;
            if (!key.is_sso() && static_cast<StringObj *>(key.heap_ptr())->immutable) {
                auto *string_key = static_cast<StringObj *>(key.heap_ptr());
                if (NARI_UNLIKELY(string_key->field_id == UINT32_MAX)) {
                    string_key->field_id = intern_field(string_key->s);
                }
                if (NARI_UNLIKELY(string_key->getter_field_id == UINT32_MAX)) {
                    string_key->getter_field_id = intern_field("__js_getter__" + string_key->s);
                }
                field_id = string_key->field_id;
                getter_field_id = string_key->getter_field_id;
            } else {
                field_id = intern_field(key.get_string());
                getter_field_id = intern_field("__js_getter__" + key.get_string());
            }
            const Value *getter = object->get_field_by_id(getter_field_id);
            const Value *method = object->get_field_by_id(field_id);
            if ((!getter || !getter->is_function()) && method && method->is_function()) {
                Value callee = *method;
                const auto &call_args = args.get_array();
                Value this_cell_value = vm->js_this_cell();
                ObjectObj *this_cell = this_cell_value.is_object() ? this_cell_value.get_obj_ptr() : nullptr;
                if (this_cell) {
                    static const uint32_t value_id = intern_field("value");
                    const Value *previous_ptr = this_cell->get_field_by_id(value_id);
                    Value previous = previous_ptr ? *previous_ptr : Value::none();
                    this_cell->set_field_by_id(value_id, receiver);
                    vm->stack.resize(slot_base);
                    vm->push(std::move(callee));
                    for (const Value &arg : call_args) vm->push(arg);
                    jit_call_value_impl(vm, static_cast<uint32_t>(call_args.size()), callee_label_idx);
                    this_cell->set_field_by_id(value_id, std::move(previous));
                    return true;
                }
            }
        }
    }

    return false;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static void jit_call_non_function(VM *vm, uint32_t argc, uint32_t callee_label_idx, size_t slot_base) {
    Value &func_val = vm->stack[slot_base];
    if (func_val.is_delegate()) {
        jit_call(vm, argc);
        return;
    }
    const char *actual_type = func_val.is_none()     ? "null"
                              : func_val.is_string() ? "string"
                              : func_val.is_int()    ? "int"
                              : func_val.is_float()  ? "float"
                              : func_val.is_bool()   ? "bool"
                              : func_val.is_array()  ? "array"
                              : func_val.is_object() ? "object"
                                                     : "other";
    std::string actual_value = func_val.to_string();
    vm->stack.resize(slot_base);
    vm->runtime_panic(Value::make_string("called a non-function value '" + vm->chunk->strings[callee_label_idx] +
                                         "' (type " + actual_type + ", value " + actual_value + ")"));
    if (vm->overflow_jmp) {
        std::longjmp(*vm->overflow_jmp, 1);
    }
}

// ---- dynamic call census (diagnostic) ----
namespace {
struct CallCensus {
    std::unordered_map<std::string, uint64_t> counts;
    ~CallCensus() {
        if (counts.empty()) return;
        std::vector<std::pair<std::string, uint64_t>> v(counts.begin(), counts.end());
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        uint64_t total = 0;
        for (const auto &e : v) total += e.second;
        fprintf(stderr, "\n=== dynamic call census (total %llu) ===\n", (unsigned long long)total);
        size_t n = v.size() < 40 ? v.size() : 40;
        for (size_t i = 0; i < n; i++) {
            fprintf(stderr, "%10llu  %5.2f%%  %s\n", (unsigned long long)v[i].second,
                    100.0 * (double)v[i].second / (double)total, v[i].first.c_str());
        }
    }
};
} // namespace

static void jit_call_census_record(const FunctionData &fd) {
    static CallCensus census;
    const std::string &nm = fd.name;
    census.counts[nm.empty() ? std::string("<anon>") : nm]++;
}

// ---- direct-call blocker census (NARI_DC_CENSUS=1, diagnostic only) ----
// For every dynamic call that reached the generic path, bucket the reason
// resolve_direct_callee() would have refused it. Answers "is the 8-local
// none-fill unroll bound the thing keeping tsc off the direct-call path".
namespace {
struct DcCensus {
    uint64_t total = 0, native = 0, no_meta = 0, rest = 0, cap0 = 0, argc_gt4 = 0, arity = 0;
    uint64_t fill[9] = {}; // fill[i] = locals-argc == i, i>=8 lumps the tail into fill[8]
    uint64_t eligible = 0;
    ~DcCensus() {
        if (total == 0) return;
        fprintf(stderr, "\n=== direct-call blocker census (total %llu) ===\n", (unsigned long long)total);
        auto p = [&](const char *n, uint64_t v) {
            fprintf(stderr, "%12llu  %5.2f%%  %s\n", (unsigned long long)v, 100.0 * (double)v / (double)total, n);
        };
        p("native_kind (intrinsic)", native);
        p("no jit_meta / not user fn", no_meta);
        p("rest param", rest);
        p("Capture0 inline kind", cap0);
        p("argc > 4", argc_gt4);
        p("arity mismatch", arity);
        p("eligible today (fill<=8)", eligible);
        for (int i = 0; i < 9; i++) {
            char buf[64];
            snprintf(buf, sizeof buf, i == 8 ? "  locals-argc >= 8" : "  locals-argc == %d", i);
            p(buf, fill[i]);
        }
    }
};
} // namespace

static void jit_dc_census_record(const FunctionData &fd, uint32_t argc) {
    static DcCensus c;
    c.total++;
    if (fd.jit_native_kind != 0) { c.native++; return; }
    if (fd.jit_func_idx < 0 || fd.jit_meta == nullptr) { c.no_meta++; return; }
    if (fd.jit_rest_param_index >= 0) { c.rest++; return; }
    if (fd.jit_inline_kind == JitInlineKind::Capture0) { c.cap0++; return; }
    if (argc > 4) { c.argc_gt4++; return; }
    if ((uint32_t)fd.jit_param_count != argc || fd.jit_locals_count < argc) { c.arity++; return; }
    const uint32_t fillcount = fd.jit_locals_count - argc;
    c.fill[fillcount < 8 ? fillcount : 8]++;
    if (fillcount <= 8) c.eligible++;
}


// fast-dispatch call when caller loaded the function from a variable (not a statically-resolved global)
// Diagnostic only (NARI_CALL_CENSUS=1): which callees dominate dynamic dispatch.
// Answers "which jsrt shim is worth a JIT intrinsic" with execution counts
// rather than emitted-call-site counts.
static void jit_call_census_record(const FunctionData &fd);

static void jit_call_value_impl(VM *vm, uint32_t argc, uint32_t callee_label_idx) {
    jit_check_call_depth(vm);
    const size_t args_base = vm->stack.size() - argc;
    const size_t slot_base = args_base - 1; // func_val slot

    Value &func_val = vm->stack[slot_base];
    if (!func_val.is_function()) {
        jit_call_non_function(vm, argc, callee_label_idx, slot_base);
        return;
    }
    FunctionData &fd = func_val.get_function();
    const int32_t fidx = fd.jit_func_idx;

    if (NARI_UNLIKELY(g_call_census)) {
        jit_call_census_record(fd);
    }
    if (NARI_UNLIKELY(g_dc_census)) {
        jit_dc_census_record(fd, argc);
    }

    if (NARI_UNLIKELY(fd.jit_native_kind != 0) && jit_try_native_call(vm, fd, argc, callee_label_idx, args_base, slot_base)) {
        return;
    }

    if (fd.jit_inline_kind == JitInlineKind::Capture0 && fd.jit_capture0_raw) {
        Value result = *fd.jit_capture0_raw;
        vm->stack[slot_base] = std::move(result);
        vm->stack.resize(slot_base + 1);
        return;
    }

    if (fidx >= 0) {
        FunctionMeta *func = fd.jit_meta ? fd.jit_meta : &vm->chunk->functions[(uint32_t)fidx];
        // Read the three call-shape scalars from FunctionData when it has them, so this
        // path never dereferences FunctionMeta's second cache line (see the comment on
        // FunctionData::jit_param_count). push_jit_frame still touches line 1 for
        // code.data(), so this removes one of the two lines touched per call.
        const bool meta_cached = fd.jit_meta != nullptr;
        // Rest arguments must be packed before entering the callee. The
        // general call path already performs that setup; the in-place fast
        // path below deliberately only handles one argument per local slot.
        const int rest_param_index = meta_cached ? (int)fd.jit_rest_param_index : (int)func->rest_param_index;
        if (NARI_UNLIKELY(rest_param_index >= 0)) {
            jit_call(vm, argc);
            return;
        }
        const size_t locals_needed = meta_cached ? fd.jit_locals_count : func->var_names.size();
        const size_t param_count = meta_cached ? (size_t)fd.jit_param_count : (size_t)func->param_count;
        Value closure_root = func_val;

        // in-place arg move: shift args down over the func_val slot.
        // This is a memmove@plt call for what is usually 8-32 bytes. Replacing it with
        // an inline ascending copy for argc<=4 was measured: -6% instructions on a
        // call-heavy microbench but +8% cycles there (variable trip count mispredicts),
        // and neutral on tsc over 14 interleaved pairs. Left as memmove.
        std::memmove(vm->stack.data() + slot_base, vm->stack.data() + args_base, argc * sizeof(Value));

        // resize to exactly locals_needed slots from slot_base.
        vm->stack.resize(slot_base + locals_needed);

        // resize() initializes newly grown slots. Fill omitted parameters with
        // the function's language-specific missing-argument value.
        if (argc < locals_needed) {
            const size_t missing_end = std::min(param_count, locals_needed);
            Value missing_arg;
            if (argc < missing_end && (meta_cached ? fd.jit_js_undefined_params : func->js_undefined_params)) {
                missing_arg = vm->js_undefined_value;
            }
            for (size_t i = argc; i < missing_end; i++) {
                vm->stack[slot_base + i] = missing_arg;
            }
        }
        // Push the call frame and retain captures for the frame's lifetime.
        CallFrame &frame = vm->push_jit_frame(func, slot_base, closure_root);

        // FunctionData caches these borrowed pointers when the closure is built,
        // avoiding repeated vector bounds checks on every compiled call.
        auto *prev_captures = vm->jit_captures_raw;
        Value *prev_capture0 = vm->jit_capture0_raw;
        Value *prev_capture1 = vm->jit_capture1_raw;
        Value *prev_capture2 = vm->jit_capture2_raw;
        vm->jit_captures_raw = fd.jit_captures_raw;
        vm->jit_capture0_raw = fd.jit_capture0_raw;
        vm->jit_capture1_raw = fd.jit_capture1_raw;
        vm->jit_capture2_raw = fd.jit_capture2_raw;

        auto compiled = nari::jit::g_jit_compiler->compiled_at((uint32_t)fidx);
        if (!compiled) {
            vm->note_jit_callee((uint32_t)fidx); // count + compile hot callees reached only via JIT
            compiled = nari::jit::g_jit_compiler->compiled_at((uint32_t)fidx);
        }
        if (compiled) {
            vm->jit_call_depth++;
            compiled(vm);
            vm->jit_call_depth--;
            vm->jit_captures_raw = prev_captures;
            vm->jit_capture0_raw = prev_capture0;
            vm->jit_capture1_raw = prev_capture1;
            vm->jit_capture2_raw = prev_capture2;
            return;
        }

        vm->jit_captures_raw = prev_captures;
        vm->jit_capture0_raw = prev_capture0;
        vm->jit_capture1_raw = prev_capture1;
        vm->jit_capture2_raw = prev_capture2;
        // Interpreter capture opcodes read ownership from the frame. Compiled
        // calls borrow through jit_captures_raw and keep the closure rooted.
        frame.captures = fd.captures;
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

    if (fd.jit_builtin_id) {
        const ScriptRuntime::BuiltinFn fn = ScriptRuntime::jit_builtin_table()[fd.jit_builtin_id];
        Value *argv = argc > 0 ? &vm->stack[args_base] : nullptr;
        vm->stack[slot_base] = vm->call_builtin_member(fn, argv, argc);
        vm->stack.resize(slot_base + 1);
        jit_abort_on_runtime_error(vm);
        jit_deliver_pending_throw(vm);
        return;
    }

    // unknown func_idx (extension builtin, AST lambda, or runtime function).
    // fall back to full jit_call; stack hasn't been modified yet.
    jit_call(vm, argc);
}

void jit_call_value(VM *vm, uint32_t argc, uint32_t callee_label_idx) {
    jit_call_value_impl(vm, argc, callee_label_idx);
    vm->jit_safepoint();
}

uint64_t jit_string_char_code_at(void *string_obj, int64_t index) {
    const std::string &text = static_cast<StringObj *>(string_obj)->s;
    const int64_t code = index >= 0 && index < static_cast<int64_t>(text.size())
                             ? static_cast<unsigned char>(text[static_cast<size_t>(index)])
                             : -1;
    return Value::make_int(code).raw_bits();
}

// __js_str_code_point_at(s, i). char_code_at is byte-based here, so every code
// unit is 0..255 and the scripted helper's surrogate-pair branches are
// unreachable: the result is the indexed byte, or undefined out of range. Needs
// the VM only to source the undefined value.
uint64_t jit_string_code_point_at(void *vmp, void *string_obj, int64_t index) {
    const std::string &text = static_cast<StringObj *>(string_obj)->s;
    if (index < 0 || index >= static_cast<int64_t>(text.size())) {
        return static_cast<VM *>(vmp)->js_undefined_value.raw_bits();
    }
    return Value::make_int(static_cast<unsigned char>(text[static_cast<size_t>(index)])).raw_bits();
}

uint64_t jit_js_length(void *heap_obj) {
    auto *header = static_cast<HeapHeader *>(heap_obj);
    const size_t size = header->type_tag == ValueTag::Array ? static_cast<ArrayObj *>(heap_obj)->v.size()
                                                            : static_cast<StringObj *>(heap_obj)->s.size();
    return Value::make_int(static_cast<int64_t>(size)).raw_bits();
}

static void jit_call_frozen_builtin_1(VM *vm) {
    const size_t func_slot = vm->stack.size() - 2;
    FunctionData &fdata = vm->stack[func_slot].get_function();
    const ScriptRuntime::BuiltinFn fn = ScriptRuntime::jit_builtin_table()[fdata.jit_builtin_id];
    vm->stack[func_slot] = vm->call_builtin_member(fn, &vm->stack[func_slot + 1], 1);
    vm->stack.resize(func_slot + 1);
    jit_abort_on_runtime_error(vm);
    jit_deliver_pending_throw(vm);
}

void jit_call_typeof(VM *vm) {
    const size_t func_slot = vm->stack.size() - 2;
    Value result = vm->runtime->typeof_value(vm->stack[func_slot + 1]);
    vm->stack[func_slot] = std::move(result);
    vm->stack.resize(func_slot + 1);
    vm->jit_safepoint();
}

void jit_call_js_to_number(VM *vm) {
    jit_call_frozen_builtin_1(vm);
    vm->jit_safepoint();
}

// handles the full OP_CALL: pop args, pop func, dispatch, run callee to completion
static void jit_call_impl(VM *vm, uint32_t argc, const Value *receiver = nullptr) {
    jit_check_call_depth(vm);
    // read args directly from the VM stack top, no heap alloc
    size_t stack_top = vm->stack.size();
    size_t args_base = stack_top - argc;   // index of arg0 in vm->stack
    Value func = vm->stack[args_base - 1]; // func_val sits just below the args

    if (!func.is_function()) {
        // Delegate call trap: d(args) -> handler.call(target, [args]).
        // After the is_function() fast path so ordinary calls are untouched
        if (func.is_delegate()) {
            Value result = jit_runtime_call(vm, [&] { return vm->runtime->delegate_call(func, vm->stack.data() + args_base, argc); });
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
    if (fdata.jit_builtin_id) {
        const ScriptRuntime::BuiltinFn fn = ScriptRuntime::jit_builtin_table()[fdata.jit_builtin_id];
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
            std::vector<Value> args(vm->stack.begin() + args_base, vm->stack.end());
            vm->stack.resize(args_base - 1);
            vm->call_user_function(static_cast<uint32_t>(user_idx), args, nullptr, captures, receiver);
        } else {
            vm->call_user_function_stack((uint32_t)user_idx, args_base, argc, {}, receiver);
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

    // Not a chunk function: dispatch the Value through the runtime (FFI/native callbacks).
    std::vector<Value> args(vm->stack.begin() + args_base, vm->stack.end());
    vm->stack.resize(args_base - 1);

    Value func_val_copy = func;
    Value result = jit_runtime_call(vm, [&] { return vm->runtime->call_function_value(func_val_copy, args, receiver); });
    vm->push(result);
}

// the called global may be an allocating builtin (to_string, etc.)
// that never re-enters the interpreter safe-point, poll here on return.
void jit_call(VM *vm, uint32_t argc) {
    jit_call_impl(vm, argc);
    vm->jit_safepoint();
}

void jit_call_spread(VM *vm, uint32_t callee_label_idx) {
    Value args_array = vm->pop();
    Value callee = vm->pop();
    if (!args_array.is_array()) {
        fprintf(stderr, "OP_CALL_SPREAD: expected args array\n");
        vm->has_error = true;
        jit_abort_on_runtime_error(vm);
        return;
    }

    const auto &args = args_array.get_array();
    vm->push(std::move(callee));
    for (const Value &arg : args) {
        vm->push(arg);
    }
    jit_call_value(vm, static_cast<uint8_t>(args.size()), callee_label_idx);
}

void jit_call_method(VM *vm, uint32_t method_name_idx, uint32_t argc) {
    const std::string &method_name = vm->chunk->strings[method_name_idx];
    const size_t stack_top = vm->stack.size();
    const size_t args_base = stack_top - argc;
    const size_t obj_idx = args_base - 1;
    Value obj = vm->stack[obj_idx];

    // An object's, class instance's, or callable object's own field shadows
    // any builtin member of the same name.
    const Value *method = nullptr;
    if (obj.is_object()) {
        ObjectObj *method_obj = obj.get_obj_ptr();
        if (!method_obj->dict_mode) {
            uint32_t slot = UINT32_MAX;
            if (vm->method_ic_shapes[method_name_idx] == method_obj->shape) {
                slot = vm->method_ic_slots[method_name_idx];
            } else if (vm->method_ic_shapes2[method_name_idx] == method_obj->shape) {
                slot = vm->method_ic_slots2[method_name_idx];
            } else {
                uint32_t method_fid = vm->method_field_ids[method_name_idx];
                if (method_fid == UINT32_MAX) {
                    method_fid = intern_field(method_name);
                    vm->method_field_ids[method_name_idx] = method_fid;
                }
                const auto &field_ids = method_obj->shape->field_ids;
                auto method_slot = std::find(field_ids.begin(), field_ids.end(), method_fid);
                if (method_slot != field_ids.end()) {
                    slot = static_cast<uint32_t>(method_slot - field_ids.begin());
                }
                vm->method_ic_shapes2[method_name_idx] = vm->method_ic_shapes[method_name_idx];
                vm->method_ic_slots2[method_name_idx] = vm->method_ic_slots[method_name_idx];
                vm->method_ic_shapes[method_name_idx] = method_obj->shape;
                vm->method_ic_slots[method_name_idx] = slot;
            }
            if (slot != UINT32_MAX) {
                Value lazy_result;
                if (method_obj->invoke_lazy_field(slot, vm->stack.data() + args_base, argc, lazy_result)) {
                    vm->stack.resize(obj_idx);
                    vm->push(std::move(lazy_result));
                    return;
                }
                // reuse the resolved slot instead of a second get_field lookup
                method = method_obj->materialize_lazy_field(slot);
            }
        } else {
            method = method_obj->get_field(method_name);
        }
    } else if (obj.is_class_instance()) {
        method = obj.get_class_instance()->get_field(method_name);
    } else if (obj.is_function()) {
        uint32_t mfid = vm->method_field_ids[method_name_idx];
        if (NARI_UNLIKELY(mfid == UINT32_MAX)) {
            mfid = intern_field(method_name);
            vm->method_field_ids[method_name_idx] = mfid;
        }
        method = obj.get_function().get_property_by_id(mfid);
    }
    if (method && method->is_function()) {
        vm->stack[obj_idx] = *method;
        jit_call_impl(vm, argc, &obj);
        vm->jit_safepoint();
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

    if (method_name == "has_key" && argc == 1 && (obj.is_array() || obj.is_object() || obj.is_function())) {
        const Value &key = vm->stack[args_base];
        bool result;
        if (obj.is_array()) {
            result = static_cast<ArrayObj *>(obj.heap_ptr())->has_property(key.to_string());
        } else {
            // fn.length is kept out of the property bag; see FunctionData::int_length.
            if (obj.is_function() && !obj.get_function().int_length.is_none() && key.is_string() &&
                FunctionData::is_length_name(static_cast<StringObj *>(key.heap_ptr())->s)) {
                vm->stack.resize(obj_idx);
                vm->push(Value::make_bool(true));
                return;
            }
            const ObjectObj *object = obj.is_object() ? obj.get_obj_ptr() : obj.get_function().properties.get();
            if (!object) {
                result = false;
            } else if (key.is_string()) {
                auto *string_key = static_cast<StringObj *>(key.heap_ptr());
                if (string_key->immutable) {
                    if (string_key->field_id == UINT32_MAX) {
                        string_key->field_id = intern_field(string_key->s);
                    }
                    result = object->has_field_by_id(string_key->field_id);
                } else {
                    result = object->has_field(string_key->s);
                }
            } else {
                result = object->has_field(key.to_string());
            }
        }
        vm->stack.resize(obj_idx);
        vm->push(Value::make_bool(result));
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
        Value result = vm->call_builtin_member(fn, &vm->stack[obj_idx], (size_t)argc + 1);
        vm->stack.resize(obj_idx);
        vm->push(std::move(result));
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
    // User objects, class instances, and callable objects can carry a
    // shadowing field.
    const ValueTag t = obj.heap_tag();
    const Value *method = nullptr;
    if (t == ValueTag::Object) {
        method = obj.get_obj_ptr()->get_field(name);
    } else if (t == ValueTag::ClassInstance) {
        method = obj.get_class_instance()->get_field(name);
    } else if (t == ValueTag::Function) {
        method = obj.get_function().get_property(name);
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
        Value receiver = obj;
        vm->stack[obj_idx] = *method;
        jit_call_impl(vm, argc, &receiver);
        vm->jit_safepoint();
        return true;
    }
    return false;
}

void jit_method_length(VM *vm) {
    const Value &receiver = vm->peek(0);
    if (receiver.is_array()) {
        const int64_t length = static_cast<int64_t>(receiver.get_array().size());
        vm->pop();
        vm->push(Value::make_int(length));
        return;
    }
    if (receiver.is_string()) {
        const int64_t length = static_cast<int64_t>(receiver.get_string().size());
        vm->pop();
        vm->push(Value::make_int(length));
        return;
    }
    if (jit_try_shadow_method(vm, "length", 0)) {
        return;
    }
    Value obj = vm->pop();
    if (obj.is_object()) {
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
    const size_t elements_begin = vm->stack.size() - size;
    Value array = Value::make_array(vm->stack.data() + elements_begin, size);
    vm->stack.resize(elements_begin);
    vm->push(std::move(array));
    vm->jit_safepoint(); // result is on the stack (rooted) before any collect
}

void jit_array_push(VM *vm) {
    Value value = vm->pop();
    Value &target = vm->peek();
    if (!target.is_array()) {
        fprintf(stderr, "OP_ARRAY_PUSH: expected array\n");
        vm->has_error = true;
        return;
    }
    target.get_array().push_back(std::move(value));
}

void jit_array_spread(VM *vm) {
    Value iterable = vm->pop();
    Value &target_value = vm->peek();
    if (!target_value.is_array()) {
        fprintf(stderr, "OP_ARRAY_SPREAD: expected array\n");
        vm->has_error = true;
        return;
    }
    if (!iterable.is_array()) {
        fprintf(stderr, "Spread operator requires an iterable (array)\n");
        vm->has_error = true;
        return;
    }

    auto &target = target_value.get_array();
    const auto &source = iterable.get_array();
    if (target_value.heap_ptr() == iterable.heap_ptr()) {
        std::vector<Value> copy(source.begin(), source.end());
        target.insert(target.end(), copy.data(), copy.data() + copy.size());
    } else {
        target.insert(target.end(), source.begin(), source.end());
    }
}

void jit_make_closure(VM *vm, void *site_ptr) {
    const uint8_t *site = static_cast<const uint8_t *>(site_ptr);
    auto read_u16 = [](const uint8_t *p) -> uint16_t { return (uint16_t)((p[0] << 8) | p[1]); };
    uint16_t func_idx = read_u16(site + 1);
    uint16_t capture_count = read_u16(site + 3);
    const uint8_t *descriptor = site + 5;
    FunctionMeta *func = &vm->chunk->functions[func_idx];
    CapturesList captures;
    auto &frame = vm->current_frame();

    // 2.66M closures are built per tsc run and their capture cells repeat 94% of the
    // time, so reuse the per-frame cached CapturesList outright when every cell matches.
    // Compare in place against the cached vector as cells are computed: a full match costs
    // zero allocations and needs no scratch buffer, and the first mismatch at index k
    // materializes a fresh vector seeded with the k cells already known to match.
    // A source==2 capture snapshots the global's value into a fresh cell, so a cached
    // vector holding one would serve a stale value: those stop reuse.
    const CapturesList &hit = frame.single_capture_cache;
    bool tracking = capture_count >= 1 && hit && hit->size() == (size_t)capture_count;
    bool cacheable = true;
    for (uint16_t i = 0; i < capture_count; i++, descriptor += 3) {
        uint8_t source = descriptor[0];
        uint16_t idx = read_u16(descriptor + 1);
        CellRef cell;
        if (source == 0) {
            cell = frame.get_or_create_cell(idx, vm->stack[frame.slot_base + idx]);
            vm->stack[frame.slot_base + idx] = *cell;
        } else if (source == 1) {
            auto *parent_captures = vm->jit_captures_raw;
            if (parent_captures && idx < parent_captures->size()) {
                cell = (*parent_captures)[idx];
            } else {
                cell = CellRef::make(Value::none());
            }
        } else {
            cell = CellRef::make(vm->get_global(vm->chunk->strings[idx]));
            cacheable = false;
            if (tracking) {
                captures = std::make_shared<std::vector<CellRef>>(capture_count);
                for (uint16_t k = 0; k < i; k++) {
                    (*captures)[k] = (*hit)[k];
                }
                tracking = false;
            }
        }
        if (tracking) {
            if ((*hit)[i] == cell) {
                continue; // cached vector still correct at this index
            }
            captures = std::make_shared<std::vector<CellRef>>(capture_count);
            for (uint16_t k = 0; k < i; k++) {
                (*captures)[k] = (*hit)[k];
            }
            tracking = false;
        }
        if (!captures) {
            captures = std::make_shared<std::vector<CellRef>>(capture_count);
        }
        (*captures)[i] = std::move(cell);
    }
    if (tracking) {
        captures = hit;
    } else if (cacheable && captures) {
        frame.single_capture_cache = captures;
    }

    Value closure = Value::make_function(func->name);
    auto &function = closure.get_function();
    function.captures = std::move(captures);
    function.cache_jit_captures();
    function.jit_func_idx = (int32_t)func_idx;
    function.jit_locals_count = (uint32_t)func->var_names.size();
    function.jit_meta = func;
    function.jit_param_count = (uint8_t)func->param_count;
    function.jit_rest_param_index = (int8_t)func->rest_param_index;
    function.jit_js_undefined_params = func->js_undefined_params;
    function.jit_inline_kind = func->jit_inline_kind;
    function.jit_inline_imm = func->jit_inline_imm;
    vm->push(std::move(closure));
    vm->jit_safepoint();
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

// ensure the VM value stack has headroom for `n` more entries before JIT code manually advances _M_finish to
// batch-store
void jit_reserve(VM *vm, uint32_t n) {
    vm->stack.reserve(vm->stack.size() + (size_t)n);
}

void jit_get_index(VM *vm) {
    Value key = vm->pop();
    Value obj = vm->pop();

    if (key.is_float()) {
        double f = key.get_float();
        if (f == std::floor(f) && !std::isinf(f) && !std::isnan(f)) {
            key = Value::make_int((int64_t)f);
        }
    }
    if (obj.is_array()) {
        auto *array_obj = static_cast<ArrayObj *>(obj.heap_ptr());
        auto &arr = array_obj->v;
        if (!key.is_int()) {
            const Value *v = array_obj->get_property(key.to_string());
            vm->push(v ? *v : Value::none());
            return;
        }
        int64_t idx = key.get_int();
        if (idx >= 0 && idx < (int64_t)arr.size()) {
            vm->push(arr[idx]);
        } else {
            vm->push(Value::none());
        }
    } else if (obj.is_object()) {
        const Value *v;
        if (key.is_string() && !key.is_sso()) {
            auto *string_key = static_cast<StringObj *>(key.heap_ptr());
            if (string_key->immutable) {
                if (NARI_UNLIKELY(string_key->field_id == UINT32_MAX)) {
                    string_key->field_id = intern_field(string_key->s);
                }
                v = obj.get_obj_ptr()->get_field_by_id(string_key->field_id);
            } else {
                v = obj.get_obj_ptr()->get_field(string_key->s);
            }
        } else {
            v = obj.get_obj_ptr()->get_field(key.to_string());
        }
        vm->push(v ? *v : Value::none());
    } else if (obj.is_function()) {
        const Value *v;
        if (key.is_string() && !key.is_sso()) {
            auto *string_key = static_cast<StringObj *>(key.heap_ptr());
            if (string_key->immutable) {
                if (NARI_UNLIKELY(string_key->field_id == UINT32_MAX)) {
                    string_key->field_id = intern_field(string_key->s);
                }
                v = obj.get_function().get_property_by_id(string_key->field_id);
            } else {
                v = obj.get_function().get_property(string_key->s);
            }
        } else {
            v = obj.get_function().get_property(key.to_string());
        }
        vm->push(v ? *v : Value::none());
    } else if (obj.is_delegate()) {
        // Delegate get trap. After the object fast path so shape prop-IC is untouched.
        vm->push(jit_runtime_call(vm, [&] { return vm->runtime->delegate_get(obj, key); }));
    } else if (obj.is_string()) {
        if (!key.is_int()) {
            vm->push(Value::none());
            return;
        }
        int64_t idx = key.get_int();
        const std::string &s = obj.get_string();
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

    if (key.is_float()) {
        double f = key.get_float();
        if (f == std::floor(f) && !std::isinf(f) && !std::isnan(f)) {
            key = Value::make_int((int64_t)f);
        }
    }
    if (obj.is_array()) {
        auto *array_obj = static_cast<ArrayObj *>(obj.heap_ptr());
        auto &arr = array_obj->v;
        if (key.is_int()) {
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
        } else {
            array_obj->set_property(key.to_string(), val);
        }
    } else if (obj.is_object()) {
        if (key.is_string() && !key.is_sso()) {
            auto *string_key = static_cast<StringObj *>(key.heap_ptr());
            if (string_key->immutable) {
                if (NARI_UNLIKELY(string_key->field_id == UINT32_MAX)) {
                    string_key->field_id = intern_field(string_key->s);
                }
                obj.get_obj_ptr()->set_field_by_id(string_key->field_id, val);
            } else {
                obj.get_obj_ptr()->set_field(string_key->s, val);
            }
        } else {
            obj.get_obj_ptr()->set_field(key.to_string(), val);
        }
    } else if (obj.is_function()) {
        obj.get_function().set_property(key.to_string(), val);
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
    } else if (obj.is_function()) {
        const Value *v = obj.get_function().get_property(name);
        vm->push(v ? *v : Value::none());
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
            const Value *sc = handle->result.is_object() ? handle->result.get_obj_ptr()->get_field("status_code") : nullptr;
            vm->push(sc ? *sc : Value::none());
        } else if (name == "duration") {
            if (handle->state == HandleData::Running) {
                auto now = chrono::steady_clock::now();
                auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - handle->start_time);
                vm->push(Value::make_int(elapsed.count()));
            } else {
                auto elapsed = chrono::duration_cast<chrono::milliseconds>(handle->end_time - handle->start_time);
                vm->push(Value::make_int(elapsed.count()));
            }
        } else if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
            // compiler-internal speculative probes stay silent
            vm->push(Value::none());
        } else {
            // a handle is not it's result
            fprintf(stderr, "RuntimeError: unknown handle member '%s' (valid: await, ready, failed, error, status_code, duration)\n", name.c_str());
            vm->has_error = true;
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
        // compiler-internal properties (like __variant, __data) are used as speculative probes in match/pattern
        // expressions
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
            vm->push(Value::none());
        } else {
            // JIT helpers cannot throw C++ exceptions through JIT-compiled frames; use the has_error flag pattern
            // instead.
            fprintf(stderr, "RuntimeError: cannot access property '%s' on %s value\n", name.c_str(), obj.is_none() ? "null" : "non-object");
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
        obj.get_obj_ptr()->set_field_by_id(vm->field_id_for_name((uint16_t)name_idx), std::move(val));
    } else if (obj.is_function()) {
        obj.get_function().set_property_by_id(vm->field_id_for_name((uint16_t)name_idx), std::move(val));
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
    // else: unknown annotation, skip, this should only be possible if we add new types in the future, and the
    // interpreter doesn't support it yet

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
// we require that CellRef is all-zero when null, and that CapturesList is a pair of pointers without
// padding.
bool stl_layouts_ok() {
    static const bool ok = [] {
        bool good = stl::null_is_all_zero<CellRef>() && stl::null_is_all_zero<CapturesList>() &&
                    sizeof(CapturesList) == 2 * sizeof(void *);
        if (!good) {
            fprintf(stderr, "nari: smart-pointer ABI check failed; JIT disabled (interpreter only)\n");
        }
        return good;
    }();
    return ok;
}

MethodJITBase *g_jit_compiler = nullptr;
bytecode::VM *g_compile_vm = nullptr;
bool g_compile_captures_ok = false;

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
