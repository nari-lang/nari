#pragma once

#ifndef DISABLE_JIT

#include <cstddef>
#include <cstdint>

namespace nari {
namespace bytecode {
class VM;
}
namespace jit {

struct JitTlsContext {
    void *vm = nullptr;
    void *stack_start = nullptr;
    void *frames_finish = nullptr;
};

bool jit_tls_prepare(bytecode::VM &vm);
bool jit_tls_gs_enabled();

constexpr int32_t jit_tls_stack_start_offset() {
    return static_cast<int32_t>(offsetof(JitTlsContext, stack_start));
}

constexpr int32_t jit_tls_frames_finish_offset() {
    return static_cast<int32_t>(offsetof(JitTlsContext, frames_finish));
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
