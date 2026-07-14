#ifndef DISABLE_FFI

#include "core_types.h"
#include "nari_ffi.h"
#include "nari_fs.h"
#include "parser_api.h"
#include "runtime.h"
#include <algorithm>
#include <ffi.h>
#include <functional>
#include <limits>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#if !defined(__APPLE__)
#include <elf.h>
#endif
#endif
#include <cstring>

namespace nari {

// static caches for prepared CIF structures

std::unordered_map<FFISignature, FFICaller::CIFCache, FFISignatureHash> FFICaller::cif_cache;
std::unordered_map<FFISignature, FFICaller::CIFCache, FFISignatureHash> FFICaller::cif_variadic_cache;

void *managed_struct_pointer(const Value &value) {
    if (!value.is_object()) {
        return nullptr;
    }
    const ObjectObj *owner = value.get_obj_ptr();
    if (!owner->is_managed_native_struct()) {
        return nullptr;
    }
    size_t expected_size = get_struct_size(owner->native_struct_type);
    if (expected_size == 0 || expected_size != owner->native_struct_storage.size()) {
        fprintf(stderr, "ERROR: Managed FFI struct type or size is invalid\n");
        return nullptr;
    }
    return const_cast<uint8_t *>(owner->native_struct_storage.data());
}

void FFILibrary::enumerate_symbols() {
    if (!handle) {
        return;
    }

    symbols.clear();

#ifdef _WIN32
    auto *module_base = reinterpret_cast<const uint8_t *>(handle);
    const auto *dos_header = reinterpret_cast<const IMAGE_DOS_HEADER *>(module_base);

    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    const auto *nt_header = reinterpret_cast<const IMAGE_NT_HEADERS *>(
        module_base + dos_header->e_lfanew);
    if (nt_header->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    const auto &export_data = nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (export_data.VirtualAddress == 0 || export_data.Size == 0) {
        return;
    }

    const auto *export_dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(
        module_base + export_data.VirtualAddress);
    if (!export_dir || export_dir->NumberOfNames == 0 ||
        export_dir->AddressOfNames == 0) {
        return;
    }

    const auto *name_rvas = reinterpret_cast<const DWORD *>(module_base + export_dir->AddressOfNames);
    symbols.reserve(export_dir->NumberOfNames);

    for (DWORD i = 0; i < export_dir->NumberOfNames; ++i) {
        DWORD name_rva = name_rvas[i];
        if (name_rva == 0) {
            continue;
        }

        const char *symbol_name = reinterpret_cast<const char *>(module_base + name_rva);
        if (symbol_name && symbol_name[0] != '\0') {
            symbols.emplace_back(symbol_name);
        }
    }
#elif defined(__APPLE__)
    // macOS: symbol enumeration from Mach-O not yet implemented.
    // FFI calls still work via dlsym; only auto-discovery is unavailable.
    (void)lib_path;
    (void)error;
#else
    FILE *file = fopen(lib_path.c_str(), "rb");
    if (!file) {
        error = "Could not open library file for symbol enumeration";
        return;
    }

    Elf64_Ehdr ehdr;
    size_t bytes_read = fread(&ehdr, 1, sizeof(ehdr), file);

    if (bytes_read != sizeof(ehdr)) {
        fclose(file);
        fprintf(stderr, "Failed to read ELF header from %s\n", lib_path.c_str());
        return;
    }

    if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        fclose(file);
        fprintf(stderr, "Not an ELF file: %s\n", lib_path.c_str());
        return;
    }

    // read section headers
    fseek(file, ehdr.e_shoff, SEEK_SET);
    std::vector<Elf64_Shdr> section_headers(ehdr.e_shnum);
    fread(section_headers.data(), sizeof(Elf64_Shdr), ehdr.e_shnum, file);

    // get string table for section names
    if (ehdr.e_shstrndx >= section_headers.size()) {
        fclose(file);
        return;
    }

    Elf64_Shdr &shstrtab = section_headers[ehdr.e_shstrndx];
    std::vector<char> section_names(shstrtab.sh_size);
    fseek(file, shstrtab.sh_offset, SEEK_SET);
    fread(section_names.data(), 1, shstrtab.sh_size, file);

    // find .dynsym and .dynstr sections
    Elf64_Shdr *dynsym_section = nullptr;
    Elf64_Shdr *dynstr_section = nullptr;

    for (auto &shdr : section_headers) {
        if (shdr.sh_name < section_names.size()) {
            const char *name = &section_names[shdr.sh_name];
            if (std::strcmp(name, ".dynsym") == 0) {
                dynsym_section = &shdr;
            } else if (std::strcmp(name, ".dynstr") == 0) {
                dynstr_section = &shdr;
            }
        }
    }

    if (!dynsym_section || !dynstr_section) {
        fclose(file);
        return; // no dynamic symbols
    }

    // .dynstr
    std::vector<char> dynstr(dynstr_section->sh_size);
    fseek(file, dynstr_section->sh_offset, SEEK_SET);
    fread(dynstr.data(), 1, dynstr_section->sh_size, file);

    // .dynsym
    size_t num_symbols = dynsym_section->sh_size / sizeof(Elf64_Sym);
    std::vector<Elf64_Sym> dynsym(num_symbols);
    fseek(file, dynsym_section->sh_offset, SEEK_SET);
    fread(dynsym.data(), sizeof(Elf64_Sym), num_symbols, file);

    // get those symbol names
    for (const auto &sym : dynsym) {
        if (sym.st_shndx == SHN_UNDEF) {
            continue;
        }
        if (ELF64_ST_BIND(sym.st_info) == STB_LOCAL) {
            continue;
        }

        int sym_type = ELF64_ST_TYPE(sym.st_info);

        // only functions and vars (objects)
        if (sym_type != STT_FUNC && sym_type != STT_OBJECT) {
            continue;
        }

        if (sym.st_name < dynstr.size()) {
            const char *name = &dynstr[sym.st_name];
            if (name && name[0] != '\0') {
                symbols.push_back(name);
            }
        }
    }

    fclose(file);
#endif // _WIN32
}

static ffi_type *get_ffi_type(FFIType type) {
    switch (type) {
        case FFIType::Void:
            return &ffi_type_void;
        case FFIType::Int8:
            return &ffi_type_sint8;
        case FFIType::UInt8:
            return &ffi_type_uint8;
        case FFIType::Int16:
            return &ffi_type_sint16;
        case FFIType::UInt16:
            return &ffi_type_uint16;
        case FFIType::Int32:
            return &ffi_type_sint32;
        case FFIType::Int64:
            return &ffi_type_sint64;
        case FFIType::UInt32:
            return &ffi_type_uint32;
        case FFIType::UInt64:
            return &ffi_type_uint64;
        case FFIType::Float:
            return &ffi_type_float;
        case FFIType::Double:
            return &ffi_type_double;
        case FFIType::Bool:
            return &ffi_type_uchar;
        case FFIType::Pointer:
            return &ffi_type_pointer;
        case FFIType::Struct:
            return nullptr; // structs need custom handling
        default:
            return &ffi_type_void;
    }
}

static ffi_type *get_ffi_struct_type(FFIStructDef *struct_def) {
    if (!struct_def) {
        return &ffi_type_void;
    }

    if (struct_def->ffi_struct_type) {
        return struct_def->ffi_struct_type;
    }

    ffi_type *ffi_struct = new ffi_type();
    ffi_struct->type = FFI_TYPE_STRUCT;
    ffi_struct->size = 0;
    ffi_struct->alignment = 0;

    size_t element_count = 0;
    for (const auto &field : struct_def->fields) {
        ffi_type *field_type = field.aggregate_def
                                   ? get_ffi_struct_type(field.aggregate_def.get())
                                   : get_ffi_type(field.type);
        if (field.count == 0 || field.count > std::numeric_limits<size_t>::max() - element_count ||
            !field_type) {
            delete ffi_struct;
            return nullptr;
        }
        element_count += field.count;
    }
    if (element_count == std::numeric_limits<size_t>::max()) {
        delete ffi_struct;
        return nullptr;
    }
    if (struct_def->is_union) {
        ffi_type *alignment_carrier = nullptr;
        size_t union_size = 0;
        for (const auto &field : struct_def->fields) {
            ffi_type *field_type = field.aggregate_def
                                       ? get_ffi_struct_type(field.aggregate_def.get())
                                       : get_ffi_type(field.type);
            if (!field_type || field_type->size == 0 ||
                field.count > std::numeric_limits<size_t>::max() / field_type->size) {
                delete ffi_struct;
                return nullptr;
            }
            union_size = std::max(union_size, field_type->size * field.count);
            if (!alignment_carrier || field_type->alignment > alignment_carrier->alignment) {
                alignment_carrier = field_type;
            }
        }
        if (!alignment_carrier) {
            delete ffi_struct;
            return nullptr;
        }
        struct_def->ffi_field_types.reserve(union_size + 1);
        struct_def->ffi_field_types.push_back(alignment_carrier);
        for (size_t i = alignment_carrier->size; i < union_size; i++) {
            struct_def->ffi_field_types.push_back(&ffi_type_uint8);
        }
    } else {
        struct_def->ffi_field_types.reserve(element_count + 1);
        for (const auto &field : struct_def->fields) {
            ffi_type *field_type = field.aggregate_def
                                       ? get_ffi_struct_type(field.aggregate_def.get())
                                       : get_ffi_type(field.type);
            for (size_t i = 0; field_type && i < field.count; i++) {
                struct_def->ffi_field_types.push_back(field_type);
            }
        }
    }

    struct_def->ffi_field_types.push_back(nullptr);

    ffi_struct->elements = struct_def->ffi_field_types.data();

    struct_def->ffi_struct_type = ffi_struct;
    const size_t carrier_count = struct_def->ffi_field_types.size() - 1;
    std::vector<size_t> element_offsets(carrier_count);
    if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, ffi_struct, element_offsets.data()) != FFI_OK) {
        delete ffi_struct;
        struct_def->ffi_struct_type = nullptr;
        struct_def->ffi_field_types.clear();
        return nullptr;
    }
    struct_def->ffi_field_offsets.reserve(struct_def->fields.size());
    if (struct_def->is_union) {
        struct_def->ffi_field_offsets.assign(struct_def->fields.size(), 0);
    } else {
        size_t element_index = 0;
        for (const auto &field : struct_def->fields) {
            struct_def->ffi_field_offsets.push_back(element_offsets[element_index]);
            element_index += field.count;
        }
    }
    return ffi_struct;
}

static Value read_ffi_scalar(const char *ptr, FFIType type) {
    switch (type) {
        case FFIType::Int8: {
            int8_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(v);
        }
        case FFIType::UInt8: {
            uint8_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(v);
        }
        case FFIType::Int16: {
            int16_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(v);
        }
        case FFIType::UInt16: {
            uint16_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(v);
        }
        case FFIType::Int32: {
            int32_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(v);
        }
        case FFIType::Int64: {
            int64_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(v);
        }
        case FFIType::UInt32: {
            uint32_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(v);
        }
        case FFIType::UInt64: {
            uint64_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(static_cast<int64_t>(v));
        }
        case FFIType::Float: {
            float v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_float(v);
        }
        case FFIType::Double: {
            double v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_float(v);
        }
        case FFIType::Bool: {
            uint8_t v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_bool(v != 0);
        }
        case FFIType::Pointer: {
            void *v;
            memcpy(&v, ptr, sizeof(v));
            return Value::make_int(reinterpret_cast<int64_t>(v));
        }
        default:
            return Value::none();
    }
}

static void write_ffi_scalar(char *ptr, FFIType type, const Value &value,
                             std::deque<std::string> *strings = nullptr) {
    switch (type) {
        case FFIType::Int8: {
            int8_t v = value.is_int() ? value.get_int() : value.is_float() ? value.get_float()
                                                                           : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::UInt8: {
            uint8_t v = value.is_int() ? value.get_int() : value.is_float()                  ? value.get_float()
                                                       : value.is_bool() && value.get_bool() ? 1
                                                                                             : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::Int16: {
            int16_t v = value.is_int() ? value.get_int() : value.is_float() ? value.get_float()
                                                                            : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::UInt16: {
            uint16_t v = value.is_int() ? value.get_int() : value.is_float()                  ? value.get_float()
                                                        : value.is_bool() && value.get_bool() ? 1
                                                                                              : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::Int32: {
            int32_t v = value.is_int() ? value.get_int() : value.is_float() ? value.get_float()
                                                                            : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::Int64: {
            int64_t v = value.is_int() ? value.get_int() : value.is_float() ? value.get_float()
                                                                            : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::UInt32: {
            uint32_t v = value.is_int() ? value.get_int() : value.is_float()                  ? value.get_float()
                                                        : value.is_bool() && value.get_bool() ? 1
                                                                                              : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::UInt64: {
            uint64_t v = value.is_int() ? value.get_int() : value.is_float()                  ? value.get_float()
                                                        : value.is_bool() && value.get_bool() ? 1
                                                                                              : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::Float: {
            float v = value.is_float() ? value.get_float() : value.is_int() ? value.get_int()
                                                                            : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::Double: {
            double v = value.is_float() ? value.get_float() : value.is_int() ? value.get_int()
                                                                             : 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::Bool: {
            uint8_t v = value.is_bool() ? value.get_bool() : value.is_int() && value.get_int() != 0;
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        case FFIType::Pointer: {
            void *v = nullptr;
            if (value.is_string() && strings) {
                strings->push_back(value.get_string());
                v = const_cast<char *>(strings->back().c_str());
            } else if (value.is_int()) {
                v = value.get_ptr();
            } else {
                v = managed_struct_pointer(value);
            }
            memcpy(ptr, &v, sizeof(v));
            break;
        }
        default:
            break;
    }
}

static bool write_ffi_struct(char *buffer, const FFIStructDef &def, const Value &object,
                             std::deque<std::string> *strings = nullptr) {
    if (!object.is_object() || def.ffi_field_offsets.size() != def.fields.size()) {
        return false;
    }
    const ObjectObj *obj = object.get_obj_ptr();
    for (const auto &field : def.fields) {
        if (field.count == 1) {
            continue;
        }
        const Value *field_value = obj->get_field(field.name);
        if (field_value && (!field_value->is_array() || field_value->get_array().size() != field.count)) {
            fprintf(stderr, "ERROR: FFI struct field '%s' requires exactly %zu array elements\n", field.name.c_str(), field.count);
            return false;
        }
    }
    for (size_t field_index = 0; field_index < def.fields.size(); field_index++) {
        const auto &field = def.fields[field_index];
        const Value *field_value = obj->get_field(field.name);
        if (!field_value) {
            continue;
        }
        ffi_type *element_type = field.aggregate_def
                                     ? get_ffi_struct_type(field.aggregate_def.get())
                                     : get_ffi_type(field.type);
        const size_t stride = element_type ? element_type->size : 0;
        if (field.count == 1) {
            if (field.aggregate_def) {
                if (!write_ffi_struct(buffer + def.ffi_field_offsets[field_index], *field.aggregate_def,
                                      *field_value, strings)) {
                    return false;
                }
            } else {
                write_ffi_scalar(buffer + def.ffi_field_offsets[field_index], field.type, *field_value, strings);
            }
            continue;
        }
        const auto &values = field_value->get_array();
        for (size_t i = 0; i < field.count; i++) {
            char *element = buffer + def.ffi_field_offsets[field_index] + i * stride;
            if (field.aggregate_def) {
                if (!write_ffi_struct(element, *field.aggregate_def, values[i], strings)) {
                    return false;
                }
            } else {
                write_ffi_scalar(element, field.type, values[i], strings);
            }
        }
    }
    return true;
}

static Value read_ffi_struct(const char *buffer, const FFIStructDef &def) {
    Value result = Value::make_object();
    ObjectObj *obj = result.get_obj_ptr();
    for (size_t field_index = 0; field_index < def.fields.size(); field_index++) {
        const auto &field = def.fields[field_index];
        ffi_type *element_type = field.aggregate_def
                                     ? get_ffi_struct_type(field.aggregate_def.get())
                                     : get_ffi_type(field.type);
        const size_t stride = element_type ? element_type->size : 0;
        if (field.count == 1) {
            obj->set_field(field.name, field.aggregate_def
                                           ? read_ffi_struct(buffer + def.ffi_field_offsets[field_index], *field.aggregate_def)
                                           : read_ffi_scalar(buffer + def.ffi_field_offsets[field_index], field.type));
            continue;
        }
        std::vector<Value> values;
        values.reserve(field.count);
        for (size_t i = 0; i < field.count; i++) {
            const char *element = buffer + def.ffi_field_offsets[field_index] + i * stride;
            values.push_back(field.aggregate_def ? read_ffi_struct(element, *field.aggregate_def)
                                                 : read_ffi_scalar(element, field.type));
        }
        obj->set_field(field.name, Value::make_array(std::move(values)));
    }
    return result;
}

FFICaller::CIFCache &FFICaller::get_or_create_cif(const FFISignature &sig) {
    auto it = cif_cache.find(sig);
    if (it != cif_cache.end()) {
        return it->second;
    }

    // create new CIF
    CIFCache cache;
    cache.param_types.reserve(sig.param_types.size());
    for (size_t i = 0; i < sig.param_types.size(); i++) {
        FFIType type = sig.param_types[i];
        if (type == FFIType::Struct && i < sig.param_struct_defs.size()) {
            cache.param_types.push_back(get_ffi_struct_type(sig.param_struct_defs[i].get()));
        } else {
            cache.param_types.push_back(get_ffi_type(type));
        }
    }

    ffi_type *return_type;
    if (sig.return_type == FFIType::Struct && sig.return_struct_def) {
        return_type = get_ffi_struct_type(sig.return_struct_def.get());
    } else {
        return_type = get_ffi_type(sig.return_type);
    }

    ffi_status status =
        ffi_prep_cif(&cache.cif, FFI_DEFAULT_ABI, cache.param_types.size(), return_type, cache.param_types.data());

    if (status != FFI_OK) {
        static CIFCache error_cache;
        return error_cache;
    }

    cache.prepared = true;
    return cif_cache.emplace(sig, std::move(cache)).first->second;
}

FFICaller::CIFCache &FFICaller::get_or_create_cif_variadic(const FFISignature &sig) {
    auto it = cif_variadic_cache.find(sig);
    if (it != cif_variadic_cache.end()) {
        return it->second;
    }

    CIFCache cache;
    cache.param_types.reserve(sig.param_types.size());
    for (FFIType type : sig.param_types) {
        cache.param_types.push_back(get_ffi_type(type));
    }

    ffi_type *return_type = get_ffi_type(sig.return_type);
    ffi_status status = ffi_prep_cif_var(
        &cache.cif, FFI_DEFAULT_ABI, sig.fixed_param_count,
        cache.param_types.size(), return_type, cache.param_types.data());

    if (status != FFI_OK) {
        static CIFCache error_cache;
        return error_cache;
    }

    cache.prepared = true;
    return cif_variadic_cache.emplace(sig, std::move(cache)).first->second;
}

// Direct call fast path
// Eliminates libffi overhead for non-variadic, non-struct signatures with <= 6 params.

static intptr_t value_to_gp(const Value &v, FFIType t) {
    switch (t) {
        case FFIType::Bool:
            if (v.is_bool()) {
                return v.get_bool() ? 1 : 0;
            }
            if (v.is_int()) {
                return v.get_int() != 0 ? 1 : 0;
            }
            return 0;
        case FFIType::Pointer:
            return v.is_int() ? static_cast<intptr_t>(v.get_ptr_bits())
                              : reinterpret_cast<intptr_t>(managed_struct_pointer(v)); // strings handled by caller
        default:                                                                       // all integer types
            return v.is_int() ? v.get_int() : (intptr_t)v.get_float();
    }
}

static float value_to_f32(const Value &v) {
    return v.is_float() ? (float)v.get_float() : (float)v.get_int();
}

static int classify_return(FFIType t) {
    switch (t) {
        case FFIType::Void:
            return 0;
        case FFIType::Float:
            return 2;
        case FFIType::Bool:
            return 3;
        default:
            return 1; // all integer/pointer types -> make_int
    }
}

// Template-recursive dispatch: at each depth, branch on GP vs float
// to accumulate the correct C++ parameter types in the pack.
// At the base case (Depth==MaxArgs), cast func_ptr to the exact
// function pointer type and call directly, with no libffi overhead.
template <int Depth, int MaxArgs, typename... Acc>
static Value dc_call(void *fp, int rk, const int *ak, const intptr_t *gp, const float *fv, Acc... a) {
    if constexpr (Depth == MaxArgs) {
        switch (rk) {
            case 0: // void
                reinterpret_cast<void (*)(Acc...)>(fp)(a...);
                return Value::none();
            case 1: // int/pointer -> make_int
                return Value::make_int((int64_t)reinterpret_cast<intptr_t (*)(Acc...)>(fp)(a...));
            case 2: // float -> make_float
                return Value::make_float((double)reinterpret_cast<float (*)(Acc...)>(fp)(a...));
            case 3: // bool -> make_bool
                return Value::make_bool(reinterpret_cast<intptr_t (*)(Acc...)>(fp)(a...) != 0);
            default:
                return Value::none();
        }
    } else {
        if (ak[Depth] == 0) { // GP arg
            return dc_call<Depth + 1, MaxArgs, Acc..., intptr_t>(fp, rk, ak, gp, fv, a..., gp[Depth]);
        } else { // float arg
            return dc_call<Depth + 1, MaxArgs, Acc..., float>(fp, rk, ak, gp, fv, a..., fv[Depth]);
        }
    }
}

static bool try_direct_call(void *func_ptr, const FFISignature &sig, const std::vector<Value> &args, Value &out) {
    size_t n = sig.param_types.size();
    if (n > 6) {
        return false;
    }
    if (sig.return_type == FFIType::Struct || sig.return_type == FFIType::Double) {
        return false;
    }

    intptr_t gp[6] = {};
    float fv[6] = {};
    int ak[6] = {};
    std::string str_keep[6]; // prevent string temporaries from dying

    for (size_t i = 0; i < n; i++) {
        FFIType t = sig.param_types[i];
        if (t == FFIType::Float) {
            ak[i] = 1;
            fv[i] = value_to_f32(args[i]);
        } else if (t == FFIType::Double || t == FFIType::Struct) {
            return false; // unsupported, fall back to libffi
        } else {
            ak[i] = 0;
            if (t == FFIType::Pointer && i < args.size() && args[i].is_string()) {
                str_keep[i] = args[i].get_string();
                gp[i] = (intptr_t)str_keep[i].c_str();
            } else {
                gp[i] = i < args.size() ? value_to_gp(args[i], t) : 0;
            }
        }
    }

    int rk = classify_return(sig.return_type);

    switch (n) {
        case 0:
            out = dc_call<0, 0>(func_ptr, rk, ak, gp, fv);
            return true;
        case 1:
            out = dc_call<0, 1>(func_ptr, rk, ak, gp, fv);
            return true;
        case 2:
            out = dc_call<0, 2>(func_ptr, rk, ak, gp, fv);
            return true;
        case 3:
            out = dc_call<0, 3>(func_ptr, rk, ak, gp, fv);
            return true;
        case 4:
            out = dc_call<0, 4>(func_ptr, rk, ak, gp, fv);
            return true;
        case 5:
            out = dc_call<0, 5>(func_ptr, rk, ak, gp, fv);
            return true;
        case 6:
            out = dc_call<0, 6>(func_ptr, rk, ak, gp, fv);
            return true;
        default:
            return false;
    }
}

static bool contains_union(const std::shared_ptr<FFIStructDef> &def) {
    if (!def) {
        return false;
    }
    if (def->is_union) {
        return true;
    }
    return std::any_of(def->fields.begin(), def->fields.end(),
                       [](const FFIStructField &field) { return contains_union(field.aggregate_def); });
}

Value FFICaller::call_function(void *func_ptr, const FFISignature &sig, const std::vector<Value> &args) {
    if (!func_ptr) {
        return Value::none();
    }
    if (contains_union(sig.return_struct_def) ||
        std::any_of(sig.param_struct_defs.begin(), sig.param_struct_defs.end(),
                    [](const auto &def) { return contains_union(def); })) {
        fprintf(stderr, "ERROR: Passing unions or aggregates containing unions by value is not supported portably; pass a pointer instead\n");
        return Value::none();
    }

    // Fast path: direct C call without libffi for simple signatures
    if (!sig.is_variadic) {
        Value direct_result;
        if (try_direct_call(func_ptr, sig, args, direct_result)) {
            return direct_result;
        }
    }

    CIFCache &cache = get_or_create_cif(sig);
    if (!cache.prepared) {
        fprintf(stderr, "ERROR: Could not prepare FFI call interface\n");
        return Value::none();
    }

    FFIArgStorage storage;
    storage.clear_and_reserve(args.size());

    auto &string_storage = storage.string_storage;
    auto &int32_storage = storage.int32_storage;
    auto &int64_storage = storage.int64_storage;
    auto &int8_storage = storage.int8_storage;
    auto &uint8_storage = storage.uint8_storage;
    auto &int16_storage = storage.int16_storage;
    auto &uint16_storage = storage.uint16_storage;
    auto &uint32_storage = storage.uint32_storage;
    auto &uint64_storage = storage.uint64_storage;
    auto &float_storage = storage.float_storage;
    auto &double_storage = storage.double_storage;
    auto &bool_storage = storage.bool_storage;
    auto &pointer_storage = storage.pointer_storage;
    auto &struct_storage = storage.struct_storage;
    auto &arg_pointers = storage.arg_pointers;

    for (size_t i = 0; i < args.size() && i < sig.param_types.size(); i++) {
        const Value &arg = args[i];
        FFIType type = sig.param_types[i];

        switch (type) {
            case FFIType::Int8: {
                int8_t val = arg.is_int()     ? static_cast<int8_t>(arg.get_int())
                             : arg.is_float() ? static_cast<int8_t>(arg.get_float())
                                              : 0;
                int8_storage.push_back(val);
                arg_pointers.push_back(&int8_storage.back());
                break;
            }

            case FFIType::UInt8: {
                uint8_t val = arg.is_int()     ? static_cast<uint8_t>(arg.get_int())
                              : arg.is_float() ? static_cast<uint8_t>(arg.get_float())
                              : arg.is_bool()  ? (arg.get_bool() ? 1u : 0u)
                                               : 0u;
                uint8_storage.push_back(val);
                arg_pointers.push_back(&uint8_storage.back());
                break;
            }

            case FFIType::Int16: {
                int16_t val = arg.is_int()     ? static_cast<int16_t>(arg.get_int())
                              : arg.is_float() ? static_cast<int16_t>(arg.get_float())
                                               : 0;
                int16_storage.push_back(val);
                arg_pointers.push_back(&int16_storage.back());
                break;
            }

            case FFIType::UInt16: {
                uint16_t val = arg.is_int()     ? static_cast<uint16_t>(arg.get_int())
                               : arg.is_float() ? static_cast<uint16_t>(arg.get_float())
                               : arg.is_bool()  ? (arg.get_bool() ? 1u : 0u)
                                                : 0u;
                uint16_storage.push_back(val);
                arg_pointers.push_back(&uint16_storage.back());
                break;
            }

            case FFIType::Int32: {
                int32_t val = arg.is_int()     ? arg.get_int()
                              : arg.is_float() ? (int32_t)arg.get_float()
                                               : 0;
                int32_storage.push_back(val);
                arg_pointers.push_back(&int32_storage.back());
                break;
            }

            case FFIType::Int64: {
                int64_t val = arg.is_int()     ? arg.get_int()
                              : arg.is_float() ? (int64_t)arg.get_float()
                                               : 0;
                int64_storage.push_back(val);
                arg_pointers.push_back(&int64_storage.back());
                break;
            }

            case FFIType::UInt32: {
                uint32_t val = arg.is_int()     ? static_cast<uint32_t>(arg.get_int())
                               : arg.is_float() ? static_cast<uint32_t>(arg.get_float())
                               : arg.is_bool()  ? (arg.get_bool() ? 1u : 0u)
                                                : 0u;
                uint32_storage.push_back(val);
                arg_pointers.push_back(&uint32_storage.back());
                break;
            }

            case FFIType::UInt64: {
                uint64_t val = arg.is_int()     ? static_cast<uint64_t>(arg.get_int())
                               : arg.is_float() ? static_cast<uint64_t>(arg.get_float())
                               : arg.is_bool()  ? (arg.get_bool() ? 1ull : 0ull)
                                                : 0ull;
                uint64_storage.push_back(val);
                arg_pointers.push_back(&uint64_storage.back());
                break;
            }

            case FFIType::Float: {
                float val = arg.is_float() ? (float)arg.get_float()
                            : arg.is_int() ? (float)arg.get_int()
                                           : 0.0f;
                float_storage.push_back(val);
                arg_pointers.push_back(&float_storage.back());
                break;
            }

            case FFIType::Double: {
                double val = arg.is_float() ? arg.get_float()
                             : arg.is_int() ? (double)arg.get_int()
                                            : 0.0;
                double_storage.push_back(val);
                arg_pointers.push_back(&double_storage.back());
                break;
            }

            case FFIType::Bool: {
                uint8_t val = arg.is_bool()  ? (arg.get_bool() ? 1 : 0)
                              : arg.is_int() ? (arg.get_int() != 0 ? 1 : 0)
                                             : 0;
                bool_storage.push_back(val);
                arg_pointers.push_back(&bool_storage.back());
                break;
            }

            case FFIType::Pointer: {
                void *ptr = nullptr;
                if (arg.is_string()) {
                    string_storage.push_back(arg.get_string());
                    ptr = (char *)string_storage.back().c_str();
                } else if (arg.is_int()) {
                    ptr = arg.get_ptr();
                } else {
                    ptr = managed_struct_pointer(arg);
                }
                pointer_storage.push_back(ptr);
                arg_pointers.push_back(&pointer_storage.back());
                break;
            }

            case FFIType::Struct: {
                // marshal struct from Nari object
                if (i >= sig.param_struct_defs.size() || !sig.param_struct_defs[i]) {
                    fprintf(stderr, "ERROR: No struct definition for parameter %zu\n", i);
                    break;
                }

                auto &struct_def = sig.param_struct_defs[i];

                if (!arg.is_object()) {
                    fprintf(stderr, "ERROR: Expected object for struct parameter %zu\n", i);
                    break;
                }

                ffi_type *ffi_struct = get_ffi_struct_type(struct_def.get());
                if (!ffi_struct || ffi_struct->size == 0) {
                    fprintf(stderr, "ERROR: Could not compute FFI struct layout for parameter %zu\n", i);
                    break;
                }
                struct_storage.emplace_back(ffi_struct->size, 0);
                char *struct_buf = struct_storage.back().data();
                if (!write_ffi_struct(struct_buf, *struct_def, arg, &string_storage)) {
                    fprintf(stderr, "ERROR: Could not marshal FFI struct parameter %zu\n", i);
                    break;
                }

                arg_pointers.push_back(struct_buf);
                break;
            }

            case FFIType::Void:
                break;
        }
    }

    union {
        int8_t i8;
        uint8_t u8_int;
        int16_t i16;
        uint16_t u16;
        int32_t i32;
        int64_t i64;
        uint32_t u32;
        uint64_t u64;
        float f32;
        double f64;
        uint8_t u8;
        void *ptr;
    } return_value;

    std::vector<char> struct_return_storage;
    void *return_ptr = &return_value;
    if (sig.return_type == FFIType::Struct) {
        ffi_type *ffi_struct = get_ffi_struct_type(sig.return_struct_def.get());
        if (!ffi_struct || ffi_struct->size == 0) {
            return Value::none();
        }
        struct_return_storage.resize(ffi_struct->size);
        return_ptr = struct_return_storage.data();
    }

    if (arg_pointers.size() != cache.param_types.size()) {
        fprintf(stderr, "ERROR: Could not marshal all FFI call arguments\n");
        return Value::none();
    }

    ffi_call(&cache.cif, FFI_FN(func_ptr), return_ptr, arg_pointers.data());

    switch (sig.return_type) {
        case FFIType::Void:
            return Value::none();
        case FFIType::Int8:
            return Value::make_int(return_value.i8);
        case FFIType::UInt8:
            return Value::make_int(static_cast<int64_t>(return_value.u8_int));
        case FFIType::Int16:
            return Value::make_int(return_value.i16);
        case FFIType::UInt16:
            return Value::make_int(static_cast<int64_t>(return_value.u16));
        case FFIType::Int32:
            return Value::make_int(return_value.i32);
        case FFIType::Int64:
            return Value::make_int(return_value.i64);
        case FFIType::UInt32:
            return Value::make_int(static_cast<int64_t>(return_value.u32));
        case FFIType::UInt64:
            return Value::make_int(static_cast<int64_t>(return_value.u64));
        case FFIType::Float:
            return Value::make_float(return_value.f32);
        case FFIType::Double:
            return Value::make_float(return_value.f64);
        case FFIType::Bool:
            return Value::make_bool(return_value.u8 != 0);
        case FFIType::Pointer:
            return Value::make_int((int64_t)return_value.ptr);
        case FFIType::Struct: {
            if (!sig.return_struct_def) {
                return Value::none();
            }
            return read_ffi_struct(struct_return_storage.data(), *sig.return_struct_def);
        }
    }

    return Value::none();
}

Value FFICaller::call_function_variadic(void *func_ptr, const FFISignature &sig, const std::vector<Value> &args) {
    if (!func_ptr) {
        return Value::none();
    }
    if (contains_union(sig.return_struct_def) ||
        std::any_of(sig.param_struct_defs.begin(), sig.param_struct_defs.end(),
                    [](const auto &def) { return contains_union(def); })) {
        fprintf(stderr, "ERROR: Passing unions or aggregates containing unions by value is not supported portably; pass a pointer instead\n");
        return Value::none();
    }
    if (sig.return_type == FFIType::Struct ||
        std::find(sig.param_types.begin(), sig.param_types.end(), FFIType::Struct) != sig.param_types.end()) {
        fprintf(stderr, "ERROR: Struct parameters and returns are not supported in variadic FFI calls\n");
        return Value::none();
    }
    if (sig.fixed_param_count > sig.param_types.size() || args.size() != sig.param_types.size()) {
        fprintf(stderr, "ERROR: Invalid variadic FFI signature or argument count\n");
        return Value::none();
    }

    CIFCache &cache = get_or_create_cif_variadic(sig);
    if (!cache.prepared) {
        fprintf(stderr, "ERROR: Could not prepare variadic FFI call interface\n");
        return Value::none();
    }

    FFIArgStorage storage_v;
    storage_v.clear_and_reserve(args.size());

    auto &string_storage = storage_v.string_storage;
    auto &int32_storage = storage_v.int32_storage;
    auto &int64_storage = storage_v.int64_storage;
    auto &int8_storage = storage_v.int8_storage;
    auto &uint8_storage = storage_v.uint8_storage;
    auto &int16_storage = storage_v.int16_storage;
    auto &uint16_storage = storage_v.uint16_storage;
    auto &uint32_storage = storage_v.uint32_storage;
    auto &uint64_storage = storage_v.uint64_storage;
    auto &float_storage = storage_v.float_storage;
    auto &double_storage = storage_v.double_storage;
    auto &bool_storage = storage_v.bool_storage;
    auto &pointer_storage = storage_v.pointer_storage;
    auto &arg_pointers = storage_v.arg_pointers;

    for (size_t i = 0; i < args.size() && i < sig.param_types.size(); i++) {
        const Value &arg = args[i];
        FFIType type = sig.param_types[i];

        switch (type) {
            case FFIType::Int8: {
                int8_t val = arg.is_int()     ? static_cast<int8_t>(arg.get_int())
                             : arg.is_float() ? static_cast<int8_t>(arg.get_float())
                                              : 0;
                int8_storage.push_back(val);
                arg_pointers.push_back(&int8_storage.back());
                break;
            }

            case FFIType::UInt8: {
                uint8_t val = arg.is_int()     ? static_cast<uint8_t>(arg.get_int())
                              : arg.is_float() ? static_cast<uint8_t>(arg.get_float())
                              : arg.is_bool()  ? (arg.get_bool() ? 1u : 0u)
                                               : 0u;
                uint8_storage.push_back(val);
                arg_pointers.push_back(&uint8_storage.back());
                break;
            }

            case FFIType::Int16: {
                int16_t val = arg.is_int()     ? static_cast<int16_t>(arg.get_int())
                              : arg.is_float() ? static_cast<int16_t>(arg.get_float())
                                               : 0;
                int16_storage.push_back(val);
                arg_pointers.push_back(&int16_storage.back());
                break;
            }

            case FFIType::UInt16: {
                uint16_t val = arg.is_int()     ? static_cast<uint16_t>(arg.get_int())
                               : arg.is_float() ? static_cast<uint16_t>(arg.get_float())
                               : arg.is_bool()  ? (arg.get_bool() ? 1u : 0u)
                                                : 0u;
                uint16_storage.push_back(val);
                arg_pointers.push_back(&uint16_storage.back());
                break;
            }

            case FFIType::Int32: {
                int32_t val = arg.is_int()     ? arg.get_int()
                              : arg.is_float() ? (int32_t)arg.get_float()
                                               : 0;
                int32_storage.push_back(val);
                arg_pointers.push_back(&int32_storage.back());
                break;
            }

            case FFIType::Int64: {
                int64_t val = arg.is_int()     ? arg.get_int()
                              : arg.is_float() ? (int64_t)arg.get_float()
                                               : 0;
                int64_storage.push_back(val);
                arg_pointers.push_back(&int64_storage.back());
                break;
            }

            case FFIType::UInt32: {
                uint32_t val = arg.is_int()     ? static_cast<uint32_t>(arg.get_int())
                               : arg.is_float() ? static_cast<uint32_t>(arg.get_float())
                               : arg.is_bool()  ? (arg.get_bool() ? 1u : 0u)
                                                : 0u;
                uint32_storage.push_back(val);
                arg_pointers.push_back(&uint32_storage.back());
                break;
            }

            case FFIType::UInt64: {
                uint64_t val = arg.is_int()     ? static_cast<uint64_t>(arg.get_int())
                               : arg.is_float() ? static_cast<uint64_t>(arg.get_float())
                               : arg.is_bool()  ? (arg.get_bool() ? 1ull : 0ull)
                                                : 0ull;
                uint64_storage.push_back(val);
                arg_pointers.push_back(&uint64_storage.back());
                break;
            }

            case FFIType::Float: {
                float val = arg.is_float() ? (float)arg.get_float()
                            : arg.is_int() ? (float)arg.get_int()
                                           : 0.0f;
                float_storage.push_back(val);
                arg_pointers.push_back(&float_storage.back());
                break;
            }

            case FFIType::Double: {
                double val = arg.is_float() ? arg.get_float()
                             : arg.is_int() ? (double)arg.get_int()
                                            : 0.0;
                double_storage.push_back(val);
                arg_pointers.push_back(&double_storage.back());
                break;
            }

            case FFIType::Bool: {
                uint8_t val = arg.is_bool()  ? (arg.get_bool() ? 1 : 0)
                              : arg.is_int() ? (arg.get_int() != 0 ? 1 : 0)
                                             : 0;
                bool_storage.push_back(val);
                arg_pointers.push_back(&bool_storage.back());
                break;
            }

            case FFIType::Pointer: {
                void *ptr = nullptr;
                if (arg.is_string()) {
                    string_storage.push_back(arg.get_string());
                    ptr = (char *)string_storage.back().c_str();
                } else if (arg.is_int()) {
                    ptr = arg.get_ptr();
                } else {
                    ptr = managed_struct_pointer(arg);
                }
                pointer_storage.push_back(ptr);
                arg_pointers.push_back(&pointer_storage.back());
                break;
            }

            case FFIType::Struct:
                break;

            case FFIType::Void:
                break;
        }
    }

    union {
        int8_t i8;
        uint8_t u8_int;
        int16_t i16;
        uint16_t u16;
        int32_t i32;
        int64_t i64;
        uint32_t u32;
        uint64_t u64;
        float f32;
        double f64;
        uint8_t u8;
        void *ptr;
        char struct_buf[256];
    } return_value;

    if (arg_pointers.size() != cache.param_types.size()) {
        fprintf(stderr, "ERROR: Could not prepare variadic FFI call\n");
        return Value::none();
    }
    ffi_call(&cache.cif, FFI_FN(func_ptr), &return_value, arg_pointers.data());

    switch (sig.return_type) {
        case FFIType::Void:
            return Value::none();
        case FFIType::Int8:
            return Value::make_int(return_value.i8);
        case FFIType::UInt8:
            return Value::make_int(static_cast<int64_t>(return_value.u8_int));
        case FFIType::Int16:
            return Value::make_int(return_value.i16);
        case FFIType::UInt16:
            return Value::make_int(static_cast<int64_t>(return_value.u16));
        case FFIType::Int32:
            return Value::make_int(return_value.i32);
        case FFIType::Int64:
            return Value::make_int(return_value.i64);
        case FFIType::UInt32:
            return Value::make_int(static_cast<int64_t>(return_value.u32));
        case FFIType::UInt64:
            return Value::make_int(static_cast<int64_t>(return_value.u64));
        case FFIType::Float:
            return Value::make_float(return_value.f32);
        case FFIType::Double:
            return Value::make_float(return_value.f64);
        case FFIType::Bool:
            return Value::make_bool(return_value.u8 != 0);
        case FFIType::Pointer:
            if (return_value.ptr != nullptr) {
                const char *str = static_cast<const char *>(return_value.ptr);
                return Value::make_string(std::string(str));
            }
            return Value::make_int(0);
        case FFIType::Struct: {
            // structs returned from variadic functions (unlikely but handle anyway)
            if (!sig.return_struct_def) {
                return Value::none();
            }

            Value result_val = Value::make_object();
            ObjectObj *result_oobj2 = result_val.get_obj_ptr();
            size_t offset = 0;

            for (const auto &field : sig.return_struct_def->fields) {
                switch (field.type) {
                    case FFIType::Int8: {
                        int8_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(int8_t));
                        result_oobj2->set_field(field.name, Value::make_int(val));
                        offset += sizeof(int8_t);
                        break;
                    }
                    case FFIType::UInt8: {
                        uint8_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(uint8_t));
                        result_oobj2->set_field(field.name,
                                                Value::make_int(static_cast<int64_t>(val)));
                        offset += sizeof(uint8_t);
                        break;
                    }
                    case FFIType::Int16: {
                        int16_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(int16_t));
                        result_oobj2->set_field(field.name, Value::make_int(val));
                        offset += sizeof(int16_t);
                        break;
                    }
                    case FFIType::UInt16: {
                        uint16_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(uint16_t));
                        result_oobj2->set_field(field.name,
                                                Value::make_int(static_cast<int64_t>(val)));
                        offset += sizeof(uint16_t);
                        break;
                    }
                    case FFIType::Int32: {
                        int32_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(int32_t));
                        result_oobj2->set_field(field.name, Value::make_int(val));
                        offset += sizeof(int32_t);
                        break;
                    }
                    case FFIType::Int64: {
                        int64_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(int64_t));
                        result_oobj2->set_field(field.name, Value::make_int(val));
                        offset += sizeof(int64_t);
                        break;
                    }
                    case FFIType::UInt32: {
                        uint32_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(uint32_t));
                        result_oobj2->set_field(field.name, Value::make_int(static_cast<int64_t>(val)));
                        offset += sizeof(uint32_t);
                        break;
                    }
                    case FFIType::UInt64: {
                        uint64_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(uint64_t));
                        result_oobj2->set_field(field.name, Value::make_int(static_cast<int64_t>(val)));
                        offset += sizeof(uint64_t);
                        break;
                    }
                    case FFIType::Float: {
                        float val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(float));
                        result_oobj2->set_field(field.name, Value::make_float(val));
                        offset += sizeof(float);
                        break;
                    }
                    case FFIType::Double: {
                        double val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(double));
                        result_oobj2->set_field(field.name, Value::make_float(val));
                        offset += sizeof(double);
                        break;
                    }
                    case FFIType::Bool: {
                        uint8_t val;
                        memcpy(&val, return_value.struct_buf + offset, sizeof(uint8_t));
                        result_oobj2->set_field(field.name, Value::make_bool(val != 0));
                        offset += sizeof(uint8_t);
                        break;
                    }
                    case FFIType::Pointer: {
                        void *ptr;
                        memcpy(&ptr, return_value.struct_buf + offset, sizeof(void *));
                        result_oobj2->set_field(field.name, Value::make_int((int64_t)ptr));
                        offset += sizeof(void *);
                        break;
                    }
                    default:
                        break;
                }
            }

            return result_val;
        }
    }

    return Value::none();
}

std::shared_ptr<FFILibrary> FFIRegistry::get_library(const std::string &path) {
    auto it = libraries.find(path);
    if (it != libraries.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<FFILibrary> FFIRegistry::load_library(const std::string &path) {
    auto it = libraries.find(path);
    if (it != libraries.end()) {
        return it->second;
    }

    std::string resolved_path = find_library(path);
    if (resolved_path.empty()) {
        // if we can't find it, try the path as is
        resolved_path = path;
    }

    auto lib = std::make_shared<FFILibrary>(resolved_path);
    if (lib->is_loaded()) {
        libraries[path] = lib;
        return lib;
    }

    return nullptr;
}

void push_linux_search_paths(std::vector<std::string> &paths) {
    paths.push_back(".");

    // standard lib dirs
    paths.push_back("/usr/lib");
    paths.push_back("/usr/local/lib");
    paths.push_back("/lib");

    // lib64 variants
    paths.push_back("/usr/lib64");
    paths.push_back("/usr/local/lib64");
    paths.push_back("/lib64");

    // LD_LIBRARY_PATH
    const char *ld_path = getenv("LD_LIBRARY_PATH");
    if (ld_path) {
        std::string ld_str(ld_path);
        size_t start = 0;
        size_t end = 0;
        while ((end = ld_str.find(':', start)) != std::string::npos) {
            if (end > start) {
                paths.push_back(ld_str.substr(start, end - start));
            }
            start = end + 1;
        }
        if (start < ld_str.length()) {
            paths.push_back(ld_str.substr(start));
        }
    }
}

void push_windows_search_paths(std::vector<std::string> &paths) {
    paths.push_back(".");

    // standard lib dirs
    paths.push_back("C:\\Windows\\System32");
    paths.push_back("C:\\Windows");
    paths.push_back("C:\\Windows\\SysWOW64");

    // PATH
    const char *path = getenv("PATH");
    if (path) {
        std::string path_str(path);
        size_t start = 0;
        size_t end = 0;
        while ((end = path_str.find(';', start)) != std::string::npos) {
            if (end > start) {
                paths.push_back(path_str.substr(start, end - start));
            }
            start = end + 1;
        }
        if (start < path_str.length()) {
            paths.push_back(path_str.substr(start));
        }
    }
}

void push_macos_search_paths(std::vector<std::string> &paths) {
    paths.push_back(".");

    // standard lib dirs
    paths.push_back("/usr/lib");
    paths.push_back("/usr/local/lib");
    paths.push_back("/opt/homebrew/lib");

    auto get_path_at_env = [&paths](const char *env_var) {
        const char *env_path = getenv(env_var);
        if (env_path) {
            std::string env_str(env_path);
            size_t start = 0;
            size_t end = 0;
            while ((end = env_str.find(':', start)) != std::string::npos) {
                if (end > start) {
                    paths.push_back(env_str.substr(start, end - start));
                }
                start = end + 1;
            }
            if (start < env_str.length()) {
                paths.push_back(env_str.substr(start));
            }
        }
    };

    get_path_at_env("DYLD_LIBRARY_PATH");
    get_path_at_env("DYLD_FALLBACK_LIBRARY_PATH");
}

std::string FFIRegistry::find_library(const std::string &name) {
    std::vector<std::string> search_paths;

#if defined(__APPLE__) && defined(__MACH__)
    push_macos_search_paths(search_paths);
#elif defined(__linux__)
    push_linux_search_paths(search_paths);
#elif defined(_WIN32)
    push_windows_search_paths(search_paths);
#else
    printf("Unsupported platform for FFI! Falling back to linux semantics, but expect issues!");
    push_linux_search_paths(search_paths);
#endif

    for (const auto &search_dir : search_paths) {
        fs::Path full_path = fs::Path(search_dir) / name;
        if (fs::exists(full_path)) {
            return full_path.string();
        }
    }

    return "";
}

// helper functions for struct marshalling
std::shared_ptr<FFIStructDef>
create_struct_def_from_type(const std::string &type_name) {
    std::vector<std::string> resolving;
    std::function<std::shared_ptr<FFIStructDef>(const std::string &)> create =
        [&](const std::string &requested_name) -> std::shared_ptr<FFIStructDef> {
        const nari::TypeDecl *type_decl = Parser::get_registered_type(requested_name);

        if (!type_decl) {
            fprintf(stderr, "ERROR: Type '%s' not found in registry\n", requested_name.c_str());
            return nullptr;
        }

        // resolve aliases
        const nari::TypeDecl *resolved_decl = type_decl;
        std::vector<std::string> aliases;
        while (resolved_decl && resolved_decl->is_alias()) {
            if (std::find(aliases.begin(), aliases.end(), resolved_decl->name) != aliases.end()) {
                fprintf(stderr, "ERROR: Cyclic FFI type alias involving '%s'\n", resolved_decl->name.c_str());
                return nullptr;
            }
            aliases.push_back(resolved_decl->name);
            if (!resolved_decl->alias_target) {
                fprintf(stderr, "ERROR: Type alias '%s' has no target\n", requested_name.c_str());
                return nullptr;
            }

            const nari::TypeDecl *next_decl =
                Parser::get_registered_type(resolved_decl->alias_target->name);
            if (!next_decl) {
                fprintf(stderr, "ERROR: Cannot create struct from primitive type '%s'\n", resolved_decl->alias_target->name.c_str());
                return nullptr;
            }

            resolved_decl = next_decl;
        }

        if (!resolved_decl || resolved_decl->is_alias()) {
            fprintf(stderr, "ERROR: Could not resolve type '%s'\n", requested_name.c_str());
            return nullptr;
        }

        if (std::find(resolving.begin(), resolving.end(), resolved_decl->name) != resolving.end()) {
            fprintf(stderr, "ERROR: Recursive by-value FFI aggregate '%s' is not supported\n", resolved_decl->name.c_str());
            return nullptr;
        }
        resolving.push_back(resolved_decl->name);

        // nari type -> ffi type
        auto map_type_to_ffi = [](const std::string &nari_type) -> FFIType {
            if (nari_type == "f32" || nari_type == "float") {
                return FFIType::Float;
            }
            if (nari_type == "f64" || nari_type == "double") {
                return FFIType::Double;
            }
            if (nari_type == "i8") {
                return FFIType::Int8;
            }
            if (nari_type == "u8") {
                return FFIType::UInt8;
            }
            if (nari_type == "i16") {
                return FFIType::Int16;
            }
            if (nari_type == "u16") {
                return FFIType::UInt16;
            }
            if (nari_type == "i32" || nari_type == "int") {
                return FFIType::Int32;
            }
            if (nari_type == "i64" || nari_type == "long") {
                return FFIType::Int64;
            }
            if (nari_type == "u32" || nari_type == "uint") {
                return FFIType::UInt32;
            }
            if (nari_type == "u64" || nari_type == "ulong") {
                return FFIType::UInt64;
            }
            if (nari_type == "bool" || nari_type == "boolean") {
                return FFIType::Bool;
            }
            if (nari_type == "string" || nari_type == "pointer") {
                return FFIType::Pointer;
            }
            return FFIType::Int32;
        };

        std::vector<FFIStructField> fields;
        for (const auto &field : resolved_decl->fields) {
            if (field.type->is_array) {
                fprintf(stderr, "ERROR: Dynamic array field '%s' cannot be used in an FFI struct\n", field.name.c_str());
                return nullptr;
            }
            const size_t count = field.type->fixed_array_count > 0 ? field.type->fixed_array_count : 1;
            const nari::TypeDecl *nested_decl = Parser::get_registered_type(field.type->name);
            const nari::TypeDecl *resolved_field_decl = nested_decl;
            std::vector<std::string> field_aliases;
            std::string resolved_field_name = field.type->name;
            while (resolved_field_decl && resolved_field_decl->is_alias()) {
                if (std::find(field_aliases.begin(), field_aliases.end(), resolved_field_decl->name) != field_aliases.end()) {
                    fprintf(stderr, "ERROR: Cyclic FFI type alias involving '%s'\n", resolved_field_decl->name.c_str());
                    resolving.pop_back();
                    return nullptr;
                }
                field_aliases.push_back(resolved_field_decl->name);
                if (!resolved_field_decl->alias_target) {
                    fprintf(stderr, "ERROR: Type alias '%s' has no target\n", resolved_field_decl->name.c_str());
                    resolving.pop_back();
                    return nullptr;
                }
                resolved_field_name = resolved_field_decl->alias_target->name;
                resolved_field_decl = Parser::get_registered_type(resolved_field_name);
            }
            if (resolved_field_decl) {
                auto nested_def = create(field.type->name);
                if (!nested_def) {
                    resolving.pop_back();
                    return nullptr;
                }
                fields.emplace_back(field.name, FFIType::Struct, count);
                fields.back().aggregate_def = std::move(nested_def);
            } else {
                fields.emplace_back(field.name, map_type_to_ffi(resolved_field_name), count);
            }
        }

        resolving.pop_back();
        return std::make_shared<FFIStructDef>(
            requested_name, fields, resolved_decl->kind == nari::TypeDeclKind::Union);
    };
    return create(type_name);
}

size_t get_struct_size(const std::string &type_name) {
    auto struct_def = create_struct_def_from_type(type_name);
    if (!struct_def) {
        fprintf(stderr, "ERROR: create_struct_def_from_type failed for '%s'\n", type_name.c_str());
        return 0;
    }

    ffi_type *ffi_struct = get_ffi_struct_type(struct_def.get());
    if (!ffi_struct) {
        fprintf(stderr, "ERROR: get_ffi_struct_type failed for '%s'\n", type_name.c_str());
        return 0;
    }

    // prepare a dummy CIF that returns this struct to force libffi to compute its layout
    ffi_cif cif;
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, ffi_struct, nullptr);
    if (status != FFI_OK) {
        fprintf(stderr, "ERROR: ffi_prep_cif failed in get_struct_size (status=%d)\n", status);
        return 0;
    }

    // Now the ffi_struct->size should be computed
    if (ffi_struct->size == 0) {
        fprintf(stderr, "ERROR: ffi_struct->size is still 0 after ffi_prep_cif\n");
        return 0;
    }

    return ffi_struct->size;
}

Value read_struct_from_memory(void *ptr, const std::string &type_name) {
    if (!ptr) {
        fprintf(stderr, "ERROR: Cannot read struct from null pointer\n");
        return Value::none();
    }

    auto struct_def = create_struct_def_from_type(type_name);
    if (!struct_def) {
        return Value::none();
    }

    // get the ffi_type to ensure size and alignment is computed
    ffi_type *ffi_struct = get_ffi_struct_type(struct_def.get());
    if (!ffi_struct) {
        fprintf(stderr, "ERROR: Failed to create ffi_type for struct '%s'\n", type_name.c_str());
        return Value::none();
    }

    return read_ffi_struct(static_cast<const char *>(ptr), *struct_def);
}

bool write_struct_to_memory(void *ptr, const std::string &type_name, const Value &obj) {
    if (!ptr) {
        fprintf(stderr, "ERROR: Cannot write struct to null pointer\n");
        return false;
    }

    if (!obj.is_object()) {
        fprintf(stderr, "ERROR: Expected object for struct write, got %s\n", obj.to_string().c_str());
        return false;
    }

    auto struct_def = create_struct_def_from_type(type_name);
    if (!struct_def) {
        return false;
    }

    // get the ffi_type to ensure size/alignment is computed
    ffi_type *ffi_struct = get_ffi_struct_type(struct_def.get());
    if (!ffi_struct) {
        fprintf(stderr, "ERROR: Failed to create ffi_type for struct '%s'\n", type_name.c_str());
        return false;
    }

    return write_ffi_struct(static_cast<char *>(ptr), *struct_def, obj);
}

FFICallback::~FFICallback() {
    if (closure) {
        ffi_closure_free(closure);
        closure = nullptr;
        executable_code = nullptr;
    }
}

// Nonzero inside an FFI callback. The JIT entry path falls back to the interpreter when set,
// callback reentry has a bad memory leak I haven't fixed yet.
thread_local int g_ffi_reentry_depth = 0;
extern "C" int ffi_reentry_depth() {
    return g_ffi_reentry_depth;
}

// trampoline function that gets called from C and dispatches to Nari
void FFICallbackManager::callback_trampoline(ffi_cif *cif, void *ret, void **args, void *user_data) {
    auto *callback = static_cast<FFICallback *>(user_data);
    if (!callback || !callback->runtime) {
        fprintf(stderr, "ERROR: Invalid callback data in trampoline\n");
        return;
    }

    std::vector<Value> nari_args;
    nari_args.reserve(callback->signature.param_types.size());

    for (size_t i = 0; i < callback->signature.param_types.size(); i++) {
        FFIType param_type = callback->signature.param_types[i];
        void *arg_ptr = args[i];

        switch (param_type) {
            case FFIType::Void:
                nari_args.push_back(Value::none());
                break;
            case FFIType::Int8:
                nari_args.push_back(Value::make_int(*static_cast<int8_t *>(arg_ptr)));
                break;
            case FFIType::UInt8:
                nari_args.push_back(Value::make_int(*static_cast<uint8_t *>(arg_ptr)));
                break;
            case FFIType::Int16:
                nari_args.push_back(Value::make_int(*static_cast<int16_t *>(arg_ptr)));
                break;
            case FFIType::UInt16:
                nari_args.push_back(Value::make_int(*static_cast<uint16_t *>(arg_ptr)));
                break;
            case FFIType::Int32:
                nari_args.push_back(Value::make_int(*static_cast<int32_t *>(arg_ptr)));
                break;
            case FFIType::Int64:
                nari_args.push_back(Value::make_int(*static_cast<int64_t *>(arg_ptr)));
                break;
            case FFIType::UInt32:
                nari_args.push_back(Value::make_int(*static_cast<uint32_t *>(arg_ptr)));
                break;
            case FFIType::UInt64:
                nari_args.push_back(Value::make_int(static_cast<int64_t>(*static_cast<uint64_t *>(arg_ptr))));
                break;
            case FFIType::Float:
                nari_args.push_back(Value::make_float(*static_cast<float *>(arg_ptr)));
                break;
            case FFIType::Double:
                nari_args.push_back(Value::make_float(*static_cast<double *>(arg_ptr)));
                break;
            case FFIType::Bool:
                nari_args.push_back(Value::make_bool(*static_cast<uint8_t *>(arg_ptr) != 0));
                break;
            case FFIType::Pointer: {
                void *ptr = *static_cast<void **>(arg_ptr);
                nari_args.push_back(Value::make_int(reinterpret_cast<int64_t>(ptr)));
                break;
            }
            default:
                nari_args.push_back(Value::none());
                break;
        }
    }

    // Bump the FFI-reentry depth so the bytecode VM and JIT entry paths can
    // detect that we're being driven by a native callback. The JIT in
    // particular has a leak when its compiled code is reentered while
    // another JIT-compiled frame is on the stack (it allocates per-frame
    // state on heap that's not freed on the reentrant return path), so
    // call sites that observe nonzero ffi_reentry_depth() must take the
    // bytecode interpreter path instead.
    extern thread_local int g_ffi_reentry_depth;
    g_ffi_reentry_depth++;

    // call nari func. nari_args is a C++ local live across the nested call
    // (safe-points); root it so a collection can't sweep its
    // elements. (callback->nari_function is rooted globally via the manager.)
    Value result;
    {
        ScriptRuntime::GcTempRoot _gr(*callback->runtime);
        _gr.add_vec(&nari_args);
        result = callback->runtime->call_function_value(callback->nari_function, nari_args);
    }

    g_ffi_reentry_depth--;

    switch (callback->signature.return_type) {
        case FFIType::Void:
            break;
        case FFIType::Int8:
            *static_cast<int8_t *>(ret) = result.is_int() ? static_cast<int8_t>(result.get_int()) : 0;
            break;
        case FFIType::UInt8:
            *static_cast<uint8_t *>(ret) = result.is_int() ? static_cast<uint8_t>(result.get_int()) : 0;
            break;
        case FFIType::Int16:
            *static_cast<int16_t *>(ret) = result.is_int() ? static_cast<int16_t>(result.get_int()) : 0;
            break;
        case FFIType::UInt16:
            *static_cast<uint16_t *>(ret) = result.is_int() ? static_cast<uint16_t>(result.get_int()) : 0;
            break;
        case FFIType::Int32:
            *static_cast<int32_t *>(ret) = result.is_int() ? static_cast<int32_t>(result.get_int()) : 0;
            break;
        case FFIType::Int64:
            *static_cast<int64_t *>(ret) = result.is_int() ? result.get_int() : 0;
            break;
        case FFIType::UInt32:
            *static_cast<uint32_t *>(ret) = result.is_int() ? static_cast<uint32_t>(result.get_int()) : 0;
            break;
        case FFIType::UInt64:
            *static_cast<uint64_t *>(ret) = result.is_int() ? static_cast<uint64_t>(result.get_int()) : 0;
            break;
        case FFIType::Float:
            if (result.is_float()) {
                *static_cast<float *>(ret) = static_cast<float>(result.get_float());
            } else if (result.is_int()) {
                *static_cast<float *>(ret) = static_cast<float>(result.get_int());
            } else {
                *static_cast<float *>(ret) = 0.0f;
            }
            break;
        case FFIType::Double:
            if (result.is_float()) {
                *static_cast<double *>(ret) = result.get_float();
            } else if (result.is_int()) {
                *static_cast<double *>(ret) = static_cast<double>(result.get_int());
            } else {
                *static_cast<double *>(ret) = 0.0;
            }
            break;
        case FFIType::Bool:
            if (result.is_bool()) {
                *static_cast<uint8_t *>(ret) = result.get_bool() ? 1 : 0;
            } else if (result.is_int()) {
                *static_cast<uint8_t *>(ret) = result.get_int() != 0 ? 1 : 0;
            } else {
                *static_cast<uint8_t *>(ret) = 0;
            }
            break;
        case FFIType::Pointer:
            if (result.is_int()) {
                *static_cast<void **>(ret) = result.get_ptr();
            } else {
                *static_cast<void **>(ret) = managed_struct_pointer(result);
            }
            break;
        default:
            break;
    }
}

static ffi_type *get_ffi_type_ptr(FFIType type) {
    switch (type) {
        case FFIType::Void:
            return &ffi_type_void;
        case FFIType::Int8:
            return &ffi_type_sint8;
        case FFIType::UInt8:
            return &ffi_type_uint8;
        case FFIType::Int16:
            return &ffi_type_sint16;
        case FFIType::UInt16:
            return &ffi_type_uint16;
        case FFIType::Int32:
            return &ffi_type_sint32;
        case FFIType::Int64:
            return &ffi_type_sint64;
        case FFIType::UInt32:
            return &ffi_type_uint32;
        case FFIType::UInt64:
            return &ffi_type_uint64;
        case FFIType::Float:
            return &ffi_type_float;
        case FFIType::Double:
            return &ffi_type_double;
        case FFIType::Bool:
            return &ffi_type_uint8;
        case FFIType::Pointer:
            return &ffi_type_pointer;
        default:
            return &ffi_type_void;
    }
}

void *FFICallbackManager::create_callback(const FFISignature &sig, const Value &nari_func, ScriptRuntime *runtime) {
    if (!nari_func.is_function() || !runtime) {
        fprintf(stderr, "ERROR: Invalid arguments to create_callback\n");
        return nullptr;
    }

    auto callback = std::make_unique<FFICallback>();
    callback->signature = sig;
    callback->nari_function = nari_func;
    callback->runtime = runtime;

    callback->param_types.reserve(sig.param_types.size());
    for (auto param_type : sig.param_types) {
        callback->param_types.push_back(get_ffi_type_ptr(param_type));
    }

    ffi_type *return_type = get_ffi_type_ptr(sig.return_type);
    ffi_status status = ffi_prep_cif(
        &callback->cif, FFI_DEFAULT_ABI,
        static_cast<unsigned int>(sig.param_types.size()),
        return_type, callback->param_types.data());

    if (status != FFI_OK) {
        fprintf(stderr, "ERROR: ffi_prep_cif failed with status %d\n", status);
        return nullptr;
    }

    callback->closure = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &callback->executable_code));
    if (!callback->closure) {
        fprintf(stderr, "ERROR: ffi_closure_alloc failed\n");
        return nullptr;
    }

    status = ffi_prep_closure_loc(
        callback->closure, &callback->cif, &callback_trampoline,
        callback.get(), // user_data passed to trampoline
        callback->executable_code);

    if (status != FFI_OK) {
        fprintf(stderr, "ERROR: ffi_prep_closure_loc failed with status %d\n", status);
        ffi_closure_free(callback->closure);
        return nullptr;
    }

    void *code_ptr = callback->executable_code;
    callbacks[code_ptr] = std::move(callback);

    return code_ptr;
}

void FFICallbackManager::free_callback(void *code_ptr) {
    auto it = callbacks.find(code_ptr);
    if (it != callbacks.end()) {
        callbacks.erase(it);
    }
}

FFICallback *FFICallbackManager::get_callback(void *code_ptr) {
    auto it = callbacks.find(code_ptr);
    if (it != callbacks.end()) {
        return it->second.get();
    }
    return nullptr;
}

} // namespace nari

#else // DISABLE_FFI
// dummy for when FFI is disabled, so the rest of the code can still compile
extern "C" int ffi_reentry_depth() {
    return 0;
}

#endif // DISABLE_FFI
