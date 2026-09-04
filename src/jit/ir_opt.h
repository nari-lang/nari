#pragma once
// Analysis passes and optimization passes for the mid-level IR.
// Refer to docs/JIT_OPTIMIZING_PIPELINE.md.
#ifndef DISABLE_JIT
#include "ir.h"

#include <unordered_map>
#include <unordered_set>

namespace nari {
namespace bytecode {
struct Chunk;
}
namespace jit {
namespace ir {

// Maps a write-once global to the proven type of its value.
using GlobalTypeMap = std::unordered_map<uint32_t, Ty>;

// Builds that map. A global qualifies if it has one OP_STORE_GLOBAL and an
// initializer with a known constant type.
GlobalTypeMap analyze_const_globals(const nari::bytecode::Chunk &chunk);

// Local slots that always hold an array of Int48 values.
using IntArraySlots = std::unordered_set<uint32_t>;

// Finds those slots. Call infer_types() first.
IntArraySlots analyze_int_array_slots(const Func &f, uint32_t push_method_name_idx, uint32_t length_method_name_idx = UINT32_MAX);

// Every slot that can possibly be an int-array. The hoist pass below uses this
// to select candidates.
IntArraySlots int_array_candidates(const Func &f);

// Gives a type to every SSA value and to each local slot. Repeats until the CFG
// comes to a fixpoint. A type that the pass cannot prove stays Unknown.
void infer_types(
    Func &f, std::vector<Ty> &out_slot_types, const GlobalTypeMap *global_types = nullptr, const IntArraySlots *int_array_slots = nullptr,
    bool int48_params = false
);

// Changes dynamic numeric operations into typed operations when the lattice
// proves both operands.
bool specialize_types(Func &f);

// Removes pairs of `Not` when the operand is known Bool.
bool fold_redundant_not(Func &f);

// Folds the `Not` of `if (!x)` into the terminator. Exchanges the true target
// and the false target.
bool fold_branch_not(Func &f);

// Marks left-associative StrConcat chains that have one use, so lowering can
// append in place. This prevents O(n^2) copies of the prefix.
bool mark_inplace_concat(Func &f);

// Proves that no code in this chunk can shadow a builtin global, such as
// "to_string".
uint32_t analyze_frozen_builtin(const nari::bytecode::Chunk &chunk, const char *name);

// Combines `s @ to_string(x)` into `s @ x` and removes the temporary StringObj.
bool fuse_tostring_concat(Func &f, uint32_t tostring_name_idx);

// Prints the redundant expressions that a global CSE pass could remove. Set
// NARI_IR_GVN_REPORT to enable. Changes nothing.
void gvn_report(const Func &f, const char *fn_name, uint32_t fn_idx);

// Plans LICM of an array base address and element count out of a read loop.
// Caches a slot only when nothing in the loop can resize or rebind it.
// Changes nothing.
struct ArrayHeaderHoist {
    // Preheader block -> slots whose header to calculate at block entry.
    std::unordered_map<BlockId, std::vector<uint32_t>> materialize;
    // Block -> slots whose cached header a LoadIndex here can use.
    std::unordered_map<BlockId, std::unordered_set<uint32_t>> valid;
    // All the cached slots.
    std::unordered_set<uint32_t> slots;
    bool empty() const {
        return slots.empty();
    }
};
ArrayHeaderHoist plan_array_header_hoist(const Func &f, const IntArraySlots &int_arr_slots);

// Runs the IR optimization passes that are safe at this time. They are
// stack-aware, because generic lowering still uses the value stack of the VM.
bool optimize(Func &f);

} // namespace ir
} // namespace jit
} // namespace nari
#endif // !DISABLE_JIT
