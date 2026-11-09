#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nari::bytecode {
class VM;

// These hooks are active only when NARI_GDB_FUNCTIONS is set before launch.
// The GDB helper script sets it for source/bytecode debugging.
bool gdb_nari_functions_enabled();
void gdb_nari_note_function(VM &vm, uint32_t func_idx);
void gdb_nari_note_module(VM &vm, const std::string &name, size_t marker_pc);
} // namespace nari::bytecode
