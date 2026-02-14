#ifndef _WIN32
#include <cxxabi.h>
#endif
#include <memory>
#include <string>

static std::string demangle(const char* name) {
#ifndef _WIN32
    int status = -1;
    std::unique_ptr<char, decltype(&std::free)> res{
        abi::__cxa_demangle(name, nullptr, nullptr, &status), std::free
    };
    return (status == 0) ? res.get() : name;
#else
    // windows doesn't have cxxabi, according to stack overflow it might already be demangled when doing typeid(T).name()?
    return name;
#endif
}
