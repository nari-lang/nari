#pragma once
// Bytecode -> SSA IR builder
#ifndef DISABLE_JIT
#include "ir.h"

namespace nari {
namespace bytecode {
struct Chunk;
}
namespace jit {
namespace ir {

// Build SSA IR for the current optimizing-JIT coverage set. Returns true and fills
// `out` on success; false if the function is not yet IR-eligible, so the VM runs it
// in the interpreter. Arithmetic is initially represented as Dyn* ops and specialized
// later by type inference/lowering when operand types are proven.
bool build(const bytecode::Chunk &chunk, uint32_t func_idx, Func &out);

} // namespace ir
} // namespace jit
} // namespace nari
#endif // !DISABLE_JIT
