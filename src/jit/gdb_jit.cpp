#ifndef DISABLE_JIT
#include "asmjit_jit.h"

#ifndef NARI_DISABLE_GDB_JIT

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <elf.h>

namespace nari {
namespace jit {

extern "C" {

struct jit_code_entry {
    jit_code_entry *next_entry;
    jit_code_entry *prev_entry;
    const char *symfile_addr;
    uint64_t symfile_size;
};

struct jit_descriptor {
    uint32_t version;
    uint32_t action_flag;
    jit_code_entry *relevant_entry;
    jit_code_entry *first_entry;
};

enum {
    JIT_NOACTION = 0,
    JIT_REGISTER_FN = 1,
    JIT_UNREGISTER_FN = 2,
};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used, visibility("default")))
#endif
jit_descriptor __jit_debug_descriptor = { 1, JIT_NOACTION, nullptr, nullptr };

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, used, visibility("default")))
#endif
void __jit_debug_register_code() {
    asm volatile("" ::: "memory");
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, used))
#endif
void nari_jit_initial_load_complete() {
    static volatile uint8_t reached = 0;
    reached = 1;
    asm volatile("" : : "m"(reached) : "memory");
}

} // extern "C"

namespace {

#if defined(__x86_64__)
constexpr uint16_t ElfMachine = EM_X86_64;
#elif defined(__aarch64__)
constexpr uint16_t ElfMachine = EM_AARCH64;
#else
constexpr uint16_t ElfMachine = 0;
#endif

static_assert(sizeof(Elf64_Ehdr) == 64);
static_assert(sizeof(Elf64_Shdr) == 64);
static_assert(sizeof(Elf64_Sym) == 24);

struct RegisteredObject {
    std::vector<char> elf;
    jit_code_entry entry;
    const void *owner = nullptr;
};

std::mutex g_gdb_jit_mutex;
std::vector<std::unique_ptr<RegisteredObject>> g_registered_objects;

size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

template <typename T>
void write_at(std::vector<char> &out, size_t offset, const T &value) {
    if (out.size() < offset + sizeof(T)) {
        out.resize(offset + sizeof(T));
    }
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

void append_bytes(std::vector<char> &out, const void *data, size_t size) {
    if (size == 0) {
        return;
    }
    const char *bytes = static_cast<const char *>(data);
    out.insert(out.end(), bytes, bytes + size);
}

std::vector<char> build_elf_symbol_file(const std::string &name, const void *code_addr, size_t code_size) {
    std::vector<char> shstrtab;
    shstrtab.push_back('\0');
    auto add_shstr = [&](const char *s) -> uint32_t {
        uint32_t off = static_cast<uint32_t>(shstrtab.size());
        shstrtab.insert(shstrtab.end(), s, s + std::strlen(s) + 1);
        return off;
    };
    uint32_t text_name = add_shstr(".text");
    uint32_t symtab_name = add_shstr(".symtab");
    uint32_t strtab_name = add_shstr(".strtab");
    uint32_t shstrtab_name = add_shstr(".shstrtab");

    std::vector<char> strtab;
    strtab.push_back('\0');
    uint32_t symbol_name = static_cast<uint32_t>(strtab.size());
    strtab.insert(strtab.end(), name.begin(), name.end());
    strtab.push_back('\0');

    std::vector<Elf64_Sym> symtab;
    symtab.push_back({});
    Elf64_Sym fn_sym = {};
    fn_sym.st_name = symbol_name;
    fn_sym.st_info = static_cast<unsigned char>((STB_GLOBAL << 4) | STT_FUNC);
    fn_sym.st_other = 0;
    fn_sym.st_shndx = 1;
    // this object is ET_REL, so ELF symbol values are section-relative
    // the executable runtime address is carried by the .text section's sh_addr
    // using an absolute st_value here makes GDB compute a bad address for breakpoints sometimes
    fn_sym.st_value = 0;
    fn_sym.st_size = static_cast<uint64_t>(code_size);
    symtab.push_back(fn_sym);

    std::vector<char> elf(sizeof(Elf64_Ehdr), 0);

    size_t text_off = align_up(elf.size(), 16);
    elf.resize(text_off);
    append_bytes(elf, code_addr, code_size);

    size_t symtab_off = align_up(elf.size(), 8);
    elf.resize(symtab_off);
    append_bytes(elf, symtab.data(), symtab.size() * sizeof(Elf64_Sym));

    size_t strtab_off = elf.size();
    append_bytes(elf, strtab.data(), strtab.size());

    size_t shstrtab_off = elf.size();
    append_bytes(elf, shstrtab.data(), shstrtab.size());

    size_t shoff = align_up(elf.size(), 8);
    elf.resize(shoff + 5 * sizeof(Elf64_Shdr));

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0] = 0x7f;
    ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';
    ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2; // ELFCLASS64
    ehdr.e_ident[5] = 1; // ELFDATA2LSB
    ehdr.e_ident[6] = 1; // EV_CURRENT
    ehdr.e_type = 1;     // ET_REL
    ehdr.e_machine = ElfMachine;
    ehdr.e_version = 1;
    ehdr.e_shoff = shoff;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum = 5;
    ehdr.e_shstrndx = 4;
    write_at(elf, 0, ehdr);

    Elf64_Shdr null_sh{};
    write_at(elf, shoff, null_sh);

    Elf64_Shdr text_sh{};
    text_sh.sh_name = text_name;
    text_sh.sh_type = SHT_PROGBITS;
    text_sh.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    text_sh.sh_addr = reinterpret_cast<uint64_t>(code_addr);
    text_sh.sh_offset = text_off;
    text_sh.sh_size = static_cast<uint64_t>(code_size);
    text_sh.sh_addralign = 16;
    write_at(elf, shoff + sizeof(Elf64_Shdr), text_sh);

    Elf64_Shdr symtab_sh{};
    symtab_sh.sh_name = symtab_name;
    symtab_sh.sh_type = SHT_SYMTAB;
    symtab_sh.sh_offset = symtab_off;
    symtab_sh.sh_size = static_cast<uint64_t>(symtab.size() * sizeof(Elf64_Sym));
    symtab_sh.sh_link = 3;
    symtab_sh.sh_info = 1;
    symtab_sh.sh_addralign = 8;
    symtab_sh.sh_entsize = sizeof(Elf64_Sym);
    write_at(elf, shoff + 2 * sizeof(Elf64_Shdr), symtab_sh);

    Elf64_Shdr strtab_sh{};
    strtab_sh.sh_name = strtab_name;
    strtab_sh.sh_type = SHT_STRTAB;
    strtab_sh.sh_offset = strtab_off;
    strtab_sh.sh_size = static_cast<uint64_t>(strtab.size());
    strtab_sh.sh_addralign = 1;
    write_at(elf, shoff + 3 * sizeof(Elf64_Shdr), strtab_sh);

    Elf64_Shdr shstrtab_sh{};
    shstrtab_sh.sh_name = shstrtab_name;
    shstrtab_sh.sh_type = SHT_STRTAB;
    shstrtab_sh.sh_offset = shstrtab_off;
    shstrtab_sh.sh_size = static_cast<uint64_t>(shstrtab.size());
    shstrtab_sh.sh_addralign = 1;
    write_at(elf, shoff + 4 * sizeof(Elf64_Shdr), shstrtab_sh);

    return elf;
}

std::string sanitize_symbol_name(std::string name) {
    if (name.empty()) {
        return "nari_jit_anon";
    }
    for (char &c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            c = '_';
        }
    }
    if (name[0] >= '0' && name[0] <= '9') {
        name.insert(name.begin(), '_');
    }
    return "nari_jit_" + name;
}

} // namespace

void register_gdb_jit_function(const void *owner, const std::string &name, const void *code_addr, size_t code_size) {
    if (ElfMachine == 0 || !code_addr || code_size == 0) {
        return;
    }

    auto obj = std::make_unique<RegisteredObject>();
    obj->owner = owner;
    obj->elf = build_elf_symbol_file(sanitize_symbol_name(name), code_addr, code_size);
    obj->entry.next_entry = nullptr;
    obj->entry.prev_entry = nullptr;
    obj->entry.symfile_addr = obj->elf.data();
    obj->entry.symfile_size = obj->elf.size();

    std::lock_guard<std::mutex> lock(g_gdb_jit_mutex);

    obj->entry.next_entry = __jit_debug_descriptor.first_entry;
    if (__jit_debug_descriptor.first_entry) {
        __jit_debug_descriptor.first_entry->prev_entry = &obj->entry;
    }
    __jit_debug_descriptor.first_entry = &obj->entry;
    __jit_debug_descriptor.relevant_entry = &obj->entry;
    __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
    __jit_debug_register_code();
    __jit_debug_descriptor.action_flag = JIT_NOACTION;

    g_registered_objects.push_back(std::move(obj));
}

void unregister_gdb_jit_functions(const void *owner) {
    std::lock_guard<std::mutex> lock(g_gdb_jit_mutex);

    for (size_t i = g_registered_objects.size(); i > 0; --i) {
        RegisteredObject &obj = *g_registered_objects[i - 1];
        if (obj.owner != owner) {
            continue;
        }
        jit_code_entry &entry = obj.entry;
        __jit_debug_descriptor.relevant_entry = &entry;
        __jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
        __jit_debug_register_code();
        __jit_debug_descriptor.action_flag = JIT_NOACTION;
        if (entry.prev_entry) {
            entry.prev_entry->next_entry = entry.next_entry;
        } else {
            __jit_debug_descriptor.first_entry = entry.next_entry;
        }
        if (entry.next_entry) {
            entry.next_entry->prev_entry = entry.prev_entry;
        }
        g_registered_objects.erase(g_registered_objects.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }
    __jit_debug_descriptor.relevant_entry = nullptr;
}

} // namespace jit
} // namespace nari

#endif // !NARI_DISABLE_GDB_JIT
#endif // !DISABLE_JIT
