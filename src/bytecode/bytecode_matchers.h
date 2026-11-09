#pragma once

// Extended-JSRT helper shape matchers, extracted verbatim from
// bytecode_compiler.cpp. Only built when NARI_EXTENDED_JSRT is enabled.
#include "ast.h"
#include "bytecode.h"

namespace nari {
namespace bytecode {

bool matches_extended_jsrt_helper(const Function *func, OpCode op);

} // namespace bytecode
} // namespace nari
