// attempts to increase C++ standard backwards compatibility with nari
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

#if __cplusplus >= 202002L
#include <bit>
#endif

namespace nari {
namespace compat {

// std::bit_cast
#if __cplusplus >= 202002L
using std::bit_cast;
#else
template <class To, class From>
typename std::enable_if<
    sizeof(To) == sizeof(From) &&
        std::is_trivially_copyable<From>::value &&
        std::is_trivially_copyable<To>::value,
    To>::type
bit_cast(const From &src) noexcept {
    static_assert(std::is_trivially_constructible<To>::value, "compat::bit_cast<To> requires To to be trivially constructible");
    To dst;
    std::memcpy(&dst, &src, sizeof(To));
    return dst;
}
#endif

// std::endian
#if __cplusplus >= 202002L
using endian = std::endian;
#else
enum class endian {
#if defined(_MSC_VER) && !defined(__clang__)
    little = 0,
    big = 1,
    native = little,
#else
    little = __ORDER_LITTLE_ENDIAN__,
    big = __ORDER_BIG_ENDIAN__,
    native = __BYTE_ORDER__,
#endif
};
#endif

} // namespace compat
} // namespace nari
