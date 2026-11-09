#include "gdb_bridge.h"

#include "bytecode.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef NARI_MCU
namespace nari::bytecode {
bool gdb_nari_functions_enabled() {
    return false;
}
void gdb_nari_note_function(VM &, uint32_t) {
}
void gdb_nari_note_module(VM &, const std::string &, size_t) {
}
} // namespace nari::bytecode
#else

namespace nari::bytecode {
namespace {

struct GdbNariEvent {
    VM *vm = nullptr;
    uint32_t func_idx = 0;
    size_t begin_pc = 0;
    size_t end_pc = 0;
    std::string name;
    std::string source_file;
};

thread_local GdbNariEvent current_event;

void publish_event(VM &vm, uint32_t func_idx, std::string name, std::string source_file, size_t begin_pc, size_t end_pc);

} // namespace

bool gdb_nari_functions_enabled() {
    static const bool enabled = std::getenv("NARI_GDB_FUNCTIONS") != nullptr;
    return enabled;
}

extern "C" {

const char *nari_gdb_event_name();
const char *nari_gdb_event_source();
void nari_gdb_dump_bytecode();

// GDB breaks here after current_event has been published. Keep the symbol in
// release ThinLTO builds, including builds with no native debug information.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, used, visibility("default")))
#endif
void nari_gdb_function_entry(const char *name) {
    static volatile uint8_t reached = 0;
    reached = 1;
#if defined(__GNUC__) || defined(__clang__)
    // Keep the GDB accessors in the executable even when ThinLTO sees no C++
    // call sites. GDB invokes them by name after stopping at this marker.
    asm volatile(
        "" : : "m"(reached), "r"(name), "r"(&nari_gdb_event_name), "r"(&nari_gdb_event_source), "r"(&nari_gdb_dump_bytecode) : "memory"
    );
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used, visibility("default")))
#endif
const char *nari_gdb_event_name() {
    return current_event.name.c_str();
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used, visibility("default")))
#endif
const char *nari_gdb_event_source() {
    return current_event.source_file.c_str();
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used, visibility("default")))
#endif
void nari_gdb_dump_bytecode() {
    if (!current_event.vm || !current_event.vm->chunk) {
        return;
    }
    dump_function_range(*current_event.vm->chunk, current_event.func_idx, current_event.begin_pc, current_event.end_pc);
    std::fflush(stdout);
}

} // extern "C"

namespace {

void publish_event(VM &vm, uint32_t func_idx, std::string name, std::string source_file, size_t begin_pc, size_t end_pc) {
    current_event.vm = &vm;
    current_event.func_idx = func_idx;
    current_event.begin_pc = begin_pc;
    current_event.end_pc = end_pc;
    current_event.name = std::move(name);
    current_event.source_file = std::move(source_file);
    nari_gdb_function_entry(current_event.name.c_str());
}

} // namespace

void gdb_nari_note_function(VM &vm, uint32_t func_idx) {
    if (!gdb_nari_functions_enabled() || !vm.chunk || func_idx >= vm.chunk->functions.size()) {
        return;
    }
    const FunctionMeta &func = vm.chunk->functions[func_idx];
    publish_event(vm, func_idx, func.name, func.source_file, 0, func.code.size());
}

void gdb_nari_note_module(VM &vm, const std::string &name, size_t marker_pc) {
    if (!gdb_nari_functions_enabled() || !vm.chunk || vm.frames.empty()) {
        return;
    }
    const FunctionMeta &func = *vm.current_function();
    const auto &code = func.code;
    size_t end_pc = code.size();
    for (size_t pc = marker_pc + 3; pc < code.size();) {
        if (static_cast<OpCode>(code[pc]) == OpCode::OP_MODULE_ENTRY) {
            end_pc = pc;
            break;
        }
        size_t size = decoded_instruction_size(code, pc);
        if (size == 0) {
            break;
        }
        pc += size;
    }
    if (end_pc == code.size() && end_pc >= 2 && static_cast<OpCode>(code[end_pc - 2]) == OpCode::OP_LOAD_NONE &&
        static_cast<OpCode>(code[end_pc - 1]) == OpCode::OP_RETURN) {
        end_pc -= 2; // compiler-generated <main> epilogue, outside the last module
    }
    std::string source_file = name;
    constexpr const char *prefix = "__top_level__@";
    if (source_file.compare(0, 14, prefix) == 0) {
        source_file.erase(0, 14);
    }
    publish_event(vm, func.self_idx, name, source_file, marker_pc, end_pc);
}

} // namespace nari::bytecode

#endif // NARI_MCU
