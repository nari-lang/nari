#pragma once

#include <unordered_map>

#ifndef DISABLE_JIT

#include "bytecode/bytecode.h"
#include "ir_opt.h"
#include <asmjit/core.h>

#include <cstddef>
#include <string>
#include <unordered_set>

namespace nari {
namespace jit {

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
    // Every scripted call performs this lookup, so going through the vtable
    // costs an uninlinable indirect call each time. The table is the subclass's
    // compiled-function vector, which must already be pointer-stable because
    // generated code bakes its address as an immediate.
    CompiledFunc compiled_at(uint32_t idx) const {
        return idx < fn_table_size_ ? fn_table_[idx] : nullptr;
    }

  protected:
    void set_fn_table(CompiledFunc *table, size_t size) {
        fn_table_ = table;
        fn_table_size_ = size;
    }

  private:
    CompiledFunc *fn_table_ = nullptr;
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

    CompiledFunc compile_chunk(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx) override;
    bool is_compiled(uint32_t chunk_idx) const override;
    CompiledFunc get_compiled(uint32_t chunk_idx) const override;
    CompiledFunc get_compiled_fast(uint32_t idx) const override;

  private:
    // Blocks until every queued background compile has been published. Must be called
    // before anything frees generated code or resizes compiled_fn_vec.
    void drain_async();
    // Set by ir_compile when a compile was handed to the worker instead of finished
    // inline, so compile_chunk knows the null return is "pending", not "impossible".
    bool async_pending_ = false;

    asmjit::JitRuntime rt;
    // compiled_fn_vec is pointer-stable for the lifetime of `bound_chunk_`
    std::vector<CompiledFunc> compiled_fn_vec;
    // Generated code bakes compiled_fn_vec.data() / &compiled_fn_vec[i] as immediates;
    // those bytes would dangle (use-after-free) if the vector reallocated after a base was baked.
    [[maybe_unused]] const void *fn_vec_base = nullptr;
    // TODO: functions containing exception handling (OP_SETUP_TRY) are not
    // compiled by the JIT, they fall back to the bytecode interpreter.
    std::unordered_set<uint32_t> not_compilable;
    // Tracks which chunk owns the currently baked-in machine code
    const nari::bytecode::Chunk *bound_chunk_ = nullptr;

    // Per-chunk write-once global-name -> proven type,
    // built by ir::analyze_const_globals() in reset_for_chunk().
    ir::GlobalTypeMap global_const_types_;

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
    CompiledFunc ir_compile(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, CompiledFunc spec_fallback = nullptr,
                            bool *spec_candidate = nullptr);
};

} // namespace jit
} // namespace nari

// Global JIT instance
namespace nari {
namespace jit {

extern MethodJITBase *g_jit_compiler;
// VM whose invocation triggered the in-progress compile_chunk (null outside one).
// Set only from VM::note_jit_callee, which runs inside jit_call_value_impl's window
// where vm->jit_captures_raw points at the *compiling* function's closure captures,
// so a Call site's LoadCapture callee can be resolved at compile time.
extern bytecode::VM *g_compile_vm;
// True only while compiling from note_jit_callee, where vm->jit_captures_raw is
// known to point at the compiled function's own closure captures. Global-callee
// resolution needs only g_compile_vm and works on every compile path.
extern bool g_compile_captures_ok;
// --- guarded direct call: pinned callee closures ---
// A direct-call site guards on a closure Value's raw bits. Those bits embed a
// heap pointer, so the guarded closure must never be freed: otherwise a later
// allocation could reuse the address and the guard would pass for the wrong
// object. Pinning stores a copy at a stable address and roots it with the GC.
// Returns the raw bits to bake into the guard.
void init_jit();
void shutdown_jit();

#ifdef NARI_ENABLE_GDB_JIT
void register_gdb_jit_function(const std::string &name, const void *code_addr, size_t code_size);
void unregister_all_gdb_jit_functions();
#else
inline void register_gdb_jit_function(const std::string &, const void *, size_t) {
}
inline void unregister_all_gdb_jit_functions() {
}
#endif

// Linux perf jitdump writer (perf_jitdump.cpp)
void perf_jitdump_register(const std::string &name, const void *code_addr, size_t code_size);
void perf_jitdump_close();

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
