#pragma once
// Internal cross-TU declarations for the IR optimizer.
// Block-level passes live in ir_opt_blocks.cpp; the driver, GVN,
// and type inference stay in ir_opt.cpp.
#include "ir_opt.h"

namespace nari {
namespace jit {
namespace ir {

bool is_int(Ty t);
bool is_float(Ty t);
bool numeric(Ty t);
Ty join(Ty a, Ty b);
Ty arith_type(Op op, Ty a, Ty b);
bool fits_int48_i64(int64_t v);
bool is_const(Op op);
bool is_pure_stack_value(Op op);
bool is_foldable_binary(Op op);
bool is_nonfolding_binary(Op op);
bool numeric_const(const Inst &in);
double const_number(const Inst &in);
bool truthy_const(const Inst &in);
Inst make_int_const(int64_t v, size_t pc);
Inst make_float_const(double v, size_t pc);
Inst make_bool_const(bool v, size_t pc);
Inst number_result_checked(bool overflowed, int64_t v, double fallback, size_t pc);
bool fold_binary_const(Op op, const Inst &lhs, const Inst &rhs, size_t pc, Inst &out);
bool simplify_block(Func &f, Block &b);
bool eliminate_slot_round_trips(Func &f, Block &b);
bool slot_aliased(const Func &f, uint32_t slot);
bool fuse_slot_stores(Func &f, Block &b);
bool fold_branch(Func &f, Block &b);
BlockId jump_thread_target(const Func &f, BlockId target);
bool thread_jumps(Func &f);
bool propagate_slot_consts(Func &f, Block &b);
bool eliminate_dead_stores(Func &f);
void rebuild_preds(Func &f);
bool local_value_numbering(Func &f, Block &b);
bool thread_phi_branches(Func &f);
bool merge_blocks(Func &f);
bool remove_unreachable_blocks(Func &f);
void rebuild_preds(Func &f);

} // namespace ir
} // namespace jit
} // namespace nari
