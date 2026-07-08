#pragma once
#include <cmath>
#include <cstdint>

#include "compiler_support.h"

// portable overflow-checked 64-bit arithmetic
// returns true if the operation overflowed; *out holds the result otherwise

// since `static_cast<int64_t>(double)` is UB in C++ when NaN, (+-)Inf, or has magnitude >= 2^63, we map them to 0 to avoid UB.
inline int64_t safe_double_to_i64(double d) noexcept {
    // 2^63 is exactly representable; any value
    // >= 2^63 or <= -2^63 - 1 would overflow on conversion.
    if (NARI_LIKELY(std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
        return static_cast<int64_t>(d);
    }
    return 0;
}

#ifdef CLANG_OR_GCC_COMPAT

inline bool add_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    return __builtin_add_overflow(a, b, out);
}
inline bool sub_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    return __builtin_sub_overflow(a, b, out);
}
inline bool mul_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    return __builtin_mul_overflow(a, b, out);
}

#else // pure MSVC fallback
#include <limits>

inline bool add_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    *out = a + b;
    return (b > 0 && a > std::numeric_limits<int64_t>::max() - b) || (b < 0 && a < std::numeric_limits<int64_t>::min() - b);
}
inline bool sub_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    *out = a - b;
    return (b < 0 && a > std::numeric_limits<int64_t>::max() + b) || (b > 0 && a < std::numeric_limits<int64_t>::min() + b);
}
inline bool mul_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return false;
    }
    if (a == -1) {
        if (b == std::numeric_limits<int64_t>::min()) {
            return true;
        }
        *out = -b;
        return false;
    }
    if (b == -1) {
        if (a == std::numeric_limits<int64_t>::min()) {
            return true;
        }
        *out = -a;
        return false;
    }
    *out = a * b;
    return (*out / a) != b;
}

#endif

inline bool mul_overflow_i48(int64_t a, int64_t b, void* out) {
    uint64_t low, adc_low;
    int64_t high;
#if COMPILER_IS_REAL_MSVC
#if !__ARM_ARCH
    low = _mul128(a, b, &high);
    _addcarry_u64(_addcarry_u64(0, low, 0x800000000000ull, &adc_low), high, 0, (uint64_t*)&high);
#else
    low = (uint64_t)a * (uint64_t)b;
    high = __mulh(a, b);
    adc_low = low + 0x800000000000ull;
    high += adc_low < low;
#endif
#else
    typedef __int128 int128_t;
    int128_t wide = (int128_t)a * (int128_t)b;
    low = (uint64_t)wide;
    high = (int64_t)(wide >> 64);
    high += (int64_t)__builtin_add_overflow(low, 0x800000000000ull, &adc_low);
#endif
    if (NARI_EXPECT(!(adc_low >> 48 | high), true)) {
        *(int64_t*)out = low;
        return false;
    }
    else {
        *(double*)out = (double)a * (double)b;
        return true;
    }
}
