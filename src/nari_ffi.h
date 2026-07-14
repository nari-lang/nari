#pragma once

#ifndef DISABLE_FFI

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Windows DLL handling macros to mimic dlopen/dlsym
#define RTLD_LAZY 0
inline void *dlopen(const char *filename, int) {
    return (void *)LoadLibraryA(filename);
}
inline void *dlsym(void *handle, const char *symbol) {
    return (void *)GetProcAddress((HMODULE)handle, symbol);
}
inline int dlclose(void *handle) {
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
inline const char *dlerror() {
    static char buf[256];
    DWORD err = GetLastError();
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, buf, sizeof(buf), NULL);
    return buf;
}
#else
#include <dlfcn.h>
#endif

#include "core_types.h"
#include <ffi.h>

class ScriptRuntime;

namespace nari {

enum class FFIType {
    Void,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    Int64,
    UInt32,
    UInt64,
    Float,
    Double,
    Bool,
    Pointer, // strings and other pointers
    Struct,  // custom structs passed by value
};

struct FFIStructField {
    std::string name;
    FFIType type;
    size_t count;
    std::shared_ptr<struct FFIStructDef> aggregate_def;

    FFIStructField(const std::string &n, FFIType t, size_t c = 1) : name(n), type(t), count(c) {
    }

    bool operator==(const FFIStructField &other) const;
};

struct FFIStructDef {
    std::string name;
    bool is_union = false;
    std::vector<FFIStructField> fields;
    // cached ffi_types for this struct
    mutable ffi_type *ffi_struct_type = nullptr;
    mutable std::vector<ffi_type *> ffi_field_types;
    mutable std::vector<size_t> ffi_field_offsets;

    FFIStructDef() = default;
    FFIStructDef(const std::string &n, const std::vector<FFIStructField> &f, bool u = false)
        : name(n), is_union(u), fields(f) {
    }

    ~FFIStructDef() {
        if (ffi_struct_type) {
            delete ffi_struct_type;
        }
    }

    FFIStructDef(const FFIStructDef &other)
        : name(other.name), is_union(other.is_union), fields(other.fields), ffi_struct_type(nullptr) {
    }

    FFIStructDef(FFIStructDef &&other) noexcept
        : name(std::move(other.name)), is_union(other.is_union), fields(std::move(other.fields)),
          ffi_struct_type(other.ffi_struct_type),
          ffi_field_types(std::move(other.ffi_field_types)),
          ffi_field_offsets(std::move(other.ffi_field_offsets)) {
        other.ffi_struct_type = nullptr;
    }

    FFIStructDef &operator=(const FFIStructDef &other) {
        if (this != &other) {
            if (ffi_struct_type) {
                delete ffi_struct_type;
            }
            name = other.name;
            is_union = other.is_union;
            fields = other.fields;
            ffi_struct_type = nullptr;
            ffi_field_types.clear();
            ffi_field_offsets.clear();
        }
        return *this;
    }

    FFIStructDef &operator=(FFIStructDef &&other) noexcept {
        if (this != &other) {
            name = std::move(other.name);
            is_union = other.is_union;
            fields = std::move(other.fields);
            if (ffi_struct_type) {
                delete ffi_struct_type;
            }
            ffi_struct_type = other.ffi_struct_type;
            ffi_field_types = std::move(other.ffi_field_types);
            ffi_field_offsets = std::move(other.ffi_field_offsets);
            other.ffi_struct_type = nullptr;
        }
        return *this;
    }

    bool operator==(const FFIStructDef &other) const {
        return name == other.name && is_union == other.is_union && fields == other.fields;
    }
};

inline bool FFIStructField::operator==(const FFIStructField &other) const {
    if (name != other.name || type != other.type || count != other.count ||
        (aggregate_def == nullptr) != (other.aggregate_def == nullptr)) {
        return false;
    }
    return !aggregate_def || aggregate_def->name == other.aggregate_def->name;
}

struct FFISignature {
    FFIType return_type;
    std::vector<FFIType> param_types;
    bool is_variadic = false;
    size_t fixed_param_count = 0;

    std::shared_ptr<FFIStructDef> return_struct_def;
    std::vector<std::shared_ptr<FFIStructDef>> param_struct_defs;

    FFISignature() : return_type(FFIType::Void) {
    }
    FFISignature(FFIType ret, std::vector<FFIType> params) : return_type(ret), param_types(std::move(params)) {
    }

    bool operator==(const FFISignature &other) const {
        if (return_type != other.return_type || param_types != other.param_types ||
            is_variadic != other.is_variadic ||
            fixed_param_count != other.fixed_param_count) {
            return false;
        }

        // Compare struct definitions
        if ((return_struct_def == nullptr) !=
            (other.return_struct_def == nullptr)) {
            return false;
        }
        if (return_struct_def && other.return_struct_def &&
            !(*return_struct_def == *other.return_struct_def)) {
            return false;
        }

        if (param_struct_defs.size() != other.param_struct_defs.size()) {
            return false;
        }

        for (size_t i = 0; i < param_struct_defs.size(); i++) {
            auto &a = param_struct_defs[i];
            auto &b = other.param_struct_defs[i];
            if ((a == nullptr) != (b == nullptr)) {
                return false;
            }
            if (a && b && !(*a == *b)) {
                return false;
            }
        }

        return true;
    }
};

struct FFISignatureHash {
    std::size_t operator()(const FFISignature &sig) const {
        // pack return type + variadic + fixed count + param count into initial seed
        std::size_t h = (size_t)sig.return_type;
        h = h * 31 + (size_t)sig.is_variadic;
        h = h * 31 + sig.fixed_param_count;
        h = h * 31 + sig.param_types.size();
        // mix param types with FNV-1a
        for (const auto &pt : sig.param_types) {
            h ^= (size_t)pt;
            h *= 0x100000001B3ULL; // FNV prime
        }

        // hash struct definitions
        if (sig.return_struct_def) {
            h ^= static_cast<size_t>(sig.return_struct_def->is_union);
            h *= 0x100000001B3ULL;
            for (char c : sig.return_struct_def->name) {
                h ^= c;
                h *= 0x100000001B3ULL;
            }
            for (const auto &field : sig.return_struct_def->fields) {
                for (char c : field.name) {
                    h ^= c;
                    h *= 0x100000001B3ULL;
                }
                h ^= (size_t)field.type;
                h *= 0x100000001B3ULL;
                h ^= field.count;
                h *= 0x100000001B3ULL;
            }
        }

        for (const auto &sdef : sig.param_struct_defs) {
            if (sdef) {
                h ^= static_cast<size_t>(sdef->is_union);
                h *= 0x100000001B3ULL;
                for (char c : sdef->name) {
                    h ^= c;
                    h *= 0x100000001B3ULL;
                }
                for (const auto &field : sdef->fields) {
                    for (char c : field.name) {
                        h ^= c;
                        h *= 0x100000001B3ULL;
                    }
                    h ^= static_cast<size_t>(field.type);
                    h *= 0x100000001B3ULL;
                    h ^= field.count;
                    h *= 0x100000001B3ULL;
                }
            }
        }

        return h;
    }
};

class FFILibrary {
  public:
    FFILibrary(const std::string &path) : lib_path(path), handle(nullptr) {
        handle = dlopen(path.c_str(), RTLD_LAZY);
        if (!handle) {
            error = std::string("Failed to load library: ") + dlerror();
        } else {
            enumerate_symbols();
        }
    }

    ~FFILibrary() {
        if (handle) {
            dlclose(handle);
        }
    }

    bool is_loaded() const {
        return handle != nullptr;
    }
    std::string get_error() const {
        return error;
    }
    const std::vector<std::string> &get_symbols() const {
        return symbols;
    }

    void *get_symbol(const std::string &name) {
        if (!handle) {
            return nullptr;
        }

        auto it = symbol_cache_.find(name);
        if (it != symbol_cache_.end()) {
            return it->second;
        }

        dlerror();
        void *sym = dlsym(handle, name.c_str());

        if (!sym) {
            const char *err = dlerror();
            if (err) {
                error = "Symbol not found: " + name + " - " + err;
            }
        }

        symbol_cache_[name] = sym;
        return sym;
    }

  private:
    void enumerate_symbols();

    std::string lib_path;
    void *handle;
    std::string error;
    std::vector<std::string> symbols;
    std::unordered_map<std::string, void *> symbol_cache_;
};

struct FFIArgStorage {
    // deque keeps c_str() addresses stable while nested aggregate fields append.
    std::deque<std::string> string_storage;
    std::vector<int32_t> int32_storage;
    std::vector<int64_t> int64_storage;
    std::vector<int8_t> int8_storage;
    std::vector<uint8_t> uint8_storage;
    std::vector<int16_t> int16_storage;
    std::vector<uint16_t> uint16_storage;
    std::vector<uint32_t> uint32_storage;
    std::vector<uint64_t> uint64_storage;
    std::vector<float> float_storage;
    std::vector<double> double_storage;
    std::vector<uint8_t> bool_storage;
    std::vector<void *> pointer_storage;
    std::vector<std::vector<char>> struct_storage;
    std::vector<void *> arg_pointers;

    void clear_and_reserve(size_t n) {
        string_storage.clear();
        int32_storage.clear();
        int64_storage.clear();
        int8_storage.clear();
        uint8_storage.clear();
        int16_storage.clear();
        uint16_storage.clear();
        uint32_storage.clear();
        uint64_storage.clear();
        float_storage.clear();
        double_storage.clear();
        bool_storage.clear();
        pointer_storage.clear();
        struct_storage.clear();
        arg_pointers.clear();
        if (int32_storage.capacity() < n) {
            int32_storage.reserve(n);
        }
        if (int64_storage.capacity() < n) {
            int64_storage.reserve(n);
        }
        if (int8_storage.capacity() < n) {
            int8_storage.reserve(n);
        }
        if (uint8_storage.capacity() < n) {
            uint8_storage.reserve(n);
        }
        if (int16_storage.capacity() < n) {
            int16_storage.reserve(n);
        }
        if (uint16_storage.capacity() < n) {
            uint16_storage.reserve(n);
        }
        if (uint32_storage.capacity() < n) {
            uint32_storage.reserve(n);
        }
        if (uint64_storage.capacity() < n) {
            uint64_storage.reserve(n);
        }
        if (float_storage.capacity() < n) {
            float_storage.reserve(n);
        }
        if (double_storage.capacity() < n) {
            double_storage.reserve(n);
        }
        if (bool_storage.capacity() < n) {
            bool_storage.reserve(n);
        }
        if (pointer_storage.capacity() < n) {
            pointer_storage.reserve(n);
        }
        if (struct_storage.capacity() < n) {
            struct_storage.reserve(n);
        }
        if (arg_pointers.capacity() < n) {
            arg_pointers.reserve(n);
        }
    }
};

// FFI functions (using libffi)
class FFICaller {
  public:
    static Value call_function(void *func_ptr, const FFISignature &sig, const std::vector<Value> &args);

    static Value call_function_variadic(void *func_ptr, const FFISignature &sig, const std::vector<Value> &args);

  private:
    struct CIFCache {
        ffi_cif cif;
        std::vector<ffi_type *> param_types;
        bool prepared = false;
    };

    static std::unordered_map<FFISignature, CIFCache, FFISignatureHash> cif_cache;
    static std::unordered_map<FFISignature, CIFCache, FFISignatureHash> cif_variadic_cache;

    static CIFCache &get_or_create_cif(const FFISignature &sig);
    static CIFCache &get_or_create_cif_variadic(const FFISignature &sig);
};

// struct marshalling helpers
std::shared_ptr<FFIStructDef>
create_struct_def_from_type(const std::string &type_name);
size_t get_struct_size(const std::string &type_name);
Value read_struct_from_memory(void *ptr, const std::string &type_name);
bool write_struct_to_memory(void *ptr, const std::string &type_name, const Value &obj);
void *managed_struct_pointer(const Value &value);

// FFI Callback support for creating native function pointers from Nari functions
struct FFICallback {
    ffi_closure *closure;
    void *executable_code;
    FFISignature signature;
    Value nari_function;
    ScriptRuntime *runtime;
    ffi_cif cif;
    std::vector<ffi_type *> param_types;

    FFICallback() : closure(nullptr), executable_code(nullptr), runtime(nullptr) {
    }

    ~FFICallback();
};

class FFICallbackManager {
  public:
    static FFICallbackManager &instance() {
        static FFICallbackManager manager;
        return manager;
    }

    void *create_callback(const FFISignature &sig, const Value &nari_func, ScriptRuntime *runtime);
    void free_callback(void *code_ptr);

    FFICallback *get_callback(void *code_ptr);

    // every live callback holds a Nari function Value that native code may invoke at any time, so it must stay alive.
    void collect_roots(std::vector<const Value *> &roots) const {
        for (const auto &[code, cb] : callbacks) {
            if (cb) {
                roots.push_back(&cb->nari_function);
            }
        }
    }

  private:
    FFICallbackManager() = default;
    std::unordered_map<void *, std::unique_ptr<FFICallback>> callbacks;

    // Trampoline function that bridges C -> Nari calls
    static void callback_trampoline(ffi_cif *cif, void *ret, void **args, void *user_data);
};

class FFIRegistry {
  public:
    static FFIRegistry &instance() {
        static FFIRegistry registry;
        return registry;
    }

    std::shared_ptr<FFILibrary> load_library(const std::string &path);
    std::shared_ptr<FFILibrary> get_library(const std::string &path);
    std::string find_library(const std::string &name);

  private:
    FFIRegistry() = default;
    std::unordered_map<std::string, std::shared_ptr<FFILibrary>> libraries;
};

} // namespace nari
#endif // DISABLE_FFI
