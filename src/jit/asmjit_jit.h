#pragma once

#include <unordered_map>

#ifndef DISABLE_JIT

#include "bytecode/bytecode.h"
#include "ir_opt.h"
#include <asmjit/core.h>

#include <cstddef>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>

namespace nari {
namespace jit {

// Lowering context shared with src/jit/ir_emit_*.cpp (defined in ir_emit.h).
struct IrEmitContext;
struct DirectCallee;
struct RegTierResult;

// abstract base for method JIT compilers
class MethodJITBase {
  public:
    using CompiledFunc = void (*)(nari::bytecode::VM *vm);

    virtual ~MethodJITBase() = default;
    virtual CompiledFunc compile_chunk(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx) = 0;
    virtual bool is_compiled(uint32_t chunk_idx) const = 0;
    virtual CompiledFunc get_compiled(uint32_t chunk_idx) const = 0;
    virtual CompiledFunc get_compiled_fast(uint32_t idx) const = 0;

    // Non-virtual form of get_compiled_fast for the per-call dispatch path.
    CompiledFunc compiled_at(uint32_t idx) const {
        return idx < fn_table_size_ ? fn_table_[idx].load(std::memory_order_acquire) : nullptr;
    }

  protected:
    void set_fn_table(std::atomic<CompiledFunc> *table, size_t size) {
        fn_table_ = table;
        fn_table_size_ = size;
    }

  private:
    std::atomic<CompiledFunc> *fn_table_ = nullptr;
    size_t fn_table_size_ = 0;
};

// AsmJIT-based method JIT compiler
class AsmJITMethodCompiler : public MethodJITBase {
  public:
    AsmJITMethodCompiler() {};
    // out-of-line: stops the background compile worker
    ~AsmJITMethodCompiler() override;

    // Runs on the background compile worker: takes ownership of `holder`'s finished
    // machine code, allocates it executable, and publishes it to compiled_fn_vec.
    void publish_async(uint32_t chunk_idx, asmjit::CodeHolder *holder, const std::string &sym);

    // Establish the function table before the VM looks up an entry. A new
    // chunk invalidates all code and drains outstanding compile jobs.
    void bind_chunk(const nari::bytecode::Chunk &chunk);

    // Wait for background publications before removing debugger registrations.
    void drain_async();

    CompiledFunc compile_chunk(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx) override;
    CompiledFunc compile_with_context(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, nari::bytecode::VM *vm,
                                      bool captures_ok);
    bool is_compiled(uint32_t chunk_idx) const override;
    CompiledFunc get_compiled(uint32_t chunk_idx) const override;
    CompiledFunc get_compiled_fast(uint32_t idx) const override;

  private:
    nari::bytecode::VM *compile_vm_ = nullptr;
    bool compile_captures_ok_ = false;
    // Set by ir_compile when a compile was handed to the worker instead of finished
    // inline, so compile_chunk knows the null return is "pending", not "impossible".
    bool async_pending = false;
    asmjit::JitRuntime rt;

    // The table is fixed for the lifetime of `bound_chunk`. Workers publish
    // with release stores; C++ and generated callers use acquire loads.
    static_assert(sizeof(std::atomic<CompiledFunc>) == sizeof(CompiledFunc), "JIT table slots must be pointer-sized");
    static_assert(std::atomic<CompiledFunc>::is_always_lock_free, "JIT table slots must be lock-free");
    std::unique_ptr<std::atomic<CompiledFunc>[]> compiled_fn_vec;
    size_t compiled_fn_count = 0;
    // Generated code bakes compiled_fn_vec.get() / &compiled_fn_vec[i] as immediates.
    [[maybe_unused]] const void *fn_vec_base = nullptr;
    // TODO: functions containing exception handling (OP_SETUP_TRY) are not
    // compiled by the JIT, they fall back to the bytecode interpreter.
    std::unordered_set<uint32_t> not_compilable;
    // Tracks which chunk owns the currently baked-in machine code
    const nari::bytecode::Chunk *bound_chunk = nullptr;

    // Per-chunk write-once global-name -> proven type,
    // built by ir::analyze_const_globals() in reset_for_chunk().
    ir::GlobalTypeMap global_const_types;
    ir::GlobalIntMap global_const_ints;

    // Per-chunk string-pool index of the "push" method name
    uint32_t push_method_name_idx_ = UINT32_MAX;

    // Per-chunk string-pool index of the "length" method name
    uint32_t length_method_name_idx_ = UINT32_MAX;

    // Per-chunk string-pool index of the "to_string" builtin name
    uint32_t tostring_fuse_name_idx_ = UINT32_MAX;

    uint32_t typeof_name_idx_ = UINT32_MAX;
    uint32_t js_to_number_name_idx_ = UINT32_MAX;

    // Static callee->builtin specialization for OP_CALL inline fast paths.
    // Maps a global-name string index to the FunctionData::jit_native_kind of the
    // builtin it names, so a call site whose callee is `LoadGlobal <that name>`
    // emits only THAT builtin's inline body instead of every inlinable builtin.
    // Purely a hint: the runtime native_kind guard is still emitted, so a rebound
    // global just takes the generic jit_call_value path.
    std::unordered_map<uint32_t, int> builtin_kind_by_name_idx_;

    void reset_for_chunk(const nari::bytecode::Chunk &chunk);

    // Debug tripwire enforcing the pointer-stability invariant relied upon by the raw addresses baked into generated
    // code
    void assert_tables_stable() const;

    // Optimizing-IR tier, builds SSA IR and lowers it to AsmJIT.
    // Returns nullptr if the function is not yet IR-eligible; the VM runs it in the interpreter.
    //
    // when `spec_fallback` is non-null, compile in SPECULATIVE mode:
    // seed all parameter slots as Int48 in type inference and attempt ONLY the register tier
    CompiledFunc ir_compile(
        const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, CompiledFunc spec_fallback = nullptr, bool *spec_candidate = nullptr
    );

  private:
    // ir_compile lowering helpers (defined in src/jit/ir_emit_*.cpp).
    // Each takes the shared IrEmitContext explicitly; see ir_emit.h.
    static void report(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, const char *what);
    DirectCallee resolve_direct_callee(IrEmitContext &ctx, const ir::Inst &in, uint32_t argc);
    int callee_kind_hint(IrEmitContext &ctx, const ir::Inst &in);
    void call0(IrEmitContext &ctx, const void *helper);
    void emit_call_typeof(IrEmitContext &ctx);
    void poll_shutdown(IrEmitContext &ctx);
    void call_u32(IrEmitContext &ctx, const void *helper, uint32_t a);
    void call_u32_u32(IrEmitContext &ctx, const void *helper, uint32_t a, uint32_t b);
    void call_i64(IrEmitContext &ctx, const void *helper, int64_t a);
    void call_u32_u64(IrEmitContext &ctx, const void *helper, uint32_t a, uint64_t b);
    bool both_float(IrEmitContext &ctx, const ir::Inst &in);
    bool lhs_int(IrEmitContext &ctx, const ir::Inst &in);
    bool rhs_int(IrEmitContext &ctx, const ir::Inst &in);
    RegTierResult compile_reg_tier(IrEmitContext &ctx);
    void
    emit_ir_call_value(IrEmitContext &ctx, uint32_t argc, uint32_t callee_label_idx, int kind_hint = -1, const DirectCallee *dc = nullptr);
    void emit_direct_call_guarded(IrEmitContext &ctx, uint32_t argc, uint32_t callee_label_idx, const DirectCallee &d);
    void emit_float_binop(IrEmitContext &ctx, ir::Op op);
    void emit_ir_load_slot(IrEmitContext &ctx, uint32_t slot);
    void emit_ir_store_slot(IrEmitContext &ctx, uint32_t slot);
    void emit_ir_store_raw_to_slot(IrEmitContext &ctx, uint32_t slot, uint64_t raw);
    void emit_ir_store_imm_slot(IrEmitContext &ctx, int64_t imm, uint32_t slot);
    void emit_ir_store_bimm_slot(IrEmitContext &ctx, int64_t imm, uint32_t slot);
    void emit_ir_store_n_slot(IrEmitContext &ctx, uint32_t slot);
    void emit_ir_store_c_slot(IrEmitContext &ctx, uint32_t const_idx, uint32_t slot);
    void emit_ir_copy_slot(IrEmitContext &ctx, uint32_t src, uint32_t dst);
    void emit_ir_pop(IrEmitContext &ctx);
    void emit_ir_dup(IrEmitContext &ctx);
    void emit_ir_load_cached_capture(IrEmitContext &ctx, int64_t capture_offset, const char *name);
    void emit_ir_store_cached_capture(IrEmitContext &ctx, int64_t capture_offset, const char *name);
    void emit_ir_load_global(IrEmitContext &ctx, uint32_t name_idx);
    void emit_ir_js_get_prop_static(IrEmitContext &ctx, uint32_t name_idx);
    void emit_ir_raw_const(IrEmitContext &ctx, uint64_t raw, const char *name);
    void emit_ir_fconst(IrEmitContext &ctx, double imm);
    void emit_ir_iconst(IrEmitContext &ctx, int64_t imm);
    bool emit_ir_sconst_inline(IrEmitContext &ctx, uint32_t const_idx);
    void emit_ir_call_method(IrEmitContext &ctx, const ir::Inst &in);
    void emit_ir_load_index(IrEmitContext &ctx, bool skip_arr_guard, bool skip_idx_guard);
    void emit_ir_store_index(IrEmitContext &ctx, bool skip_arr_guard, bool skip_idx_guard);
    void emit_int_binop(IrEmitContext &ctx, ir::Op op, bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false);
    void emit_int_mul(IrEmitContext &ctx, bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false);
    void emit_int_mod(IrEmitContext &ctx, bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false);
    void emit_int_cmp(IrEmitContext &ctx, ir::Op op, bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false);
    void emit_float_cmp(IrEmitContext &ctx, ir::Op op);
    void lower_body(IrEmitContext &ctx, const ir::Inst &in);
    void lower_term(IrEmitContext &ctx, const ir::Inst &t);
    bool is_fcmp_branch_fusable(IrEmitContext &ctx, const ir::Inst &cmp);
    void lower_fused_float_cmp_branch(IrEmitContext &ctx, const ir::Inst &cmp, const ir::Inst &term);
    void lower_fused_int_cmp_branch(IrEmitContext &ctx, const ir::Inst &cmp, const ir::Inst &term);
};

} // namespace jit
} // namespace nari

// Global JIT instance
namespace nari {
namespace jit {

#ifndef NARI_DISABLE_GDB_JIT
void register_gdb_jit_function(const void *owner, const std::string &name, const void *code_addr, size_t code_size);
void unregister_gdb_jit_functions(const void *owner);
#else
inline void register_gdb_jit_function(const void *, const std::string &, const void *, size_t) {
}
inline void unregister_gdb_jit_functions(const void *) {
}
#endif

// Linux perf jitdump writer (perf_jitdump.cpp)
void perf_jitdump_register(const std::string &name, const void *code_addr, size_t code_size);
void perf_jitdump_close();

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
