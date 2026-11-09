#ifndef DISABLE_JIT
#include "asmjit_dump.h"
#include "asmjit_jit.h"
#include "asmjit_jit_async.h"
#include "core_types.h"
#include "ir.h"
#include "ir_build.h"
#include "ir_opt.h"
#include "jit_arch.h"
#include "jit_helpers.h"
#include "jit_layout.h"
#include "stl_layout.h"
#include "util.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "ir_emit.h"

using namespace asmjit;
using namespace nari::bytecode;

namespace nari {
namespace jit {

// both operands proven Float -> inline native f64 op on the value stack (no helper call, no runtime type dispatch).
void AsmJITMethodCompiler::emit_float_binop(IrEmitContext &ctx, ir::Op op) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    // fc.get()/set() so the finish load is reused by any following emitter in the same block.
    arch::Gp endp = fc.get();
    arch::Vec fa = arch::new_vec_f64(cc, "fb_a");
    arch::Vec fb = arch::new_vec_f64(cc, "fb_b");
    arch::load_f64(cc, fb, endp, static_cast<int>((-ValSize)));     // top  = rhs
    arch::load_f64(cc, fa, endp, static_cast<int>((-2 * ValSize))); // 2nd  = lhs
    arch::Vec fr = arch::new_vec_f64(cc, "fb_r");
    switch (op) {
        case ir::Op::DynAdd:
        case ir::Op::IAdd:
        case ir::Op::FAdd:
            arch::fadd(cc, fr, fa, fb);
            break;
        case ir::Op::DynSub:
        case ir::Op::ISub:
        case ir::Op::FSub:
            arch::fsub(cc, fr, fa, fb);
            break;
        case ir::Op::DynMul:
        case ir::Op::IMul:
        case ir::Op::FMul:
            arch::fmul(cc, fr, fa, fb);
            break;
        default:
            arch::fdiv(cc, fr, fa, fb);
            break; // DynDiv / FDiv
    }
    arch::store_f64(cc, endp, static_cast<int>((-2 * ValSize)), fr); // write result to 2nd
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));              // pop one
    fc.set(endp);
}

// Slot-address CSE: IR functions cannot have upvalues, so slot_base is
// fixed at frame init; hoist `slot_base*8` to entry (see emit_ir_slot_addr).
arch::Gp emit_ir_slot_addr(IrEmitContext &ctx, uint32_t slot, const char *name) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    (void)name;
    // stack_start + slot_base_bytes + slot*8, with the base cached per block
    // instead of reloaded and re-added for every slot access. IR functions
    // can't have upvalues, so slot_base is fixed and no upvalue guard is
    // needed. Callers only ever read the returned register.
    arch::Gp base = fc.slot_base();
    if (slot == 0) {
        return base;
    }
    arch::Gp addr = cc.new_gp64("ir_sa");
    arch::lea(cc, addr, base, static_cast<int64_t>(slot) * static_cast<int64_t>(ValSize));
    return addr;
}

// Slot access as a memory operand: folds the slot displacement into the
// load/store, so no address register (and no extra vreg for the register
// allocator) is needed at all.
arch::Mem emit_ir_slot_mem(IrEmitContext &ctx, uint32_t slot) {
    auto &fc = ctx.fc;
    return arch::ptr(fc.slot_base(), static_cast<int>((static_cast<int64_t>(slot) * static_cast<int64_t>(ValSize))));
}

void AsmJITMethodCompiler::emit_ir_load_slot(IrEmitContext &ctx, uint32_t slot) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &slot_cache = ctx.slot_cache;
    // fast path via SlotRegCache. If this slot's raw value is
    // already in a vreg from an earlier LoadSlot/StoreSlot in this block, skip the memory reload.
    {
        arch::Gp cached;
        if (slot_cache.try_get(slot, cached)) {
            fc.push_deferred(cached);
            return;
        }
    }
    arch::Mem slotm = emit_ir_slot_mem(ctx, slot);
    arch::Gp raw = cc.new_gp64("ir_ls_raw");
    arch::load(cc, raw, slotm);
    fc.push_deferred(raw);
    // cache the just-loaded raw value for this slot.
    slot_cache.set(slot, raw);
}

void AsmJITMethodCompiler::emit_ir_store_slot(IrEmitContext &ctx, uint32_t slot) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &slot_cache = ctx.slot_cache;
    arch::Mem slotm = emit_ir_slot_mem(ctx, slot);
    // store_slot doesn't modify _M_finish, and it need not touch the value stack
    // at all while its input is still pending: reading the value out of the
    // register avoids both the load and -- more importantly -- the queue flush
    // that fc.get() would force at every assignment. The entry stays queued,
    // because store_slot does not pop.
    arch::Gp raw;
    if (const auto *top = fc.peek(0)) {
        if (top->is_imm) {
            raw = cc.new_gp64("ir_ss_imm");
            cc.mov(raw, Imm(top->imm));
        } else {
            raw = top->reg;
        }
    } else {
        arch::Gp endp = fc.get();
        raw = cc.new_gp64("ir_ss_raw");
        arch::load(cc, raw, arch::ptr(endp, static_cast<int>((-ValSize))));
    }
    arch::store(cc, slotm, raw);
    // cache the just-stored raw value for this slot.
    slot_cache.set(slot, raw);
}

// fused slot-store emitters. Write a raw 64-bit value directly into a stack slot
void AsmJITMethodCompiler::emit_ir_store_raw_to_slot(IrEmitContext &ctx, uint32_t slot, uint64_t raw) {
    auto &cc = ctx.cc;
    auto &slot_cache = ctx.slot_cache;
    arch::Mem slotm = emit_ir_slot_mem(ctx, slot);
    arch::Gp rawr = cc.new_gp64("ir_sifs_raw");
    cc.mov(rawr, Imm(static_cast<int64_t>(raw)));
    arch::store(cc, slotm, rawr);
    // this immediate is now the slot's live value, cache it.
    slot_cache.set(slot, rawr);
}

void AsmJITMethodCompiler::emit_ir_store_imm_slot(IrEmitContext &ctx, int64_t imm, uint32_t slot) {
    // Int48 -> raw NaN-boxed uint64 (or float-fallback for out-of-range).
    uint64_t raw = Value::make_int_checked(imm).raw_bits();
    emit_ir_store_raw_to_slot(ctx, slot, raw);
}

void AsmJITMethodCompiler::emit_ir_store_bimm_slot(IrEmitContext &ctx, int64_t imm, uint32_t slot) {
    uint64_t raw = Value::make_bool(imm != 0).raw_bits();
    emit_ir_store_raw_to_slot(ctx, slot, raw);
}

void AsmJITMethodCompiler::emit_ir_store_n_slot(IrEmitContext &ctx, uint32_t slot) {
    uint64_t raw = Value::none().raw_bits();
    emit_ir_store_raw_to_slot(ctx, slot, raw);
}

void AsmJITMethodCompiler::emit_ir_store_c_slot(IrEmitContext &ctx, uint32_t const_idx, uint32_t slot) {
    auto &chunk = ctx.chunk;
    auto &chunk_idx = ctx.chunk_idx;
    // Read from the constant pool at JIT time when the value is a primitive (INT/FLOAT/NONE),
    // strings/functions require the runtime helper.
    auto *fmeta = &chunk.functions[chunk_idx];
    if (const_idx < fmeta->constants.size()) {
        const auto &cst = fmeta->constants[const_idx];
        switch (cst.type) {
            case nari::bytecode::Constant::Type::NONE:
                emit_ir_store_raw_to_slot(ctx, slot, Value::none().raw_bits());
                return;
            case nari::bytecode::Constant::Type::INT:
                emit_ir_store_raw_to_slot(ctx, slot, Value::make_int_checked(cst.as_int).raw_bits());
                return;
            case nari::bytecode::Constant::Type::FLOAT: {
                uint64_t raw;
                double d = cst.as_float;
                std::memcpy(&raw, &d, sizeof(raw));
                emit_ir_store_raw_to_slot(ctx, slot, raw);
                return;
            }
            case nari::bytecode::Constant::Type::STRING: {
                // Same safety as emit_ir_sconst_inline
                emit_ir_store_raw_to_slot(ctx, slot, chunk.get_const_string(cst.string_idx).raw_bits());
                return;
            }
            case nari::bytecode::Constant::Type::FUNCTION:
                break;
        }
    }
    // Non-primitive: fall back to raw runtime helpers (no fast path).
    // The old split-form fusion would push+store+pop; the runtime path
    // has the same semantics but avoids the forward-reference to
    // emit_ir_pop and keeps this rare branch simple.
    call_u32(ctx, reinterpret_cast<const void *>(jit_load_const), const_idx);
    call_u32(ctx, reinterpret_cast<const void *>(jit_store_var), slot);
    call0(ctx, reinterpret_cast<const void *>(jit_pop));
}

void AsmJITMethodCompiler::emit_ir_copy_slot(IrEmitContext &ctx, uint32_t src, uint32_t dst) {
    auto &cc = ctx.cc;
    auto &slot_cache = ctx.slot_cache;
    // if src slot is already cached, reuse it as the value to store into dst
    arch::Mem srcm = emit_ir_slot_mem(ctx, src);
    arch::Mem dstm = emit_ir_slot_mem(ctx, dst);
    arch::Gp raw = cc.new_gp64("ir_cp_raw");
    arch::Gp cached;
    if (slot_cache.try_get(src, cached)) {
        arch::mov_reg(cc, raw, cached);
    } else {
        arch::load(cc, raw, srcm);
        slot_cache.set(src, raw);
    }
    arch::store(cc, dstm, raw);
    // dst now holds `raw`; propagate to cache.
    slot_cache.set(dst, raw);
}

void AsmJITMethodCompiler::emit_ir_pop(IrEmitContext &ctx) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    // Value is NaN-boxed, trivially copyable, with no destructor (heap lifetime is GC-managed)
    // Popping a push that never reached memory is free: dropping the queue entry
    // cancels the two of them. Doing this through fc.get() instead would
    // materialize the value and then immediately pop it again.
    if (fc.peek(0) != nullptr) {
        fc.drop_pend(1);
        return;
    }
    arch::Gp endp = fc.get();
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));
    fc.set(endp);
}

void AsmJITMethodCompiler::emit_ir_dup(IrEmitContext &ctx) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    // Plain 8-byte copy. Value has no dtor/refcount, so dup of a heap value is
    // the same raw store as a non-heap one.
    // Capacity check is covered by the function-entry reservation.
    // If the top of stack is still a pending push, duplicating it is just
    // recording the same vreg twice: zero instructions.
    if (const auto *top = fc.peek(0)) {
        fc.push_pend(*top);
        return;
    }
    arch::Gp endp = fc.get();
    arch::Gp raw = cc.new_gp64("ir_dup_raw");
    arch::load(cc, raw, arch::ptr(endp, static_cast<int>((-ValSize))));
    fc.push_deferred(raw);
}

void AsmJITMethodCompiler::emit_ir_load_cached_capture(IrEmitContext &ctx, int64_t capture_offset, const char *name) {
    auto &cc = ctx.cc;
    auto &cmp_scratch = ctx.cmp_scratch;
    auto &fc = ctx.fc;
    auto &vm_reg = ctx.vm_reg;
    Label missing = cc.new_label();
    Label store = cc.new_label();
    // Both values are dead by the end of this sequence: `cell` after the cell
    // load, `raw` after the push. `raw` is defined on both arms and merged at
    // `store`, so using one fixed register on each arm keeps them consistent.
    // `raw` outlives this emitter as a deferred push, so it cannot be the
    // shared const_scratch; `cell` is dead at the merge and still can be.
    arch::Gp cell = cmp_scratch;
    arch::Gp raw = cc.new_gp64(name);

    arch::load(cc, cell, arch::ptr(vm_reg, static_cast<int>(capture_offset)));
    arch::test_zero(cc, cell);
    arch::jcc(cc, arch::CC::kEQ, missing);
    arch::load(cc, raw, arch::ptr(cell, 0));
    arch::jmp(cc, store);
    cc.bind(missing);
    cc.mov(raw, Imm(static_cast<int64_t>(nb.none_tag)));
    cc.bind(store);

    // No helper call and no fc use between the branches, so an incoming
    // pending queue stays valid across them.
    fc.push_deferred(raw);
}

void AsmJITMethodCompiler::emit_ir_store_cached_capture(IrEmitContext &ctx, int64_t capture_offset, const char *name) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_reg = ctx.vm_reg;
    Label done = cc.new_label();
    arch::Gp cell = cc.new_gp64(name);
    arch::load(cc, cell, arch::ptr(vm_reg, static_cast<int>(capture_offset)));
    arch::test_zero(cc, cell);
    arch::jcc(cc, arch::CC::kEQ, done);
    arch::Gp endp = fc.get();
    arch::Gp raw = cc.new_gp64("ir_sc_raw");
    arch::load(cc, raw, arch::ptr(endp, static_cast<int>(-ValSize)));
    arch::store(cc, arch::ptr(cell, 0), raw);
    cc.bind(done);
}

void AsmJITMethodCompiler::emit_ir_load_global(IrEmitContext &ctx, uint32_t name_idx) {
    auto &cc = ctx.cc;
    auto &cmp_scratch = ctx.cmp_scratch;
    auto &fc = ctx.fc;
    auto &vm_reg = ctx.vm_reg;
    // Inlines jit_load_global's fast path, name_idx is a compile-time constant
    // Any incoming pending pushes must reach memory before the branch below so
    // that both arms agree on the materialized stack; only the result differs,
    // and it is carried in one vreg (`raw`) that both arms define. get() (not
    // just materialize()) also guarantees the cached endp register is live on
    // entry, so both arms leave it in the same state and the join needs no
    // invalidate -- which would otherwise force every downstream op to reload it.
    (void)fc.get();
    Label slow = cc.new_label();
    Label done = cc.new_label();

    // name_idx < global_cache_valid.size() <=> (vstart + name_idx) < vfin
    arch::Gp vaddr = cc.new_gp64("ir_lg_vaddr");
    arch::Gp vfin = cmp_scratch; // dead immediately after the cmp below
    arch::load(cc, vaddr, arch::ptr(vm_reg, static_cast<int>(VMGlobalCacheValidStartOff)));
    arch::load(cc, vfin, arch::ptr(vm_reg, static_cast<int>(VMGlobalCacheValidFinishOff)));
    arch::add_imm(cc, vaddr, static_cast<int64_t>(name_idx));
    cc.cmp(vaddr, vfin);
    arch::jcc(cc, arch::CC::kFGE, slow);

    // global_cache_valid[name_idx] != 0
    arch::Gp valid = cmp_scratch; // vfin is dead here; vaddr is a distinct reg
    arch::load8_zx(cc, valid, arch::ptr8(vaddr, 0));
    arch::test_zero(cc, valid);
    arch::jcc(cc, arch::CC::kEQ, slow);

    // stack capacity check covered by the function-entry reservation.
    // read global_cache[name_idx] (plain 8-byte copy; no write barrier / refcount)
    arch::Gp cstart = cc.new_gp64("ir_lg_cstart");
    arch::load(cc, cstart, arch::ptr(vm_reg, static_cast<int>(VMGlobalCacheStartOff)));
    arch::Gp raw = cc.new_gp64("ir_lg_raw");
    const int64_t cache_off = static_cast<int64_t>(name_idx) * static_cast<int64_t>(ValSize);
    if (cache_off >= INT32_MIN && cache_off <= INT32_MAX) {
        arch::load(cc, raw, arch::ptr(cstart, static_cast<int>(cache_off)));
    } else {
        arch::add_imm(cc, cstart, cache_off);
        arch::load(cc, raw, arch::ptr(cstart, 0));
    }
    // The push is deferred: no value store, no stack bump, and no write-back of
    // the finish pointer to the VM on this hot arm.
    arch::jmp(cc, done);

    cc.bind(slow);
    call_u32(ctx, reinterpret_cast<const void *>(jit_load_global), name_idx);
    fc.merge_slow_path();
    // The helper pushed its result to memory. Bring it back into the same vreg
    // and undo the bump so this arm matches the fast arm's deferred state
    // (memory finish below the result, result pending in `raw`). This arm is
    // cold, and duplicating work into cold blocks is free (iteration 24).
    {
        arch::Gp e = fc.get();
        arch::load(cc, raw, arch::ptr(e, static_cast<int>(-ValSize)));
        arch::sub_imm(cc, e, static_cast<int>(ValSize));
        fc.set(e);
    }
    cc.bind(done);
    fc.push_deferred(raw);
}

void AsmJITMethodCompiler::emit_ir_js_get_prop_static(IrEmitContext &ctx, uint32_t name_idx) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_reg = ctx.vm_reg;
    static const bool disabled = std::getenv("NARI_DISABLE_JS_GET_PROP_STATIC") != nullptr;
    if (disabled) {
        call_u32(ctx, reinterpret_cast<const void *>(jit_js_get_prop_static), name_idx);
        return;
    }

    Label slow = cc.new_label();
    Label done = cc.new_label();
    arch::Gp ic = cc.new_gp64("ir_jsgp_ic");
    arch::load(cc, ic, arch::ptr(vm_reg, static_cast<int>(VMJsGetPropStaticICStartOff)));
    arch::add_imm(cc, ic, static_cast<int64_t>(name_idx) * static_cast<int64_t>(sizeof(JsGetPropStaticIC)));

    arch::Gp endp = fc.get();
    arch::Gp raw = cc.new_gp64("ir_jsgp_raw");
    arch::Gp jsgp_tag = cc.new_gp64("ir_jsgp_tag");
    arch::load(cc, raw, arch::ptr(endp, static_cast<int>(-ValSize)));
    arch::mov_reg(cc, jsgp_tag, raw);
    arch::shr(cc, jsgp_tag, 48);
    arch::cmp_imm(cc, jsgp_tag.r32(), Imm(static_cast<int>(tag._heap)));
    arch::jcc(cc, arch::CC::kNE, slow);

    arch::zero_extend_48(cc, raw);
    arch::cmp_mem8_imm(cc, arch::ptr8(raw, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(ValueTag::Object)));
    arch::jcc(cc, arch::CC::kNE, slow);

    arch::Gp shape = cc.new_gp64("ir_jsgp_shape");
    arch::load(cc, shape, arch::ptr(ic, static_cast<int>(NARI_JIT_OFF(bytecode::JsGetPropStaticIC, shape))));
    Label primary = cc.new_label();
    arch::cmp_mem(cc, shape, arch::ptr(raw, static_cast<int>(ObjShapeOff)));
    arch::jcc(cc, arch::CC::kEQ, primary);
    arch::load(cc, shape, arch::ptr(ic, static_cast<int>(NARI_JIT_OFF(bytecode::JsGetPropStaticIC, shape2))));
    arch::cmp_mem(cc, shape, arch::ptr(raw, static_cast<int>(ObjShapeOff)));
    arch::jcc(cc, arch::CC::kNE, slow);

    arch::Gp lazy_mask = cc.new_gp64("ir_jsgp_lazy_mask");
    arch::load(cc, lazy_mask, arch::ptr(ic, static_cast<int>(NARI_JIT_OFF(bytecode::JsGetPropStaticIC, lazy_mask2))));
    arch::Gp slot = cc.new_gp64("ir_jsgp_slot");
    arch::load32_zx_mem(cc, slot, arch::ptr(ic, static_cast<int>(NARI_JIT_OFF(bytecode::JsGetPropStaticIC, slot2))));
    Label selected = cc.new_label();
    arch::jmp(cc, selected);

    cc.bind(primary);
    arch::load(cc, lazy_mask, arch::ptr(ic, static_cast<int>(NARI_JIT_OFF(bytecode::JsGetPropStaticIC, lazy_mask))));
    arch::load32_zx_mem(cc, slot, arch::ptr(ic, static_cast<int>(NARI_JIT_OFF(bytecode::JsGetPropStaticIC, slot))));

    cc.bind(selected);
    arch::Gp object_lazy_mask = cc.new_gp64("ir_jsgp_object_lazy_mask");
    arch::load(cc, object_lazy_mask, arch::ptr(raw, static_cast<int>(ObjLazyFieldMaskOff)));
    arch::and_reg(cc, object_lazy_mask, lazy_mask);
    arch::test_zero(cc, object_lazy_mask);
    arch::jcc(cc, arch::CC::kNE, slow);

    arch::Gp field = cc.new_gp64("ir_jsgp_field");
    arch::load(cc, field, arch::ptr(raw, static_cast<int>(ObjFieldsStartOff)));
    arch::Gp fields_end = cc.new_gp64("ir_jsgp_fields_end");
    arch::load(cc, fields_end, arch::ptr(raw, static_cast<int>(ObjFieldsFinishOff)));
    arch::shl(cc, slot, slot, 3);
    arch::add_reg(cc, field, slot);
    arch::cmp_jcc(cc, field, fields_end, arch::CC::kUGE, slow);
    arch::load(cc, raw, arch::ptr(field, 0));
    arch::store(cc, arch::ptr(endp, static_cast<int>(-ValSize)), raw);
    arch::jmp(cc, done);

    cc.bind(slow);
    call_u32(ctx, reinterpret_cast<const void *>(jit_js_get_prop_static), name_idx);
    fc.merge_slow_path();
    cc.bind(done);
}

// One reusable scratch for constant materialization. Every use is
// def-then-store with no call in between, so the live ranges are disjoint and
// short; giving the allocator a single vreg with many defs instead of one fresh
// vreg per emitted constant cuts register-allocation work, and unlike splitting
// the store into two dwords it keeps a single qword store (a split store to a
// slot later loaded as a qword costs a store-forwarding stall: measured -0.27%
// instructions but +4.7% time).
void AsmJITMethodCompiler::emit_ir_raw_const(IrEmitContext &ctx, uint64_t raw, const char *name) {
    auto &fc = ctx.fc;
    // Deferred as a bare immediate: the `mov reg, imm64` is emitted only if and
    // where the value actually has to become a register or reach memory.
    (void)name;
    fc.push_deferred_imm(static_cast<int64_t>(raw));
}

void AsmJITMethodCompiler::emit_ir_fconst(IrEmitContext &ctx, double imm) {
    // FConst's operand is a compile-time constant double. make_float()
    // memcpys the bits straight into _raw with no tag, so the pushed
    // value is just the f64 bit pattern. Never a heap pointer, no write
    // barrier. Mirrors emit_ir_iconst exactly.
    uint64_t bits;
    memcpy(&bits, &imm, sizeof(bits));
    emit_ir_raw_const(ctx, bits, "ir_fc_raw");
}

void AsmJITMethodCompiler::emit_ir_iconst(IrEmitContext &ctx, int64_t imm) {
    emit_ir_raw_const(ctx, Value::make_int_checked(imm).raw_bits(), "ir_ic_raw");
}

// Push a string constant inline.
// LoadConst only carries STRING/FUNCTION/NONE (ir_build siphons INT->IConst, FLOAT->FConst).
bool AsmJITMethodCompiler::emit_ir_sconst_inline(IrEmitContext &ctx, uint32_t const_idx) {
    auto &chunk = ctx.chunk;
    auto &chunk_idx = ctx.chunk_idx;
    auto &fc = ctx.fc;
    auto &fmeta = chunk.functions[chunk_idx];
    if (const_idx >= fmeta.constants.size()) {
        return false;
    }
    const auto &cst = fmeta.constants[const_idx];
    if (cst.type != nari::bytecode::Constant::Type::STRING) {
        return false;
    }
    // get_const_string mutates only the lazy cache (mutable members, logically
    // const); the returned Value is rooted for the chunk's lifetime.
    const uint64_t raw = chunk.get_const_string(cst.string_idx).raw_bits();
    // Deferred as a bare immediate, like every other constant: no store, no
    // stack bump, no finish write-back -- and, unlike the eager form, no
    // fc.get(), which used to flush the whole pending-push queue at every
    // string constant.
    fc.push_deferred_imm(static_cast<int64_t>(raw));
    return true;
}

void AsmJITMethodCompiler::emit_ir_call_method(IrEmitContext &ctx, const ir::Inst &in) {
    auto &cc = ctx.cc;
    auto &chunk = ctx.chunk;
    auto &fc = ctx.fc;
    auto &irFuncs = ctx.irFuncs;
    uint32_t method_idx = in.imm_u32;
    uint32_t argc = static_cast<uint32_t>(in.imm_int);
    // The value type can be proven, but a slot containing an integer-only
    // array may still be uninitialized on this control-flow path.
    bool push_skip_val_guard = false;
    if (in.operands.size() >= 2) {
        const ir::Inst &arg = irFuncs.inst(in.operands[1]);
        if (arg.type == ir::Ty::Int48) {
            push_skip_val_guard = true;
        }
    }
    if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "length" && argc == 0) {
        call0(ctx, reinterpret_cast<const void *>(jit_method_length));
    } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "char_code_at" && argc == 1) {
        call0(ctx, reinterpret_cast<const void *>(jit_method_char_code_at));
    } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "starts_with" && argc == 1) {
        call0(ctx, reinterpret_cast<const void *>(jit_method_starts_with));
    } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "substr" && argc <= 2) {
        call_u32(ctx, reinterpret_cast<const void *>(jit_method_substr), argc);
    } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "push" && argc == 1) {
        Label slow = cc.new_label();
        Label done = cc.new_label();

        arch::Gp endp = fc.get();
        arch::Gp push_tag = cc.new_gp64("ir_push_tag");

        // keep only the trivial-value case inline
        if (!push_skip_val_guard) {
            arch::load16_zx(cc, push_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, push_tag.r32(), Imm(static_cast<int>(tag._heap)));
            arch::jcc(cc, arch::CC::kEQ, slow);
        }

        arch::load16_zx(cc, push_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, push_tag.r32(), Imm(static_cast<int>(tag._heap)));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp arr = cc.new_gp64("ir_push_arr");
        arch::load(cc, arr, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
        arch::zero_extend_48(cc, arr);
        arch::cmp_mem8_imm(
            cc, arch::ptr8(arr, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._array))
        );
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp fin = cc.new_gp64("ir_push_fin");
        arch::Gp cap = cc.new_gp64("ir_push_cap");
        arch::load(cc, fin, arch::ptr(arr, static_cast<int>(ArrayVecFinishOff)));
        arch::load(cc, cap, arch::ptr(arr, static_cast<int>(ArrayVecCapacityOff)));
        cc.cmp(fin, cap);
        arch::jcc(cc, arch::CC::kEQ, slow);

        arch::Gp val = cc.new_gp64("ir_push_val");
        arch::load(cc, val, arch::ptr(endp, static_cast<int>((-ValSize))));
        arch::store(cc, arch::ptr(fin, 0), val);
        arch::add_imm(cc, fin, static_cast<int>(ValSize));
        arch::store(cc, arch::ptr(arr, static_cast<int>(ArrayVecFinishOff)), fin);

        arch::Gp none = cc.new_gp64("ir_push_none");
        cc.mov(none, Imm(static_cast<int64_t>(nb.none_tag)));
        arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), none);
        arch::sub_imm(cc, endp, static_cast<int>(ValSize));
        fc.set(endp);
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_method), method_idx, argc);
        fc.merge_slow_path();
        cc.bind(done);
    } else {
        call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_method), method_idx, argc);
    }
}

void AsmJITMethodCompiler::emit_ir_load_index(IrEmitContext &ctx, bool skip_arr_guard, bool skip_idx_guard) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    Label slow = cc.new_label();
    Label done = cc.new_label();
    arch::Gp endp = fc.get();
    arch::Gp gi_tag = cc.new_gp64("ir_gi_tag");

    if (!skip_idx_guard) {
        arch::load16_zx(cc, gi_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
        arch::cmp_imm(cc, gi_tag.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    if (!skip_arr_guard) {
        arch::load16_zx(cc, gi_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, gi_tag.r32(), Imm(static_cast<int>(tag._heap)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }

    arch::Gp idx = cc.new_gp64("ir_gi_idx");
    arch::load(cc, idx, arch::ptr(endp, static_cast<int>((-ValSize))));
    arch::sign_extend_48(cc, idx);

    arch::Gp arr = cc.new_gp64("ir_gi_arr");
    arch::load(cc, arr, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
    arch::zero_extend_48(cc, arr);
    if (!skip_arr_guard) {
        arch::cmp_mem8_imm(cc, arch::ptr8(arr, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._array)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    arch::Gp start = cc.new_gp64("ir_gi_start");
    arch::Gp size = cc.new_gp64("ir_gi_size");
    arch::load(cc, start, arch::ptr(arr, static_cast<int>(ArrayVecStartOff)));
    arch::load(cc, size, arch::ptr(arr, static_cast<int>(ArrayVecFinishOff)));
    arch::sub2(cc, size, start);
    arch::shr(cc, size, 3);

    Label positive = cc.new_label();
    arch::jns(cc, idx, positive);
    arch::jmp(cc, slow);
    cc.bind(positive);
    cc.cmp(idx, size);
    arch::jcc(cc, arch::CC::kGE, slow);

    arch::Gp elem = cc.new_gp64("ir_gi_elem");
    arch::shl(cc, elem, idx, 3);
    arch::add2(cc, elem, start);

    arch::Gp val = cc.new_gp64("ir_gi_val");
    arch::load(cc, val, arch::ptr(elem, 0));
    arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), val);
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));
    fc.set(endp);
    arch::jmp(cc, done);

    cc.bind(slow);
    call0(ctx, reinterpret_cast<const void *>(jit_get_index));
    fc.merge_slow_path();
    cc.bind(done);
}

void AsmJITMethodCompiler::emit_ir_store_index(IrEmitContext &ctx, bool skip_arr_guard, bool skip_idx_guard) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    // Fast path for array[int] = val with in-bounds idx.
    Label slow = cc.new_label();
    Label done = cc.new_label();
    arch::Gp endp = fc.get();
    arch::Gp si_tag = cc.new_gp64("ir_si_tag");

    if (!skip_idx_guard) {
        // key (at endp - 2*ValSize) must be int
        arch::load16_zx(cc, si_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, si_tag.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    if (!skip_arr_guard) {
        // obj (at endp - 3*ValSize) must be heap
        arch::load16_zx(cc, si_tag, arch::ptr16(endp, static_cast<int>((-3 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, si_tag.r32(), Imm(static_cast<int>(tag._heap)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }

    arch::Gp idx = cc.new_gp64("ir_si_idx");
    arch::load(cc, idx, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
    arch::sign_extend_48(cc, idx);

    arch::Gp arr = cc.new_gp64("ir_si_arr");
    arch::load(cc, arr, arch::ptr(endp, static_cast<int>((-3 * ValSize))));
    arch::zero_extend_48(cc, arr);
    if (!skip_arr_guard) {
        arch::cmp_mem8_imm(cc, arch::ptr8(arr, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._array)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    arch::Gp start = cc.new_gp64("ir_si_start");
    arch::Gp size = cc.new_gp64("ir_si_size");
    arch::load(cc, start, arch::ptr(arr, static_cast<int>(ArrayVecStartOff)));
    arch::load(cc, size, arch::ptr(arr, static_cast<int>(ArrayVecFinishOff)));
    arch::sub2(cc, size, start);
    arch::shr(cc, size, 3);

    // bounds: handle negative-as-from-end, then require 0 <= idx < size.
    Label positive = cc.new_label();
    arch::jns(cc, idx, positive);
    arch::add2(cc, idx, size);
    arch::jns(cc, idx, positive);
    arch::jmp(cc, slow);
    cc.bind(positive);
    cc.cmp(idx, size);
    arch::jcc(cc, arch::CC::kGE, slow);

    // elem_ptr = start + idx*8
    arch::Gp elem = cc.new_gp64("ir_si_elem");
    arch::shl(cc, elem, idx, 3);
    arch::add2(cc, elem, start);

    // val (at endp - ValSize) -> *elem
    arch::Gp val = cc.new_gp64("ir_si_val");
    arch::load(cc, val, arch::ptr(endp, static_cast<int>((-ValSize))));
    arch::store(cc, arch::ptr(elem, 0), val);

    // Stack: pop obj and key (2 slots), leaving val on top.
    // Move val from endp-ValSize down to endp-3*ValSize.
    arch::store(cc, arch::ptr(endp, static_cast<int>((-3 * ValSize))), val);
    arch::sub_imm(cc, endp, static_cast<int>((2 * ValSize)));
    fc.set(endp);
    arch::jmp(cc, done);

    cc.bind(slow);
    call0(ctx, reinterpret_cast<const void *>(jit_set_index));
    fc.merge_slow_path();
    cc.bind(done);
}

// Skip each operand's tag check when the IR has proven it is statically Int48.
void AsmJITMethodCompiler::emit_int_binop(IrEmitContext &ctx, ir::Op op, bool skip_lhs_tag_check, bool skip_rhs_tag_check) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    asmjit::Label slow = cc.new_label();
    asmjit::Label done = cc.new_label();
    arch::Gp endp = fc.get();
    if (!skip_rhs_tag_check) {
        arch::Gp tagB = cc.new_gp64("ib_tb");
        arch::load16_zx(cc, tagB, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
        arch::cmp_imm(cc, tagB.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    if (!skip_lhs_tag_check) {
        arch::Gp tagA = cc.new_gp64("ib_ta");
        arch::load16_zx(cc, tagA, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, tagA.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    arch::Gp vb = cc.new_gp64("ib_vb");
    arch::Gp va = cc.new_gp64("ib_va");
    arch::load(cc, vb, arch::ptr(endp, static_cast<int>((-ValSize))));
    arch::sign_extend_48(cc, vb);
    arch::load(cc, va, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
    arch::sign_extend_48(cc, va);
    if (op == ir::Op::DynAdd || op == ir::Op::IAdd) {
        arch::add2(cc, va, vb); // va += vb
    } else {
        arch::sub2(cc, va, vb); // va -= vb
    }
    // int48 overflow -> fall to the helper (which boxes as float)
    arch::Gp ovf = cc.new_gp64("ib_ovf");
    arch::mov_reg(cc, ovf, va);
    arch::sign_extend_48(cc, ovf);
    cc.cmp(ovf, va);
    arch::jcc(cc, arch::CC::kNE, slow);
    arch::Gp itag = cc.new_gp64("ib_itag");
    arch::nanbox_encode_int(cc, va, itag);
    arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), va);
    arch::sub_imm(cc, endp, static_cast<int>(ValSize)); // pop one
    fc.set(endp);
    arch::jmp(cc, done);
    cc.bind(slow);
    call0(
        ctx,
        (op == ir::Op::DynAdd || op == ir::Op::IAdd) ? reinterpret_cast<const void *>(jit_add) : reinterpret_cast<const void *>(jit_sub)
    );
    fc.merge_slow_path();
    cc.bind(done);
}

void AsmJITMethodCompiler::emit_int_mul(IrEmitContext &ctx, bool skip_lhs_tag_check, bool skip_rhs_tag_check) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    Label slow = cc.new_label();
    Label done = cc.new_label();
    arch::Gp endp = fc.get();
    if (!skip_rhs_tag_check) {
        arch::Gp tagB = cc.new_gp64("im_tb");
        arch::load16_zx(cc, tagB, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
        arch::cmp_imm(cc, tagB.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    if (!skip_lhs_tag_check) {
        arch::Gp tagA = cc.new_gp64("im_ta");
        arch::load16_zx(cc, tagA, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, tagA.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }

    arch::Gp vb = cc.new_gp64("im_vb");
    arch::Gp va = cc.new_gp64("im_va");
    arch::load(cc, vb, arch::ptr(endp, static_cast<int>((-ValSize))));
    arch::sign_extend_48(cc, vb);
    arch::load(cc, va, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
    arch::sign_extend_48(cc, va);
#if NARI_JIT_ARM64
    // MUL keeps only the low 64 bits. Check the signed high half before
    // deciding whether the product can be represented as an int48.
    arch::Gp high = cc.new_gp64("im_high");
    arch::Gp sign = cc.new_gp64("im_sign");
    cc.smulh(high, va, vb);
#endif
    arch::imul(cc, va, va, vb);
#if NARI_JIT_ARM64
    cc.asr(sign, va, Imm(63));
    cc.cmp(high, sign);
    arch::jcc(cc, arch::CC::kNE, slow);
#else
    // IMUL sets OF when the exact product does not fit in 64 bits.
    cc.jo(slow);
#endif

    arch::Gp ovf = cc.new_gp64("im_ovf");
    arch::mov_reg(cc, ovf, va);
    arch::sign_extend_48(cc, ovf);
    cc.cmp(ovf, va);
    arch::jcc(cc, arch::CC::kNE, slow);
    arch::Gp itag = cc.new_gp64("im_itag");
    arch::nanbox_encode_int(cc, va, itag);
    arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), va);
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));
    fc.set(endp);
    arch::jmp(cc, done);

    cc.bind(slow);
    call0(ctx, reinterpret_cast<const void *>(jit_mul));
    fc.merge_slow_path();
    cc.bind(done);
}

void AsmJITMethodCompiler::emit_int_mod(IrEmitContext &ctx, bool skip_lhs_tag_check, bool skip_rhs_tag_check) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    Label slow = cc.new_label();
    Label done = cc.new_label();
    arch::Gp endp = fc.get();
    if (!skip_rhs_tag_check) {
        arch::Gp tagB = cc.new_gp64("imod_tb");
        arch::load16_zx(cc, tagB, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
        arch::cmp_imm(cc, tagB.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    if (!skip_lhs_tag_check) {
        arch::Gp tagA = cc.new_gp64("imod_ta");
        arch::load16_zx(cc, tagA, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, tagA.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }

    arch::Gp rhs = cc.new_gp64("imod_rhs");
    arch::Gp lhs = cc.new_gp64("imod_lhs");
    arch::load(cc, rhs, arch::ptr(endp, static_cast<int>((-ValSize))));
    arch::sign_extend_48(cc, rhs);
    arch::load(cc, lhs, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
    arch::sign_extend_48(cc, lhs);
    arch::test_zero(cc, rhs);
    arch::jcc(cc, arch::CC::kEQ, slow);
    arch::cmp_imm(cc, rhs, Imm(-1));
    arch::jcc(cc, arch::CC::kEQ, slow);

    arch::Gp hi = cc.new_gp64("imod_hi");
    cc.mov(hi, lhs);
    arch::sar(cc, hi, 63);
    arch::smod_only(cc, hi, lhs, rhs);
    arch::Gp itag = cc.new_gp64("imod_itag");
    arch::nanbox_encode_int(cc, hi, itag);
    arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), hi);
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));
    fc.set(endp);
    arch::jmp(cc, done);

    cc.bind(slow);
    call0(ctx, reinterpret_cast<const void *>(jit_mod));
    fc.merge_slow_path();
    cc.bind(done);
}

void AsmJITMethodCompiler::emit_int_cmp(IrEmitContext &ctx, ir::Op op, bool skip_lhs_tag_check, bool skip_rhs_tag_check) {
    auto &cc = ctx.cc;
    auto &cmp_scratch = ctx.cmp_scratch;
    auto &const_scratch = ctx.const_scratch;
    auto &fc = ctx.fc;
    const bool skip_tag_check = skip_lhs_tag_check && skip_rhs_tag_check;
    Label slow = cc.new_label();
    Label done = cc.new_label();
    arch::Gp endp = fc.get();
    if (!skip_rhs_tag_check) {
        arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
        arch::cmp_imm(cc, cmp_scratch.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }
    if (!skip_lhs_tag_check) {
        arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, cmp_scratch.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    }

    arch::Gp rhs = cc.new_gp64("ic_rhs");
    arch::Gp lhs = cc.new_gp64("ic_lhs");
    arch::load(cc, rhs, arch::ptr(endp, static_cast<int>((-ValSize))));
    arch::sign_extend_48(cc, rhs);
    arch::load(cc, lhs, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
    arch::sign_extend_48(cc, lhs);
    cc.cmp(lhs, rhs);
    arch::CC::Cond cond = arch::CC::kEQ;
    switch (op) {
        case ir::Op::DynCmpLt:
        case ir::Op::ICmpLt:
            cond = arch::CC::kLT;
            break;
        case ir::Op::DynCmpLe:
        case ir::Op::ICmpLe:
            cond = arch::CC::kLE;
            break;
        case ir::Op::DynCmpGt:
        case ir::Op::ICmpGt:
            cond = arch::CC::kGT;
            break;
        case ir::Op::DynCmpGe:
        case ir::Op::ICmpGe:
            cond = arch::CC::kGE;
            break;
        case ir::Op::DynCmpEq:
        case ir::Op::DynStrictCmpEq:
        case ir::Op::ICmpEq:
            cond = arch::CC::kEQ;
            break;
        default:
            cond = arch::CC::kNE;
            break;
    }
    arch::Gp out = cc.new_gp64("ic_out");
    arch::cset(cc, out, cond);
    arch::or_imm(cc, out, out, static_cast<int64_t>(nb.bool_tag));
    arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), out);
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));
    fc.set(endp);
    // When the tag guards above were elided, no path references `slow`,
    // and emitting it as a dead block confuses asmjit's RA liveness
    // analysis (Compiler RA crashes when computing liveness for a block
    // with no predecessor and a vreg-using call). Skip the slow tail
    // entirely in that case; the fast path is straight-line.
    if (skip_tag_check) {
        cc.bind(done);
        return;
    }
    arch::jmp(cc, done);

    cc.bind(slow);
    // Strict equality with differing tags, neither of them a float, is unequal
    if (op == ir::Op::DynStrictCmpEq || op == ir::Op::DynStrictCmpNe) {
        const bool ne = (op == ir::Op::DynStrictCmpNe);
        // Exactly one operand is a proven Int48 here
        const bool one_int = skip_lhs_tag_check || skip_rhs_tag_check;
        Label streq_helper = cc.new_label();
        Label same_tag = cc.new_label();
        Label unequal = cc.new_label();
        Label equal = cc.new_label();
        fc.invalidate();
        arch::Gp e = fc.get();
        if (one_int) {
            // only the unproven operand's tag matters
            const int unk = skip_rhs_tag_check ? static_cast<int>((-2 * ValSize)) : static_cast<int>((-ValSize));
            arch::load16_zx(cc, cmp_scratch, arch::ptr16(e, unk + static_cast<int>(tag.word_off)));
        } else {
            arch::load16_zx(cc, cmp_scratch, arch::ptr16(e, static_cast<int>((-2 * ValSize + tag.word_off))));
            arch::load16_zx(cc, const_scratch, arch::ptr16(e, static_cast<int>((-ValSize + tag.word_off))));
            cc.cmp(cmp_scratch.r32(), const_scratch.r32());
            arch::jcc(cc, arch::CC::kEQ, same_tag); // same tag: needs a real compare
        }
        arch::cmp_imm(cc, cmp_scratch.r32(), Imm(static_cast<int>(tag._heap)));
        arch::jcc(cc, arch::CC::kULT, streq_helper); // unproven side is a float
        if (!one_int) {
            arch::cmp_imm(cc, const_scratch.r32(), Imm(static_cast<int>(tag._heap)));
            arch::jcc(cc, arch::CC::kULT, streq_helper); // rhs is a float
        }
        cc.bind(unequal);
        cc.mov(cmp_scratch, Imm(static_cast<int64_t>((nb.bool_tag | (ne ? 1ULL : 0ULL)))));
        arch::store(cc, arch::ptr(e, static_cast<int>((-2 * ValSize))), cmp_scratch);
        arch::sub_imm(cc, e, static_cast<int>(ValSize));
        fc.set(e);
        arch::jmp(cc, done);

        // Both operands carry the same tag. When that tag is HEAP, two of the three outcomes need no helper
        if (!one_int) {
            cc.bind(same_tag);
            arch::cmp_imm(cc, cmp_scratch.r32(), Imm(static_cast<int>(tag._heap)));
            arch::jcc(cc, arch::CC::kNE, streq_helper); // int/bool/none/float: helper decides
            arch::load(cc, cmp_scratch, arch::ptr(e, static_cast<int>((-2 * ValSize))));
            arch::cmp_mem(cc, cmp_scratch, arch::ptr(e, static_cast<int>((-ValSize))));
            arch::jcc(cc, arch::CC::kEQ, equal); // same heap pointer
            arch::zero_extend_48(cc, cmp_scratch);
            arch::cmp_mem8_imm(
                cc, arch::ptr8(cmp_scratch, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._string))
            );
            arch::jcc(cc, arch::CC::kNE, unequal);
            // lhs is a String, so the rhs pointer is worth loading now
            arch::load(cc, const_scratch, arch::ptr(e, static_cast<int>((-ValSize))));
            arch::zero_extend_48(cc, const_scratch);
            arch::cmp_mem8_imm(
                cc, arch::ptr8(const_scratch, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._string))
            );
            arch::jcc(cc, arch::CC::kNE, unequal);
            arch::jmp(cc, streq_helper); // String === String: compare contents

            cc.bind(equal);
            cc.mov(cmp_scratch, Imm(static_cast<int64_t>((nb.bool_tag | (ne ? 0ULL : 1ULL)))));
            arch::store(cc, arch::ptr(e, static_cast<int>((-2 * ValSize))), cmp_scratch);
            arch::sub_imm(cc, e, static_cast<int>(ValSize));
            fc.set(e);
            arch::jmp(cc, done);
        } // !one_int

        cc.bind(streq_helper);
        fc.invalidate();
    }
    switch (op) {
        case ir::Op::DynCmpLt:
            call0(ctx, reinterpret_cast<const void *>(jit_lt));
            break;
        case ir::Op::DynCmpLe:
            call0(ctx, reinterpret_cast<const void *>(jit_le));
            break;
        case ir::Op::DynCmpGt:
            call0(ctx, reinterpret_cast<const void *>(jit_gt));
            break;
        case ir::Op::DynCmpGe:
            call0(ctx, reinterpret_cast<const void *>(jit_ge));
            break;
        case ir::Op::DynCmpEq:
            call0(ctx, reinterpret_cast<const void *>(jit_eq));
            break;
        case ir::Op::DynStrictCmpEq:
            call0(ctx, reinterpret_cast<const void *>(jit_strict_eq));
            break;
        case ir::Op::DynStrictCmpNe:
            call0(ctx, reinterpret_cast<const void *>(jit_strict_ne));
            break;
        default:
            call0(ctx, reinterpret_cast<const void *>(jit_ne));
            break;
    }
    fc.merge_slow_path();
    cc.bind(done);
}

void AsmJITMethodCompiler::emit_float_cmp(IrEmitContext &ctx, ir::Op op) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    // no slow path, uses fc directly.
    arch::Gp endp = fc.get();
    arch::Vec fa = arch::new_vec_f64(cc, "fc_a");
    arch::Vec fb = arch::new_vec_f64(cc, "fc_b");
    arch::load_f64(cc, fb, arch::ptr(endp, static_cast<int>((-ValSize))));
    arch::load_f64(cc, fa, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
    arch::Gp out = cc.new_gp64("fc_out");
    arch::float_cmp(cc, fa, fb);
    if (op == ir::Op::FCmpEq) {
        arch::float_to_bool_eq(cc, out, true);
    } else if (op == ir::Op::FCmpNe) {
        arch::float_to_bool_eq(cc, out, false);
    } else {
        bool swap = (op == ir::Op::FCmpLt || op == ir::Op::FCmpLe);
        arch::CC::Cond fcc = (op == ir::Op::FCmpLt || op == ir::Op::FCmpGt) ? arch::CC::kFGT : arch::CC::kFGE;
        if (swap) {
            arch::float_cmp(cc, fb, fa);
        }
        arch::cset(cc, out, fcc);
    }
    // or_imm folds the tag in directly (as emit_int_cmp already does),
    // so the extra vreg that only carried nb.bool_tag is unnecessary.
    arch::or_imm(cc, out, out, static_cast<int64_t>(nb.bool_tag));
    arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), out);
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));
    fc.set(endp);
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
