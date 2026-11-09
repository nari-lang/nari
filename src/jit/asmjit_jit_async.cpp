/*
  asmjit_jit_async.cpp
  Background RA+encode worker split from src/jit/asmjit_jit.cpp.
*/

#ifndef DISABLE_JIT

#include "asmjit_jit_async.h"
#include "asmjit_dump.h"
#include "asmjit_jit.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace asmjit;
using namespace nari::bytecode;

namespace nari {
namespace jit {

// Background register allocation + encoding.
namespace {
std::deque<std::unique_ptr<CompileJob>> g_jobs;
std::mutex g_jobs_mtx;
std::condition_variable g_jobs_cv; // signals: work available, or stop
std::condition_variable g_idle_cv; // signals: queue drained and nothing running
std::vector<std::thread> g_workers;
bool g_worker_stop = false;
bool g_worker_started = false;
unsigned g_jobs_active = 0; // queued + in-flight, guarded by g_jobs_mu

// Guards the JitRuntime allocator and the symbol registries, which the main
// thread also touches. Held only across rt.add(), never across finalize().
std::mutex g_rt_mtx;

void compile_worker_main() {
    for (;;) {
        std::unique_ptr<CompileJob> job;
        {
            std::unique_lock<std::mutex> lock(g_jobs_mtx);
            g_jobs_cv.wait(lock, [] { return g_worker_stop || !g_jobs.empty(); });
            if (g_jobs.empty()) {
                if (g_worker_stop) {
                    return;
                }
                continue;
            }
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        if (job->cc->finalize() == kErrorOk) {
            job->owner->publish_async(job->chunk_idx, job->holder.get(), job->sym);
        }
        // Release the asmjit objects before reporting idle, so a drain that
        // returns cannot race with this job still holding the CodeHolder.
        job.reset();
        {
            std::lock_guard<std::mutex> lock(g_jobs_mtx);
            if (--g_jobs_active == 0) {
                g_idle_cv.notify_all();
            }
        }
    }
}

void stop_compile_worker();

// process.exit() calls libc exit() directly, so ~AsmJITMethodCompiler never runs and
// static teardown would hit a still-joinable std::thread (-> std::terminate). Registered
// once the worker starts, i.e. after every static ctor, so it runs before their dtors.
void drain_and_stop_at_exit() {
    {
        std::unique_lock<std::mutex> lock(g_jobs_mtx);
        g_idle_cv.wait(lock, [] { return g_jobs_active == 0; });
    }
    stop_compile_worker();
}

void stop_compile_worker() {
    {
        std::lock_guard<std::mutex> lock(g_jobs_mtx);
        if (!g_worker_started) {
            return;
        }
        g_worker_stop = true;
    }
    g_jobs_cv.notify_all();
    for (auto &t : g_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::lock_guard<std::mutex> lock(g_jobs_mtx);
    g_workers.clear();
    g_worker_started = false;
    g_worker_stop = false;
}
} // namespace

std::mutex &jit_runtime_mutex() {
    return g_rt_mtx;
}

void enqueue_compile_job(std::unique_ptr<CompileJob> job) {
    std::lock_guard<std::mutex> lock(g_jobs_mtx);
    if (!g_worker_started) {
        g_worker_started = true;
        // RA+encode is independent per function. Bound the environment override
        // so an invalid value cannot create an unbounded number of threads.
        unsigned n = 2;
        if (const char *e = getenv("NARI_JIT_WORKERS")) {
            char *end = nullptr;
            const long parsed = std::strtol(e, &end, 10);
            if (end != e && *end == '\0') {
                n = static_cast<unsigned>(std::clamp(parsed, 1L, 8L));
            }
        }
        for (unsigned i = 0; i < n; i++) {
            g_workers.emplace_back(compile_worker_main);
        }
        std::atexit(drain_and_stop_at_exit);
    }
    ++g_jobs_active;
    g_jobs.push_back(std::move(job));
    g_jobs_cv.notify_one();
}

bool async_jit_enabled() {
    // asm dumping attaches a stack-local StringLogger to the CodeHolder, which would dangle once the frame goes away
    static const bool on = getenv("NARI_DISABLE_ASYNC_JIT") == nullptr && !jit_dump_asm_enabled() &&
                           getenv("NARI_JIT_VALIDATE") == nullptr && getenv("NARI_JIT_DUMP_NODES") == nullptr;
    return on;
}

void AsmJITMethodCompiler::publish_async(uint32_t chunk_idx, asmjit::CodeHolder *holder, const std::string &sym) {
    CompiledFunc fn = nullptr;
    size_t code_size = holder->code_size();
    {
        std::lock_guard<std::mutex> lock(jit_runtime_mutex());
        if (this->rt.add(&fn, holder) != kErrorOk || !fn) {
            return;
        }
        register_gdb_jit_function(this, sym, reinterpret_cast<const void *>(fn), code_size);
        perf_jitdump_register(sym, reinterpret_cast<const void *>(fn), code_size);
    }
    // Publish only after rt.add() has made the code executable. The acquire
    // loads in VM dispatch and generated calls observe the initialized code.
    if (chunk_idx < this->compiled_fn_count) {
        compiled_fn_vec[chunk_idx].store(fn, std::memory_order_release);
    }
}

void AsmJITMethodCompiler::drain_async() {
    std::unique_lock<std::mutex> lock(g_jobs_mtx);
    g_idle_cv.wait(lock, [] { return g_jobs_active == 0; });
}

AsmJITMethodCompiler::~AsmJITMethodCompiler() {
    drain_async();
    stop_compile_worker();
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
