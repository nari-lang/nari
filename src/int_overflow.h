#pragma once
#include <cmath>
#include <cstdint>

#include "compiler_support.h"

#if COMPILER_IS_REAL_MSVC
#include <intrin.h>
#endif

// portable overflow-checked 64-bit arithmetic
// returns true if the operation overflowed; *out holds the result otherwise

// Reject nonfinite and out-of-range doubles before casting. The positive bound
// is exclusive because double(INT64_MAX) rounds to 2^63.
inline bool try_double_to_i64(double dbl, int64_t *out) noexcept {
    *out = 0;
    if (NARI_LIKELY(std::isfinite(dbl) && dbl >= -9223372036854775808.0 && dbl < 9223372036854775808.0)) {
        *out = static_cast<int64_t>(dbl);
        return true;
    }
    return false;
}

inline int64_t safe_double_to_i64(double dbl) noexcept {
    int64_t result;
    try_double_to_i64(dbl, &result);
    return result;
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
    *out = 0;
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) || (b < 0 && a < std::numeric_limits<int64_t>::min() - b)) {
        return true;
    }
    *out = a + b;
    return false;
}
inline bool sub_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    *out = 0;
    if ((b < 0 && a > std::numeric_limits<int64_t>::max() + b) || (b > 0 && a < std::numeric_limits<int64_t>::min() + b)) {
        return true;
    }
    *out = a - b;
    return false;
}
inline bool mul_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    *out = 0;
    if (a == 0 || b == 0) {
        return false;
    }
    const int64_t low = std::numeric_limits<int64_t>::min();
    const int64_t high = std::numeric_limits<int64_t>::max();
    if ((a > 0 && ((b > 0 && a > high / b) || (b < 0 && b < low / a))) || (a < 0 && ((b > 0 && a < low / b) || (b < 0 && a < high / b)))) {
        return true;
    }
    *out = a * b;
    return false;
}

#endif

inline bool mul_overflow_i48(int64_t a, int64_t b, void *out) {
    uint64_t low, adc_low;
    int64_t high;
#if COMPILER_IS_REAL_MSVC
#if !__ARM_ARCH
    low = _mul128(a, b, &high);
    _addcarry_u64(_addcarry_u64(0, low, 0x800000000000ull, &adc_low), high, 0, reinterpret_cast<uint64_t *>(&high));
#else
    low = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
    high = __mulh(a, b);
    adc_low = low + 0x800000000000ull;
    high += adc_low < low;
#endif
#else
    high = static_cast<int64_t>(__builtin_smulll_overflow(a, b, reinterpret_cast<long long *>(&low)));
    high += static_cast<int64_t>(__builtin_add_overflow(low, 0x8000'0000'0000ull, &adc_low));
#endif
    if (NARI_EXPECT(!(adc_low >> 48 | high), true)) {
        *static_cast<int64_t *>(out) = low;
        return false;
    } else {
        *static_cast<double *>(out) = static_cast<double>(a) * static_cast<double>(b);
        return true;
    }
}
