#pragma once
// Runtime checks for standard-library smart-pointer representations used by
// generated CallFrame setup.

#ifndef DISABLE_JIT

#include <cstring>
#include <memory>

namespace nari {
namespace jit {
namespace stl {

// the JITs null smart-pointer frame slots by storing zero words, so a null smart pointer must be all-zero bytes
template <class P>
inline bool null_is_all_zero() {
    alignas(P) unsigned char buf[sizeof(P)];
    std::memset(buf, 0xAB, sizeof(buf));
    ::new (static_cast<void *>(buf)) P();
    bool zero = true;
    for (size_t i = 0; i < sizeof(P); i++) {
        zero = zero && buf[i] == 0;
    }
    reinterpret_cast<P *>(buf)->~P();
    return zero;
}

} // namespace stl

// True when the smart-pointer ABI assumptions made by generated code hold.
bool stl_layouts_ok();

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
