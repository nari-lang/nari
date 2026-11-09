#ifndef DISABLE_JIT

#include "runtime/session.h"
#include "bytecode.h"
#include "jit_tls.h"
#include "stl_layout.h"
#include "trace/compiler.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace nari {
namespace jit {

JitSession::JitSession() : method(std::make_unique<AsmJITMethodCompiler>()), trace_stats_enabled(std::getenv("NARI_TRACE_STATS") != nullptr) {
    if (!std::getenv("NARI_DISABLE_TRACE_JIT")) {
        this->trace = std::make_unique<TraceJITCompilerAsmJit>();
    }
}

JitSession::~JitSession() {
    method->drain_async();
    unregister_gdb_jit_functions(method.get());
    if (trace) {
        unregister_gdb_jit_functions(trace.get());
    }
    perf_jitdump_close();
}

void JitSession::bind_chunk(const bytecode::Chunk &chunk) {
    // Trace keys contain only a function index and PC. A trace also embeds
    // absolute bytecode addresses, so it cannot survive a new VM run.
    if (this->trace) {
        this->trace->reset();
    }
    this->trace_iter_stats.clear();
    this->edge_heat.clear();
    this->method->bind_chunk(chunk);
}

bool JitSession::has_profitable_trace(uint32_t func_idx) const {
    static constexpr uint64_t kMinAvgIters = 1000;
    static constexpr uint32_t kMinEntries = 4;
    for (const auto &kv : this->trace_iter_stats) {
        if (static_cast<uint32_t>(kv.first >> 32) == func_idx && kv.second.second >= kMinEntries &&
            kv.second.first / kv.second.second >= kMinAvgIters) {
            return true;
        }
    }
    return false;
}

bool JitSession::on_backedge(bytecode::VM &vm, size_t anchor_pc, size_t target_pc, bool was_recording) {
    if (!this->trace) {
        return false;
    }
    const uint32_t func_idx = vm.current_function()->self_idx;
    TraceRecording &rec = vm.trace_recorder;
    if (was_recording && func_idx != rec.func_idx) {
        return false; // an inlined callee's back edge is not this trace's loop
    }
    if (was_recording && rec.anchor_pc == anchor_pc) {
        rec.steps.push_back({ TraceStep::Kind::LoopBack });
        if (rec.exit_pc != 0 && !rec.aborted) {
            rec.valid = true;
            rec.recording = false;
            jit_tls_prepare(vm);
            compile_trace(rec, *vm.chunk, func_idx);
        } else {
            rec.recording = false;
            rec.aborted = true;
            // A failed trace should not leave the function interpreting forever.
            if (!method_compiled(func_idx)) {
                compile_method(*vm.chunk, func_idx);
            }
        }
        return false;
    }
    if (was_recording || rec.pending_record || rec.recording) {
        return false;
    }

    const CompiledTrace *compiled = find_trace(func_idx, anchor_pc);
    if (compiled && compiled->valid) {
        assert(!vm.frames.empty() && "trace JIT entered with empty call frames");
        jit_tls_prepare(vm);
        vm.trace_last_iters = 0;
        compiled->fn(&vm); // compiled trace sets the bytecode IP

        const uint64_t key = (static_cast<uint64_t>(func_idx) << 32) | static_cast<uint64_t>(anchor_pc);
        auto &stats = this->trace_iter_stats[key];
        stats.first += vm.trace_last_iters;
        ++stats.second;
        if (this->trace_stats_enabled && (stats.second & (stats.second - 1)) == 0) {
            std::fprintf(stderr, "[trace-stats] func=%u anchor=%zu entries=%u total_iters=%llu avg=%llu\n", func_idx,
                         anchor_pc, stats.second, static_cast<unsigned long long>(stats.first),
                         static_cast<unsigned long long>(stats.first / stats.second));
        }
        return true;
    }

    const uint64_t key = (static_cast<uint64_t>(func_idx) << 32) | static_cast<uint64_t>(anchor_pc);
    if (++this->edge_heat[key] == TRACE_HOT_THRESHOLD) {
        rec.reset();
        rec.pending_record = true;
        rec.anchor_pc = anchor_pc;
        rec.target_pc = target_pc;
        rec.func_idx = func_idx;
    }
    return false;
}

MethodJITBase::CompiledFunc JitSession::compile_method(const bytecode::Chunk &chunk, uint32_t idx, bytecode::VM *vm, bool captures_ok) {
    if (!this->method) {
        return nullptr;
    }
    return this->method->compile_with_context(chunk, idx, vm, captures_ok);
}

const CompiledTrace *JitSession::find_trace(uint32_t func_idx, size_t anchor_pc) const {
    return this->trace ? this->trace->find(func_idx, anchor_pc) : nullptr;
}

CompiledTrace JitSession::compile_trace(const TraceRecording &rec, const bytecode::Chunk &chunk, uint32_t func_idx) {
    return this->trace ? this->trace->compile(rec, chunk, func_idx) : CompiledTrace{};
}

std::unique_ptr<JitSession> create_session() {
    if (std::getenv("NARI_DISABLE_JIT") || !stl_layouts_ok()) {
        return nullptr;
    }
    return std::make_unique<JitSession>();
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
