#include "asmjit/core/cpuinfo.h"
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

// Guarded direct call. Structure follows the NARI_ENABLE_DIRECT_CALL_JIT path
// below, but replaces its ~19 dynamic guards with one closure-identity
// compare and bakes the callee's locals/meta/captures as immediates. The
// compiled entry is *loaded* rather than baked so a later speculative
// recompile of the callee is picked up.
void AsmJITMethodCompiler::emit_direct_call_guarded(IrEmitContext &ctx, uint32_t argc, uint32_t callee_label_idx, const DirectCallee &d) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    auto &vm_reg = ctx.vm_reg;
    Label slow = cc.new_label();
    Label done = cc.new_label();
    // Every temporary below is written and consumed within two instructions,
    // so one virtual register per role is enough. Allocating a fresh vreg per
    // use instead cost ~10 extra vregs at each of the thousands of direct-call
    // sites, and asmjit's register allocation is superlinear in vreg count.
    arch::Gp dc_tmp = cc.new_gp64("dc_tmp");
    auto jump_if_owned = [&](const arch::Mem &field, const char *) {
        arch::load(cc, dc_tmp, field);
        arch::test_zero(cc, dc_tmp);
        arch::jcc(cc, arch::CC::kNE, slow);
    };

    arch::Gp endp = fc.get();

    // callee identity, pinned on FunctionMeta rather than on one closure value.
    const int callee_off = static_cast<int>((-static_cast<int64_t>((argc + 1)) * ValSize));
    arch::Gp closure = cc.new_gp64("dc_closure");
    arch::load(cc, closure, arch::ptr(endp, callee_off));
    arch::cmp_mem16_imm(cc, arch::ptr16(endp, callee_off + tag.word_off), Imm(static_cast<int>(tag._heap)));
    arch::jcc(cc, arch::CC::kNE, slow);
    arch::Gp dc_fd = cc.new_gp64("dc_fd");
    cc.mov(dc_fd, closure);
    arch::zero_extend_48(cc, dc_fd);
    arch::cmp_mem8_imm(cc, arch::ptr8(dc_fd, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._function)));
    arch::jcc(cc, arch::CC::kNE, slow);
    // jit_func_idx is a 32-bit chunk-unique function id, so this is a single
    // cmp-with-immediate: no vreg for the expected value.
    arch::cmp_mem32_imm(cc, arch::ptr32(dc_fd, FDFuncIdxOff), Imm(static_cast<int>(d.fidx)));
    arch::jcc(cc, arch::CC::kNE, slow);

    // compiled entry, from a fixed slot in the stable fn table
    assert_tables_stable();
    arch::Gp cslot = cc.new_gp64("dc_cslot");
    cc.mov(cslot, Imm(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(compiled_fn_vec.get() + d.fidx))));
    arch::Gp callee = cc.new_gp64("dc_callee");
    arch::load_acquire(cc, callee, arch::ptr(cslot));
    arch::test_zero(cc, callee);
    arch::jcc(cc, arch::CC::kEQ, slow);

    // (3) frame headroom and call depth
    arch::Gp ff = cc.new_gp64("dc_ff");
    arch::Gp fcap = cc.new_gp64("dc_fcap");
    arch::load(cc, ff, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)));
    arch::load(cc, fcap, arch::ptr(vm_reg, static_cast<int>(VMFramesCapacityOff)));
    arch::sub2(cc, fcap, ff);
    arch::cmp_imm(cc, fcap, Imm(static_cast<int>(FrameSize)));
    arch::jcc(cc, arch::CC::kFLT, slow);
    arch::Gp frame_begin = cc.new_gp64("dc_fbegin");
    arch::load(cc, frame_begin, arch::ptr(vm_reg, static_cast<int>(FramesStartOff)));
    arch::add_imm(cc, frame_begin, static_cast<int64_t>(MAX_CALL_DEPTH) * FrameSize);
    cc.cmp(ff, frame_begin);
    arch::jcc(cc, arch::CC::kUGE, slow);

    // (4) the raw frame slot may be reused only if it owns no C++ objects
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameCapturesOff)), "dc_fc_ptr");
    jump_if_owned(arch::ptr(ff, static_cast<int>((FrameCapturesOff + 8))), "dc_fc_own");
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameInlineUpvalueOff)), "dc_fi_ptr");
    jump_if_owned(arch::ptr(ff, static_cast<int>((FrameInlineUpvalueOff + 8))), "dc_fi_own");
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameOpenUpvalOff)), "dc_fu");
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameSingleCaptureCacheOff)), "dc_fs_ptr");
    jump_if_owned(arch::ptr(ff, static_cast<int>((FrameSingleCaptureCacheOff + 8))), "dc_fs_own");

    // (5) value-stack headroom. locals is an immediate, so this is one lea.
    arch::Gp nf = cc.new_gp64("dc_nf");
    arch::lea(cc, nf, endp, (static_cast<int64_t>(d.locals) - static_cast<int64_t>(argc) - 1) * static_cast<int64_t>(ValSize));
    arch::Gp stack_cap = cc.new_gp64("dc_scap");
    arch::load(cc, stack_cap, arch::ptr(vm_reg, static_cast<int>(VMStackCapacityOff)));
    cc.cmp(nf, stack_cap);
    arch::jcc(cc, arch::CC::kUGT, slow);

    // (6) shift the arguments down over the callee slot
    for (int i = 0; i < static_cast<int>(argc); i++) {
        const int src = (i - static_cast<int>(argc)) * static_cast<int>(ValSize);
        arch::Gp tmp = dc_tmp;
        arch::load(cc, tmp, arch::ptr(endp, src));
        arch::store(cc, arch::ptr(endp, src - static_cast<int>(ValSize)), tmp);
    }

    // (7) none-fill the non-parameter locals (count is a compile-time constant,
    // so the helper's runtime init loop becomes a short unrolled run)
    arch::Gp none = cc.new_gp64("dc_none");
    cc.mov(none, Imm(static_cast<uint64_t>(Value::none().raw_bits())));
    for (uint32_t i = argc; i < d.locals; i++) {
        arch::store(cc, arch::ptr(endp, (static_cast<int>(i) - static_cast<int>(argc) - 1) * static_cast<int>(ValSize)), none);
    }
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), nf);

    // (8) publish the frame
    arch::Gp sbi = cc.new_gp64("dc_sbi");
    arch::lea(cc, sbi, endp, static_cast<int64_t>((-static_cast<int64_t>((argc + 1)) * ValSize)));
    arch::sub_mem(cc, sbi, arch::ptr(vm_reg, static_cast<int>(VMStackStartOff)));
    arch::shr(cc, sbi, 3);
    arch::Gp meta = cc.new_gp64("dc_meta");
    cc.mov(meta, Imm(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.meta))));
    arch::Gp ip = cc.new_gp64("dc_ip");
    arch::load(cc, ip, arch::ptr(meta, static_cast<int>(NARI_JIT_OFF(bytecode::FunctionMeta, code.storage_begin))));
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameFunctionOff)), meta);
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameIpOff)), ip);
    arch::store(cc, arch::ptr(ff, static_cast<int>(SlotBaseOff)), sbi);
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameCapturesOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>((FrameCapturesOff + 8))), Imm(0));
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameClosureRootOff)), closure);
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameReceiverOff)), none);
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameInlineUpvalueIdxOff)), Imm(UINT16_MAX));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameInlineUpvalueOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>((FrameInlineUpvalueOff + 8))), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameOpenUpvalOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameSingleCaptureCacheOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>((FrameSingleCaptureCacheOff + 8))), Imm(0));
    arch::add_imm(cc, ff, static_cast<int>(FrameSize));
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)), ff);

    // (9) rebind the VM's borrowed capture pointers to the callee's, exactly as
    // jit_call_value_impl does. These are immediates because the guard pinned
    // the closure; the caller's are runtime values and must be saved.
    arch::Gp sav_c = cc.new_gp64("dc_sav_c");
    arch::Gp sav_0 = cc.new_gp64("dc_sav_0");
    arch::Gp sav_1 = cc.new_gp64("dc_sav_1");
    arch::Gp sav_2 = cc.new_gp64("dc_sav_2");
    arch::load(cc, sav_c, arch::ptr(vm_reg, static_cast<int>(VMCapturesRawOff)));
    arch::load(cc, sav_0, arch::ptr(vm_reg, static_cast<int>(VMCapture0RawOff)));
    arch::load(cc, sav_1, arch::ptr(vm_reg, static_cast<int>(VMCapture1RawOff)));
    arch::load(cc, sav_2, arch::ptr(vm_reg, static_cast<int>(VMCapture2RawOff)));
    // Runtime loads, not immediates: the guard pinned the function, not the
    // closure, so each instance brings its own capture cells.
    auto copy_cap = [&](int64_t src_off, int dst_off) {
        arch::load(cc, dc_tmp, arch::ptr(dc_fd, static_cast<int>(src_off)));
        arch::store(cc, arch::ptr(vm_reg, dst_off), dc_tmp);
    };
    copy_cap(FDCapturesRawOff, static_cast<int>(VMCapturesRawOff));
    copy_cap(FDCapture0RawOff, static_cast<int>(VMCapture0RawOff));
    copy_cap(FDCapture1RawOff, static_cast<int>(VMCapture1RawOff));
    copy_cap(FDCapture2RawOff, static_cast<int>(VMCapture2RawOff));

    fc.invalidate();
    InvokeNode *inv;
    (void)vm_arg0(ctx);
    cc.invoke(Out(inv), callee, FuncSignature::build<void, void *>());
    inv->set_arg(0, vm_arg_scratch);

    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMCapturesRawOff)), sav_c);
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMCapture0RawOff)), sav_0);
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMCapture1RawOff)), sav_1);
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMCapture2RawOff)), sav_2);
    arch::jmp(cc, done);

    cc.bind(slow);
    call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_value), argc, callee_label_idx);
    cc.bind(done);
    fc.invalidate();
}

void AsmJITMethodCompiler::emit_ir_call_value(
    IrEmitContext &ctx, uint32_t argc, uint32_t callee_label_idx, int kind_hint, const DirectCallee *dc
) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    auto &vm_reg = ctx.vm_reg;
    if (dc != nullptr && dc->ok) {
        emit_direct_call_guarded(ctx, argc, callee_label_idx, *dc);
        return;
    }
    if (argc == 1 && getenv("NARI_ENABLE_DIRECT_CALL_JIT") == nullptr) {
        if (kind_hint >= 0 && kind_hint != 9) {
            // callee is a known builtin with no argc==1 inline body: skip the guards
            call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_value), argc, callee_label_idx);
            return;
        }
        Label slow = cc.new_label();
        Label length = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        arch::Gp length_tag = cc.new_gp64("ir_cv_length_tag");
        arch::load16_zx(cc, length_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, length_tag.r32(), Imm(static_cast<int>(tag._heap)));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp fd = cc.new_gp64("ir_cv_length_fd");
        arch::load(cc, fd, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
        arch::zero_extend_48(cc, fd);
        arch::cmp_mem8_imm(cc, arch::ptr8(fd, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._function)));
        arch::jcc(cc, arch::CC::kNE, slow);
        arch::Gp native_kind = cc.new_gp64("ir_cv_length_kind");
        arch::load32_zx(cc, native_kind, fd, static_cast<int>(FDNativeKindOff));
        arch::cmp_imm(cc, native_kind, Imm(9));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::load16_zx(cc, length_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
        arch::cmp_imm(cc, length_tag.r32(), Imm(static_cast<int>(tag._heap)));
        arch::jcc(cc, arch::CC::kNE, slow);
        arch::Gp object = cc.new_gp64("ir_cv_length_object");
        arch::load(cc, object, arch::ptr(endp, static_cast<int>(-ValSize)));
        arch::zero_extend_48(cc, object);
        arch::cmp_mem8_imm(cc, arch::ptr8(object, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._array)));
        arch::jcc(cc, arch::CC::kEQ, length);
        arch::cmp_mem8_imm(
            cc, arch::ptr8(object, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._string))
        );
        arch::jcc(cc, arch::CC::kNE, slow);

        cc.bind(length);
        arch::Gp result = cc.new_gp64("ir_cv_length_result");
        InvokeNode *inv;
        arch::invoke_imm(
            cc, &inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_js_length)), FuncSignature::build<uint64_t, void *>()
        );
        inv->set_arg(0, object);
        inv->set_ret(0, result);
        arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), result);
        arch::sub_imm(cc, endp, static_cast<int>(ValSize));
        fc.set(endp);
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_value), argc, callee_label_idx);
        fc.merge_slow_path();
        cc.bind(done);
        return;
    }
    if (argc == 2 && getenv("NARI_ENABLE_DIRECT_CALL_JIT") == nullptr) {
        const bool want_cmp = kind_hint >= 2 && kind_hint <= 5;
        const bool want_get_prop = kind_hint == 6;
        const bool want_char_code = kind_hint == 10;
        const bool want_add = kind_hint == 16;
        const bool want_code_point = kind_hint == 15;
        if (!want_cmp && !want_get_prop && !want_char_code && !want_add && !want_code_point) {
            call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_value), argc, callee_label_idx);
            return;
        }
        Label slow = cc.new_label();
        Label cmp_lt = cc.new_label();
        Label cmp_gt = cc.new_label();
        Label cmp_le = cc.new_label();
        Label get_prop = cc.new_label();
        Label char_code = cc.new_label();
        Label add_ints = cc.new_label();
        Label code_point = cc.new_label();
        Label store = cc.new_label();
        Label done = cc.new_label();

        arch::Gp endp = fc.get();
        arch::Gp native_tag = cc.new_gp64("ir_cv_native_tag");
        arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-3 * ValSize + tag.word_off))));
        arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._heap)));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp fd = cc.new_gp64("ir_cv_native_fd");
        arch::load(cc, fd, arch::ptr(endp, static_cast<int>((-3 * ValSize))));
        arch::zero_extend_48(cc, fd);
        arch::cmp_mem8_imm(cc, arch::ptr8(fd, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._function)));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp native_kind = cc.new_gp64("ir_cv_native_kind");
        arch::load32_zx(cc, native_kind, fd, static_cast<int>(FDNativeKindOff));
        if (want_char_code) {
            arch::cmp_imm(cc, native_kind, Imm(10));
            arch::jcc(cc, want_cmp || want_get_prop ? arch::CC::kEQ : arch::CC::kNE, want_cmp || want_get_prop ? char_code : slow);
        }
        if (want_get_prop) {
            arch::cmp_imm(cc, native_kind, Imm(6));
            arch::jcc(cc, want_cmp ? arch::CC::kEQ : arch::CC::kNE, want_cmp ? get_prop : slow);
        }
        if (want_add) {
            arch::cmp_imm(cc, native_kind, Imm(16));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::jmp(cc, add_ints);
        }
        if (want_code_point) {
            arch::cmp_imm(cc, native_kind, Imm(15));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::jmp(cc, code_point);
        }
        if (!want_cmp) {
            // fall straight into whichever single body remains
            if (want_get_prop) {
                arch::jmp(cc, get_prop);
            } else if (want_char_code) {
                arch::jmp(cc, char_code);
            }
        } else {

            arch::cmp_imm(cc, native_kind, Imm(2));
            arch::jcc(cc, arch::CC::kLT, slow);
            arch::cmp_imm(cc, native_kind, Imm(5));
            arch::jcc(cc, arch::CC::kGT, slow);

            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);

            arch::Gp lhs = cc.new_gp64("ir_cv_native_lhs");
            arch::Gp rhs = cc.new_gp64("ir_cv_native_rhs");
            arch::load(cc, lhs, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
            arch::load(cc, rhs, arch::ptr(endp, static_cast<int>((-ValSize))));
            arch::sign_extend_48(cc, lhs);
            arch::sign_extend_48(cc, rhs);

            arch::cmp_imm(cc, native_kind, Imm(2));
            arch::jcc(cc, arch::CC::kEQ, cmp_lt);
            arch::cmp_imm(cc, native_kind, Imm(3));
            arch::jcc(cc, arch::CC::kEQ, cmp_gt);
            arch::cmp_imm(cc, native_kind, Imm(4));
            arch::jcc(cc, arch::CC::kEQ, cmp_le);

            arch::Gp result = cc.new_gp64("ir_cv_native_result");
            cc.cmp(lhs, rhs);
            arch::cset(cc, result, arch::CC::kGE);
            arch::jmp(cc, store);
            cc.bind(cmp_lt);
            cc.cmp(lhs, rhs);
            arch::cset(cc, result, arch::CC::kLT);
            arch::jmp(cc, store);
            cc.bind(cmp_gt);
            cc.cmp(lhs, rhs);
            arch::cset(cc, result, arch::CC::kGT);
            arch::jmp(cc, store);
            cc.bind(cmp_le);
            cc.cmp(lhs, rhs);
            arch::cset(cc, result, arch::CC::kLE);

            cc.bind(store);
            arch::or_imm(cc, result, result, static_cast<int64_t>(nb.bool_tag));
            arch::store(cc, arch::ptr(endp, static_cast<int>((-3 * ValSize))), result);
            arch::sub_imm(cc, endp, static_cast<int>((2 * ValSize)));
            fc.set(endp);
            arch::jmp(cc, done);
        } // want_cmp

        if (want_get_prop) {
            cc.bind(get_prop);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._heap)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp array = cc.new_gp64("ir_cv_get_prop_array");
            arch::load(cc, array, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
            arch::zero_extend_48(cc, array);
            arch::cmp_mem8_imm(
                cc, arch::ptr8(array, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._array))
            );
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp index = cc.new_gp64("ir_cv_get_prop_index");
            arch::load(cc, index, arch::ptr(endp, static_cast<int>(-ValSize)));
            arch::sign_extend_48(cc, index);
            arch::js(cc, index, slow);
            arch::Gp start = cc.new_gp64("ir_cv_get_prop_start");
            arch::Gp finish = cc.new_gp64("ir_cv_get_prop_finish");
            arch::load(cc, start, arch::ptr(array, static_cast<int>(ArrayVecStartOff)));
            arch::load(cc, finish, arch::ptr(array, static_cast<int>(ArrayVecFinishOff)));
            arch::sub2(cc, finish, start);
            arch::shr(cc, finish, 3);
            cc.cmp(index, finish);
            arch::jcc(cc, arch::CC::kGE, slow);
            arch::Gp element = cc.new_gp64("ir_cv_get_prop_element");
            arch::shl(cc, element, index, 3);
            arch::add2(cc, element, start);
            arch::Gp get_prop_result = cc.new_gp64("ir_cv_get_prop_result");
            arch::load(cc, get_prop_result, arch::ptr(element));
            arch::store(cc, arch::ptr(endp, static_cast<int>((-3 * ValSize))), get_prop_result);
            arch::sub_imm(cc, endp, static_cast<int>((2 * ValSize)));
            fc.set(endp);
            arch::jmp(cc, done);
        } // want_get_prop

        if (want_char_code) {
            cc.bind(char_code);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._heap)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp string_obj = cc.new_gp64("ir_cv_char_string");
            arch::load(cc, string_obj, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
            arch::zero_extend_48(cc, string_obj);
            arch::cmp_mem8_imm(
                cc, arch::ptr8(string_obj, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._string))
            );
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp char_index = cc.new_gp64("ir_cv_char_index");
            arch::load(cc, char_index, arch::ptr(endp, static_cast<int>((-ValSize))));
            arch::sign_extend_48(cc, char_index);
            arch::cmp_imm(cc, char_index, Imm(std::numeric_limits<int>::min()));
            arch::jcc(cc, arch::CC::kLT, slow);
            arch::cmp_imm(cc, char_index, Imm(std::numeric_limits<int>::max()));
            arch::jcc(cc, arch::CC::kGT, slow);
            arch::Gp char_result = cc.new_gp64("ir_cv_char_result");
            InvokeNode *char_inv;
            arch::invoke_imm(
                cc, &char_inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_string_char_code_at)),
                FuncSignature::build<uint64_t, void *, int64_t>()
            );
            char_inv->set_arg(0, string_obj);
            char_inv->set_arg(1, char_index);
            char_inv->set_ret(0, char_result);
            arch::store(cc, arch::ptr(endp, static_cast<int>((-3 * ValSize))), char_result);
            arch::sub_imm(cc, endp, static_cast<int>((2 * ValSize)));
            fc.set(endp);
            arch::jmp(cc, done);
        } // want_char_code

        if (want_add) {
            // __js_add(int, int).
            // Both operands must be int-tagged and fit in int32
            cc.bind(add_ints);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp add_lhs = cc.new_gp64("ir_cv_add_lhs");
            arch::Gp add_rhs = cc.new_gp64("ir_cv_add_rhs");
            arch::load(cc, add_lhs, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
            arch::load(cc, add_rhs, arch::ptr(endp, static_cast<int>((-ValSize))));
            arch::sign_extend_48(cc, add_lhs);
            arch::sign_extend_48(cc, add_rhs);
            arch::cmp_imm(cc, add_lhs, Imm(std::numeric_limits<int>::min()));
            arch::jcc(cc, arch::CC::kLT, slow);
            arch::cmp_imm(cc, add_lhs, Imm(std::numeric_limits<int>::max()));
            arch::jcc(cc, arch::CC::kGT, slow);
            arch::cmp_imm(cc, add_rhs, Imm(std::numeric_limits<int>::min()));
            arch::jcc(cc, arch::CC::kLT, slow);
            arch::cmp_imm(cc, add_rhs, Imm(std::numeric_limits<int>::max()));
            arch::jcc(cc, arch::CC::kGT, slow);
            arch::add2(cc, add_lhs, add_rhs);
            arch::nanbox_encode_int(cc, add_lhs, add_rhs);
            arch::store(cc, arch::ptr(endp, static_cast<int>((-3 * ValSize))), add_lhs);
            arch::sub_imm(cc, endp, static_cast<int>((2 * ValSize)));
            fc.set(endp);
            arch::jmp(cc, done);
        } // want_add

        if (want_code_point) {
            // __js_str_code_point_at(s, i)
            // this helper differs from `str.char_code_at(index)` by returning undefined rather than -1 when the index is out of range.
            cc.bind(code_point);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-2 * ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._heap)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp cp_string = cc.new_gp64("ir_cv_cp_string");
            arch::load(cc, cp_string, arch::ptr(endp, static_cast<int>((-2 * ValSize))));
            arch::zero_extend_48(cc, cp_string);
            arch::cmp_mem8_imm(
                cc, arch::ptr8(cp_string, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._string))
            );
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::load16_zx(cc, native_tag, arch::ptr16(endp, static_cast<int>((-ValSize + tag.word_off))));
            arch::cmp_imm(cc, native_tag.r32(), Imm(static_cast<int>(tag._int)));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp cp_index = cc.new_gp64("ir_cv_cp_index");
            arch::load(cc, cp_index, arch::ptr(endp, static_cast<int>((-ValSize))));
            arch::sign_extend_48(cc, cp_index);
            arch::cmp_imm(cc, cp_index, Imm(std::numeric_limits<int>::min()));
            arch::jcc(cc, arch::CC::kLT, slow);
            arch::cmp_imm(cc, cp_index, Imm(std::numeric_limits<int>::max()));
            arch::jcc(cc, arch::CC::kGT, slow);
            arch::Gp cp_result = cc.new_gp64("ir_cv_cp_result");
            InvokeNode *cp_inv;
            arch::invoke_imm(
                cc, &cp_inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_string_code_point_at)),
                FuncSignature::build<uint64_t, void *, void *, int64_t>()
            );
            cp_inv->set_arg(0, vm_arg0(ctx));
            cp_inv->set_arg(1, cp_string);
            cp_inv->set_arg(2, cp_index);
            cp_inv->set_ret(0, cp_result);
            arch::store(cc, arch::ptr(endp, static_cast<int>((-3 * ValSize))), cp_result);
            arch::sub_imm(cc, endp, static_cast<int>((2 * ValSize)));
            fc.set(endp);
            arch::jmp(cc, done);
        } // want_code_point

        cc.bind(slow);
        call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_value), argc, callee_label_idx);
        fc.merge_slow_path();
        cc.bind(done);
        return;
    }

    if (argc > 4 || getenv("NARI_ENABLE_DIRECT_CALL_JIT") == nullptr) {
        call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_value), argc, callee_label_idx);
        return;
    }
    Label slow = cc.new_label();
    Label done = cc.new_label();
    auto jump_if_owned = [&](const arch::Mem &field, const char *name) {
        arch::Gp value = cc.new_gp64(name);
        arch::load(cc, value, field);
        arch::test_zero(cc, value);
        arch::jcc(cc, arch::CC::kNE, slow);
    };

    arch::Gp endp = cc.new_gp64("ir_cv_end");
    arch::Gp cv_tag = cc.new_gp64("ir_cv_tag");
    arch::load(cc, endp, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));

    arch::load16_zx(cc, cv_tag, arch::ptr16(endp, static_cast<int>((-static_cast<int64_t>((argc + 1)) * ValSize + tag.word_off))));
    arch::cmp_imm(cc, cv_tag.r32(), Imm(static_cast<int>(tag._heap)));
    arch::jcc(cc, arch::CC::kNE, slow);

    arch::Gp fd = cc.new_gp64("ir_cv_fd");
    arch::load(cc, fd, arch::ptr(endp, static_cast<int>((-static_cast<int64_t>((argc + 1)) * ValSize))));
    arch::zero_extend_48(cc, fd);
    arch::cmp_mem8_imm(cc, arch::ptr8(fd, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))), Imm(static_cast<int>(tag._function)));
    arch::jcc(cc, arch::CC::kNE, slow);

    arch::Gp fidx = cc.new_gp64("ir_cv_fidx");
    arch::load32_sx(cc, fidx, arch::ptr32(fd, FDFuncIdxOff));
    arch::js(cc, fidx, slow);
    arch::Gp locals = cc.new_gp64("ir_cv_locals");
    arch::load32_zx(cc, locals, fd, (int)FDLocalsCountOff);

    arch::Gp inline_kind = cc.new_gp64("ir_cv_inline");
    arch::load32_zx(cc, inline_kind, fd, static_cast<int>(FDInlineKindOff));
    arch::cmp_imm(cc, inline_kind, Imm(to_int(JitInlineKind::Capture0)));
    arch::jcc(cc, arch::CC::kEQ, slow);
    jump_if_owned(arch::ptr(fd, static_cast<int>(FDCapturesRawOff)), "ir_cv_captures");

    arch::Gp callee = cc.new_gp64("ir_cv_cal");
    {
        arch::Gp base = cc.new_gp64("ir_cv_base");
        assert_tables_stable();
        cc.mov(base, Imm(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(compiled_fn_vec.get()))));
        arch::shl(cc, fidx, fidx, 3);
        arch::add2(cc, fidx, base);
        arch::load_acquire(cc, callee, arch::ptr(fidx));
    }
    arch::test_zero(cc, callee);
    arch::jcc(cc, arch::CC::kEQ, slow);

    arch::Gp ff = cc.new_gp64("ir_cv_ff");
    arch::Gp fcap = cc.new_gp64("ir_cv_fcap");
    arch::load(cc, ff, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)));
    arch::load(cc, fcap, arch::ptr(vm_reg, static_cast<int>(VMFramesCapacityOff)));
    arch::sub2(cc, fcap, ff);
    arch::cmp_imm(cc, fcap, Imm(static_cast<int>(FrameSize)));
    arch::jcc(cc, arch::CC::kFLT, slow);
    arch::Gp frame_begin = cc.new_gp64("ir_cv_frame_begin");
    arch::load(cc, frame_begin, arch::ptr(vm_reg, static_cast<int>(FramesStartOff)));
    arch::add_imm(cc, frame_begin, static_cast<int64_t>(MAX_CALL_DEPTH) * FrameSize);
    cc.cmp(ff, frame_begin);
    arch::jcc(cc, arch::CC::kUGE, slow);

    // Reusing a raw frame slot is safe only while it owns no C++ objects.
    // push_jit_frame() remains the slow path that releases dirty slots.
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameCapturesOff)), "ir_cv_frame_caps_ptr");
    jump_if_owned(arch::ptr(ff, static_cast<int>((FrameCapturesOff + 8))), "ir_cv_frame_caps_owner");
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameInlineUpvalueOff)), "ir_cv_frame_inline_ptr");
    jump_if_owned(arch::ptr(ff, static_cast<int>((FrameInlineUpvalueOff + 8))), "ir_cv_frame_inline_owner");
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameOpenUpvalOff)), "ir_cv_frame_upvals");
    jump_if_owned(arch::ptr(ff, static_cast<int>(FrameSingleCaptureCacheOff)), "ir_cv_frame_cache_ptr");
    jump_if_owned(arch::ptr(ff, static_cast<int>((FrameSingleCaptureCacheOff + 8))), "ir_cv_frame_cache_owner");

    arch::Gp meta = cc.new_gp64("ir_cv_meta");
    arch::load(cc, meta, arch::ptr(fd, (int)FDMetaOff));
    arch::test_zero(cc, meta);
    arch::jcc(cc, arch::CC::kEQ, slow);
    arch::cmp_mem8_imm(cc, arch::ptr8(meta, static_cast<int>(NARI_JIT_OFF(bytecode::FunctionMeta, rest_param_index))), Imm(-1));
    arch::jcc(cc, arch::CC::kNE, slow);
    arch::Gp param_count = cc.new_gp64("ir_cv_params");
    arch::load8_zx(cc, param_count, arch::ptr(meta, static_cast<int>(NARI_JIT_OFF(bytecode::FunctionMeta, param_count))));
    arch::cmp_imm(cc, param_count, Imm(static_cast<int>(argc)));
    Label args_ready = cc.new_label();
    arch::jcc(cc, arch::CC::kULE, args_ready);
    arch::cmp_mem8_imm(cc, arch::ptr8(meta, static_cast<int>(NARI_JIT_OFF(bytecode::FunctionMeta, js_undefined_params))), Imm(0));
    arch::jcc(cc, arch::CC::kNE, slow);
    cc.bind(args_ready);

    arch::Gp closure = cc.new_gp64("ir_cv_closure");
    arch::load(cc, closure, arch::ptr(endp, static_cast<int>((-static_cast<int64_t>((argc + 1)) * ValSize))));

    arch::Gp nf = cc.new_gp64("ir_cv_nf");
    cc.mov(nf, locals);
    arch::shl(cc, nf, nf, 3);
    arch::add2(cc, nf, endp);
    arch::sub_imm(cc, nf, static_cast<int64_t>((argc + 1)) * static_cast<int64_t>(ValSize));

    arch::Gp stack_cap = cc.new_gp64("ir_cv_stack_cap");
    arch::load(cc, stack_cap, arch::ptr(vm_reg, static_cast<int>(VMStackCapacityOff)));
    cc.cmp(nf, stack_cap);
    arch::jcc(cc, arch::CC::kUGT, slow);

    for (int i = 0; i < static_cast<int>(argc); i++) {
        int src = (i - static_cast<int>(argc)) * static_cast<int>(ValSize);
        int dst = src - static_cast<int>(ValSize);
        arch::Gp tmp = cc.new_gp64("ir_cv_arg");
        arch::load(cc, tmp, arch::ptr(endp, src));
        arch::store(cc, arch::ptr(endp, dst), tmp);
    }

    arch::Gp initp = cc.new_gp64("ir_cv_initp");
    arch::lea(cc, initp, endp, static_cast<int64_t>((-static_cast<int32_t>(ValSize))));
    arch::Gp none = cc.new_gp64("ir_cv_none");
    cc.mov(none, Imm(static_cast<uint64_t>(Value::none().raw_bits())));
    Label init_loop = cc.new_label();
    Label init_done = cc.new_label();
    cc.bind(init_loop);
    cc.cmp(initp, nf);
    arch::jcc(cc, arch::CC::kUGE, init_done);
    arch::store(cc, arch::ptr(initp), none);
    arch::add_imm(cc, initp, static_cast<int>(ValSize));
    arch::jmp(cc, init_loop);
    cc.bind(init_done);
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), nf);

    arch::Gp sbp = cc.new_gp64("ir_cv_sbp");
    arch::lea(cc, sbp, endp, static_cast<int64_t>((-static_cast<int32_t>((static_cast<int64_t>((argc + 1) * ValSize))))));
    arch::Gp sbi = cc.new_gp64("ir_cv_sbi");
    cc.mov(sbi, sbp);
    arch::sub_mem(cc, sbi, arch::ptr(vm_reg, static_cast<int>(VMStackStartOff)));
    arch::shr(cc, sbi, 3);

    arch::Gp ip = cc.new_gp64("ir_cv_ip");
    arch::load(cc, ip, arch::ptr(meta, static_cast<int>(NARI_JIT_OFF(bytecode::FunctionMeta, code.storage_begin))));
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameFunctionOff)), meta);
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameIpOff)), ip);
    arch::store(cc, arch::ptr(ff, static_cast<int>(SlotBaseOff)), sbi);
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameCapturesOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>((FrameCapturesOff + 8))), Imm(0));
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameClosureRootOff)), closure);
    arch::store(cc, arch::ptr(ff, static_cast<int>(FrameReceiverOff)), none);
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameInlineUpvalueIdxOff)), Imm(UINT16_MAX));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameInlineUpvalueOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>((FrameInlineUpvalueOff + 8))), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameOpenUpvalOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>(FrameSingleCaptureCacheOff)), Imm(0));
    arch::store_imm(cc, arch::ptr(ff, static_cast<int>((FrameSingleCaptureCacheOff + 8))), Imm(0));
    arch::add_imm(cc, ff, static_cast<int>(FrameSize));
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)), ff);

    // The trampoline invokes another JITted Nari function which reads and writes the VM stack.
    fc.invalidate();
    InvokeNode *inv;
    (void)vm_arg0(ctx);
    cc.invoke(Out(inv), callee, FuncSignature::build<void, void *>());
    inv->set_arg(0, vm_arg_scratch);
    arch::jmp(cc, done);

    cc.bind(slow);
    call_u32_u32(ctx, reinterpret_cast<const void *>(jit_call_value), argc, callee_label_idx);
    cc.bind(done);
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
