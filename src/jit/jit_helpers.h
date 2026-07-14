#pragma once

#include "asmjit_jit.h"

// helper function definitions for asmjit

using namespace nari::bytecode;

extern "C" {
#ifdef NARI_ENABLE_GDB_JIT
void nari_jit_initial_load_complete();
#endif
void jit_load_const(VM *, uint32_t);
void jit_push_int(VM *, int64_t);
void jit_push_float(VM *, int64_t); // f64 bit pattern
void jit_push_bool(VM *, int64_t);
void jit_load_var(VM *, uint32_t);
void jit_store_var(VM *, uint32_t);
void jit_load_global(VM *, uint32_t);
void jit_store_global(VM *, uint32_t);
void jit_pop(VM *);
void jit_dup(VM *);
void jit_load_none(VM *);
void jit_load_true(VM *);
void jit_load_false(VM *);
void jit_load_zero(VM *);
void jit_load_one(VM *);
void jit_add(VM *);
void jit_sub(VM *);
void jit_mul(VM *);
void jit_div(VM *);
void jit_mod(VM *);
void jit_neg(VM *);
void jit_str_concat(VM *);
void jit_str_concat_inplace(VM *);
void jit_format_value(VM *, uint32_t);
void jit_bit_and(VM *);
void jit_bit_or(VM *);
void jit_bit_xor(VM *);
void jit_bit_not(VM *);
void jit_lshift(VM *);
void jit_rshift(VM *);
void jit_not(VM *);
void jit_eq(VM *);
void jit_ne(VM *);
void jit_lt(VM *);
void jit_le(VM *);
void jit_gt(VM *);
void jit_ge(VM *);
int64_t jit_check_truthy(VM *);
int64_t jit_check_none(VM *);
void jit_call(VM *, uint32_t);
void jit_call_value(VM *, uint32_t, uint32_t);
void jit_return(VM *);
void jit_make_array(VM *, uint32_t);
void jit_iter_array(VM *);
void jit_make_object(VM *, uint32_t);
void jit_make_object_site(VM *, uint32_t, void *);
void jit_reserve(VM *, uint32_t);
void jit_get_index(VM *);
void jit_set_index(VM *);
void jit_get_property(VM *, uint32_t);
void jit_set_property(VM *, uint32_t);
void jit_load_capture(VM *, uint32_t);
void jit_store_capture(VM *, uint32_t);
void jit_make_closure(VM *, uint32_t, uint32_t, const uint8_t *);
void jit_spawn(VM *);
void jit_pow(VM *);
void jit_throw(VM *);
void jit_new_instance(VM *, uint32_t, uint32_t);
void jit_load_this(VM *);
void jit_call_method(VM *, uint32_t, uint32_t);
void jit_method_length(VM *);
void jit_method_char_code_at(VM *);
void jit_method_starts_with(VM *);
void jit_method_substr(VM *, uint32_t);
void jit_check_type(VM *, uint32_t type_str_idx, uint32_t packed);
void jit_poll_shutdown(VM *);

void jit_slot_store_raw(VM *, uint32_t idx, uint64_t raw);
void jit_slot_copy(VM *, uint32_t src_idx, uint32_t dst_idx);
}
