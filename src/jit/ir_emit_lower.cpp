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

#include <vector>

using namespace asmjit;
using namespace nari::bytecode;

namespace nari {
namespace jit {

// Layout sanity check for the capture-vector fast path (moved from asmjit_jit.cpp).
static bool nari_verify_capture_vec_layout() {
    std::vector<CellRef> v;
    v.push_back(CellRef::make());
    v.push_back(CellRef::make());
    if (sizeof(CellRef) != 16 || sizeof(void *) != 8) {
        return false;
    }
    void *const *words = reinterpret_cast<void *const *>(&v);
    const void *begin = words[0];
    const void *end = words[1];
    if (begin != static_cast<const void *>(v.data()) || end != static_cast<const void *>(v.data() + v.size())) {
        return false;
    }
    for (size_t i = 0; i < v.size(); i++) {
        if (*reinterpret_cast<Value *const *>(v.data() + i) != v[i].get()) {
            return false;
        }
    }
    return true;
}
static const bool kCaptureVecLayoutOk = nari_verify_capture_vec_layout();

// Return-path emitter shared by lower_term (moved from asmjit_jit.cpp).
static void emit_inline_return(arch::Compiler &cc, arch::Gp vm_reg) {
    Label done = cc.new_label();
    arch::Gp ff = cc.new_gp64("iret_ff");
    arch::Gp sc = cc.new_gp64("iret_sc");
    arch::Gp res = cc.new_gp64("iret_res");
    arch::load(cc, ff, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)));
    arch::sub_imm(cc, ff, static_cast<int>(FrameSize));
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)), ff);
    arch::load(cc, sc, arch::ptr(vm_reg, static_cast<int>(FramesStartOff)));
    cc.cmp(ff, sc);
    arch::load(cc, sc, arch::ptr(ff, static_cast<int>(SlotBaseOff))); // flags preserved
    arch::jcc(cc, arch::CC::kEQ, done);                               // frames now empty: stack unchanged
    // result is at [finish - 8]; sc holds the popped frame's slot_base
    arch::load(cc, res, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
    arch::load(cc, res, arch::ptr(res, static_cast<int>((-ValSize))));
    arch::shl(cc, sc, sc, 3);
    arch::Gp nf = ff; // frame ptr dead; reuse for the new finish
    arch::load(cc, nf, arch::ptr(vm_reg, static_cast<int>(VMStackStartOff)));
    arch::add2(cc, nf, sc);
    arch::store(cc, arch::ptr(nf, 0), res);
    arch::add_imm(cc, nf, static_cast<int>(ValSize));
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), nf);
    cc.bind(done);
}

void AsmJITMethodCompiler::lower_body(IrEmitContext &ctx, const ir::Inst &in) {
    auto &cc = ctx.cc;
    auto &chunk = ctx.chunk;
    auto &chunk_idx = ctx.chunk_idx;
    auto &cmp_scratch = ctx.cmp_scratch;
    auto &const_scratch = ctx.const_scratch;
    auto &fc = ctx.fc;
    auto &irFuncs = ctx.irFuncs;
    auto &ok = ctx.ok;
    auto &unhandled_op = ctx.unhandled_op;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    auto &vm_reg = ctx.vm_reg;
    if (jit_dump_asm_enabled()) {
        char obuf[64];
        snprintf(obuf, sizeof(obuf), "OP:%s", ir::op_name(in.op));
        cc.comment(obuf);
    }
    switch (in.op) {
        case ir::Op::IConst:
            emit_ir_iconst(ctx, in.imm_int);
            break;
        case ir::Op::FConst:
            emit_ir_fconst(ctx, in.imm_float);
            break;
        case ir::Op::BConst:
            emit_ir_raw_const(ctx, Value::make_bool(in.imm_int != 0).raw_bits(), "ir_bc_raw");
            break;
        case ir::Op::NConst:
            emit_ir_raw_const(ctx, Value::none().raw_bits(), "ir_nc_raw");
            break;
        case ir::Op::LoadConst:
            if (!emit_ir_sconst_inline(ctx, in.imm_u32)) {
                call_u32(ctx, reinterpret_cast<const void *>(jit_load_const), in.imm_u32);
            }
            break;
        case ir::Op::LoadSlot:
            if (in.imm_u32 < irFuncs.captured_local_slots.size() && irFuncs.captured_local_slots[in.imm_u32]) {
                call_u32(ctx, reinterpret_cast<const void *>(jit_load_var), in.imm_u32);
            } else {
                emit_ir_load_slot(ctx, in.imm_u32);
            }
            break;
        case ir::Op::StoreSlot:
            if (in.imm_u32 < irFuncs.captured_local_slots.size() && irFuncs.captured_local_slots[in.imm_u32]) {
                call_u32(ctx, reinterpret_cast<const void *>(jit_store_var), in.imm_u32);
            } else {
                emit_ir_store_slot(ctx, in.imm_u32);
            }
            break;
        case ir::Op::LoadGlobal:
            emit_ir_load_global(ctx, in.imm_u32);
            break;
        case ir::Op::CloseUpvalues:
            call_u32(ctx, reinterpret_cast<const void *>(jit_close_upvalues), in.imm_u32);
            break;
        case ir::Op::StoreGlobal:
            call_u32(ctx, reinterpret_cast<const void *>(jit_store_global), in.imm_u32);
            break;
        case ir::Op::LoadCapture:
            // idx 0/1/2 have dedicated cached raw pointers
            if (in.imm_u32 >= 3 && kCaptureVecLayoutOk) {
                Label lc_slow = cc.new_label();
                Label lc_done = cc.new_label();
                arch::Gp endp = fc.get();
                arch::load(cc, cmp_scratch, arch::ptr(vm_reg, static_cast<int>(VMCapturesRawOff)));
                arch::test_zero(cc, cmp_scratch);
                arch::jcc(cc, arch::CC::kEQ, lc_slow);
                arch::load(cc, const_scratch, arch::ptr(cmp_scratch, 8));              // vector end
                arch::load(cc, cmp_scratch, arch::ptr(cmp_scratch, 0));                // vector begin
                arch::add_imm(cc, cmp_scratch, static_cast<int64_t>(in.imm_u32) * 16); // &elem[idx]
                cc.cmp(cmp_scratch, const_scratch);
                arch::jcc(cc, arch::CC::kUGE, lc_slow);                 // out of range
                arch::load(cc, cmp_scratch, arch::ptr(cmp_scratch, 0)); // shared_ptr -> cell
                arch::load(cc, cmp_scratch, arch::ptr(cmp_scratch, 0)); // cell -> Value
                arch::store(cc, arch::ptr(endp, 0), cmp_scratch);
                arch::add_imm(cc, endp, static_cast<int>(ValSize));
                fc.set(endp);
                arch::jmp(cc, lc_done);
                cc.bind(lc_slow);
                call_u32(ctx, reinterpret_cast<const void *>(jit_load_capture), in.imm_u32);
                fc.merge_slow_path();
                cc.bind(lc_done);
                break;
            }
            if (in.imm_u32 == 0) {
                emit_ir_load_cached_capture(ctx, VMCapture0RawOff, "ir_lc0_cell");
            } else if (in.imm_u32 == 1) {
                emit_ir_load_cached_capture(ctx, VMCapture1RawOff, "ir_lc1_cell");
            } else if (in.imm_u32 == 2) {
                emit_ir_load_cached_capture(ctx, VMCapture2RawOff, "ir_lc2_cell");
            } else {
                call_u32(ctx, reinterpret_cast<const void *>(jit_load_capture), in.imm_u32);
            }
            break;
        case ir::Op::StoreCapture:
            if (in.imm_u32 == 0) {
                emit_ir_store_cached_capture(ctx, VMCapture0RawOff, "ir_sc0_cell");
            } else if (in.imm_u32 == 1) {
                emit_ir_store_cached_capture(ctx, VMCapture1RawOff, "ir_sc1_cell");
            } else if (in.imm_u32 == 2) {
                emit_ir_store_cached_capture(ctx, VMCapture2RawOff, "ir_sc2_cell");
            } else {
                call_u32(ctx, reinterpret_cast<const void *>(jit_store_capture), in.imm_u32);
            }
            break;
        case ir::Op::Pop:
            emit_ir_pop(ctx);
            break;
        case ir::Op::Dup:
            emit_ir_dup(ctx);
            break;
        case ir::Op::StoreImmSlot:
            emit_ir_store_imm_slot(ctx, in.imm_int, in.imm_u32);
            break;
        case ir::Op::StoreBImmSlot:
            emit_ir_store_bimm_slot(ctx, in.imm_int, in.imm_u32);
            break;
        case ir::Op::StoreNSlot:
            emit_ir_store_n_slot(ctx, in.imm_u32);
            break;
        case ir::Op::StoreCSlot:
            emit_ir_store_c_slot(ctx, static_cast<uint32_t>(in.imm_int), in.imm_u32);
            break;
        case ir::Op::CopySlot:
            emit_ir_copy_slot(ctx, static_cast<uint32_t>(in.imm_int), in.imm_u32);
            break;
        case ir::Op::Not: {
            Label not_call = cc.new_label();
            Label not_done = cc.new_label();
            arch::Gp endp = fc.get();
            arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, cmp_scratch.r32(), Imm(static_cast<int>(tag._bool)));
            arch::jcc(cc, arch::CC::kNE, not_call);
            arch::load(cc, const_scratch, arch::ptr(endp, static_cast<int>((-ValSize))));
            arch::xor_imm(cc, const_scratch, 1);
            arch::store(cc, arch::ptr(endp, static_cast<int>((-ValSize))), const_scratch);
            arch::jmp(cc, not_done);
            cc.bind(not_call);
            call0(ctx, reinterpret_cast<const void *>(jit_not)); // Not does not change stack height
            cc.bind(not_done);
            break;
        }
        case ir::Op::JsTruthy: {
            // is_js_truthystatic_cast<bool>(returns) the bool unchanged, so for a Bool operand
            // this whole operation is the identity and needs no code at all.
            // Measured: 11,182,828 of 19,216,275 calls (58%) carry TAG_BOOL.
            // int/heap keep the helper (heap needs string-emptiness rules).
            Label truthy_skip = cc.new_label();
            arch::Gp endp = fc.get();
            arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, cmp_scratch.r32(), Imm(static_cast<int>(tag._bool)));
            arch::jcc(cc, arch::CC::kEQ, truthy_skip);
            call0(ctx, reinterpret_cast<const void *>(jit_js_truthy));
            cc.bind(truthy_skip);
            break;
        }
        case ir::Op::IsNone: {
            fc.invalidate();
            InvokeNode *check;
            (void)vm_arg0(ctx);
            arch::invoke_imm(
                cc, &check, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_check_none)), FuncSignature::build<int64_t, void *>()
            );
            check->set_arg(0, vm_arg_scratch);
            arch::Gp result = cc.new_gp64("ir_is_none");
            check->set_ret(0, result);
            InvokeNode *push;
            (void)vm_arg0(ctx);
            arch::invoke_imm(
                cc, &push, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_push_bool)), FuncSignature::build<void, void *, int64_t>()
            );
            push->set_arg(0, vm_arg_scratch);
            push->set_arg(1, result);
            break;
        }
        case ir::Op::INeg:
            call0(ctx, reinterpret_cast<const void *>(jit_neg));
            break;
        case ir::Op::IAnd:
            call0(ctx, reinterpret_cast<const void *>(jit_bit_and));
            break;
        case ir::Op::IOr:
            call0(ctx, reinterpret_cast<const void *>(jit_bit_or));
            break;
        case ir::Op::IXor:
            call0(ctx, reinterpret_cast<const void *>(jit_bit_xor));
            break;
        case ir::Op::INot:
            call0(ctx, reinterpret_cast<const void *>(jit_bit_not));
            break;
        case ir::Op::IShl:
            call0(ctx, reinterpret_cast<const void *>(jit_lshift));
            break;
        case ir::Op::IShr:
            call0(ctx, reinterpret_cast<const void *>(jit_rshift));
            break;
        case ir::Op::JsBitBinary:
            call_u32(ctx, reinterpret_cast<const void *>(jit_js_bit_binary), in.imm_u32);
            break;
        case ir::Op::JsBitNot:
            call0(ctx, reinterpret_cast<const void *>(jit_js_bit_not));
            break;
        case ir::Op::LoadIndex: {
            // An integer-only array slot can still be uninitialized on this path.
            bool si = false;
            if (in.operands.size() >= 2 && irFuncs.inst(in.operands[1]).type == ir::Ty::Int48) {
                si = true;
            }
            emit_ir_load_index(ctx, false, si);
            break;
        }
        case ir::Op::StoreIndex: {
            bool si = false;
            if (in.operands.size() >= 2 && irFuncs.inst(in.operands[1]).type == ir::Ty::Int48) {
                si = true;
            }
            emit_ir_store_index(ctx, false, si);
            break;
        }
        case ir::Op::LoadProperty:
            call_u32(ctx, reinterpret_cast<const void *>(jit_get_property), in.imm_u32);
            break;
        case ir::Op::JsGetPropStatic:
            emit_ir_js_get_prop_static(ctx, in.imm_u32);
            break;
        case ir::Op::StoreProperty:
            call_u32(ctx, reinterpret_cast<const void *>(jit_set_property), in.imm_u32);
            break;
        case ir::Op::JsSetPropStatic:
            call_u32(ctx, reinterpret_cast<const void *>(jit_js_set_prop_static), in.imm_u32);
            break;
        case ir::Op::JsPostinc:
            call_u32(ctx, reinterpret_cast<const void *>(jit_js_postinc), in.imm_u32);
            break;
        case ir::Op::MakeArray:
            call_u32(ctx, reinterpret_cast<const void *>(jit_make_array), in.imm_u32);
            break;
        case ir::Op::ArrayPush:
            call0(ctx, reinterpret_cast<const void *>(jit_array_push));
            break;
        case ir::Op::ArraySpread:
            call0(ctx, reinterpret_cast<const void *>(jit_array_spread));
            break;
        case ir::Op::MakeObject: {
            const uint8_t *site = chunk.functions[chunk_idx].code.data() + in.bytecode_pc + 1;
            call_u32_u64(
                ctx, reinterpret_cast<const void *>(jit_make_object_site), in.imm_u32,
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(site))
            );
            break;
        }
        case ir::Op::MakeClosure: {
            const uint8_t *site = chunk.functions[chunk_idx].code.data() + in.bytecode_pc;
            call_i64(ctx, reinterpret_cast<const void *>(jit_make_closure), static_cast<int64_t>(reinterpret_cast<uintptr_t>(site)));
            break;
        }
        case ir::Op::StrConcat:
            // imm_u32==1: mark_inplace_concat proved the lhs is a single-use
            // StrConcat result (fresh, uniquely-owned mutable string), so we
            // append into its buffer instead of copy+alloc.
            call0(ctx, reinterpret_cast<const void *>(in.imm_u32 ? jit_str_concat_inplace : jit_str_concat));
            break;
        case ir::Op::StrAppendSlot:
            call_u32(ctx, reinterpret_cast<const void *>(jit_str_append_slot), in.imm_u32);
            break;
        case ir::Op::FormatValue:
            call_u32(ctx, reinterpret_cast<const void *>(jit_format_value), in.imm_u32);
            break;
        case ir::Op::IterArray:
            call0(ctx, reinterpret_cast<const void *>(jit_iter_array));
            break;
        case ir::Op::Call: {
            if (in.imm_u32 == 1 && !in.operands.empty()) {
                const ir::Inst &callee = irFuncs.inst(in.operands[0]);
                if (callee.op == ir::Op::LoadGlobal && callee.imm_u32 == typeof_name_idx_) {
                    emit_call_typeof(ctx);
                    break;
                }
                if (callee.op == ir::Op::LoadGlobal && callee.imm_u32 == js_to_number_name_idx_) {
                    call0(ctx, reinterpret_cast<const void *>(jit_call_js_to_number));
                    break;
                }
            }
            {
                const DirectCallee dc = resolve_direct_callee(ctx, in, in.imm_u32);
                emit_ir_call_value(ctx, in.imm_u32, static_cast<uint32_t>(in.imm_int), callee_kind_hint(ctx, in), dc.ok ? &dc : nullptr);
            }
            break;
        }
        case ir::Op::CallSpread:
            call_u32(ctx, reinterpret_cast<const void *>(jit_call_spread), in.imm_u32);
            break;
        case ir::Op::CallMethod:
            emit_ir_call_method(ctx, in);
            break;
        case ir::Op::DynAdd:
        case ir::Op::IAdd:
            if (both_float(ctx, in)) {
                emit_float_binop(ctx, in.op);
            } else {
                // when IR proves both operands Int48, skip the tag guard.
                emit_int_binop(ctx, in.op, lhs_int(ctx, in), rhs_int(ctx, in));
            }
            break;
        case ir::Op::DynSub:
        case ir::Op::ISub:
            if (both_float(ctx, in)) {
                emit_float_binop(ctx, in.op);
            } else {
                // Inline int fast-path with a runtime tag guard; slow path -> jit_sub.
                emit_int_binop(ctx, in.op, lhs_int(ctx, in), rhs_int(ctx, in));
            }
            break;
        case ir::Op::DynMul:
        case ir::Op::IMul:
            if (both_float(ctx, in)) {
                emit_float_binop(ctx, in.op);
            } else {
                // Inline int fast-path with a runtime tag guard; slow path -> jit_mul.
                emit_int_mul(ctx, lhs_int(ctx, in), rhs_int(ctx, in));
            }
            break;
        case ir::Op::DynMod:
        case ir::Op::IMod:
            // Inline int fast-path with a runtime tag guard; slow path -> jit_mod.
            emit_int_mod(ctx, lhs_int(ctx, in), rhs_int(ctx, in));
            break;
        case ir::Op::Pow:
            call0(ctx, reinterpret_cast<const void *>(jit_pow));
            break;
        case ir::Op::DynDiv:
        case ir::Op::FAdd:
        case ir::Op::FSub:
        case ir::Op::FMul:
        case ir::Op::FDiv:
            if (both_float(ctx, in)) {
                emit_float_binop(ctx, in.op);
            } else {
                call0(ctx, reinterpret_cast<const void *>(jit_div));
            }
            break;
        case ir::Op::DynCmpLt:
        case ir::Op::DynCmpLe:
        case ir::Op::DynCmpGt:
        case ir::Op::DynCmpGe:
        case ir::Op::DynCmpEq:
        case ir::Op::DynCmpNe:
        case ir::Op::DynStrictCmpEq:
        case ir::Op::DynStrictCmpNe:
        case ir::Op::ICmpLt:
        case ir::Op::ICmpLe:
        case ir::Op::ICmpGt:
        case ir::Op::ICmpGe:
        case ir::Op::ICmpEq:
        case ir::Op::ICmpNe:
            // Inline int fast-path with a runtime tag guard; slow path falls back
            // to the matching jit_lt/le/gt/ge/eq/ne helper.
            //
            // when IR proves both operands Int48, skip the tag guard.
            emit_int_cmp(ctx, in.op, lhs_int(ctx, in), rhs_int(ctx, in));
            break;
        case ir::Op::FCmpLt:
        case ir::Op::FCmpLe:
        case ir::Op::FCmpGt:
        case ir::Op::FCmpGe:
        case ir::Op::FCmpEq:
        case ir::Op::FCmpNe:
            emit_float_cmp(ctx, in.op);
            break;
        default:
            ok = false; // op not handled by IR lowering; VM interprets it
            unhandled_op = in.op;
            break;
    }
}

void AsmJITMethodCompiler::lower_term(IrEmitContext &ctx, const ir::Inst &t) {
    auto &blabels = ctx.blabels;
    auto &cc = ctx.cc;
    auto &cmp_scratch = ctx.cmp_scratch;
    auto &fc = ctx.fc;
    auto &irFuncs = ctx.irFuncs;
    auto &lowering_block = ctx.lowering_block;
    auto &ok = ctx.ok;
    auto &unhandled_op = ctx.unhandled_op;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    auto &vm_reg = ctx.vm_reg;
    if (jit_dump_asm_enabled()) {
        char obuf[64];
        snprintf(obuf, sizeof(obuf), "OP:%s", ir::op_name(t.op));
        cc.comment(obuf);
    }
    // any deferred finish-store must hit memory before control leaves the block
    fc.invalidate();
    switch (t.op) {
        case ir::Op::Jump:
            if (irFuncs.blocks[t.target0].start_pc <= irFuncs.blocks[lowering_block].start_pc &&
                !irFuncs.blocks[t.target0].short_bounded_loop) {
                poll_shutdown(ctx);
            }
            arch::jmp(cc, blabels[t.target0]);
            break;
        case ir::Op::Branch: {
            if ((irFuncs.blocks[t.target0].start_pc <= irFuncs.blocks[lowering_block].start_pc &&
                 !irFuncs.blocks[t.target0].short_bounded_loop) ||
                (irFuncs.blocks[t.target1].start_pc <= irFuncs.blocks[lowering_block].start_pc &&
                 !irFuncs.blocks[t.target1].short_bounded_loop)) {
                poll_shutdown(ctx);
            }
            // fast path: condition is statically Bool
            bool cond_is_bool =
                !t.operands.empty() && t.operands[0] != ir::InvalidValue && irFuncs.inst(t.operands[0]).type == ir::Ty::Bool;
            if (cond_is_bool) {
                arch::Gp endp = fc.get();
                arch::Gp truth = cc.new_gp64("ir_br_truth");
                // load the low byte of the top-of-stack Value (Value is at
                // [endp - ValSize], low byte at the same offset on LE).
                arch::load8_zx(cc, truth, arch::ptr8(endp, static_cast<int>((-ValSize))));
                arch::sub_imm(cc, endp, static_cast<int>(ValSize));
                fc.set(endp);
                arch::test_zero(cc, truth);                       // ZF=1 iff truth==0 (falsy)
                arch::jcc(cc, arch::CC::kNE, blabels[t.target0]); // truthy
                arch::jmp(cc, blabels[t.target1]);                // falsy
                break;
            }
            // The condition is not *statically* Bool, but at run time it almost always is
            arch::Gp truth = cc.new_gp64("ir_truth");
            Label truthy_call = cc.new_label();
            Label truthy_done = cc.new_label();
            {
                arch::Gp endp = fc.get();
                arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
                arch::cmp_imm(cc, cmp_scratch.r32(), Imm(static_cast<int>(tag._bool)));
                arch::jcc(cc, arch::CC::kNE, truthy_call);
                // Bool: the payload's low byte is the value (same as the static path).
                arch::load8_zx(cc, truth, arch::ptr8(endp, static_cast<int>((-ValSize))));
                arch::sub_imm(cc, endp, static_cast<int>(ValSize));
                fc.set(endp);
                arch::jmp(cc, truthy_done);
            }
            cc.bind(truthy_call);
            // jit_check_truthy pops the condition and returns 1 (truthy) / 0.
            fc.invalidate();
            InvokeNode *inv;
            (void)vm_arg0(ctx);
            arch::invoke_imm(
                cc, &inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_check_truthy)), FuncSignature::build<int64_t, void *>()
            );
            inv->set_arg(0, vm_arg_scratch);
            inv->set_ret(0, truth);
            cc.bind(truthy_done);
            arch::test_zero(cc, truth);                       // ZF=1 iff truth==0 (falsy)
            arch::jcc(cc, arch::CC::kNE, blabels[t.target0]); // truthy
            arch::jmp(cc, blabels[t.target1]);                // falsy
            break;
        }
        case ir::Op::SelfTailCall: {
            const uint32_t argc = t.imm_u32;
            arch::Gp endp = fc.get();
            arch::Gp slot_base = emit_ir_slot_addr(ctx, 0, "ir_stc_base");
            for (uint32_t i = 0; i < argc && i < irFuncs.num_params; i++) {
                arch::Gp arg = cc.new_gp64("ir_stc_arg");
                const int64_t offset = -static_cast<int64_t>(argc - i) * ValSize;
                arch::load(cc, arg, arch::ptr(endp, offset));
                arch::store(cc, arch::ptr(slot_base, static_cast<int64_t>(i) * ValSize), arg);
            }
            const uint64_t none = Value::none().raw_bits();
            for (uint32_t i = argc; i < irFuncs.num_params; i++) {
                arch::store_imm(cc, arch::ptr(slot_base, static_cast<int64_t>(i) * ValSize), Imm(static_cast<int64_t>(none)));
            }
            arch::lea(cc, endp, slot_base, static_cast<int64_t>(irFuncs.num_slots) * ValSize);
            fc.set(endp);
            poll_shutdown(ctx);
            arch::jmp(cc, blabels[t.target0]);
            break;
        }
        case ir::Op::Return:
            if (t.operands.empty()) {
                call0(ctx, reinterpret_cast<const void *>(jit_load_none)); // implicit return none
            }
            fc.invalidate();
            emit_inline_return(cc, vm_reg);
            cc.ret();
            break;
        default:
            ok = false;
            unhandled_op = t.op;
            break;
    }
}

bool AsmJITMethodCompiler::is_fcmp_branch_fusable(IrEmitContext &ctx, const ir::Inst &cmp) {
    auto &irFuncs = ctx.irFuncs;
    switch (cmp.op) {
        case ir::Op::FCmpLt:
        case ir::Op::FCmpLe:
        case ir::Op::FCmpGt:
        case ir::Op::FCmpGe:
            break;
        default:
            return false;
    }
    return irFuncs.inst(cmp.operands[0]).type == ir::Ty::Float && irFuncs.inst(cmp.operands[1]).type == ir::Ty::Float;
}

void AsmJITMethodCompiler::lower_fused_float_cmp_branch(IrEmitContext &ctx, const ir::Inst &cmp, const ir::Inst &term) {
    auto &blabels = ctx.blabels;
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    // terminator branches out of this block.
    // Both successors re-enter via `cc.bind(blabels[..])`
    arch::Gp endp = fc.get();
    arch::Vec fa = arch::new_vec_f64(cc, "ffcb_a");
    arch::Vec fb = arch::new_vec_f64(cc, "ffcb_b");
    arch::load_f64(cc, fb, arch::ptr(endp, static_cast<int>((-ValSize))));     // rhs
    arch::load_f64(cc, fa, arch::ptr(endp, static_cast<int>((-2 * ValSize)))); // lhs
    arch::sub_imm(cc, endp, static_cast<int>((2 * ValSize)));
    fc.set(endp);
    bool swap = (cmp.op == ir::Op::FCmpLt || cmp.op == ir::Op::FCmpLe);
    arch::CC::Cond fcc = (cmp.op == ir::Op::FCmpLt || cmp.op == ir::Op::FCmpGt) ? arch::CC::kFGT : arch::CC::kFGE;
    if (swap) {
        arch::float_cmp(cc, fb, fa);
    } else {
        arch::float_cmp(cc, fa, fb);
    }
    arch::jcc(cc, fcc, blabels[term.target0]);
    arch::jmp(cc, blabels[term.target1]);
}

void AsmJITMethodCompiler::lower_fused_int_cmp_branch(IrEmitContext &ctx, const ir::Inst &cmp, const ir::Inst &term) {
    auto &blabels = ctx.blabels;
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &irFuncs = ctx.irFuncs;
    // Operands whose IR type is already Int48 carry tagInt by construction, so
    // their runtime tag check is dead code -- and if BOTH are proven the whole
    // generic fallback block (a duplicate of cmp+term) is unreachable too.
    const bool lhs_known_int = cmp.operands.size() >= 2 && irFuncs.inst(cmp.operands[0]).type == ir::Ty::Int48;
    const bool rhs_known_int = cmp.operands.size() >= 2 && irFuncs.inst(cmp.operands[1]).type == ir::Ty::Int48;
    const bool need_slow = !(lhs_known_int && rhs_known_int);
    Label slow = cc.new_label();
    arch::Gp endp = fc.materialize_keep(2);
    const auto *prhs = fc.peek(0);
    const auto *plhs = fc.peek(1);
    const bool fuse_rhs = prhs != nullptr;
    const bool fuse_lhs = plhs != nullptr;
    const int n_fused = static_cast<int>(fuse_rhs) + static_cast<int>(fuse_lhs);

    // A constant rhs whose tag is statically Int48 folds straight into `cmp lhs, imm`
    int64_t rhs_imm = 0;
    bool rhs_as_imm = false;
    if (fuse_rhs && prhs->is_imm && static_cast<uint64_t>((static_cast<uint64_t>(prhs->imm) >> 48)) == static_cast<uint64_t>(tag._int)) {
        const int64_t pay = static_cast<int64_t>(static_cast<uint64_t>(prhs->imm) << 16) >> 16;
        if (pay >= INT32_MIN && pay <= INT32_MAX) {
            rhs_as_imm = true;
            rhs_imm = pay;
        }
    }
    // Any other pending constant still has to become a register here
    auto pend_reg = [&](const FinishCache::Pend &pv, const char *name) {
        if (!pv.is_imm) {
            return pv.reg;
        }
        arch::Gp r = cc.new_gp64(name);
        cc.mov(r, Imm(pv.imm));
        return r;
    };
    arch::Gp rhs_src;
    arch::Gp lhs_src;
    if (fuse_rhs && !rhs_as_imm) {
        rhs_src = pend_reg(*prhs, "fcb_rsrc");
    }
    if (fuse_lhs) {
        lhs_src = pend_reg(*plhs, "fcb_lsrc");
    }
    // Memory slot offsets: a fused rhs is not on the stack, so an unfused lhs
    // sits at the top instead of one below it.
    const int rhs_off = -ValSize;
    const int lhs_off = fuse_rhs ? -ValSize : -2 * ValSize;

    // A pend register must survive into the slow arm, which stores it to
    // memory, so read the tag out of a copy rather than mutating it.
    auto fused_tag_check = [&](arch::Gp src, const char *name) {
        arch::Gp tag_check = cc.new_gp64(name);
        arch::mov_reg(cc, tag_check, src);
        arch::shr(cc, tag_check, 48);
        arch::cmp_imm(cc, tag_check.r32(), Imm(static_cast<int>(tag._int)));
        arch::jcc(cc, arch::CC::kNE, slow);
    };
    if (!rhs_known_int && !rhs_as_imm) {
        if (fuse_rhs) {
            fused_tag_check(rhs_src, "fcb_tb");
        } else {
            arch::Gp tagB = cc.new_gp64("fcb_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, static_cast<int>((rhs_off + tag.word_off))));
            arch::cmp_imm(cc, tagB.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
    }
    if (!lhs_known_int) {
        if (fuse_lhs) {
            fused_tag_check(lhs_src, "fcb_ta");
        } else {
            arch::Gp tagA = cc.new_gp64("fcb_ta");
            arch::load16_zx(cc, tagA, arch::ptr16(endp, static_cast<int>((lhs_off + tag.word_off))));
            arch::cmp_imm(cc, tagA.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
    }

    arch::Gp rhs = cc.new_gp64("fcb_rhs");
    arch::Gp lhs = cc.new_gp64("fcb_lhs");
    // sign_extend_48 mutates in place, so a fused operand is copied first: a
    // queued vreg must stay intact for the slow arm to store.
    if (!rhs_as_imm) {
        if (fuse_rhs) {
            arch::mov_reg(cc, rhs, rhs_src);
        } else {
            arch::load(cc, rhs, arch::ptr(endp, static_cast<int>(rhs_off)));
        }
        arch::sign_extend_48(cc, rhs);
    }
    if (fuse_lhs) {
        arch::mov_reg(cc, lhs, lhs_src);
    } else {
        arch::load(cc, lhs, arch::ptr(endp, static_cast<int>(lhs_off)));
    }
    arch::sign_extend_48(cc, lhs);
    // Pop the operands that really are in memory BEFORE the compare: the
    // stack-pointer sub sets EFLAGS, so it must not sit between `cmp` and the
    // `jcc` that reads its result.
    const int n_mem = 2 - n_fused;
    if (n_mem > 0) {
        arch::sub_imm(cc, endp, static_cast<int>((n_mem * ValSize)));
        fc.set(endp);
    }
    if (rhs_as_imm) {
        arch::cmp_imm(cc, lhs, Imm(rhs_imm));
    } else {
        cc.cmp(lhs, rhs);
    }

    arch::CC::Cond cond = arch::CC::kEQ;
    switch (cmp.op) {
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
    arch::jcc(cc, cond, blabels[term.target0]);
    arch::jmp(cc, blabels[term.target1]);

    if (need_slow) {
        cc.bind(slow);
        lower_body(ctx, cmp); // its fc.get() materializes the fused operands here
        lower_term(ctx, term);
    } else {
        fc.drop_pend(n_fused); // no slow arm materialized them
    }
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
