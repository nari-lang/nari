#pragma once
// Compile-time offsets for standard-layout types. Offsets of non-standard
// layout VM objects live in abi.h and use the compiler's layout extension.

#ifndef DISABLE_JIT

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Only use this for standard-layout types!
#define NARI_JIT_OFF(T, member)                                                                                                            \
    ([]() constexpr -> int64_t {                                                                                                           \
        static_assert(std::is_standard_layout<T>::value, "JIT offsetof requires a standard-layout type");                                  \
        return static_cast<int64_t>(__builtin_offsetof(T, member));                                                                        \
    }())

#endif // !DISABLE_JIT
