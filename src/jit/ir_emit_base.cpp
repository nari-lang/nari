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

void AsmJITMethodCompiler::report(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, const char *what) {
    static const bool kJitReport = getenv("NARI_JIT_REPORT") != nullptr;
    if (kJitReport) {
        fprintf(
            stderr, "[JIT] %-30s %s\n", chunk.functions[chunk_idx].name.empty() ? "<anon>" : chunk.functions[chunk_idx].name.c_str(), what
        );
    }
}

DirectCallee AsmJITMethodCompiler::resolve_direct_callee(IrEmitContext &ctx, const ir::Inst &in, uint32_t argc) {
    static const bool kNoDirectCall = getenv("NARI_DISABLE_DIRECT_CALL") != nullptr;
    static DcsCensus dcs;
    static const bool kDcsCensus = getenv("NARI_DCS_CENSUS") != nullptr;
    auto &chunk = ctx.chunk;
    auto &irFuncs = ctx.irFuncs;
    DirectCallee d;
    if (kDcsCensus) {
        dcs.sites++;
    }
    if (kNoDirectCall || in.operands.empty() || argc > 4) {
        if (kDcsCensus && !in.operands.empty()) {
            dcs.argc_gt4++;
        }
        return d;
    }
    if (!compile_vm_) {
        return d;
    }
    const ir::Inst &callee_in = irFuncs.inst(in.operands[0]);
    if (kDcsCensus && callee_in.op != ir::Op::LoadCapture && callee_in.op != ir::Op::LoadGlobal) {
        dcs.callee_op[0]++;
    }
    // Either an upvalue read (module-level function referenced through the
    // caller's closure) or a global read. Both are compile-time observations
    // that the emitted identity guard re-validates at run time, so a global
    // being reassigned later simply fails the guard.
    const Value *cvp = nullptr;
    if (callee_in.op == ir::Op::LoadCapture) {
        // Captures are only known to belong to the function being compiled on the
        // note_jit_callee path; elsewhere jit_captures_raw may describe an unrelated
        // closure, which would make this a guaranteed guard miss.
        if (!compile_captures_ok_) {
            if (kDcsCensus) {
                dcs.cap_not_ok++;
            }
            return d;
        }
        auto *caps = compile_vm_->jit_captures_raw;
        if (!caps || callee_in.imm_u32 >= caps->size()) {
            if (kDcsCensus) {
                dcs.cap_oob++;
            }
            return d;
        }
        cvp = (*caps)[callee_in.imm_u32].get();
    } else if (callee_in.op == ir::Op::LoadGlobal) {
        const uint32_t ni = callee_in.imm_u32;
        if (ni >= compile_vm_->global_cache_valid.size() || !compile_vm_->global_cache_valid[ni] ||
            ni >= compile_vm_->global_cache.size()) {
            if (kDcsCensus) {
                dcs.cap_oob++;
            }
            return d;
        }
        cvp = &compile_vm_->global_cache[ni];
    } else {
        return d;
    }
    if (cvp == nullptr || !cvp->is_function()) {
        if (kDcsCensus) {
            dcs.not_fn++;
        }
        return d;
    }
    const Value &cv = *cvp;
    const FunctionData &fd = cv.get_function();
    if (fd.jit_func_idx < 0 || static_cast<size_t>(fd.jit_func_idx) >= chunk.functions.size()) {
        if (kDcsCensus) {
            dcs.no_meta++;
        }
        return d;
    }
    if (fd.jit_native_kind != 0 || fd.jit_meta == nullptr) {
        if (kDcsCensus) {
            dcs.native++;
        }
        return d;
    }
    if (fd.jit_rest_param_index >= 0 || fd.jit_inline_kind == JitInlineKind::Capture0) {
        if (kDcsCensus) {
            dcs.rest_or_cap0++;
        }
        return d;
    }
    // Exact arity only: no missing-argument fill, so param_count needs no check.
    if (static_cast<uint32_t>(fd.jit_param_count) != argc || fd.jit_locals_count < argc) {
        if (kDcsCensus) {
            dcs.arity++;
        }
        return d;
    }
    // The none-fill of non-parameter locals is unrolled, so bound it.
    if (fd.jit_locals_count - argc > 8) {
        if (kDcsCensus) {
            dcs.fill_gt8++;
        }
        return d;
    }
    if (kDcsCensus) {
        dcs.ok++;
    }
    d.ok = true;
    d.expected_raw = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fd.jit_meta));
    d.fidx = static_cast<uint32_t>(fd.jit_func_idx);
    d.locals = fd.jit_locals_count;
    d.meta = static_cast<const void *>(fd.jit_meta);
    d.captures_raw = static_cast<const void *>(fd.jit_captures_raw);
    d.cap0 = static_cast<const void *>(fd.jit_capture0_raw);
    d.cap1 = static_cast<const void *>(fd.jit_capture1_raw);
    d.cap2 = static_cast<const void *>(fd.jit_capture2_raw);
    return d;
}

// Statically-known jit_native_kind of a Call's callee (-1 = unknown), used to
// emit only the matching builtin inline body at the site.
int AsmJITMethodCompiler::callee_kind_hint(IrEmitContext &ctx, const ir::Inst &in) {
    auto &irFuncs = ctx.irFuncs;
    if (in.operands.empty()) {
        return -1;
    }
    const ir::Inst &callee = irFuncs.inst(in.operands[0]);
    if (callee.op != ir::Op::LoadGlobal) {
        return -1;
    }
    auto it = builtin_kind_by_name_idx_.find(callee.imm_u32);
    return it == builtin_kind_by_name_idx_.end() ? -1 : it->second;
}

arch::Gp vm_arg0(IrEmitContext &ctx) {
    auto &cc = ctx.cc;
    auto &pin_vm_callee_saved = ctx.pin_vm_callee_saved;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    auto &vm_reg = ctx.vm_reg;
    if (pin_vm_callee_saved) {
        arch::mov_reg(cc, vm_arg_scratch, vm_reg);
    }
    return vm_arg_scratch;
}

void AsmJITMethodCompiler::call0(IrEmitContext &ctx, const void *helper) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    fc.invalidate();
    InvokeNode *inv;
    (void)vm_arg0(ctx);
    arch::invoke_imm(cc, &inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(helper)), FuncSignature::build<void, void *>());
    inv->set_arg(0, vm_arg_scratch);
}

void AsmJITMethodCompiler::emit_call_typeof(IrEmitContext &ctx) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_reg = ctx.vm_reg;
    fc.invalidate();
    Label is_none = cc.new_label();
    Label is_int = cc.new_label();
    Label is_bool = cc.new_label();
    Label is_float = cc.new_label();
    Label is_heap = cc.new_label();
    Label is_string = cc.new_label();
    Label is_array = cc.new_label();
    Label is_object = cc.new_label();
    Label is_function = cc.new_label();
    Label is_regex = cc.new_label();
    Label is_handle = cc.new_label();
    Label slow = cc.new_label();
    Label store = cc.new_label();
    Label done = cc.new_label();

    arch::Gp endp = cc.new_gp64("typeof_end");
    arch::Gp raw = cc.new_gp64("typeof_raw");
    arch::Gp tag = cc.new_gp64("typeof_tag");
    arch::Gp result = cc.new_gp64("typeof_result");
    arch::load(cc, endp, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
    arch::load(cc, raw, arch::ptr(endp, static_cast<int>(-ValSize)));
    arch::mov_reg(cc, tag, raw);
    arch::shr(cc, tag, 48);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(Value::TAG_NONE)));
    arch::jcc(cc, arch::CC::kEQ, is_none);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(Value::TAG_INT)));
    arch::jcc(cc, arch::CC::kEQ, is_int);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(Value::TAG_BOOL)));
    arch::jcc(cc, arch::CC::kEQ, is_bool);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(Value::TAG_HEAP)));
    arch::jcc(cc, arch::CC::kEQ, is_heap);
    arch::jmp(cc, is_float);

    cc.bind(is_heap);
    arch::zero_extend_48(cc, raw);
    arch::load8_zx(cc, tag, arch::ptr8(raw, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))));
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(ValueTag::String)));
    arch::jcc(cc, arch::CC::kEQ, is_string);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(ValueTag::Array)));
    arch::jcc(cc, arch::CC::kEQ, is_array);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(ValueTag::Object)));
    arch::jcc(cc, arch::CC::kEQ, is_object);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(ValueTag::Function)));
    arch::jcc(cc, arch::CC::kEQ, is_function);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(ValueTag::Regex)));
    arch::jcc(cc, arch::CC::kEQ, is_regex);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(ValueTag::Handle)));
    arch::jcc(cc, arch::CC::kEQ, is_handle);
    arch::cmp_imm(cc, tag.r32(), Imm(static_cast<int>(ValueTag::ClassInstance)));
    arch::jcc(cc, arch::CC::kEQ, slow);
    arch::jmp(cc, is_none);

    auto load_typeof_value = [&](Label label, int index) {
        cc.bind(label);
        arch::load(cc, result, arch::ptr(vm_reg, static_cast<int>(VMRuntimeOff)));
        arch::load(cc, result, arch::ptr(result, static_cast<int>((RuntimeTypeofValuesOff + index * ValSize))));
        arch::jmp(cc, store);
    };
    load_typeof_value(is_none, ScriptRuntime::TY_NULL);
    load_typeof_value(is_int, ScriptRuntime::TY_INT);
    load_typeof_value(is_float, ScriptRuntime::TY_FLOAT);
    load_typeof_value(is_string, ScriptRuntime::TY_STRING);
    load_typeof_value(is_bool, ScriptRuntime::TY_BOOL);
    load_typeof_value(is_array, ScriptRuntime::TY_ARRAY);
    load_typeof_value(is_object, ScriptRuntime::TY_OBJECT);
    load_typeof_value(is_function, ScriptRuntime::TY_FUNCTION);
    load_typeof_value(is_regex, ScriptRuntime::TY_REGEX);
    load_typeof_value(is_handle, ScriptRuntime::TY_HANDLE);

    cc.bind(store);
    arch::store(cc, arch::ptr(endp, static_cast<int>((-2 * ValSize))), result);
    arch::sub_imm(cc, endp, static_cast<int>(ValSize));
    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), endp);
    arch::jmp(cc, done);
    cc.bind(slow);
    call0(ctx, reinterpret_cast<const void *>(jit_call_typeof));
    cc.bind(done);
}

void AsmJITMethodCompiler::poll_shutdown(IrEmitContext &ctx) {
    auto &cc = ctx.cc;
    auto &shutdown_requested = ctx.shutdown_requested;
    Label running = cc.new_label();
    arch::Gp flag_addr = cc.new_gp64("shutdown_addr");
    arch::Gp requested = cc.new_gp64("shutdown_requested");
    cc.mov(flag_addr, Imm(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&Runtime::g_shutdown_requested))));
    arch::load8_zx(cc, requested, arch::ptr8(flag_addr));
    arch::test_zero(cc, requested);
    arch::jcc(cc, arch::CC::kEQ, running);
    arch::jmp(cc, shutdown_requested);
    cc.bind(running);
}

void AsmJITMethodCompiler::call_u32(IrEmitContext &ctx, const void *helper, uint32_t a) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    fc.invalidate();
    InvokeNode *inv;
    (void)vm_arg0(ctx);
    arch::invoke_imm(cc, &inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(helper)), FuncSignature::build<void, void *, uint32_t>());
    inv->set_arg(0, vm_arg_scratch);
    inv->set_arg(1, Imm(a));
}

void AsmJITMethodCompiler::call_u32_u32(IrEmitContext &ctx, const void *helper, uint32_t a, uint32_t b) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    fc.invalidate();
    InvokeNode *inv;
    (void)vm_arg0(ctx);
    arch::invoke_imm(
        cc, &inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(helper)), FuncSignature::build<void, void *, uint32_t, uint32_t>()
    );
    inv->set_arg(0, vm_arg_scratch);
    inv->set_arg(1, Imm(a));
    inv->set_arg(2, Imm(b));
}

void AsmJITMethodCompiler::call_i64(IrEmitContext &ctx, const void *helper, int64_t a) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    fc.invalidate();
    InvokeNode *inv;
    (void)vm_arg0(ctx);
    arch::invoke_imm(cc, &inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(helper)), FuncSignature::build<void, void *, int64_t>());
    inv->set_arg(0, vm_arg_scratch);
    inv->set_arg(1, Imm(a));
}

// jit_slot_store_raw(vm, uint32_t idx, uint64_t raw).
void AsmJITMethodCompiler::call_u32_u64(IrEmitContext &ctx, const void *helper, uint32_t a, uint64_t b) {
    auto &cc = ctx.cc;
    auto &fc = ctx.fc;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    fc.invalidate();
    InvokeNode *inv;
    (void)vm_arg0(ctx);
    arch::invoke_imm(
        cc, &inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(helper)), FuncSignature::build<void, void *, uint32_t, uint64_t>()
    );
    inv->set_arg(0, vm_arg_scratch);
    inv->set_arg(1, Imm(a));
    inv->set_arg(2, Imm(b));
}

bool AsmJITMethodCompiler::both_float(IrEmitContext &ctx, const ir::Inst &in) {
    auto &irFuncs = ctx.irFuncs;
    return irFuncs.inst(in.operands[0]).type == ir::Ty::Float && irFuncs.inst(in.operands[1]).type == ir::Ty::Float;
}

// Per-operand: an Int48-typed operand carries tagInt by construction, so its
// runtime tag guard is dead code even when the other operand is unknown.
bool AsmJITMethodCompiler::lhs_int(IrEmitContext &ctx, const ir::Inst &in) {
    auto &irFuncs = ctx.irFuncs;
    return in.operands.size() >= 2 && irFuncs.inst(in.operands[0]).type == ir::Ty::Int48;
}

bool AsmJITMethodCompiler::rhs_int(IrEmitContext &ctx, const ir::Inst &in) {
    auto &irFuncs = ctx.irFuncs;
    return in.operands.size() >= 2 && irFuncs.inst(in.operands[1]).type == ir::Ty::Int48;
}

bool is_cmp_op(ir::Op op) {
    return op == ir::Op::DynCmpLt || op == ir::Op::DynCmpLe || op == ir::Op::DynCmpGt || op == ir::Op::DynCmpGe || op == ir::Op::DynCmpEq ||
           op == ir::Op::DynCmpNe || op == ir::Op::DynStrictCmpEq || op == ir::Op::DynStrictCmpNe || op == ir::Op::ICmpLt ||
           op == ir::Op::ICmpLe || op == ir::Op::ICmpGt || op == ir::Op::ICmpGe || op == ir::Op::ICmpEq || op == ir::Op::ICmpNe;
}

// Writes a one-time legend mapping struct offsets to field names so a reader
// can decode memory operands like `[reg+160]` in the dump.
void jit_write_layout_legend(FILE *f) {
    static bool written = false;
    if (written) {
        return;
    }
    written = true;
    fprintf(
        f,
        "; ============================================================\n"
        "; NARI JIT asm dump. `call` targets are symbolized to helper names where known.\n"
        "; Memory operands `[base+off]` into the VM pointer (first arg) or a CallFrame use these offsets:\n"
        ";\n"
        ";   VM.stack.begin      +%-4lld    VM.stack.end        +%-4lld\n"
        ";   VM.stack.cap        +%-4lld    VM.jit_captures_raw +%-4lld\n"
        ";   VM.global_cache     +%-4lld    VM.gcache_valid     +%-4lld\n"
        ";   VM.frames.begin     +%-4lld    VM.frames.end       +%-4lld\n"
        ";   VM.frames.cap       +%-4lld\n"
        ";   sizeof(Value)=%lld  sizeof(CallFrame)=%lld\n"
        ";   CallFrame.function  +%-4lld    CallFrame.ip        +%-4lld\n"
        ";   CallFrame.slot_base +%-4lld    CallFrame.captures  +%-4lld\n"
        ";   CallFrame.open_upvalues +%lld\n"
        "; ============================================================\n",
        static_cast<long long>(VMStackStartOff), static_cast<long long>(VMStackFinishOff), static_cast<long long>(VMStackCapacityOff),
        static_cast<long long>(VMCapturesRawOff), static_cast<long long>(VMGlobalCacheStartOff),
        static_cast<long long>(VMGlobalCacheValidStartOff), static_cast<long long>(FramesStartOff), static_cast<long long>(FramesFinishOff),
        static_cast<long long>(VMFramesCapacityOff), static_cast<long long>(ValSize), static_cast<long long>(FrameSize),
        static_cast<long long>(FrameFunctionOff), static_cast<long long>(FrameIpOff), static_cast<long long>(SlotBaseOff),
        static_cast<long long>(FrameCapturesOff), static_cast<long long>(FrameOpenUpvalOff)
    );
}

void jit_dump_asm(const std::string &func_name, const char *asm_text, uint64_t extra_addr, const char *extra_name) {
    const char *env = getenv("NARI_JIT_DUMP_ASM");
    if (!env) {
        return;
    }
    const char *path = (env[0] != '\0') ? env : "output.asm";
    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    nari::jit::jit_write_layout_legend(f);
    fprintf(f, "\n; -- %s --\n", func_name.c_str());
    std::string symbolized = jit_symbolize_calls(asm_text, extra_addr, extra_name);
    fputs(symbolized.c_str(), f);
    fclose(f);
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
