#ifndef DISABLE_JIT
#include "trace_jit.h"
#include "bytecode.h"
#include "core_types.h"
#include "stl_layout.h"
#include "trace_jit_asmjit.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace nari {
namespace jit {
TraceJITBase *g_trace_jit = nullptr;

void init_trace_jit() {
    if (getenv("NARI_DISABLE_TRACE_JIT")) {
        return;
    }
    if (!stl_layouts_ok()) {
        return;
    }
    g_trace_jit = new TraceJITCompilerAsmJit();
}
void shutdown_trace_jit() {
    delete g_trace_jit;
    g_trace_jit = nullptr;
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
