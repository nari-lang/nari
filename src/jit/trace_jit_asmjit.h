#pragma once
// #ifndef DISABLE_JIT

// Forward-declare VM/Chunk to avoid circular include with bytecode.h
namespace nari {
namespace bytecode {
class VM;
struct Chunk;
} // namespace bytecode
} // namespace nari

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "trace_jit.h"
#include "jit_arch.h"

namespace nari {
namespace jit {

// TraceJITCompiler (AsmJIT backend)
class TraceJITCompilerAsmJit : public TraceJITBase {
  public:
    TraceJITCompilerAsmJit();
    ~TraceJITCompilerAsmJit() override;

    // Compile a recorded trace.  Returns valid=false on failure.
    CompiledTrace compile(const TraceRecording &rec, const nari::bytecode::Chunk &chunk, uint32_t func_idx) override;

    // Look up a compiled trace for (func_idx, anchor_pc).
    const CompiledTrace *find(uint32_t func_idx, size_t anchor_pc) const override;

    // Invalidate all cached traces and free their machine code by
    // destroying + reconstructing asmjit::JitRuntime in place.
    void reset() override;

  private:
    asmjit::JitRuntime rt;

    struct Entry {
        CompiledTrace trace;
    };
    std::unordered_map<uint64_t, Entry> cache;

    static uint64_t make_key(uint32_t func_idx, size_t anchor_pc) {
        return ((uint64_t)func_idx << 32) | (uint32_t)anchor_pc;
    }
};

} // namespace jit
} // namespace nari

// #endif // !DISABLE_JIT
