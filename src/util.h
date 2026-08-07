#pragma once

#ifndef _WIN32
#include <cxxabi.h>
#endif
#include <cctype>
#include <memory>
#include <string>
#include <string_view>

// trim leading/trailing whitespace. 
// single definition shared by the lexer and the module resolver, which each had their own identical copy.
inline std::string trim_ascii(std::string_view s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

inline std::string demangle(const char *name) {
#ifndef _WIN32
    int status = -1;
    std::unique_ptr<char, decltype(&std::free)> res{ abi::__cxa_demangle(name, nullptr, nullptr, &status), std::free };
    return (status == 0) ? res.get() : name;
#else
    // windows doesn't have cxxabi, according to stack overflow it might already
    // be demangled when doing typeid(T).name()?
    return name;
#endif
}

template <class T, class... Args> typename std::enable_if<!std::is_array<T>::value, T *>::type construct_at(T *p, Args &&...args) {
    return ::new ((void *)p) T(std::forward<Args>(args)...);
}

template <class T> void destroy_at(T *p) {
    p->~T();
}
