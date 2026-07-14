#ifndef DISABLE_JIT
#include "asmjit_jit.h"

// perf jitdump only exists on Linux; other platforms get no-op stubs (at EOF)
#if !defined(__linux__)
namespace nari {
namespace jit {
void perf_jitdump_register(const std::string &, const void *, size_t) {
}
void perf_jitdump_close() {
}
} // namespace jit
} // namespace nari
#else

// Linux perf jitdump writer (spec version 1, +CLOCK_MONOTONIC timestamps).
//
//  get relevant data by doing the following:
//      perf record -k mono -g -- <nari ...>                        # -k mono: sample clock == ours
//      perf inject --jit --input perf.data --output perf.jit.data
//      perf report  --input perf.jit.data                          # now shows nari_jit_* symbols
//      perf annotate --input perf.jit.data <sym>                   # disassembles the JIT function.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace nari {
namespace jit {

namespace {

#if defined(__x86_64__)
constexpr uint32_t kElfMach = EM_X86_64; // EM_X86_64
#elif defined(__aarch64__)
constexpr uint32_t kElfMach = EM_AARCH64; // EM_AARCH64
#else
constexpr uint32_t kElfMach = EM_NONE;
#endif

enum {
    JIT_CODE_LOAD = 0,
    JIT_CODE_MOVE = 1,
    JIT_CODE_DEBUG_INFO = 2,
    JIT_CODE_CLOSE = 3,
    JIT_CODE_UNWINDING_INFO = 4,
};

// All jitdump structs are packed, so the in-memory layout matches
// the on-disk format with no compiler padding
struct FileHeader {
    uint32_t magic;      // "JiTD" (0x4A695444)
    uint32_t version;    // 1
    uint32_t total_size; // size of this header
    uint32_t elf_mach;   // ELF e_machine
    uint32_t pad1;       // reserved
    uint32_t pid;
    uint64_t timestamp;
    uint64_t flags;
};

struct RecordHeader {
    uint32_t id;         // JIT_CODE_*
    uint32_t total_size; // header + payload
    uint64_t timestamp;
};

struct CodeLoadRecord {
    uint32_t pid;
    uint32_t tid;
    uint64_t vma;
    uint64_t code_addr;
    uint64_t code_size;
    uint64_t code_index;
    // char name[] (NUL-terminated) + raw code bytes follow
};

static_assert(sizeof(FileHeader) == 40, "jitdump file header layout mismatch!");
static_assert(sizeof(RecordHeader) == 16, "jitdump record header layout mismatch!");
static_assert(sizeof(CodeLoadRecord) == 40, "jitdump code-load layout mismatch!");

std::mutex g_mutex;
int g_fd = -1;
bool g_init_done = false;
bool g_enabled = false;
void *g_marker = nullptr;
size_t g_marker_size = 0;
uint64_t g_code_index = 0;

uint64_t monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint32_t current_tid() {
#if defined(__linux__)
    return (uint32_t)syscall(SYS_gettid);
#else
    return (uint32_t)getpid();
#endif
}

// must hold g_mutex
void write_all_locked(const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    while (n > 0) {
        ssize_t w = write(g_fd, p, n);
        if (w <= 0) {
            return; // give up silently; diagnostics-only feature
        }
        p += w;
        n -= (size_t)w;
    }
}

// must hold g_mutex
void ensure_init_locked() {
    if (g_init_done) {
        return;
    }
    g_init_done = true;
    if (getenv("NARI_JIT_PERF_DUMP") == nullptr || kElfMach == 0) {
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "jit-%d.dump", (int)getpid());
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        return;
    }

    FileHeader hdr{};
    hdr.magic = 'J' | ('i' << 8) | ('T' << 16) | ('D' << 24);
    hdr.version = 1;
    hdr.total_size = sizeof(FileHeader);
    hdr.elf_mach = kElfMach;
    hdr.pad1 = 0;
    hdr.pid = (uint32_t)getpid();
    hdr.timestamp = monotonic_ns();
    hdr.flags = 0;

    const char *p = reinterpret_cast<const char *>(&hdr);
    size_t left = sizeof(hdr);
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            close(fd);
            return;
        }
        p += w;
        left -= (size_t)w;
    }

    // marker mmap: perf inject --jit keys on an executable mmap of a file named jit-*.dump,
    // mapping page 0 with PROT_EXEC makes perf record emit the MMAP event it needs.
    long pg = sysconf(_SC_PAGESIZE);
    g_marker_size = (pg > 0) ? (size_t)pg : 4096;
    g_marker = mmap(nullptr, g_marker_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (g_marker == MAP_FAILED) {
        g_marker = nullptr;
    }

    g_fd = fd;
    g_enabled = true;
}

} // namespace

void perf_jitdump_register(const std::string &name, const void *code_addr, size_t code_size) {
    if (code_addr == nullptr || code_size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_init_locked();
    if (!g_enabled) {
        return;
    }

    const uint32_t namelen = (uint32_t)name.size() + 1; // include NUL

    RecordHeader rh = {};
    rh.id = JIT_CODE_LOAD;
    rh.total_size = (uint32_t)(sizeof(RecordHeader) + sizeof(CodeLoadRecord) + namelen + code_size);
    rh.timestamp = monotonic_ns();

    CodeLoadRecord clr = {};
    clr.pid = (uint32_t)getpid();
    clr.tid = current_tid();
    clr.vma = (uint64_t)(uintptr_t)code_addr;
    clr.code_addr = (uint64_t)(uintptr_t)code_addr;
    clr.code_size = (uint64_t)code_size;
    clr.code_index = g_code_index++;

    // Assemble the whole record in one buffer and emit with a single write so a
    // concurrent compile thread cannot interleave bytes into ours.
    std::vector<char> buf;
    buf.reserve(rh.total_size);
    auto append = [&](const void *p, size_t n) {
        const char *b = static_cast<const char *>(p);
        buf.insert(buf.end(), b, b + n);
    };
    append(&rh, sizeof(rh));
    append(&clr, sizeof(clr));
    append(name.c_str(), namelen);
    append(code_addr, code_size);
    write_all_locked(buf.data(), buf.size());
}

void perf_jitdump_close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled) {
        return;
    }
    if (g_fd >= 0) {
        RecordHeader rh{};
        rh.id = JIT_CODE_CLOSE;
        rh.total_size = (uint32_t)sizeof(RecordHeader);
        rh.timestamp = monotonic_ns();
        write_all_locked(&rh, sizeof(rh));
        close(g_fd);
        g_fd = -1;
    }
    if (g_marker) {
        munmap(g_marker, g_marker_size);
        g_marker = nullptr;
    }
    g_enabled = false;
}

} // namespace jit
} // namespace nari

#endif // __linux__
#endif // !DISABLE_JIT
