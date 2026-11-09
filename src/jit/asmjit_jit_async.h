#pragma once
// Background register-allocation + encoding worker for the AsmJIT method JIT.
// (Split from src/jit/asmjit_jit.cpp; see asmjit_jit_async.cpp.)
//
// cc.finalize() is purely a function of the CodeHolder's node graph, so it
// runs on a worker while the main thread keeps interpreting. Only the enqueue
// entry points are shared; all queue state lives in asmjit_jit_async.cpp.

#include "asmjit_jit.h"
#include "jit_arch.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace nari {
namespace jit {

// A finished CodeHolder awaiting RA+encode on the worker. Constructed by
// ir_compile (main thread), consumed by compile_worker_main.
struct CompileJob {
    std::unique_ptr<asmjit::CodeHolder> holder;
    std::unique_ptr<arch::Compiler> cc;
    uint32_t chunk_idx = 0;
    std::string sym;
    AsmJITMethodCompiler *owner = nullptr;
};

// Enqueue; starts the worker on first use so a run that never tiers up pays nothing.
void enqueue_compile_job(std::unique_ptr<CompileJob> job);

// False when asm dumping or JIT validation is on (the worker would dangle
// stack-local loggers and hide nondeterminism).
bool async_jit_enabled();

// Serializes JitRuntime allocations and symbol registration with worker jobs.
std::mutex &jit_runtime_mutex();

} // namespace jit
} // namespace nari
