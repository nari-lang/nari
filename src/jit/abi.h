#pragma once
// Layout and NaN-box representation consumed by generated machine code.
#ifndef DISABLE_JIT

#include "bytecode/bytecode.h"
#include <cstddef>
#include <cstdint>

namespace nari {
namespace jit {
namespace abi {

static_assert(sizeof(Value) == 8, "JIT requires eight-byte Values");

// The compiler and JIT use the same C++ ABI. __builtin_offsetof handles the
// non-standard-layout VM/GC classes without constructing them at startup.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
#define NARI_JIT_ABI_OFF(T, member) static_cast<int64_t>(__builtin_offsetof(T, member))

// Offsets used by both method and trace code generators.
inline const int64_t ValSize = static_cast<int64_t>(sizeof(Value));
inline const int64_t VMRuntimeOff = NARI_JIT_ABI_OFF(bytecode::VM, runtime);
inline const int64_t RuntimeTypeofValuesOff = __builtin_offsetof(ScriptRuntime, typeof_values);
inline const int64_t VMStackStartOff = NARI_JIT_ABI_OFF(bytecode::VM, stack) + offsetof(Array, storage_begin);
inline const int64_t VMStackFinishOff = NARI_JIT_ABI_OFF(bytecode::VM, stack) + offsetof(Array, storage_end);

inline constexpr struct {
    int64_t word_off = 6;
    int64_t _none = static_cast<int64_t>(0xFFFF);
    int64_t _int = static_cast<int64_t>(0xFFFC);
    int64_t _float = static_cast<int64_t>(0x0000);
    int64_t _bool = static_cast<int64_t>(0xFFFE);
    int64_t _heap = static_cast<int64_t>(0xFFFB);
    int64_t _array = static_cast<int64_t>(ValueTag::Array);
    int64_t _string = static_cast<int64_t>(ValueTag::String);
    int64_t _function = static_cast<int64_t>(ValueTag::Function);
} tag;

inline constexpr struct {
    uint64_t int_tag = 0xFFFC000000000000ULL;
    uint64_t bool_tag = 0xFFFE000000000000ULL;
    uint64_t none_tag = 0xFFFF000000000000ULL;
    uint64_t ptr_mask = 0x0000FFFFFFFFFFFFULL;
} nb;

inline const int64_t ObjShapeVersionOff = NARI_JIT_ABI_OFF(ObjectObj, shape_version);
inline const int64_t ObjShapeOff = NARI_JIT_ABI_OFF(ObjectObj, shape);
inline const int64_t ObjFieldsOff = NARI_JIT_ABI_OFF(ObjectObj, fields);
inline const int64_t ObjFieldsStartOff = ObjFieldsOff + offsetof(Array, storage_begin);
inline const int64_t ObjFieldsFinishOff = ObjFieldsOff + offsetof(Array, storage_end);
inline const int64_t ObjLazyFieldMaskOff = NARI_JIT_ABI_OFF(ObjectObj, lazy_field_mask);
inline const int64_t FDInlineKindOff = NARI_JIT_ABI_OFF(FunctionData, jit_inline_kind);
inline const int64_t FDNativeKindOff = NARI_JIT_ABI_OFF(FunctionData, jit_native_kind);
inline const int64_t FDCapturesRawOff = NARI_JIT_ABI_OFF(FunctionData, jit_captures_raw);
inline const int64_t FDCapture0RawOff = NARI_JIT_ABI_OFF(FunctionData, jit_capture0_raw);
inline const int64_t FDCapture1RawOff = NARI_JIT_ABI_OFF(FunctionData, jit_capture1_raw);
inline const int64_t FDCapture2RawOff = NARI_JIT_ABI_OFF(FunctionData, jit_capture2_raw);
inline const int64_t VMCapturesRawOff = NARI_JIT_ABI_OFF(bytecode::VM, jit_captures_raw);
inline const int64_t VMCapture0RawOff = NARI_JIT_ABI_OFF(bytecode::VM, jit_capture0_raw);
inline const int64_t VMCapture1RawOff = NARI_JIT_ABI_OFF(bytecode::VM, jit_capture1_raw);
inline const int64_t VMCapture2RawOff = NARI_JIT_ABI_OFF(bytecode::VM, jit_capture2_raw);
inline const int64_t VMStackCapacityOff = NARI_JIT_ABI_OFF(bytecode::VM, stack) + offsetof(Array, storage_capacity);
inline const int64_t VMGlobalCacheStartOff = NARI_JIT_ABI_OFF(bytecode::VM, global_cache) + offsetof(Array, storage_begin);
inline const int64_t VMGlobalCacheValidStartOff = NARI_JIT_ABI_OFF(bytecode::VM, global_cache_valid) + offsetof(ByteArray, storage_begin);
inline const int64_t VMGlobalCacheValidFinishOff = NARI_JIT_ABI_OFF(bytecode::VM, global_cache_valid) + offsetof(ByteArray, storage_end);
inline const int64_t ArrayVecStartOff = NARI_JIT_ABI_OFF(ArrayObj, v) + offsetof(Array, storage_begin);
inline const int64_t ArrayVecFinishOff = NARI_JIT_ABI_OFF(ArrayObj, v) + offsetof(Array, storage_end);
inline const int64_t ArrayVecCapacityOff = NARI_JIT_ABI_OFF(ArrayObj, v) + offsetof(Array, storage_capacity);
inline const int64_t FramesStartOff = NARI_JIT_ABI_OFF(bytecode::VM, frames) + offsetof(bytecode::FrameArray, storage_begin);
inline const int64_t FramesFinishOff = NARI_JIT_ABI_OFF(bytecode::VM, frames) + offsetof(bytecode::FrameArray, storage_end);
inline const int64_t VMFramesCapacityOff = NARI_JIT_ABI_OFF(bytecode::VM, frames) + offsetof(bytecode::FrameArray, storage_capacity);
inline const int64_t FrameSize = static_cast<int64_t>(sizeof(bytecode::CallFrame));

inline const int64_t FrameFunctionOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, function);
inline const int64_t FrameIpOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, ip);
inline const int64_t SlotBaseOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, slot_base);
inline const int64_t FrameCapturesOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, captures);
inline const int64_t FrameClosureRootOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, closure_root);
inline const int64_t FrameReceiverOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, receiver);
inline const int64_t FrameInlineUpvalueIdxOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, inline_upvalue_idx);
inline const int64_t FrameInlineUpvalueOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, inline_upvalue);
inline const int64_t FrameOpenUpvalOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, open_upvalues);
inline const int64_t FrameSingleCaptureCacheOff = NARI_JIT_ABI_OFF(bytecode::CallFrame, single_capture_cache);

inline const int64_t VMJsGetPropStaticICStartOff =
    NARI_JIT_ABI_OFF(bytecode::VM, js_get_prop_static_ic) + offsetof(bytecode::JsGetPropStaticICArray, storage_begin);
inline const int64_t VMJsGetPropStaticICFinishOff =
    NARI_JIT_ABI_OFF(bytecode::VM, js_get_prop_static_ic) + offsetof(bytecode::JsGetPropStaticICArray, storage_end);

inline const int64_t VMTraceItersOff = NARI_JIT_ABI_OFF(bytecode::VM, trace_last_iters);
inline const int64_t ObjFrozenOff = NARI_JIT_ABI_OFF(ObjectObj, frozen);
inline const int64_t ObjDictModeOff = NARI_JIT_ABI_OFF(ObjectObj, dict_mode);
inline const int64_t FDInlineImmOff = NARI_JIT_ABI_OFF(FunctionData, jit_inline_imm);
inline const int64_t FDFuncIdxOff = NARI_JIT_ABI_OFF(FunctionData, jit_func_idx);
inline const int64_t FDLocalsCountOff = NARI_JIT_ABI_OFF(FunctionData, jit_locals_count);
inline const int64_t FDMetaOff = NARI_JIT_ABI_OFF(FunctionData, jit_meta);

#undef NARI_JIT_ABI_OFF
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace abi
} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
