#ifndef DISABLE_JIT
#include "asmjit_jit.h"
#include "asmjit_dump.h"
#include "asmjit_jit_async.h"
#include "core_types.h"
#include "ir.h"
#include "ir_build.h"
#include "ir_emit.h"
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

using namespace asmjit;
using namespace nari::bytecode;

// Fast sin/cos defined in trace/lower.cpp.
extern double nari_fast_sin(double) noexcept;
extern double nari_fast_cos(double) noexcept;

// writes a one-time legend mapping struct offsets to field names so a reader
// can decode memory operands like `[reg+160]` in the dump.
namespace nari {
namespace jit {}
} // namespace nari

namespace nari {
namespace jit {

// Value / VM layout constants
// NaN-boxing: tag is upper 2 bytes at offset 6 within the 8-byte Value.
// Raw NaN-box tag values (upper 16 bits of uint64_t _raw):
// offset of NaN-box tag word

// floats: upper16 < tagHeap

// kTagTrivialMax: values with upper16 < 0xFFFB are NOT heap (trivial)
// detect heap by: upper16 == tagHeap (0xFFFB)
// any heap-allocated type
// heap type_tag byte for Array
// heap type_tag byte for String
// heap type_tag byte for Function
// HeapTypeTagOff is computed via field_offset below

// NaN-box encoding constants (for boxing results)

// FunctionData and ObjectObj field offsets (for generic CALL inline path)
// `field_offset` is the safe alternative to `offsetof` for non-standard-layout types

// JIT-visible containers expose an explicit {begin, end, capacity} layout.

// vm->jit_captures_raw points at a std::vector<CellRef>. Indexing it
// from generated code needs two layout facts that the standard does not guarantee:
// the vector's first two words are begin/end, and a shared_ptr's first word is the
// stored pointer. Everything else the JIT touches uses the project's own standard-
// layout `Array`, so rather than assume silently, verify both once at startup and
// fall back to calling jit_load_capture when the check fails.

// VM::stack storage pointers.

// VM::frames storage pointers

// CallFrame field offsets

// Emits the jit_return(vm) equivalent. Must be immediately followed by cc.ret().
// This function assumes the return value has already been pushed on the VM stack.
// Deliberately uses less registers to avoid register pressure at return sites.

bool AsmJITMethodCompiler::is_compiled(uint32_t chunk_idx) const {
    return chunk_idx < this->compiled_fn_count && compiled_fn_vec[chunk_idx].load(std::memory_order_acquire) != nullptr;
}

using CompiledFunc = AsmJITMethodCompiler::CompiledFunc;

CompiledFunc AsmJITMethodCompiler::get_compiled(uint32_t chunk_idx) const {
    return chunk_idx < this->compiled_fn_count ? compiled_fn_vec[chunk_idx].load(std::memory_order_acquire) : nullptr;
}
CompiledFunc AsmJITMethodCompiler::get_compiled_fast(uint32_t idx) const {
    return idx < this->compiled_fn_count ? compiled_fn_vec[idx].load(std::memory_order_acquire) : nullptr;
}

// reset all per-chunk state when the bound chunk changes.
// when a new chunk arrives, free all generated code via the JitRuntime
// drop all per-chunk tables, and resize fresh for the incoming chunk.
void AsmJITMethodCompiler::reset_for_chunk(const nari::bytecode::Chunk &chunk) {
    // In-flight jobs are about to have their JitRuntime destroyed and their target
    // slot in compiled_fn_vec cleared, so they must all land first.
    drain_async();
    unregister_gdb_jit_functions(this);
    destroy_at(&rt);
    construct_at(&rt);

    compiled_fn_vec.reset();
    not_compilable.clear();

    this->compiled_fn_count = chunk.functions.size();
    compiled_fn_vec = std::make_unique<std::atomic<CompiledFunc>[]>(this->compiled_fn_count);
    for (size_t i = 0; i < this->compiled_fn_count; ++i) {
        compiled_fn_vec[i].store(nullptr, std::memory_order_relaxed);
    }

    fn_vec_base = compiled_fn_vec.get();
    // Publish the (now pointer-stable) table for the non-virtual call-path lookup.
    set_fn_table(compiled_fn_vec.get(), this->compiled_fn_count);

    this->bound_chunk = &chunk;

    // One-shot bytecode scan: find write-once globals initialized to a literal
    // and remember the value's static type. Cheap (linear in total bytecode
    // size, once per chunk) and unlocks cascade-typing in infer_types() for
    // expressions that depend on module-scope constants.
    this->global_const_types = ir::analyze_const_globals(chunk);
    this->global_const_ints = ir::analyze_const_int_globals(chunk, this->global_const_types);

    // Cache the string-pool index of "push" and "length" once per chunk. Used
    // by analyze_int_array_slots() to whitelist `arr.push(<int>)` as a mutation
    // that keeps a slot int-array-pure, and `arr.length()` as a pure read that
    // does NOT escape the array (returns an int count, stores nothing).
    push_method_name_idx_ = UINT32_MAX;
    length_method_name_idx_ = UINT32_MAX;
    for (uint32_t i = 0; i < chunk.strings.size(); i++) {
        if (chunk.strings[i] == "push") {
            push_method_name_idx_ = i;
        } else if (chunk.strings[i] == "length") {
            length_method_name_idx_ = i;
        }
    }

    // One-shot: is the "to_string" builtin unshadowable in this chunk? If so,
    // fuse_tostring_concat() may rewrite `s @ to_string(x)` -> `s @ x`. UINT32_MAX
    // (the common no-op case) when to_string is absent, user-defined, or ever
    // rebound via OP_STORE_GLOBAL.
    tostring_fuse_name_idx_ = ir::analyze_frozen_builtin(chunk, "to_string");
    typeof_name_idx_ = ir::analyze_frozen_builtin(chunk, "typeof");
    js_to_number_name_idx_ = ir::analyze_frozen_builtin(chunk, "__js_to_number");

    // Resolve the transpiler-runtime builtins that emit_ir_call_value can inline,
    // so each call site only emits the body for the builtin it actually names.
    // Same name -> jit_native_kind table as Chunk::mark_native_kinds.
    builtin_kind_by_name_idx_.clear();
    static const struct {
        const char *name;
        int kind;
    } kInlinableBuiltins[] = {
        { "__js_lt", 2 },
        { "__js_gt", 3 },
        { "__js_le", 4 },
        { "__js_ge", 5 },
        { "__js_get_prop", 6 },
        { "__js_length", 9 },
        { "__js_str_char_code_at", 10 },
        { "__js_loose_eq", 12 },
        { "__js_to_string", 14 },
        { "__js_str_code_point_at", 15 },
        { "__js_add", 16 },
    };
    for (const auto &b : kInlinableBuiltins) {
        for (uint32_t i = 0; i < chunk.strings.size(); i++) {
            if (chunk.strings[i] == b.name) {
                builtin_kind_by_name_idx_[i] = b.kind;
                break;
            }
        }
    }
}

// A Call whose callee resolves at compile time to one specific closure.

// verify the vectors whose storage addresses get baked as immediates into generated code have not reallocated
void AsmJITMethodCompiler::assert_tables_stable() const {
    assert(compiled_fn_vec.get() == fn_vec_base && "compiled_fn_vec reallocated: baked code pointers now dangle");
}

void AsmJITMethodCompiler::bind_chunk(const nari::bytecode::Chunk &chunk) {
    if (this->bound_chunk != &chunk || this->compiled_fn_count != chunk.functions.size()) {
        reset_for_chunk(chunk);
    }
}

AsmJITMethodCompiler::CompiledFunc AsmJITMethodCompiler::compile_with_context(
    const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, nari::bytecode::VM *vm, bool captures_ok
) {
    auto *previous_vm = compile_vm_;
    const bool previous_captures_ok = compile_captures_ok_;
    compile_vm_ = vm;
    compile_captures_ok_ = captures_ok;
    auto result = compile_chunk(chunk, chunk_idx);
    compile_vm_ = previous_vm;
    compile_captures_ok_ = previous_captures_ok;
    return result;
}

AsmJITMethodCompiler::CompiledFunc AsmJITMethodCompiler::compile_chunk(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx) {
    // A live eval can append functions while older generated frames are still
    // executing. Keep their fixed entry table and executable code alive until
    // the next VM run; newly appended functions interpret in the meantime.
    if (this->bound_chunk != &chunk) {
        reset_for_chunk(chunk);
    }
    // chunk_idx must be in range; reset_for_chunk sized the vector to chunk.functions.size().
    if (chunk_idx >= this->compiled_fn_count) {
        return nullptr; // out-of-range function index for the bound chunk
    }
    if (CompiledFunc compiled = compiled_fn_vec[chunk_idx].load(std::memory_order_acquire)) {
        return compiled;
    }
    // already determined this function cannot be JIT-compiled.
    if (not_compilable.count(chunk_idx)) {
        return nullptr;
    }

    bool spec_candidate = true;
    this->async_pending = false;
    auto fn = ir_compile(chunk, chunk_idx, nullptr, &spec_candidate);
    if (this->async_pending) {
        // The worker publishes into compiled_fn_vec when it lands. Recording this
        // chunk as not_compilable would disable it permanently.
        return nullptr;
    }
    // for parameterized functions, attempt a speculative register-tier
    // recompile with params typed Int48 (entry tag guards, a failed
    // guard dispatches to the general-path version just compiled)
    if (fn && spec_candidate && getenv("NARI_DISABLE_SPEC_JIT") == nullptr && chunk.functions[chunk_idx].param_count > 0 &&
        chunk.functions[chunk_idx].param_count <= 8) {
        if (CompiledFunc spec = ir_compile(chunk, chunk_idx, fn)) {
            fn = spec;
        }
    }
    if (fn) {
        compiled_fn_vec[chunk_idx].store(fn, std::memory_order_release);
    } else {
        not_compilable.insert(chunk_idx);
    }
    return fn;
}

// Optimizing-IR tier
//
// Build SSA IR, then lower it to AsmJIT
AsmJITMethodCompiler::CompiledFunc
AsmJITMethodCompiler::ir_compile(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, CompiledFunc spec_fallback, bool *spec_candidate) {
    const bool spec = spec_fallback != nullptr;
    static const bool kJitReport = getenv("NARI_JIT_REPORT") != nullptr;
    ir::Func irFuncs;
    if (!ir::build(chunk, chunk_idx, irFuncs)) {
        report(chunk, chunk_idx, "BAIL build (non-P1 op or stack-spans-block)");
        return nullptr;
    }
    ir::optimize(irFuncs);
    ir::mark_bounded_counter_adds(irFuncs, this->global_const_ints, length_method_name_idx_);

    // ---- inlining feasibility census (NARI_INLINE_CENSUS=1, diagnostic only) ----
    static const bool kInlineCensus = getenv("NARI_INLINE_CENSUS") != nullptr;
    if (kInlineCensus) {
        struct Census {
            uint64_t sites = 0, cap_callee = 0, resolved = 0, small12 = 0, small30 = 0, self_rec = 0;
            ~Census() {
                fprintf(
                    stderr,
                    "\n[INLINE CENSUS] Call sites in compiled fns: %llu\n"
                    "  callee is LoadCapture:            %llu (%.1f%%)\n"
                    "  resolved to a live FunctionData:  %llu (%.1f%%)\n"
                    "    of which callee code <= 64B:    %llu (%.1f%%)\n"
                    "    of which callee code <= 160B:   %llu (%.1f%%)\n"
                    "    self-recursive (refuse):        %llu\n",
                    static_cast<unsigned long long>(sites), static_cast<unsigned long long>(cap_callee),
                    sites ? 100.0 * static_cast<double>(cap_callee) / static_cast<double>(sites) : 0.0,
                    static_cast<unsigned long long>(resolved),
                    sites ? 100.0 * static_cast<double>(resolved) / static_cast<double>(sites) : 0.0,
                    static_cast<unsigned long long>(small12),
                    sites ? 100.0 * static_cast<double>(small12) / static_cast<double>(sites) : 0.0,
                    static_cast<unsigned long long>(small30),
                    sites ? 100.0 * static_cast<double>(small30) / static_cast<double>(sites) : 0.0,
                    static_cast<unsigned long long>(self_rec)
                );
            }
        };
        static Census census;
        for (const auto &in : irFuncs.insts) {
            if (in.op != ir::Op::Call || in.operands.empty()) {
                continue;
            }
            census.sites++;
            const ir::Inst &callee_in = irFuncs.insts[in.operands[0]];
            if (callee_in.op != ir::Op::LoadCapture) {
                continue;
            }
            census.cap_callee++;
            auto *caps = compile_vm_ ? compile_vm_->jit_captures_raw : nullptr;
            if (!caps || callee_in.imm_u32 >= caps->size()) {
                continue;
            }
            const Value &cv = *(*caps)[callee_in.imm_u32];
            if (!cv.is_function()) {
                continue;
            }
            const int32_t fidx = cv.get_function().jit_func_idx;
            if (fidx < 0 || static_cast<size_t>(fidx) >= chunk.functions.size()) {
                continue;
            }
            census.resolved++;
            if (static_cast<uint32_t>(fidx) == chunk_idx) {
                census.self_rec++;
                continue;
            }
            const size_t bytes = chunk.functions[static_cast<uint32_t>(fidx)].code.size();
            if (bytes <= 64) {
                census.small12++;
            }
            if (bytes <= 160) {
                census.small30++;
            }
        }
    }
    // Resolve a Call site to a single pinned closure, or return {ok=false}.
    // Reads compile_vm_->jit_captures_raw, i.e. the *compiling* caller's
    // captures, so this is a speculation -- the emitted guard is what makes it
    // sound, and a failed guard falls back to jit_call_value.
    // TEMP: A/B the inline strict-eq tag-dispatch tail
    std::vector<ir::Ty> slot_types;
    // Initial type inference (no int-array info yet), so MakeArray
    // initializers and CallMethod push args are typed before
    // analyze_int_array_slots() inspects them.
    ir::infer_types(irFuncs, slot_types, &this->global_const_types, nullptr, spec);
    // identify slots that statically hold an int-only array.
    // Requires value types from the previous infer_types() to check that init values
    // and push args are Int48.
    ir::IntArraySlots int_arr_slots = ir::int_array_candidates(irFuncs);
    ir::infer_types(irFuncs, slot_types, &this->global_const_types, &int_arr_slots, spec);
    for (int it = 0; it < 8; it++) {
        ir::IntArraySlots next = ir::analyze_int_array_slots(irFuncs, push_method_name_idx_, length_method_name_idx_);
        if (next.size() == int_arr_slots.size()) {
            int_arr_slots = std::move(next);
            break;
        }
        int_arr_slots = std::move(next);
        ir::infer_types(irFuncs, slot_types, &this->global_const_types, &int_arr_slots, spec);
    }
    ir::mark_fixed_array_bounded_reads(irFuncs, int_arr_slots, length_method_name_idx_);
    ir::IntArraySlots filled_array_slots = ir::mark_filled_array_bounded_reads(irFuncs, push_method_name_idx_);
    ir::infer_types(irFuncs, slot_types, &this->global_const_types, &int_arr_slots, spec);
    for (int it = 0; it < 8; it++) {
        ir::IntArraySlots next = ir::analyze_int_array_slots(irFuncs, push_method_name_idx_, length_method_name_idx_);
        if (next == int_arr_slots) {
            break;
        }
        int_arr_slots = std::move(next);
        ir::infer_types(irFuncs, slot_types, &this->global_const_types, &int_arr_slots, spec);
    }
    ir::mark_bounded_binary_accumulators(irFuncs, filled_array_slots, push_method_name_idx_);
    ir::infer_types(irFuncs, slot_types, &this->global_const_types, &int_arr_slots, spec);
    ir::specialize_types(irFuncs);
    if (!ir::verify_checked_arithmetic(irFuncs)) {
        report(chunk, chunk_idx, "checked arithmetic IR invalid");
        return nullptr;
    }
    ir::fold_redundant_not(irFuncs);
    ir::fold_branch_not(irFuncs);
    // fuse `s @ to_string(x)` -> `s @ x`, eliding the throwaway StringObj that builtin_toString allocates
    ir::fuse_tostring_concat(irFuncs, tostring_fuse_name_idx_);
    // Mark single-use left-associative StrConcat chains for in-place append
    // lowering (avoids O(n^2) prefix copies + per-link StringObj allocs). Runs
    // last, on the final IR, so global use-counts are stable.
    ir::mark_inplace_concat(irFuncs);
    if (spec_candidate) {
        // These operations are unaffected by parameter type specialization and
        // are not supported by the register tier. Avoid rebuilding and
        // optimizing the same IR only to reject it during lowering.
        for (const ir::Inst &in : irFuncs.insts) {
            switch (in.op) {
                case ir::Op::LoadConst:
                case ir::Op::StoreGlobal:
                case ir::Op::StoreCSlot:
                case ir::Op::MakeObject:
                case ir::Op::MakeClosure:
                case ir::Op::StrConcat:
                case ir::Op::FormatValue:
                case ir::Op::IterArray:
                case ir::Op::LoadProperty:
                case ir::Op::StoreProperty:
                case ir::Op::JsSetPropStatic:
                case ir::Op::SelfTailCall:
                // Closure upvalue access, static property reads, JS bitwise ops
                // and Pow have no register-tier lowering either, and the
                // transpiled compiler emits them almost everywhere.
                case ir::Op::LoadCapture:
                case ir::Op::StoreCapture:
                case ir::Op::JsGetPropStatic:
                case ir::Op::JsPostinc:
                case ir::Op::JsBitBinary:
                case ir::Op::Pow:
                    *spec_candidate = false;
                    break;
                default:
                    break;
            }
            if (!*spec_candidate) {
                break;
            }
        }
    }
    if (getenv("NARI_IR_DUMP")) {
        fprintf(stderr, "=== IR for '%s' (func %u) ===\n%s", chunk.functions[chunk_idx].name.c_str(), chunk_idx, ir::dump(irFuncs).c_str());
    }
    // quantify cross-block redundant expressions that a future global-CSE/GVN pass could eliminate.
    // pure analysis: does not touch irFuncs or codegen. Runs on the final typed IR.
    static const bool kGvnReport = getenv("NARI_IR_GVN_REPORT") != nullptr;
    if (kGvnReport) {
        ir::gvn_report(irFuncs, chunk.functions[chunk_idx].name.c_str(), chunk_idx);
    }

    // Heap-allocated so ownership can transfer to the background compile worker.
    // The references below keep every existing `code_holder` / `cc` use unchanged.
    auto code_holder_owned = std::make_unique<CodeHolder>();
    CodeHolder &code_holder = *code_holder_owned;
    code_holder.init(this->rt.environment(), this->rt.cpu_features());
    StringLogger asm_logger;
    if (jit_dump_asm_enabled()) {
        asm_logger.add_flags(FormatFlags::kMachineCode);
        code_holder.set_logger(&asm_logger);
    }
    auto cc_owned = std::make_unique<arch::Compiler>(&code_holder);
    arch::Compiler &cc = *cc_owned;
    if (getenv("NARI_JIT_VALIDATE") != nullptr) {
        cc.add_diagnostic_options(asmjit::DiagnosticOptions::kValidateAssembler | asmjit::DiagnosticOptions::kValidateIntermediate);
    }
    FuncNode *func_node = cc.add_func(FuncSignature::build<void, void *>());
    // Density of operations that lower to a helper invoke. 
    // Pinning `vm` to a callee-saved register below pays off once per call but costs one register
    // from the allocation pool for the whole function, so it is only worth it
    // where calls are frequent compared to the amount of code competing for registers.
    unsigned helper_call_ops = 0;
    for (const auto &in : irFuncs.insts) {
        switch (in.op) {
            case ir::Op::Call:
            case ir::Op::CallMethod:
            case ir::Op::CallSpread:
            case ir::Op::LoadProperty:
            case ir::Op::StoreProperty:
            case ir::Op::JsSetPropStatic:
            case ir::Op::JsGetPropStatic:
            case ir::Op::JsPostinc:
            case ir::Op::MakeClosure:
            case ir::Op::StrConcat:
            case ir::Op::MakeObject:
                helper_call_ops++;
                break;
            default:
                break;
        }
    }
    const bool pin_vm_callee_saved = helper_call_ops * 20 >= irFuncs.insts.size();
    arch::Gp vm_reg = cc.new_gp64("vm");
    arch::Gp vm_arg_scratch = vm_reg;
    if (pin_vm_callee_saved) {
        arch::Gp vm_arg = cc.new_gp64("vm_arg");
        func_node->set_arg(0, vm_arg);
        arch::mov_reg(cc, vm_reg, vm_arg);
        if (asmjit::VirtReg *vm_vreg = cc.virt_reg_by_reg(vm_reg)) {
#if NARI_JIT_ARM64
            vm_vreg->set_home_id_hint(19); // x19: first callee-saved GPR in AAPCS64
#else
            vm_vreg->set_home_id_hint(asmjit::x86::Gp::kIdBx);
#endif
            vm_vreg->set_weight(255);
        }
        vm_arg_scratch = cc.new_gp64("vm_arg0");
    } else {
        // Not worth a register: bind the argument directly, exactly as before, so
        // functions below the density threshold get byte-identical code.
        func_node->set_arg(0, vm_reg);
    }

    // block-scoped load-only cache for the VM stack's _M_finish field
    FinishCache fc(cc, vm_reg);

    // Two shared scratch registers for the emitters below
    arch::Gp const_scratch = cc.new_gp64("ir_const_scratch");
    arch::Gp cmp_scratch = cc.new_gp64("ir_cmp_scratch");

    Label shutdown_requested = cc.new_label();
    std::vector<asmjit::Label> blabels(irFuncs.blocks.size());
    bool ok = true;
    ir::Op unhandled_op = ir::Op::Return; // captured for NARI_JIT_REPORT
    ir::BlockId lowering_block = ir::InvalidBlock;
    SlotRegCache slot_cache(cc, irFuncs.num_slots);

    IrEmitContext ctx{
        chunk,
        chunk_idx,
        spec,
        spec_candidate,
        spec_fallback,
        irFuncs,
        slot_types,
        int_arr_slots,
        code_holder,
        asm_logger,
        cc,
        func_node,
        pin_vm_callee_saved,
        vm_reg,
        vm_arg_scratch,
        shutdown_requested,
        blabels,
        ok,
        unhandled_op,
        lowering_block,
        fc,
        slot_cache,
        const_scratch,
        cmp_scratch,
    };

    // register lowering
    {
        RegTierResult reg = compile_reg_tier(ctx);
        if (reg.outcome == RegTierOutcome::Failed) {
            return nullptr;
        }
        if (reg.outcome == RegTierOutcome::Ok) {
            return reg.entry;
        }
    }

    // speculative mode compiles only the register tier, the general-path version already exists (it's `spec_fallback`).
    if (spec) {
        report(chunk, chunk_idx, "spec param tier ineligible");
        return nullptr;
    }

    // one AsmJIT label per IR block (so jumps/branches can target any block)
    for (size_t i = 0; i < irFuncs.blocks.size(); i++) {
        blabels[i] = cc.new_label();
    }

    {
        const uint32_t stack_bound = static_cast<uint32_t>(irFuncs.insts.size()) + 8;
        Label rz_ok = cc.new_label();
        arch::Gp rz_need = cc.new_gp64("rz_need");
        arch::Gp rz_cap = cc.new_gp64("rz_cap");
        arch::load(cc, rz_need, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
        arch::load(cc, rz_cap, arch::ptr(vm_reg, static_cast<int>(VMStackCapacityOff)));
        arch::add_imm(cc, rz_need, static_cast<int64_t>(stack_bound) * static_cast<int64_t>(ValSize));
        cc.cmp(rz_need, rz_cap);
        arch::jcc(cc, arch::CC::kULT, rz_ok); // finish + bound*8 < capacity -> enough headroom
        call_u32(ctx, reinterpret_cast<const void *>(jit_reserve), stack_bound);
        cc.bind(rz_ok);
    }
    // Slot-address CSE: IR functions can't have upvalues, so slot_base is
    // fixed at frame init; hoist `slot_base*8` to entry (see emit_ir_slot_addr).
    arch::Gp slot_base_bytes_vreg = cc.new_gp64("slot_base_bytes");
    {
        arch::Gp frame_init = cc.new_gp64("frame_init");
        arch::load(cc, frame_init, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)));
        arch::sub_imm(cc, frame_init, static_cast<int>(FrameSize));
        arch::load(cc, slot_base_bytes_vreg, arch::ptr(frame_init, static_cast<int>(SlotBaseOff)));
        arch::shl(cc, slot_base_bytes_vreg, slot_base_bytes_vreg, 3);
    }
    fc.bind_slot_base_bytes(slot_base_bytes_vreg);

    // lower a straight-line (non-terminator) instruction.
    // lower a block terminator (Jump/Branch/Return).
    // fusable only when both operands are statically Float and the op is an ordered relational (Lt/Le/Gt/Ge)
    // emit blocks in id order
    std::vector<bool> reachable(irFuncs.blocks.size(), false);
    if (irFuncs.entry >= 0 && static_cast<size_t>(irFuncs.entry) < irFuncs.blocks.size()) {
        std::vector<ir::BlockId> stk;
        stk.push_back(irFuncs.entry);
        reachable[irFuncs.entry] = true;
        while (!stk.empty()) {
            ir::BlockId bid = stk.back();
            stk.pop_back();
            for (ir::BlockId s : irFuncs.blocks[bid].succs) {
                if (s >= 0 && static_cast<size_t>(s) < reachable.size() && !reachable[s]) {
                    reachable[s] = true;
                    stk.push_back(s);
                }
            }
        }
    }
    for (size_t bid = 0; bid < irFuncs.blocks.size() && ok; bid++) {
        if (!reachable[bid]) {
            continue; // unreachable after optimization -> don't lower
        }
        const ir::Block &b = irFuncs.blocks[bid];
        lowering_block = static_cast<ir::BlockId>(bid);
        cc.bind(blabels[bid]);
        // every predecessor flushed before its terminator
        fc.invalidate();
        // the cached vreg reflects whatever the pred wrote; on a back-edge or divergent
        // control-flow join, the cached vreg is stale.
        slot_cache.invalidate_all();
        bool fuse_cmp_branch = false;
        bool fuse_fcmp_branch = false;
        if (b.terminator != ir::InvalidValue && !b.insts.empty()) {
            const ir::Inst &term = irFuncs.inst(b.terminator);
            const ir::Inst &last = irFuncs.inst(b.insts.back());
            bool branch_to_last = term.op == ir::Op::Branch && !term.operands.empty() && term.operands[0] == b.insts.back();
            // fuse compare-and-branch for both statically-typed int compares and dynamically-typed compares
            fuse_cmp_branch = branch_to_last && is_cmp_op(last.op);
            // static-float ordered cmp + branch -> ucomisd + jcc, skipping bool synthesis and check_truthy
            if (!fuse_cmp_branch && branch_to_last && is_fcmp_branch_fusable(ctx, last)) {
                fuse_fcmp_branch = true;
            }
        }
        bool any_fuse = fuse_cmp_branch || fuse_fcmp_branch;
        size_t body_n = any_fuse ? b.insts.size() - 1 : b.insts.size();
        for (size_t ii = 0; ii < body_n; ii++) {
            ir::ValueId i = b.insts[ii];
            lower_body(ctx, irFuncs.inst(i));
            if (!ok) {
                break;
            }
        }
        if (ok && b.terminator != ir::InvalidValue) {
            if (fuse_cmp_branch) {
                lower_fused_int_cmp_branch(ctx, irFuncs.inst(b.insts.back()), irFuncs.inst(b.terminator));
            } else if (fuse_fcmp_branch) {
                lower_fused_float_cmp_branch(ctx, irFuncs.inst(b.insts.back()), irFuncs.inst(b.terminator));
            } else {
                lower_term(ctx, irFuncs.inst(b.terminator));
            }
        }
    }
    if (!ok) {
        if (kJitReport) {
            fprintf(
                stderr, "[JIT] %-30s BAIL general (unhandled op %s)\n",
                chunk.functions[chunk_idx].name.empty() ? "<anon>" : chunk.functions[chunk_idx].name.c_str(), ir::op_name(unhandled_op)
            );
        }
        return nullptr; // unhandled op; VM runs this function in the interpreter
    }

    cc.bind(shutdown_requested);
    {
        InvokeNode *invoke;
        (void)vm_arg0(ctx);
        arch::invoke_imm(
            cc, &invoke, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_poll_shutdown)), FuncSignature::build<void, void *>()
        );
        invoke->set_arg(0, vm_arg_scratch);
    }
    cc.ret();
    cc.end_func();
    if (getenv("NARI_JIT_DUMP_NODES") != nullptr) {
        asmjit::String nodes_sb;
        asmjit::FormatOptions fo;
        asmjit::Formatter::format_node_list(nodes_sb, fo, &cc);
        fprintf(stderr, "==== pre-RA nodes for '%s' ====\n%s\n", chunk.functions[chunk_idx].name.c_str(), nodes_sb.data());
    }
    // Hand RA+encode to the worker unless something downstream needs the pointer
    // immediately. Spec recompiles are excluded both ways: compile_chunk passes this
    // general-tier pointer in as the spec tier's guard-failure fallback target.
    const bool will_spec = spec_candidate != nullptr && *spec_candidate && getenv("NARI_DISABLE_SPEC_JIT") == nullptr &&
                           chunk.functions[chunk_idx].param_count > 0 && chunk.functions[chunk_idx].param_count <= 8;
    
    if (async_jit_enabled() && spec_fallback == nullptr && !will_spec) {
        auto job = std::make_unique<CompileJob>();
        job->chunk_idx = chunk_idx;
        job->sym = chunk.functions[chunk_idx].name.empty() ? std::string("anon_ir") : chunk.functions[chunk_idx].name + "_ir";
        job->owner = this;
        job->holder = std::move(code_holder_owned);
        job->cc = std::move(cc_owned);
        // `cc` and `code_holder` dangle past this point; nothing below touches them.
        enqueue_compile_job(std::move(job));
        this->async_pending = true;
        report(chunk, chunk_idx, "OK  [ir-stack general path, async RA]");
        return nullptr;
    }
    Error err = cc.finalize();
    if (err != kErrorOk) {
        if (kJitReport) {
            fprintf(
                stderr, "[JIT] %-30s BAIL asmjit finalize err=%u (%s)\n",
                chunk.functions[chunk_idx].name.empty() ? "<anon>" : chunk.functions[chunk_idx].name.c_str(), static_cast<unsigned>(err),
                asmjit::DebugUtils::error_as_string(err)
            );
            if (jit_dump_asm_enabled()) {
                fprintf(stderr, "---- partial asm ----\n%s---------------------\n", asm_logger.data());
            }
        }
        return nullptr;
    }
    size_t generated_code_size = code_holder.code_size();
    CompiledFunc fn = nullptr;
    {
        std::lock_guard<std::mutex> lk(jit_runtime_mutex());
        err = this->rt.add(&fn, &code_holder);
        if (err != kErrorOk || !fn) {
            return nullptr;
        }
        std::string sym = chunk.functions[chunk_idx].name.empty() ? std::string("anon_ir") : chunk.functions[chunk_idx].name + "_ir";
        register_gdb_jit_function(this, sym, reinterpret_cast<const void *>(fn), generated_code_size);
        perf_jitdump_register(sym, reinterpret_cast<const void *>(fn), generated_code_size);
    }
    jit_dump_asm(chunk.functions[chunk_idx].name.empty() ? "<anon> [ir]" : chunk.functions[chunk_idx].name + " [ir]", asm_logger.data());
    report(chunk, chunk_idx, "OK  [ir-stack general path]");
    return fn;
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
