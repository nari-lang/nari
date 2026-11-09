// JIT-callable shared op bodies (split out of bytecode.cpp).
#include "ast.h"
#include "bytecode.h"
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

// Constant-key JS property ops (OP_JS_GET/SET_PROP_STATIC, OP_JS_POSTINC) and their shared shape->slot stub cache.
namespace {
// Global megamorphic property stub cache, keyed by (shape, field_id)
//
// A shape's field layout is fixed at construction and shapes belong to the process-global registry,
// so (shape, field_id) -> slot is immutable.
struct PropStubEntry {
    const ObjectShape *shape = nullptr;
    uint32_t fid = 0;
    uint32_t slot = 0;
};

enum {
    kPropStubN = 4096
};
PropStubEntry g_prop_stub[kPropStubN];
inline size_t prop_stub_hash(const ObjectShape *s, uint32_t fid) {
    uint64_t h = (static_cast<uint64_t>(((uintptr_t)s >> 4))) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(fid) * 0x100000001B3ull;
    return static_cast<size_t>(((h ^ (h >> 29)) & (static_cast<uint64_t>(kPropStubN - 1))));
}
} // namespace

// Constant-key property store, mirrors OP_JS_GET_PROP_STATIC on the write side
void jit_js_set_prop_static(VM *vm, uint32_t name_idx) {
    const size_t val_idx = vm->stack.size() - 1;
    const size_t obj_idx = val_idx - 1;
    Value &obj = vm->stack[obj_idx];

    if (NARI_LIKELY(obj.is_object())) {
        ObjectObj *object = obj.get_obj_ptr();
        if (NARI_LIKELY(!object->dict_mode && !object->frozen)) {
            static const uint32_t proto_id = intern_field("__proto__");
            const uint32_t setter_id = vm->setter_field_id_for_name(static_cast<uint16_t>(name_idx));
            const uint64_t guard_mask = (uint64_t{ 1 } << (setter_id & 63)) | (uint64_t{ 1 } << (proto_id & 63));
            if (NARI_LIKELY((object->shape->field_mask & guard_mask) == 0)) {
                Value value = vm->stack[val_idx];
                object->set_field_by_id(vm->field_id_for_name(static_cast<uint16_t>(name_idx)), value);
                vm->stack[obj_idx] = std::move(value);
                vm->stack.resize(obj_idx + 1);
                return;
            }
        }
    }

    Value args[] = { obj, vm->chunk->get_const_string(name_idx), vm->stack[val_idx] };
    const Value &helper = vm->get_global("__js_set_prop_static");
    Value result = vm->call_function_value_span(helper, args, 3);
    vm->stack[obj_idx] = std::move(result);
    vm->stack.resize(obj_idx + 1);
}

// Constant-key post-increment: `obj.k++`
void jit_js_postinc(VM *vm, uint32_t name_idx) {
    const size_t obj_idx = vm->stack.size() - 1;
    Value obj = vm->stack[obj_idx];

    if (NARI_LIKELY(obj.is_object())) {
        ObjectObj *object = obj.get_obj_ptr();
        if (NARI_LIKELY(!object->dict_mode && !object->frozen)) {
            static const uint32_t proto_id = intern_field("__proto__");
            const uint32_t getter_id = vm->getter_field_id_for_name(static_cast<uint16_t>(name_idx));
            const uint32_t setter_id = vm->setter_field_id_for_name(static_cast<uint16_t>(name_idx));
            const uint64_t guard_mask =
                (uint64_t{ 1 } << (getter_id & 63)) | (uint64_t{ 1 } << (setter_id & 63)) | (uint64_t{ 1 } << (proto_id & 63));
            if (NARI_LIKELY((object->shape->field_mask & guard_mask) == 0)) {
                const uint32_t fid = vm->field_id_for_name(static_cast<uint16_t>(name_idx));
                const PropStubEntry &e = g_prop_stub[prop_stub_hash(object->shape, fid)];
                if (NARI_LIKELY(e.shape == object->shape && e.fid == fid)) {
                    if (Value *field = object->materialize_lazy_field(e.slot)) {
                        // Int48 is the only case worth inlining: `pos` is always a
                        // small integer. Anything else (double, string, undefined)
                        // needs full ToNumber and takes the helper.
                        if (NARI_LIKELY(field->is_int())) {
                            const int64_t old = field->get_int();
                            if (NARI_LIKELY(old < Value::INT48_MAX)) {
                                *field = Value::make_int(old + 1);
                                vm->stack[obj_idx] = Value::make_int(old);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    Value args[] = { obj, vm->chunk->get_const_string(name_idx) };
    const Value &helper = vm->get_global("__js_postinc");
    Value result = vm->call_function_value_span(helper, args, 2);
    vm->stack[obj_idx] = std::move(result);
}

void jit_js_get_prop_static(VM *vm, uint32_t name_idx) {
    const size_t obj_idx = vm->stack.size() - 1;
    Value obj = vm->stack[obj_idx];
    static const bool disabled = std::getenv("NARI_DISABLE_JS_GET_PROP_STATIC") != nullptr;
    if (!disabled && obj.is_object()) {
        ObjectObj *object = obj.get_obj_ptr();
        if (!object->dict_mode) {
            const uint32_t cached_fid = vm->field_id_for_name(static_cast<uint16_t>(name_idx));
            const PropStubEntry &e = g_prop_stub[prop_stub_hash(object->shape, cached_fid)];
            if (e.shape == object->shape && e.fid == cached_fid) {
                if (Value *field = object->materialize_lazy_field(e.slot)) {
                    // Keep feeding the name-keyed IC that the *inline* JIT fast path
                    // reads. Returning without refilling it starves that path and costs
                    // far more than this cache saves (measured: +1.75B instructions).
                    if (name_idx < vm->js_get_prop_static_ic.size()) {
                        auto &ic = vm->js_get_prop_static_ic[name_idx];
                        if (ic.shape != object->shape) {
                            ic.shape2 = ic.shape;
                            ic.slot2 = ic.slot;
                            ic.lazy_mask2 = ic.lazy_mask;
                            ic.shape = object->shape;
                            ic.slot = e.slot;
                            ic.lazy_mask = e.slot < 64 ? uint64_t{ 1 } << e.slot : uint64_t{ 0 };
                        }
                    }
                    vm->stack[obj_idx] = *field;
                    return;
                }
            }
        }
        const uint32_t getter_field_id = vm->getter_field_id_for_name(static_cast<uint16_t>(name_idx));
        const uint64_t getter_mask = uint64_t{ 1 } << (getter_field_id & 63);
        if (object->dict_mode || (object->shape->field_mask & getter_mask)) {
            const Value *getter = object->get_field_by_id(getter_field_id);
            if (getter && getter->is_function()) {
                Value args[] = { obj, vm->chunk->get_const_string(name_idx) };
                const Value &helper = vm->get_global("__js_get_prop_static");
                vm->stack[obj_idx] = vm->call_function_value_span(helper, args, 2);
                return;
            }
        }

        const uint32_t field_id = vm->field_id_for_name(static_cast<uint16_t>(name_idx));
        const uint64_t field_mask = uint64_t{ 1 } << (field_id & 63);
        if (object->dict_mode || (object->shape->field_mask & field_mask)) {
            if (!object->dict_mode) {
                const uint32_t getter_slot = object->shape->slot_of(getter_field_id);
                const uint32_t field_slot = object->shape->slot_of(field_id);
                const bool plain_field = getter_slot == ObjectShape::NoSlot && field_slot != ObjectShape::NoSlot;
                if (plain_field) {
                    g_prop_stub[prop_stub_hash(object->shape, field_id)] = PropStubEntry{ object->shape, field_id, field_slot };
                }
                if (plain_field && name_idx < vm->js_get_prop_static_ic.size()) {
                    const uint32_t slot = field_slot;
                    auto &ic = vm->js_get_prop_static_ic[name_idx];
                    ic.shape2 = ic.shape;
                    ic.slot2 = ic.slot;
                    ic.lazy_mask2 = ic.lazy_mask;
                    ic.shape = object->shape;
                    ic.slot = slot;
                    ic.lazy_mask = slot < 64 ? uint64_t{ 1 } << slot : uint64_t{ 0 };
                }
            }
            const Value *field = object->get_field_by_id(field_id);
            if (field) {
                vm->stack[obj_idx] = *field;
                return;
            }
        }

        Value args[] = { obj, vm->chunk->get_const_string(name_idx), Value::make_bool(true) };
        const Value &helper = vm->get_global("__js_get_prop_static");
        vm->stack[obj_idx] = vm->call_function_value_span(helper, args, 3);
        return;
    }

    Value args[] = { obj, vm->chunk->get_const_string(name_idx) };
    const Value &helper = vm->get_global("__js_get_prop_static");
    vm->stack[obj_idx] = vm->call_function_value_span(helper, args, 2);
}

} // namespace bytecode
} // namespace nari
