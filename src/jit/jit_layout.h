#pragma once
// shared helper for computing field offsets in non-standard-layout types.
// `offsetof` is undefined behavior on such types, so instead we construct an instance
// and take the difference between the member's address and the object's base address.

#ifndef DISABLE_JIT

#include <cstdint>
#include <new>
#include <type_traits>

namespace nari {
namespace jit {

template <typename T, typename M>
static inline int64_t field_offset(M T::*mp) {
    alignas(T) char buf[sizeof(T)];
    T *obj = new (buf) T{};
    int64_t off = reinterpret_cast<const char *>(&(obj->*mp)) - reinterpret_cast<const char *>(obj);
    obj->~T();
    return off;
}

template <typename>
struct member_pointer_traits;

template <typename T, typename M>
struct member_pointer_traits<M T::*> {
    using class_type = T;
    using member_type = M;
};

template <auto Member>
inline constexpr std::ptrdiff_t field_offset_v = [] {
    using T = typename member_pointer_traits<decltype(Member)>::class_type;

    alignas(T) std::byte storage[sizeof(T)];
    T *obj = ::new (storage) T{};
    auto off = reinterpret_cast<char *>(&(obj->*Member)) - reinterpret_cast<char *>(obj);
    obj->~T();
    return off;
}();

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
