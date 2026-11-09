#pragma once

#ifndef DISABLE_JIT

#include "asmjit_jit.h"
#include "trace_jit.h"
#include <memory>
#include <unordered_map>
#include <utility>

namespace nari {
namespace jit {

// Owns both execution tiers and their generated code for one VM.
class JitSession final {
  public:
    JitSession();
    ~JitSession();

    JitSession(const JitSession &) = delete;
    JitSession &operator=(const JitSession &) = delete;

    void bind_chunk(const bytecode::Chunk &chunk);
    // Returns true when a compiled trace handled the back edge and set VM::ip.
    bool on_backedge(bytecode::VM &vm, size_t anchor_pc, size_t target_pc, bool was_recording);
    bool has_profitable_trace(uint32_t func_idx) const;
    MethodJITBase::CompiledFunc method_entry(uint32_t idx) const { return this->method ? this->method->compiled_at(idx) : nullptr; }
    bool method_compiled(uint32_t idx) const { return method_entry(idx) != nullptr; }
    MethodJITBase::CompiledFunc compile_method(const bytecode::Chunk &chunk, uint32_t idx, bytecode::VM *vm = nullptr,
                                               bool captures_ok = false);

  private:
    const CompiledTrace *find_trace(uint32_t func_idx, size_t anchor_pc) const;
    CompiledTrace compile_trace(const TraceRecording &rec, const bytecode::Chunk &chunk, uint32_t func_idx);
    std::unique_ptr<AsmJITMethodCompiler> method;
    std::unique_ptr<TraceJITBase> trace;
    bool trace_stats_enabled = false;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> trace_iter_stats;
    std::unordered_map<uint64_t, uint32_t> edge_heat;
    static constexpr uint32_t TRACE_HOT_THRESHOLD = 8;
};

std::unique_ptr<JitSession> create_session();

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
