#pragma once
// Runtime-probed STL container layout for JIT code generation.
//
// The JITs read std::vector's internal {begin, end, end-of-capacity} pointers straight out of memory
// If a probe fails (exotic STL where begin/end aren't stored as plain pointers), 
// the JITs must stay disabled, stl_layouts_ok() gates both.

#ifndef DISABLE_JIT

#include <cstdint>
#include <cstring>
#include <memory>

namespace nari {
namespace jit {
namespace stl {

struct VecOffsets {
    int64_t begin = -1;  // byte offset of the data/begin pointer within the vector object
    int64_t end = -1;    // byte offset of the one-past-last-element pointer
    int64_t cap = -1;    // byte offset of the end-of-capacity pointer
    bool ok = false;
};

// find where std::vector<T> keeps its begin/end/cap pointers by scanning the bytes of a live instance
template <class Vec>
inline VecOffsets probe_vec() {
    VecOffsets out;
    Vec v;
    v.reserve(7);
    v.resize(3);
    const char *base = reinterpret_cast<const char *>(&v);
    const char *want_begin = reinterpret_cast<const char *>(v.data());
    const char *want_end = reinterpret_cast<const char *>(v.data() + v.size());
    const char *want_cap = reinterpret_cast<const char *>(v.data() + v.capacity());
    for (size_t off = 0; off + sizeof(void *) <= sizeof(Vec); off += sizeof(void *)) {
        const char *w;
        std::memcpy(&w, base + off, sizeof(w));
        if (w == want_begin && out.begin < 0) {
            out.begin = static_cast<int64_t>(off);
        } else if (w == want_end && out.end < 0) {
            out.end = static_cast<int64_t>(off);
        } else if (w == want_cap && out.cap < 0) {
            out.cap = static_cast<int64_t>(off);
        }
    }
    out.ok = out.begin >= 0 && out.end >= 0 && out.cap >= 0;
    return out;
}

// The JITs null smart-pointer frame slots by storing zero words, so a null smart pointer must be all-zero bytes
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

// True when every container layout the JITs depend on was successfully probed
bool stl_layouts_ok();

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
