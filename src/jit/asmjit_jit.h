#pragma once

#ifndef DISABLE_JIT

#include "bytecode.h"
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
};

// AsmJIT-based method JIT compiler
class AsmJITMethodCompiler : public MethodJITBase {
  public:
    AsmJITMethodCompiler() {};
    ~AsmJITMethodCompiler() override {};

    CompiledFunc compile_chunk(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx) override;
    bool is_compiled(uint32_t chunk_idx) const override;
    CompiledFunc get_compiled(uint32_t chunk_idx) const override;
    CompiledFunc get_compiled_fast(uint32_t idx) const override;

  private:
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

    void reset_for_chunk(const nari::bytecode::Chunk &chunk);

    // Debug tripwire enforcing the pointer-stability invariant relied upon by the raw addresses baked into generated
    // code
    void assert_tables_stable() const;

    // Optimizing-IR tier, builds SSA IR and lowers it to AsmJIT.
    // Returns nullptr if the function is not yet IR-eligible; the VM runs it in the interpreter.
    //
    // when `spec_fallback` is non-null, compile in SPECULATIVE mode:
    // seed all parameter slots as Int48 in type inference and attempt ONLY the register tier
    CompiledFunc ir_compile(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx,
                            CompiledFunc spec_fallback = nullptr);
};

} // namespace jit
} // namespace nari

// Global JIT instance
namespace nari {
namespace jit {

extern MethodJITBase *g_jit_compiler;
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
