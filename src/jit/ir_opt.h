#pragma once
// Analysis + optimization passes over the mid-level IR.
// See docs/JIT_OPTIMIZING_PIPELINE.md.
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

// Per-chunk map: name-table index of a write-once global -> proven type of the
// stored value. Built once per chunk by analyze_const_globals() and passed to
// infer_types()
using GlobalTypeMap = std::unordered_map<uint32_t, Ty>;

// Scan every function's bytecode in the chunk. For each global name with exactly
// one OP_STORE_GLOBAL and a recognizable constant-typed initializer, record the value's static type.
GlobalTypeMap analyze_const_globals(const nari::bytecode::Chunk &chunk);

// Set of local slot indices whose runtime value is always an array of Int48
// (within this function's visible scope). 
// Computed by analyze_int_array_slots() and consulted by infer_types() so `LoadIndex(LoadSlot @s, _)` types as Int48.
using IntArraySlots = std::unordered_set<uint32_t>;

// Analyze a built IR function and return the set of its slots that are
// statically proven to hold int-only arrays. Must be called AFTER infer_types() has run at least once
IntArraySlots analyze_int_array_slots(
    const Func &f, uint32_t push_method_name_idx,
    uint32_t length_method_name_idx = UINT32_MAX);

// Optimistic "top" of the int-array lattice: every slot that COULD be an
// int-array based on its definitions alone, this determines which slots to
// treat as int-array candidates for the array-header-hoist optimization.
IntArraySlots int_array_candidates(const Func &f);

// Forward type inference: assigns a Ty to every SSA value (and a derived type to
// each local slot) by iterating to a CFG fixpoint. Leaves a value
// Unknown when a type can't be proven
void infer_types(
    Func &f, std::vector<Ty> &out_slot_types,
    const GlobalTypeMap *global_types = nullptr,
    const IntArraySlots *int_array_slots = nullptr,
    bool int48_params = false);

// Rewrite dynamic numeric ops to typed ops when the lattice proves both operands.
// This makes specialization explicit in the IR instead of leaving it as an ad-hoc
// lowering-time check.
bool specialize_types(Func &f);

// Drop `Not Not` pairs whose operand is now known Bool (cmps typed by
// specialize_types). Common in `&&`/`||` lowering output.
bool fold_redundant_not(Func &f);

// After redundant-pair folding, the only Not left feeding a Branch is `if (!x) ...`.
// Fold it into the terminator by swapping true/false targets.
bool fold_branch_not(Func &f);

// Mark single-use left-associative StrConcat chains for in-place append lowering.
// Avoids O(n^2) prefix copies + per-link StringObj allocs.
//
// Runs on the final IR, so global use-counts are stable.
bool mark_inplace_concat(Func &f);

// prove a builtin global (e.g. "to_string") is unshadowable in this chunk
uint32_t analyze_frozen_builtin(const nari::bytecode::Chunk &chunk, const char *name);

// fuse `s @ to_string(x)` -> `s @ x`, eliding the throwaway StringObj that builtin_toString allocates
bool fuse_tostring_concat(Func &f, uint32_t tostring_name_idx);

// Report-only GVN / global-CSE opportunity analysis
//
// When NARI_IR_GVN_REPORT is set, prints a per-function summary of cross-block
// redundant expressions that a global CSE pass + slot carrier could eliminate,
// bucketed by op class and by whether the redundancy is inside a loop.
//
// This is purely for analysis!
void gvn_report(const Func &f, const char *fn_name, uint32_t fn_idx);

// LICM of an invariant array base/size out of a read loop. The proven int-array
// LoadIndex fast-path re-derives the backing pointer + element count on every
// access, for each alias-free proven int-array slot this pass finds the
// outermost natural loop across which that backing pointer is invariant and that
// has a dedicated preheader, so codegen can materialize start+size once at the
// preheader and reuse them at every LoadIndex in the loop.
//
// A (slot s, loop L) is cached only when it is provably safe: s is a proven,
// alias-free int-array slot and neither L's body nor its unique preheader can resize or rebind s.
//
// This is purely for analysis!
struct ArrayHeaderHoist {
    // preheader block id -> slots whose header to materialize at block entry
    std::unordered_map<BlockId, std::vector<uint32_t>> materialize;
    // block id -> slots whose cached header is valid for a LoadIndex here
    std::unordered_map<BlockId, std::unordered_set<uint32_t>> valid;
    // union of all cached slots
    std::unordered_set<uint32_t> slots;
    bool empty() const {
        return slots.empty();
    }
};
ArrayHeaderHoist plan_array_header_hoist(const Func &f, const IntArraySlots &int_arr_slots);

// Run the currently-safe IR optimization passes. These passes are deliberately
// stack-aware because the generic lowering still emits from block
// instruction streams using the VM value stack
bool optimize(Func &f);

} // namespace ir
} // namespace jit
} // namespace nari
#endif // !DISABLE_JIT
