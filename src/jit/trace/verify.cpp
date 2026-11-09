#ifndef DISABLE_JIT
#include "trace_jit.h"
#include "bytecode.h"
#include <cstdint>

namespace nari {
namespace jit {

bool validate_trace_recording(const TraceRecording &rec, const bytecode::Chunk &chunk, uint32_t func_idx) {
    if (!rec.valid || rec.aborted || rec.func_idx != func_idx || func_idx >= chunk.functions.size() || rec.steps.empty()) {
        return false;
    }
    const size_t code_size = chunk.functions[func_idx].code.size();
    if (rec.exit_pc == 0 || rec.exit_pc > code_size || rec.anchor_pc >= code_size || rec.target_pc >= code_size) {
        return false;
    }
    for (const TraceStep &step : rec.steps) {
        using Kind = TraceStep::Kind;
        const bool requires_resume = step.kind == Kind::IntAdd || step.kind == Kind::IntSub || step.kind == Kind::IntMul ||
                                     step.kind == Kind::IntMod || step.kind == Kind::SideExitIfFalse ||
                                     step.kind == Kind::SideExitIfTrue || step.kind == Kind::ArrayGetIdx ||
                                     step.kind == Kind::ArraySetIdx || step.kind == Kind::ClosureInc ||
                                     step.kind == Kind::ClosureAddConst;
        const bool branch_exit = step.kind == Kind::SideExitIfFalse || step.kind == Kind::SideExitIfTrue;
        if (requires_resume && (!step.resume_pc || *step.resume_pc > code_size ||
                                (*step.resume_pc == code_size && !branch_exit))) {
            return false;
        }
    }
    return rec.steps.back().kind == TraceStep::Kind::LoopBack;
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
