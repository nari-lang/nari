#pragma once
// JIT asm-dump helpers (split out of asmjit_jit.cpp).
// Pure string processing over disassembly text; no AsmJit dependency.
#include <cstdint>
#include <string>
#include <unordered_map>

bool jit_dump_asm_enabled();
const std::unordered_map<uint64_t, const char *> &jit_helper_symbols();
std::string jit_symbolize_calls(const char *asm_text, uint64_t extra_addr = 0, const char *extra_name = nullptr);
