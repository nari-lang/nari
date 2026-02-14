#pragma once

#ifndef NO_FFI

#include <cstdint>
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
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, buf, sizeof(buf),
                 NULL);
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

// struct field definition
struct FFIStructField {
  std::string name;
  FFIType type;

  FFIStructField(const std::string &n, FFIType t) : name(n), type(t) {}

  bool operator==(const FFIStructField &other) const {
    return name == other.name && type == other.type;
  }
};

// struct definition
struct FFIStructDef {
  std::string name;
  std::vector<FFIStructField> fields;
  // cached ffi_types for this struct
  mutable ffi_type *ffi_struct_type = nullptr;
  mutable std::vector<ffi_type *> ffi_field_types;

  FFIStructDef() = default;
  FFIStructDef(const std::string &n, const std::vector<FFIStructField> &f)
      : name(n), fields(f) {}

  ~FFIStructDef() {
    if (ffi_struct_type) {
      delete ffi_struct_type;
    }
  }

  // copy ctor
  FFIStructDef(const FFIStructDef &other)
      : name(other.name), fields(other.fields), ffi_struct_type(nullptr) {}

  // move ctor
  FFIStructDef(FFIStructDef &&other) noexcept
      : name(std::move(other.name)), fields(std::move(other.fields)),
        ffi_struct_type(other.ffi_struct_type),
        ffi_field_types(std::move(other.ffi_field_types)) {
    other.ffi_struct_type = nullptr;
  }

  FFIStructDef &operator=(const FFIStructDef &other) {
    if (this != &other) {
      name = other.name;
      fields = other.fields;
      ffi_struct_type = nullptr;
      ffi_field_types.clear();
    }
    return *this;
  }

  FFIStructDef &operator=(FFIStructDef &&other) noexcept {
    if (this != &other) {
      name = std::move(other.name);
      fields = std::move(other.fields);
      if (ffi_struct_type)
        delete ffi_struct_type;
      ffi_struct_type = other.ffi_struct_type;
      ffi_field_types = std::move(other.ffi_field_types);
      other.ffi_struct_type = nullptr;
    }
    return *this;
  }

  bool operator==(const FFIStructDef &other) const {
    return name == other.name && fields == other.fields;
  }
};

struct FFISignature {
  FFIType return_type;
  std::vector<FFIType> param_types;
  bool is_variadic = false;
  size_t fixed_param_count = 0;

  std::shared_ptr<FFIStructDef> return_struct_def;
  std::vector<std::shared_ptr<FFIStructDef>> param_struct_defs;

  FFISignature() : return_type(FFIType::Void) {}
  FFISignature(FFIType ret, std::vector<FFIType> params)
      : return_type(ret), param_types(std::move(params)) {}

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

// fast hash for FFISignature
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
      }
    }

    for (const auto &sdef : sig.param_struct_defs) {
      if (sdef) {
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
        }
      }
    }

    return h;
  }
};

class FFILibrary {
public:
  FFILibrary(const std::string &path) : lib_path(path), handle(nullptr) {
    // try to load the library
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

  bool is_loaded() const { return handle != nullptr; }
  std::string get_error() const { return error; }
  const std::vector<std::string> &get_symbols() const { return symbols; }

  // get a function pointer by name
  void *get_symbol(const std::string &name) {
    if (!handle)
      return nullptr;

    dlerror(); // clear any existing error
    void *sym = dlsym(handle, name.c_str());

    if (!sym) {
      const char *err = dlerror();
      if (err) {
        error = "Symbol not found: " + name + " - " + err;
      }
    }

    return sym;
  }

private:
  void enumerate_symbols();

  std::string lib_path;
  void *handle;
  std::string error;
  std::vector<std::string> symbols;
};

// FFI functions (using libffi)
class FFICaller {
public:
  static Value call_function(void *func_ptr, const FFISignature &sig,
                             const std::vector<Value> &args);

  static Value call_function_variadic(void *func_ptr, const FFISignature &sig,
                                      const std::vector<Value> &args);

private:
  struct CIFCache {
    ffi_cif cif;
    std::vector<ffi_type *> param_types;
  };

  static std::unordered_map<FFISignature, CIFCache, FFISignatureHash>
      cif_cache_;
  static std::unordered_map<FFISignature, CIFCache, FFISignatureHash>
      cif_variadic_cache_;

  static CIFCache &get_or_create_cif(const FFISignature &sig);
  static CIFCache &get_or_create_cif_variadic(const FFISignature &sig);
};

// struct marshalling helpers
std::shared_ptr<FFIStructDef>
create_struct_def_from_type(const std::string &type_name);
size_t get_struct_size(const std::string &type_name);
Value read_struct_from_memory(void *ptr, const std::string &type_name);
void write_struct_to_memory(void *ptr, const std::string &type_name,
                            const Value &obj);

// FFI Callback support for creating native function pointers from Nari
// functions
struct FFICallback {
  ffi_closure *closure;
  void *executable_code;
  FFISignature signature;
  Value nari_function;    // function to call
  ScriptRuntime *runtime; // runtime context
  ffi_cif cif;
  std::vector<ffi_type *> param_types;

  FFICallback()
      : closure(nullptr), executable_code(nullptr), runtime(nullptr) {}

  ~FFICallback();
};

// callback management
class FFICallbackManager {
public:
  static FFICallbackManager &instance() {
    static FFICallbackManager manager;
    return manager;
  }

  void *create_callback(const FFISignature &sig, const Value &nari_func,
                        ScriptRuntime *runtime);
  void free_callback(void *code_ptr);

  FFICallback *get_callback(void *code_ptr);

private:
  FFICallbackManager() = default;
  std::unordered_map<void *, std::unique_ptr<FFICallback>> callbacks_;

  // Trampoline function that bridges C -> Nari calls
  static void callback_trampoline(ffi_cif *cif, void *ret, void **args,
                                  void *user_data);
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

#endif // NO_FFI
