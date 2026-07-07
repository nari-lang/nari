#ifndef DISABLE_JIT

#include "jit_tls.h"
#include "bytecode.h"

#if defined(__linux__) && defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace nari {
namespace jit {

static thread_local JitTlsContext tls_context;

#if defined(__linux__) && defined(__x86_64__)
static thread_local bool tls_gs_tried = false;
static thread_local bool tls_gs_enabled = false;
#endif

bool jit_tls_prepare(bytecode::VM &vm) {
    tls_context.vm = &vm;
    tls_context.stack_start = vm.stack.data();
    tls_context.frames_finish = vm.frames.data() + vm.frames.size();

#if defined(__linux__) && defined(__x86_64__)
    if (!tls_gs_tried) {
        tls_gs_tried = true;
        tls_gs_enabled = syscall(SYS_arch_prctl, ARCH_SET_GS, &tls_context) == 0;
    }
    return tls_gs_enabled;
#else
    return false;
#endif
}

bool jit_tls_gs_enabled() {
#if defined(__linux__) && defined(__x86_64__)
    return tls_gs_enabled;
#else
    return false;
#endif
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
