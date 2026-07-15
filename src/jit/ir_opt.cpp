// IR analysis + optimization passes
#ifndef DISABLE_JIT
#include "ir_opt.h"

#include "bytecode.h"
#include "int_overflow.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nari {
namespace jit {
namespace ir {

static bool is_int(Ty t) {
    return t == Ty::Int48;
}
static bool is_float(Ty t) {
    return t == Ty::Float;
}
static bool numeric(Ty t) {
    return t == Ty::Int48 || t == Ty::Float || t == Ty::Number;
}

// Lattice join. Bottom is the identity (no info yet); Unknown is absorbing (top).
// Numeric types merge to Number rather than collapsing to Unknown, so a loop that
// mixes int and float stays numeric.
static Ty join(Ty a, Ty b) {
    if (a == Ty::Bottom) {
        return b;
    }
    if (b == Ty::Bottom) {
        return a;
    }
    if (a == b) {
        return a;
    }
    if (numeric(a) && numeric(b)) {
        return Ty::Number;
    }
    return Ty::Unknown;
}

// Result type of an arithmetic Dyn op. A Bottom operand means it's not computable *yet*
static Ty arith_type(Op op, Ty a, Ty b) {
    if (a == Ty::Bottom || b == Ty::Bottom) {
        return Ty::Bottom;
    }
    switch (op) {
        case Op::DynAdd:
        case Op::DynSub:
        case Op::DynMul:
        case Op::DynMod:
            if (is_int(a) && is_int(b)) {
                return Ty::Int48; // integer-valued (overflow -> float at box time)
            }
            if ((is_float(a) || is_float(b)) && numeric(a) && numeric(b)) {
                return Ty::Float;
            }
            return Ty::Unknown; // heap/unknown operand: may be string concat etc.
        case Op::DynDiv:
            return (numeric(a) && numeric(b)) ? Ty::Number : Ty::Unknown;
        default:
            return Ty::Unknown;
    }
}

static constexpr int64_t kInt48Max = (1LL << 47) - 1;
static constexpr int64_t kInt48Min = -(1LL << 47);

static bool fits_int48_i64(int64_t v) {
    return v >= kInt48Min && v <= kInt48Max;
}

static bool is_const(Op op) {
    return op == Op::IConst || op == Op::FConst || op == Op::BConst || op == Op::NConst;
}

static bool is_pure_stack_value(Op op) {
    switch (op) {
        case Op::IConst:
        case Op::FConst:
        case Op::BConst:
        case Op::NConst:
        case Op::LoadConst:
        case Op::Dup:
            return true;
        default:
            return false;
    }
}

static bool is_foldable_binary(Op op) {
    switch (op) {
        case Op::DynAdd:
        case Op::DynSub:
        case Op::DynMul:
        case Op::DynMod:
        case Op::DynDiv:
        case Op::DynCmpLt:
        case Op::DynCmpLe:
        case Op::DynCmpGt:
        case Op::DynCmpGe:
        case Op::DynCmpEq:
        case Op::DynCmpNe:
            return true;
        default:
            return false;
    }
}

static bool is_nonfolding_binary(Op op) {
    switch (op) {
        case Op::IAnd:
        case Op::IOr:
        case Op::IXor:
        case Op::IShl:
        case Op::IShr:
        case Op::StrConcat:
            return true;
        default:
            return false;
    }
}

static bool numeric_const(const Inst &in) {
    return in.op == Op::IConst || in.op == Op::FConst;
}

static double const_number(const Inst &in) {
    return in.op == Op::IConst ? (double)in.imm_int : in.imm_float;
}

static bool truthy_const(const Inst &in) {
    switch (in.op) {
        case Op::IConst:
            return in.imm_int != 0;
        case Op::FConst:
            return in.imm_float != 0.0;
        case Op::BConst:
            return in.imm_int != 0;
        case Op::NConst:
            return false;
        default:
            return true;
    }
}

static Inst make_int_const(int64_t v, size_t pc) {
    Inst out;
    out.op = Op::IConst;
    out.type = Ty::Int48;
    out.imm_int = v;
    out.bytecode_pc = pc;
    return out;
}

static Inst make_float_const(double v, size_t pc) {
    Inst out;
    out.op = Op::FConst;
    out.type = Ty::Float;
    out.imm_float = v;
    out.bytecode_pc = pc;
    return out;
}

static Inst make_bool_const(bool v, size_t pc) {
    Inst out;
    out.op = Op::BConst;
    out.type = Ty::Bool;
    out.imm_int = v ? 1 : 0;
    out.bytecode_pc = pc;
    return out;
}

// `overflowed` = exact integer result didn't fit int64 (so certainly not int48)
static Inst number_result_checked(bool overflowed, int64_t v, double fallback, size_t pc) {
    if (!overflowed && fits_int48_i64(v)) {
        return make_int_const(v, pc);
    }
    return make_float_const(fallback, pc);
}

static bool fold_binary_const(Op op, const Inst &lhs, const Inst &rhs,
                              size_t pc, Inst &out) {
    switch (op) {
        case Op::DynAdd:
            if (lhs.op == Op::IConst && rhs.op == Op::IConst) {
                int64_t r;
                bool ovf = add_overflow_i64(lhs.imm_int, rhs.imm_int, &r);
                out = number_result_checked(ovf, r, (double)lhs.imm_int + (double)rhs.imm_int, pc);
                return true;
            }
            if (numeric_const(lhs) && numeric_const(rhs)) {
                out = make_float_const(const_number(lhs) + const_number(rhs), pc);
                return true;
            }
            return false;
        case Op::DynSub:
            if (lhs.op == Op::IConst && rhs.op == Op::IConst) {
                int64_t r;
                bool ovf = sub_overflow_i64(lhs.imm_int, rhs.imm_int, &r);
                out = number_result_checked(ovf, r, (double)lhs.imm_int - (double)rhs.imm_int, pc);
                return true;
            }
            if (numeric_const(lhs) && numeric_const(rhs)) {
                out = make_float_const(const_number(lhs) - const_number(rhs), pc);
                return true;
            }
            return false;
        case Op::DynMul:
            if (lhs.op == Op::IConst && rhs.op == Op::IConst) {
                int64_t r;
                if (mul_overflow_i48(lhs.imm_int, rhs.imm_int, &r)) {
                    out = make_float_const((double)lhs.imm_int * (double)rhs.imm_int, pc);
                } else {
                    out = make_int_const(r, pc);
                }
                return true;
            }
            if (numeric_const(lhs) && numeric_const(rhs)) {
                out = make_float_const(const_number(lhs) * const_number(rhs), pc);
                return true;
            }
            return false;
        case Op::DynMod:
            if (lhs.op == Op::IConst && rhs.op == Op::IConst) {
                if (rhs.imm_int == 0) {
                    out = make_float_const(std::nan(""), pc);
                } else {
                    out = make_int_const(lhs.imm_int % rhs.imm_int, pc);
                }
                return true;
            }
            if (numeric_const(lhs) && numeric_const(rhs)) {
                out = make_float_const(std::fmod(const_number(lhs), const_number(rhs)), pc);
                return true;
            }
            return false;
        case Op::DynDiv:
            if (numeric_const(lhs) && numeric_const(rhs)) {
                double rn = const_number(rhs);
                if (rn == 0.0) {
                    out = make_float_const(std::nan(""), pc);
                } else if (lhs.op == Op::IConst && rhs.op == Op::IConst &&
                           lhs.imm_int % rhs.imm_int == 0) {
                    out = make_int_const(lhs.imm_int / rhs.imm_int, pc);
                } else {
                    out = make_float_const(const_number(lhs) / rn, pc);
                }
                return true;
            }
            return false;
        case Op::DynCmpLt:
        case Op::DynCmpLe:
        case Op::DynCmpGt:
        case Op::DynCmpGe:
            if (!numeric_const(lhs) || !numeric_const(rhs)) {
                return false;
            }
            if (op == Op::DynCmpLt) {
                out = make_bool_const(const_number(lhs) < const_number(rhs), pc);
            }
            if (op == Op::DynCmpLe) {
                out = make_bool_const(const_number(lhs) <= const_number(rhs), pc);
            }
            if (op == Op::DynCmpGt) {
                out = make_bool_const(const_number(lhs) > const_number(rhs), pc);
            }
            if (op == Op::DynCmpGe) {
                out = make_bool_const(const_number(lhs) >= const_number(rhs), pc);
            }
            return true;
        case Op::DynCmpEq:
        case Op::DynCmpNe: {
            bool known = false;
            bool eq = false;
            if (numeric_const(lhs) && numeric_const(rhs)) {
                known = true;
                eq = const_number(lhs) == const_number(rhs);
            } else if (lhs.op == rhs.op && (lhs.op == Op::BConst || lhs.op == Op::NConst)) {
                known = true;
                eq = lhs.op == Op::NConst || lhs.imm_int == rhs.imm_int;
            }
            if (!known) {
                return false;
            }
            out = make_bool_const(op == Op::DynCmpEq ? eq : !eq, pc);
            return true;
        }
        default:
            return false;
    }
}

struct StackEntry {
    ValueId value = InvalidValue;
    bool removable = false;
};

static bool simplify_block(Func &f, Block &b) {
    bool changed = false;
    std::vector<ValueId> old = b.insts;
    std::vector<ValueId> body;
    std::vector<StackEntry> stack;

    auto push_existing = [&](ValueId v, bool removable) {
        body.push_back(v);
        stack.push_back({ v, removable });
    };
    auto push_new = [&](Inst in, bool removable) {
        ValueId v = f.add_inst(std::move(in));
        body.push_back(v);
        stack.push_back({ v, removable });
    };
    auto clear_removable = [&]() {
        for (StackEntry &e : stack) {
            e.removable = false;
        }
    };

    for (ValueId v : old) {
        Inst &in = f.inst(v);
        switch (in.op) {
            case Op::IConst:
            case Op::FConst:
            case Op::BConst:
            case Op::NConst:
            case Op::LoadConst:
                push_existing(v, true);
                break;
            case Op::LoadSlot:
                push_existing(v, false);
                break;
            case Op::LoadGlobal:
                push_existing(v, false);
                break;
            case Op::LoadCapture:
                push_existing(v, false);
                break;
            case Op::Dup:
                if (stack.empty()) {
                    push_existing(v, false);
                    break;
                }
                body.push_back(v);
                stack.push_back({ v, true });
                break;
            case Op::Pop:
                if (stack.empty()) {
                    body.push_back(v);
                    break;
                }
                if (stack.back().removable && !body.empty() && body.back() == stack.back().value) {
                    body.pop_back();
                    stack.pop_back();
                    changed = true;
                } else {
                    body.push_back(v);
                    stack.pop_back();
                    clear_removable();
                }
                break;
            case Op::StoreSlot:
                body.push_back(v);
                clear_removable();
                break;
            case Op::StoreGlobal:
                body.push_back(v);
                clear_removable();
                break;
            case Op::StoreCapture:
                body.push_back(v);
                clear_removable();
                break;
            case Op::Not:
            case Op::INeg:
            case Op::INot:
            case Op::FormatValue:
            case Op::IterArray:
                if (stack.empty()) {
                    body.push_back(v);
                    break;
                }
                stack.pop_back();
                push_existing(v, false);
                break;
            case Op::Call: {
                size_t n = (size_t)in.imm_u32 + 1;
                if (stack.size() < n) {
                    body.push_back(v);
                    stack.clear();
                    break;
                }
                for (size_t i = 0; i < n; i++) {
                    stack.pop_back();
                }
                push_existing(v, false);
                break;
            }
            case Op::CallMethod: {
                size_t n = (size_t)in.imm_int + 1;
                if (stack.size() < n) {
                    body.push_back(v);
                    stack.clear();
                    break;
                }
                for (size_t i = 0; i < n; i++) {
                    stack.pop_back();
                }
                push_existing(v, false);
                break;
            }
            case Op::LoadIndex:
                if (stack.size() < 2) {
                    body.push_back(v);
                    stack.clear();
                    break;
                }
                stack.pop_back();
                stack.pop_back();
                push_existing(v, false);
                break;
            case Op::StoreIndex:
                if (stack.size() < 3) {
                    body.push_back(v);
                    stack.clear();
                    break;
                }
                stack.pop_back();
                stack.pop_back();
                body.push_back(v);
                clear_removable();
                break;
            case Op::MakeArray:
            case Op::MakeObject: {
                size_t n = in.op == Op::MakeArray ? (size_t)in.imm_u32 : (size_t)in.imm_u32 * 2;
                if (stack.size() < n) {
                    body.push_back(v);
                    stack.clear();
                    break;
                }
                for (size_t i = 0; i < n; i++) {
                    stack.pop_back();
                }
                push_existing(v, false);
                break;
            }
            default:
                if ((is_foldable_binary(in.op) || is_nonfolding_binary(in.op)) && stack.size() >= 2) {
                    StackEntry rhs = stack.back();
                    StackEntry lhs = stack[stack.size() - 2];
                    Inst folded;
                    bool operands_are_trailing = body.size() >= 2 &&
                                                 body[body.size() - 2] == lhs.value &&
                                                 body[body.size() - 1] == rhs.value;
                    if (is_foldable_binary(in.op) && lhs.removable && rhs.removable && operands_are_trailing &&
                        fold_binary_const(in.op, f.inst(lhs.value), f.inst(rhs.value),
                                          in.bytecode_pc, folded)) {
                        body.pop_back();
                        body.pop_back();
                        stack.pop_back();
                        stack.pop_back();
                        push_new(std::move(folded), true);
                        changed = true;
                        break;
                    }
                    stack.pop_back();
                    stack.pop_back();
                    push_existing(v, is_pure_stack_value(in.op));
                    break;
                }
                body.push_back(v);
                clear_removable();
                break;
        }
    }

    if (body != b.insts) {
        b.insts = std::move(body);
        changed = true;
    }
    return changed;
}

static bool eliminate_slot_round_trips(Func &f, Block &b) {
    bool changed = false;
    std::vector<ValueId> body;
    body.reserve(b.insts.size());

    for (size_t i = 0; i < b.insts.size();) {
        if (i + 2 < b.insts.size()) {
            const Inst &a = f.inst(b.insts[i]);
            const Inst &mid = f.inst(b.insts[i + 1]);
            const Inst &c = f.inst(b.insts[i + 2]);

            // StoreSlot leaves the stored value on the VM stack. A following
            // Pop + LoadSlot of the same slot can be dropped and the original
            // stored value remains on top of the stack for the consumer.
            if (a.op == Op::StoreSlot && mid.op == Op::Pop && c.op == Op::LoadSlot &&
                a.imm_u32 == c.imm_u32) {
                body.push_back(b.insts[i]);
                i += 3;
                changed = true;
                continue;
            }

            // `x = x` used as a statement lowers to LoadSlot, StoreSlot, Pop.
            // The slot and final stack are unchanged, so all three helpers are
            // dead. Keep this exact and local to avoid crossing side effects.
            if (a.op == Op::LoadSlot && mid.op == Op::StoreSlot && c.op == Op::Pop &&
                a.imm_u32 == mid.imm_u32) {
                i += 3;
                changed = true;
                continue;
            }
        }

        body.push_back(b.insts[i]);
        i++;
    }

    if (changed) {
        b.insts = std::move(body);
    }
    return changed;
}

// Fuse producer -> StoreSlot -> Pop trios into a single fused store op that
// writes the slot directly, bypassing the operand stack. Fires only when the
// producer's SSA value has no other users in the block.
//
// Recognized shapes (i, i+1, i+2 must be a contiguous triple in b.insts):
//   IConst v; StoreSlot v @X; Pop v      ->  StoreImmSlot #imm -> @X
//   BConst v; StoreSlot v @X; Pop v      ->  StoreBImmSlot #imm -> @X
//   NConst v; StoreSlot v @X; Pop v      ->  StoreNSlot -> @X
//   LoadConst v @K; StoreSlot v @X; Pop  ->  StoreCSlot const#K -> @X
//   LoadSlot v @Y; StoreSlot v @X; Pop v ->  CopySlot @Y -> @X   (X != Y;
//                                            X == Y is already killed by
//                                            eliminate_slot_round_trips.)
static bool fuse_slot_stores(Func &f, Block &b) {
    bool changed = false;
    std::vector<ValueId> body;
    body.reserve(b.insts.size());

    // Cheap in-block user check: does any inst outside {store_i, pop_i} reference
    // the producer's ValueId? For the patterns we match, the producer is at
    // position i; the store is at i+1 and consumes it; the pop is at i+2 and
    // consumes it. In stack-machine IR the only other in-block users would be
    // subsequent ops; we scan forward from i+3. Terminators can also read it.
    auto no_other_users = [&](ValueId prod, size_t after_idx) -> bool {
        for (size_t j = after_idx; j < b.insts.size(); j++) {
            const Inst &in = f.inst(b.insts[j]);
            for (ValueId o : in.operands) {
                if (o == prod) {
                    return false;
                }
            }
        }
        if (b.terminator != InvalidValue) {
            const Inst &t = f.inst(b.terminator);
            for (ValueId o : t.operands) {
                if (o == prod) {
                    return false;
                }
            }
        }
        for (const Block &ob : f.blocks) {
            for (ValueId phi : ob.phis) {
                const Inst &pi = f.inst(phi);
                for (ValueId o : pi.operands) {
                    if (o == prod) {
                        return false;
                    }
                }
            }
        }
        return true;
    };

    for (size_t i = 0; i < b.insts.size();) {
        if (i + 2 < b.insts.size()) {
            const Inst &a = f.inst(b.insts[i]);
            const Inst &mid = f.inst(b.insts[i + 1]);
            const Inst &c = f.inst(b.insts[i + 2]);
            ValueId prod_id = b.insts[i];

            bool prod_is_const = (a.op == Op::IConst || a.op == Op::BConst ||
                                  a.op == Op::NConst || a.op == Op::LoadConst ||
                                  a.op == Op::LoadSlot);

            if (prod_is_const && mid.op == Op::StoreSlot && c.op == Op::Pop &&
                !mid.operands.empty() && mid.operands[0] == prod_id &&
                !c.operands.empty() && c.operands[0] == prod_id &&
                no_other_users(prod_id, i + 3)) {
                // Snapshot immediates BEFORE add_inst() may reallocate f.insts.
                Op prod_op = a.op;
                int64_t prod_imm_int = a.imm_int;
                uint32_t prod_imm_u32 = a.imm_u32;
                uint32_t dst_slot = mid.imm_u32;
                size_t bpc = mid.bytecode_pc;

                Inst fused;
                fused.bytecode_pc = bpc;
                fused.imm_u32 = dst_slot;
                switch (prod_op) {
                    case Op::IConst:
                        fused.op = Op::StoreImmSlot;
                        fused.imm_int = prod_imm_int;
                        break;
                    case Op::BConst:
                        fused.op = Op::StoreBImmSlot;
                        fused.imm_int = prod_imm_int ? 1 : 0;
                        break;
                    case Op::NConst:
                        fused.op = Op::StoreNSlot;
                        break;
                    case Op::LoadConst:
                        fused.op = Op::StoreCSlot;
                        fused.imm_int = (int64_t)prod_imm_u32; // const pool idx
                        break;
                    case Op::LoadSlot:
                        // Skip the copy-to-self case: eliminate_slot_round_trips
                        // handles LoadSlot @X; StoreSlot @X; Pop as a no-op.
                        if (prod_imm_u32 == dst_slot) {
                            goto no_fuse;
                        }
                        fused.op = Op::CopySlot;
                        fused.imm_int = (int64_t)prod_imm_u32; // src slot
                        break;
                    default:
                        goto no_fuse;
                }
                {
                    ValueId fused_id = f.add_inst(std::move(fused));
                    body.push_back(fused_id);
                    i += 3;
                    changed = true;
                    continue;
                }
            no_fuse:;
            }
        }
        body.push_back(b.insts[i]);
        i++;
    }
    if (changed) {
        b.insts = std::move(body);
    }
    return changed;
}

static bool fold_branch(Func &f, Block &b) {
    if (b.terminator == InvalidValue) {
        return false;
    }
    Inst &t = f.inst(b.terminator);
    if (t.op != Op::Branch || t.operands.empty() || b.insts.empty()) {
        return false;
    }
    ValueId cond = t.operands[0];
    if (b.insts.back() != cond || !is_const(f.inst(cond).op)) {
        return false;
    }
    bool take_true = truthy_const(f.inst(cond));
    b.insts.pop_back();
    t.op = Op::Jump;
    t.operands.clear();
    t.target0 = take_true ? t.target0 : t.target1;
    t.target1 = InvalidBlock;
    b.succs.clear();
    b.succs.push_back(t.target0);
    return true;
}

static BlockId jump_thread_target(const Func &f, BlockId target) {
    BlockId cur = target;
    for (size_t depth = 0; depth < f.blocks.size(); depth++) {
        if (cur < 0 || (size_t)cur >= f.blocks.size()) {
            return target;
        }
        const Block &b = f.blocks[cur];
        if (!b.phis.empty() || !b.insts.empty() || b.terminator == InvalidValue) {
            return cur;
        }
        const Inst &t = f.inst(b.terminator);
        if (t.op != Op::Jump || t.target0 == cur) {
            return cur;
        }
        cur = t.target0;
    }
    return target; // cycle guard
}

static bool thread_jumps(Func &f) {
    bool changed = false;
    for (Block &b : f.blocks) {
        if (b.terminator == InvalidValue) {
            continue;
        }
        Inst &t = f.inst(b.terminator);
        if (t.op == Op::Jump) {
            BlockId nt = jump_thread_target(f, t.target0);
            if (nt != t.target0) {
                t.target0 = nt;
                b.succs.clear();
                b.succs.push_back(nt);
                changed = true;
            }
        } else if (t.op == Op::Branch) {
            BlockId nt0 = jump_thread_target(f, t.target0);
            BlockId nt1 = jump_thread_target(f, t.target1);
            if (nt0 != t.target0 || nt1 != t.target1) {
                t.target0 = nt0;
                t.target1 = nt1;
                b.succs.clear();
                b.succs.push_back(nt0);
                b.succs.push_back(nt1);
                changed = true;
            }
        }
    }
    return changed;
}

// Local (within-block) constant propagation through slots.
// Between a StoreSlot of a constant and the next StoreSlot to that slot,
// every LoadSlot of it yields that constant, so the load is rewritten in place to a copy of the constant.
// This is stack-safe within a block (a LoadSlot and a constant both push exactly one value).
static bool propagate_slot_consts(Func &f, Block &b) {
    bool changed = false;
    std::unordered_map<uint32_t, ValueId> known; // slot -> const-valued inst id
    for (ValueId v : b.insts) {
        Inst &in = f.inst(v);
        if (in.op == Op::LoadSlot) {
            auto it = known.find(in.imm_u32);
            if (it != known.end()) {
                const Inst &c = f.inst(it->second);
                in.op = c.op; // rewrite load -> constant, keeping this value's id
                in.type = c.type;
                in.imm_int = c.imm_int;
                in.imm_float = c.imm_float;
                in.operands.clear();
                changed = true;
            }
        } else if (in.op == Op::StoreSlot) {
            ValueId sv = in.operands.empty() ? InvalidValue : in.operands[0];
            if (sv != InvalidValue && is_const(f.inst(sv).op)) {
                known[in.imm_u32] = sv;
            } else {
                known.erase(in.imm_u32);
            }
        }
        // No other op modifies a slot; LoadSlot reads only.
    }
    return changed;
}

// Dead store elimination: a StoreSlot to a slot that is never read (no LoadSlot
// anywhere) is unobservable - slots are function-local and IR-eligible code has no
// closures/upvalues that could alias them. StoreSlot is stack-neutral (it peeks),
// so dropping it preserves the value stack; the peeked value stays on the stack for
// the following Pop. Composes with const-prop (which removes the loads, making the
// stores dead) and DCE (which then removes the now-unused value + its Pop).
static bool eliminate_dead_stores(Func &f) {
    std::vector<bool> loaded(f.num_slots, false);
    for (const Inst &in : f.insts) {
        if (in.op == Op::LoadSlot && in.imm_u32 < loaded.size()) {
            loaded[in.imm_u32] = true;
        }
    }
    bool changed = false;
    for (Block &b : f.blocks) {
        std::vector<ValueId> body;
        body.reserve(b.insts.size());
        for (ValueId v : b.insts) {
            const Inst &in = f.inst(v);
            if (in.op == Op::StoreSlot && in.imm_u32 < loaded.size() && !loaded[in.imm_u32]) {
                changed = true; // unobservable write -> drop
                continue;
            }
            body.push_back(v);
        }
        if (body.size() != b.insts.size()) {
            b.insts = std::move(body);
        }
    }
    return changed;
}

static void rebuild_preds(Func &f);

// Local value numbering (within-block CSE). Simulates the block's operand stack,
// giving each value an exact integer value-number.
// When a freshly pushed value equals the entry directly below it on the stack (i.e.
// a sub-expression was recomputed immediately after an identical one), its whole
// span is replaced by a single Dup of that lower value.
static bool local_value_numbering(Func &f, Block &b) {
    std::unordered_map<std::string, int> vnmap;
    int next_vn = 0;
    auto getvn = [&](const std::string &key) -> int {
        auto it = vnmap.find(key);
        if (it != vnmap.end()) {
            return it->second;
        }
        return vnmap[key] = next_vn++;
    };
    std::unordered_map<uint32_t, int> slot_version;
    auto slot_ver = [&](uint32_t s) -> int {
        auto it = slot_version.find(s);
        return it == slot_version.end() ? 0 : it->second;
    };

    struct E {
        int vn;
        ValueId vid;
        size_t body_pos; // index in `body` where this value's sub-expression begins
    };
    std::vector<E> st;
    std::vector<ValueId> body;
    bool changed = false;

    auto push_leaf = [&](ValueId v, int vn) {
        // CSE a leaf (const / LoadSlot) that re-pushes the value already on top.
        if (!st.empty() && st.back().vn == vn) {
            Inst dup;
            dup.op = Op::Dup;
            dup.operands = { st.back().vid };
            dup.bytecode_pc = f.inst(v).bytecode_pc;
            ValueId d = f.add_inst(std::move(dup));
            body.push_back(d);
            st.push_back({ vn, d, body.size() - 1 });
            changed = true;
        } else {
            body.push_back(v);
            st.push_back({ vn, v, body.size() - 1 });
        }
    };

    for (ValueId v : b.insts) {
        Inst &in = f.inst(v);
        switch (in.op) {
            case Op::IConst:
                push_leaf(v, getvn("i:" + std::to_string(in.imm_int)));
                break;
            case Op::FConst: {
                int64_t bits;
                memcpy(&bits, &in.imm_float, sizeof(bits));
                push_leaf(v, getvn("f:" + std::to_string(bits)));
                break;
            }
            case Op::BConst:
                push_leaf(v, getvn("b:" + std::to_string(in.imm_int)));
                break;
            case Op::NConst:
                push_leaf(v, getvn("none"));
                break;
            case Op::LoadConst:
                push_leaf(v, getvn("const:" + std::to_string(in.imm_u32)));
                break;
            case Op::LoadSlot:
                push_leaf(v, getvn("ld:" + std::to_string(in.imm_u32) + ":" +
                                   std::to_string(slot_ver(in.imm_u32))));
                break;
            case Op::LoadGlobal:
                push_leaf(v, getvn("global:" + std::to_string(in.imm_u32)));
                break;
            case Op::LoadCapture:
                // Capture cell idx is fixed for the whole function body, so
                // "capture:idx" is a stable key like a global.
                // The immediate-CSE window only fires against a matching stack top.
                // StoreCapture (clears vnmap) and Call (pushes a distinct vn) both break it.
                push_leaf(v, getvn("capture:" + std::to_string(in.imm_u32)));
                break;
            case Op::Dup:
                body.push_back(v);
                if (st.empty()) {
                    st.clear();
                } else {
                    st.push_back({ st.back().vn, v, body.size() - 1 });
                }
                break;
            case Op::StoreSlot:
                // stack-neutral (peek); invalidate later loads of this slot
                slot_version[in.imm_u32] = slot_ver(in.imm_u32) + 1;
                body.push_back(v);
                break;
            case Op::StoreGlobal:
                // stack-neutral (peek); invalidate global/load-index value numbers.
                vnmap.clear();
                body.push_back(v);
                break;
            case Op::StoreCapture:
                // stack-neutral (peek); a write-through to a captured cell can be
                // observed by aliasing loads, so drop all value numbers.
                vnmap.clear();
                body.push_back(v);
                break;
            case Op::Not:
            case Op::INeg:
            case Op::INot:
            case Op::FormatValue: {
                if (st.empty()) {
                    body.push_back(v);
                    st.clear();
                    break;
                }
                E a = st.back();
                st.pop_back();
                in.operands = { a.vid };
                int vn = getvn(std::to_string((int)in.op) + ":" + std::to_string(a.vn));
                body.push_back(v);
                st.push_back({ vn, v, a.body_pos });
                break;
            }
            case Op::Pop:
                body.push_back(v);
                if (!st.empty()) {
                    st.pop_back();
                }
                break;
            case Op::DynAdd:
            case Op::DynSub:
            case Op::DynMul:
            case Op::DynMod:
            case Op::DynDiv:
            case Op::DynCmpLt:
            case Op::DynCmpLe:
            case Op::DynCmpGt:
            case Op::DynCmpGe:
            case Op::DynCmpEq:
            case Op::DynCmpNe: {
                if (st.size() < 2) {
                    body.push_back(v);
                    st.clear(); // lost track -> stop CSE for the rest of the block
                    break;
                }
                E rhs = st.back();
                st.pop_back();
                E lhs = st.back();
                st.pop_back();
                in.operands = { lhs.vid, rhs.vid }; // keep operands consistent with CSE
                int o1 = lhs.vn, o2 = rhs.vn;
                if ((in.op == Op::DynAdd || in.op == Op::DynMul) && o1 > o2) {
                    std::swap(o1, o2); // commutative: normalize operand order
                }
                int vn = getvn(std::to_string((int)in.op) + ":" + std::to_string(o1) +
                               ":" + std::to_string(o2));
                size_t span_start = lhs.body_pos; // expression begins at the left operand
                body.push_back(v);
                // CSE: identical to the value directly below? -> replace span with Dup.
                if (!st.empty() && st.back().vn == vn) {
                    ValueId below = st.back().vid;
                    Inst dup;
                    dup.op = Op::Dup;
                    dup.operands = { below };
                    dup.bytecode_pc = in.bytecode_pc;
                    ValueId d = f.add_inst(std::move(dup));
                    body.resize(span_start);
                    body.push_back(d);
                    st.push_back({ vn, d, span_start });
                    changed = true;
                } else {
                    st.push_back({ vn, v, span_start });
                }
                break;
            }
            case Op::StrConcat:
            case Op::IAnd:
            case Op::IOr:
            case Op::IXor:
            case Op::IShl:
            case Op::IShr: {
                if (st.size() < 2) {
                    body.push_back(v);
                    st.clear();
                    break;
                }
                E rhs = st.back();
                st.pop_back();
                E lhs = st.back();
                st.pop_back();
                in.operands = { lhs.vid, rhs.vid };
                int o1 = lhs.vn, o2 = rhs.vn;
                if ((in.op == Op::IAnd || in.op == Op::IOr || in.op == Op::IXor) && o1 > o2) {
                    std::swap(o1, o2);
                }
                int vn = getvn(std::to_string((int)in.op) + ":" + std::to_string(o1) +
                               ":" + std::to_string(o2));
                body.push_back(v);
                st.push_back({ vn, v, lhs.body_pos });
                break;
            }
            case Op::Call: {
                size_t n = (size_t)in.imm_u32 + 1;
                body.push_back(v);
                if (st.size() < n) {
                    st.clear();
                } else {
                    for (size_t i = 0; i < n; i++) {
                        st.pop_back();
                    }
                    st.push_back({ getvn("call:" + std::to_string((int)v)), v, body.size() - 1 });
                }
                break;
            }
            case Op::CallMethod: {
                size_t n = (size_t)in.imm_int + 1;
                body.push_back(v);
                if (st.size() < n) {
                    st.clear();
                } else {
                    for (size_t i = 0; i < n; i++) {
                        st.pop_back();
                    }
                    st.push_back({ getvn("callm:" + std::to_string((int)v)), v, body.size() - 1 });
                }
                break;
            }
            case Op::LoadIndex:
                body.push_back(v);
                if (st.size() < 2) {
                    st.clear();
                } else {
                    st.pop_back();
                    st.pop_back();
                    st.push_back({ getvn("idx:" + std::to_string((int)v)), v, body.size() - 1 });
                }
                break;
            case Op::StoreIndex:
                body.push_back(v);
                if (st.size() < 3) {
                    st.clear();
                } else {
                    st.pop_back();
                    st.pop_back();
                    // object remains on stack; unknown mutation invalidates index values
                    vnmap.clear();
                }
                break;
            case Op::MakeArray:
            case Op::MakeObject: {
                size_t n = in.op == Op::MakeArray ? (size_t)in.imm_u32 : (size_t)in.imm_u32 * 2;
                body.push_back(v);
                if (st.size() < n) {
                    st.clear();
                } else {
                    for (size_t i = 0; i < n; i++) {
                        st.pop_back();
                    }
                    st.push_back({ getvn("agg:" + std::to_string((int)v)), v, body.size() - 1 });
                }
                break;
            }
            case Op::IterArray: {
                // Pop 1 / push 1. Use a per-instruction unique value number so it
                // is never CSE-deduped (normalizing the same object twice yields
                // distinct fresh key-arrays; two calls must not be merged).
                body.push_back(v);
                if (st.empty()) {
                    st.clear();
                } else {
                    E a = st.back();
                    st.pop_back();
                    in.operands = { a.vid };
                    st.push_back({ getvn("iter:" + std::to_string((int)v)), v, body.size() - 1 });
                }
                break;
            }
            default:
                body.push_back(v);
                st.clear();
                break;
        }
    }

    if (changed && body != b.insts) {
        b.insts = std::move(body);
        return true;
    }
    return false;
}

// Phi-branch threading: collapse a merge block B of the shape
//   B: v = phi(s0..sk);  branch v ? T : F        (one phi, empty body)
// by pushing the branch into every predecessor.
//
// Requires: phi.operands[i] positionally matches B.preds[i], T and F have no
// phis, and the phi result is used only as B's branch condition (otherwise a live use dangles when B is removed).
static bool thread_phi_branches(Func &f) {
    bool any = false;
    for (size_t guard = 0; guard <= f.blocks.size(); guard++) {
        rebuild_preds(f);
        bool changed = false;
        for (size_t bi = 0; bi < f.blocks.size(); bi++) {
            if ((BlockId)bi == f.entry) {
                continue;
            }
            Block &B = f.blocks[bi];
            if (B.preds.empty() || !B.insts.empty() || B.phis.size() != 1 ||
                B.terminator == InvalidValue) {
                continue;
            }
            Inst &term = f.inst(B.terminator);
            if (term.op != Op::Branch || term.operands.size() != 1) {
                continue;
            }
            ValueId phi_id = B.phis[0];
            if (term.operands[0] != phi_id) {
                continue; // branch must test the phi
            }
            Inst &phi = f.inst(phi_id);
            if (phi.op != Op::Phi || phi.operands.size() != B.preds.size()) {
                continue; // desynced operands<->preds: skip (never true pre-fixpoint)
            }
            BlockId T = term.target0, F = term.target1;
            if (T < 0 || F < 0 || (size_t)T >= f.blocks.size() ||
                (size_t)F >= f.blocks.size() || T == (BlockId)bi ||
                F == (BlockId)bi) {
                continue;
            }
            if (!f.blocks[T].phis.empty() || !f.blocks[F].phis.empty()) {
                continue; // targets would gain preds needing phi fixup
            }
            // Every predecessor must reach B via an unconditional Jump.
            bool preds_ok = true;
            for (BlockId p : B.preds) {
                if (p < 0 || (size_t)p >= f.blocks.size()) {
                    preds_ok = false;
                    break;
                }
                const Block &P = f.blocks[p];
                if (P.terminator == InvalidValue) {
                    preds_ok = false;
                    break;
                }
                const Inst &pt = f.inst(P.terminator);
                if (pt.op != Op::Jump || pt.target0 != (BlockId)bi) {
                    preds_ok = false;
                    break;
                }
            }
            if (!preds_ok) {
                continue;
            }
            // The phi result must feed nothing but B's branch condition.
            int uses = 0;
            for (const Block &blk : f.blocks) {
                for (ValueId pv : blk.phis) {
                    for (ValueId o : f.inst(pv).operands) {
                        if (o == phi_id) {
                            uses++;
                        }
                    }
                }
                for (ValueId iv : blk.insts) {
                    for (ValueId o : f.inst(iv).operands) {
                        if (o == phi_id) {
                            uses++;
                        }
                    }
                }
                if (blk.terminator != InvalidValue) {
                    for (ValueId o : f.inst(blk.terminator).operands) {
                        if (o == phi_id) {
                            uses++;
                        }
                    }
                }
            }
            if (uses != 1) {
                continue;
            }
            // Rewrite each pred's terminating Jump into the merged Branch.
            for (size_t i = 0; i < B.preds.size(); i++) {
                Block &P = f.blocks[B.preds[i]];
                Inst &pt = f.inst(P.terminator);
                pt.op = Op::Branch;
                pt.operands = { phi.operands[i] };
                pt.target0 = T;
                pt.target1 = F;
                P.succs.clear();
                P.succs.push_back(T);
                P.succs.push_back(F);
            }
            // B is now unreachable. Drop its outgoing edges so a subsequent
            // rebuild_preds cannot hand T/F a phantom predecessor before
            // dead-block removal compacts B away.
            B.succs.clear();
            B.preds.clear();
            changed = true;
            any = true;
            break; // preds mutated -> rebuild and rescan
        }
        if (!changed) {
            break;
        }
    }
    if (any) {
        rebuild_preds(f);
    }
    return any;
}

// Straight-line block merging: when A ends in an unconditional Jump to B and B's
// only predecessor is A, fuse B's body and terminator into A and drop the jump.
static bool merge_blocks(Func &f) {
    rebuild_preds(f);
    std::vector<bool> removed(f.blocks.size(), false);
    bool changed = false;
    for (size_t ai = 0; ai < f.blocks.size(); ai++) {
        if (removed[ai]) {
            continue;
        }
        Block &A = f.blocks[ai];
        if (A.terminator == InvalidValue) {
            continue;
        }
        if (f.inst(A.terminator).op != Op::Jump) {
            continue;
        }
        BlockId bid = f.inst(A.terminator).target0;
        if (bid < 0 || (size_t)bid >= f.blocks.size() || bid == (BlockId)ai ||
            bid == f.entry || removed[bid]) {
            continue;
        }
        Block &B = f.blocks[bid];
        if (B.preds.size() != 1 || B.preds[0] != (BlockId)ai || !B.phis.empty()) {
            continue; // B must be entered only from A
        }
        A.insts.insert(A.insts.end(), B.insts.begin(), B.insts.end());
        A.terminator = B.terminator;
        A.succs = B.succs;
        removed[bid] = true;
        B.insts.clear();
        B.terminator = InvalidValue;
        B.succs.clear();
        B.preds.clear();
        changed = true;
    }
    if (changed) {
        rebuild_preds(f);
    }
    return changed;
}

// Remove blocks unreachable from the entry (left behind by branch-folding and jump-threading) from the IR proper.
// Compacts f.blocks and remaps every BlockId reference: Func::entry, each Block::id/succs/preds, and Jump/Branch terminator targets.
static bool remove_unreachable_blocks(Func &f) {
    if (f.blocks.empty() || f.entry < 0 || (size_t)f.entry >= f.blocks.size()) {
        return false;
    }
    std::vector<bool> reach(f.blocks.size(), false);
    std::vector<BlockId> stk;
    stk.push_back(f.entry);
    reach[f.entry] = true;
    while (!stk.empty()) {
        BlockId b = stk.back();
        stk.pop_back();
        for (BlockId s : f.blocks[b].succs) {
            if (s >= 0 && (size_t)s < reach.size() && !reach[s]) {
                reach[s] = true;
                stk.push_back(s);
            }
        }
    }

    size_t nreach = 0;
    for (bool r : reach) {
        if (r) {
            nreach++;
        }
    }
    if (nreach == f.blocks.size()) {
        return false; // nothing unreachable
    }

    // old BlockId -> new (compacted) BlockId
    std::vector<BlockId> remap(f.blocks.size(), InvalidBlock);
    BlockId next = 0;
    for (size_t i = 0; i < f.blocks.size(); i++) {
        if (reach[i]) {
            remap[i] = next++;
        }
    }

    std::vector<Block> compact;
    compact.reserve(nreach);
    for (size_t i = 0; i < f.blocks.size(); i++) {
        if (!reach[i]) {
            continue;
        }
        Block b = std::move(f.blocks[i]);
        b.id = remap[i];
        for (BlockId &s : b.succs) {
            s = remap[s]; // every succ of a reachable block is reachable
        }
        if (b.terminator != InvalidValue) {
            Inst &t = f.inst(b.terminator);
            if (t.op == Op::Jump) {
                t.target0 = remap[t.target0];
            } else if (t.op == Op::Branch) {
                t.target0 = remap[t.target0];
                t.target1 = remap[t.target1];
            }
        }
        b.preds.clear(); // rebuilt below
        compact.push_back(std::move(b));
    }
    f.blocks = std::move(compact);
    f.entry = remap[f.entry];
    for (Block &b : f.blocks) {
        for (BlockId s : b.succs) {
            if (s >= 0 && (size_t)s < f.blocks.size()) {
                f.blocks[s].preds.push_back(b.id);
            }
        }
    }
    return true;
}

static void rebuild_preds(Func &f) {
    for (Block &b : f.blocks) {
        b.preds.clear();
    }
    for (Block &b : f.blocks) {
        for (BlockId s : b.succs) {
            if (s >= 0 && (size_t)s < f.blocks.size()) {
                f.blocks[s].preds.push_back(b.id);
            }
        }
    }
}

// report-only GVN / global-CSE opportunity detector.
//
// Purely analysis!
// Builds a dominator tree, natural-loop
// detection, available-expressions dataflow, then counts value-producing
// instructions that are fully redundant across block boundaries.
// Dominators classify loop membership only.
// Memory loads carry a dependency set so a store or call kills exactly the loads it may invalidate.
namespace {

enum GvnClass {
    GC_DIVMOD_INT = 0, // idiv-class: Div, IMod, DynDiv, DynMod  (EXPENSIVE)
    GC_FDIV,           // FDiv                                   (medium)
    GC_LGLOBAL,        // LoadGlobal   (hash + IC)               (EXPENSIVE)
    GC_LPROP,          // LoadProperty (shape/IC)                (EXPENSIVE)
    GC_LINDEX,         // LoadIndex    (bounds + box)            (EXPENSIVE)
    GC_LSLOT,          // LoadSlot     (cheap reload)
    GC_LCAP,           // LoadCapture
    GC_STRCAT,         // StrConcat / FormatValue (alloc)        (EXPENSIVE)
    GC_FARITH,         // FAdd/FSub/FMul
    GC_IARITH,         // integer arith / bitops
    GC_CMP,            // comparisons
    GC_DYN,            // DynAdd/DynSub/DynMul (verify purity)   (EXPENSIVE-ish)
    GC_BOX,            // Box/Unbox/GuardInt48/Not
    GC_OTHER,          // not a redundancy candidate
    GC_N
};
static const char *gc_name(int c) {
    static const char *n[GC_N] = {
        "divmod", "fdiv", "lglobal", "lprop", "lindex", "lslot", "lcap",
        "strcat", "farith", "iarith", "cmp", "dyn", "box", "other"
    };
    return (c >= 0 && c < GC_N) ? n[c] : "?";
}
static bool gc_expensive(int c) {
    return c == GC_DIVMOD_INT || c == GC_LGLOBAL || c == GC_LPROP ||
           c == GC_LINDEX || c == GC_STRCAT || c == GC_DYN;
}
static int classify(Op op) {
    switch (op) {
        case Op::Div:
        case Op::IMod:
        case Op::DynDiv:
        case Op::DynMod:
            return GC_DIVMOD_INT;
        case Op::FDiv:
            return GC_FDIV;
        case Op::LoadGlobal:
            return GC_LGLOBAL;
        case Op::LoadProperty:
            return GC_LPROP;
        case Op::LoadIndex:
            return GC_LINDEX;
        case Op::LoadSlot:
            return GC_LSLOT;
        case Op::LoadCapture:
            return GC_LCAP;
        case Op::StrConcat:
        case Op::FormatValue:
            return GC_STRCAT;
        case Op::FAdd:
        case Op::FSub:
        case Op::FMul:
            return GC_FARITH;
        case Op::IAdd:
        case Op::ISub:
        case Op::IMul:
        case Op::INeg:
        case Op::IAnd:
        case Op::IOr:
        case Op::IXor:
        case Op::INot:
        case Op::IShl:
        case Op::IShr:
            return GC_IARITH;
        case Op::ICmpLt:
        case Op::ICmpLe:
        case Op::ICmpGt:
        case Op::ICmpGe:
        case Op::ICmpEq:
        case Op::ICmpNe:
        case Op::FCmpLt:
        case Op::FCmpLe:
        case Op::FCmpGt:
        case Op::FCmpGe:
        case Op::FCmpEq:
        case Op::FCmpNe:
        case Op::DynCmpLt:
        case Op::DynCmpLe:
        case Op::DynCmpGt:
        case Op::DynCmpGe:
        case Op::DynCmpEq:
        case Op::DynCmpNe:
            return GC_CMP;
        case Op::DynAdd:
        case Op::DynSub:
        case Op::DynMul:
            return GC_DYN;
        case Op::Box:
        case Op::Unbox:
        case Op::GuardInt48:
        case Op::Not:
            return GC_BOX;
        default:
            return GC_OTHER;
    }
}
// Commutative ops whose operand order should be normalized when value-numbering.
static bool commutative(Op op) {
    switch (op) {
        case Op::IAdd:
        case Op::IMul:
        case Op::IAnd:
        case Op::IOr:
        case Op::IXor:
        case Op::FAdd:
        case Op::FMul:
        case Op::ICmpEq:
        case Op::ICmpNe:
        case Op::FCmpEq:
        case Op::FCmpNe:
        case Op::DynAdd:
        case Op::DynMul:
        case Op::DynCmpEq:
        case Op::DynCmpNe:
            return true;
        default:
            return false;
    }
}

// Base memory locations an expression's value depends on, slots are tracked per-id.
struct Deps {
    std::vector<uint32_t> slots; // sorted, unique
    bool global = false, heap = false, capture = false;
    void add_slot(uint32_t s) {
        auto it = std::lower_bound(slots.begin(), slots.end(), s);
        if (it == slots.end() || *it != s) {
            slots.insert(it, s);
        }
    }
    void merge(const Deps &o) {
        for (uint32_t s : o.slots) {
            add_slot(s);
        }
        global |= o.global;
        heap |= o.heap;
        capture |= o.capture;
    }
    bool has_slot(uint32_t s) const {
        return std::binary_search(slots.begin(), slots.end(), s);
    }
};

// Program-wide accumulator; its destructor prints the aggregate at process exit
struct GvnTotals {
    long xred = 0, xred_loop = 0, part = 0;
    long cls[GC_N] = { 0 };
    long cls_loop[GC_N] = { 0 };
    int funcs = 0, funcs_hit = 0;
    ~GvnTotals() {
        if (funcs == 0) {
            return;
        }
        fprintf(stderr,
                "[GVN] ===== TOTAL over %d funcs (%d with cross-block redundancy) =====\n",
                funcs,
                funcs_hit);
        fprintf(stderr, "[GVN] xred=%ld (in-loop=%ld)  partial=%ld\n", xred, xred_loop, part);
        long exp = 0, exp_loop = 0;
        for (int c = 0; c < GC_N; c++) {
            if (cls[c] == 0) {
                continue;
            }

            fprintf(stderr,
                    "[GVN]   %-8s %5ld (loop %ld)%s\n",
                    gc_name(c),
                    cls[c],
                    cls_loop[c],
                    gc_expensive(c) ? "  <== EXPENSIVE" : "");

            if (gc_expensive(c)) {
                exp += cls[c];
                exp_loop += cls_loop[c];
            }
        }
        fprintf(stderr,
                "[GVN]   EXPENSIVE eliminable = %ld (in-loop = %ld)\n",
                exp, exp_loop);
    }
};

} // namespace

void gvn_report(const Func &f, const char *fn_name, uint32_t fn_idx) {
    static GvnTotals totals; // prints aggregate at exit
    const size_t NB = f.blocks.size();
    if (NB == 0 || f.entry < 0 || (size_t)f.entry >= NB) {
        return;
    }
    totals.funcs++;

    // reverse-postorder over reachable blocks
    std::vector<int> rpo_num(NB, -1);
    std::vector<BlockId> order; // postorder
    {
        std::vector<char> seen(NB, 0);
        std::vector<std::pair<BlockId, size_t>> stk;
        stk.push_back({ f.entry, 0 });
        seen[f.entry] = 1;
        while (!stk.empty()) {
            BlockId b = stk.back().first;
            size_t &i = stk.back().second;
            const Block &B = f.blocks[b];
            if (i < B.succs.size()) {
                BlockId s = B.succs[i++];
                if (s >= 0 && (size_t)s < NB && !seen[s]) {
                    seen[s] = 1;
                    stk.push_back({ s, 0 });
                }
            } else {
                order.push_back(b);
                stk.pop_back();
            }
        }
    }
    std::vector<BlockId> rpo(order.rbegin(), order.rend());
    for (size_t i = 0; i < rpo.size(); i++) {
        rpo_num[rpo[i]] = (int)i;
    }

    // dominator tree (Cooper-Harvey-Kennedy)
    std::vector<BlockId> idom(NB, InvalidBlock);
    idom[f.entry] = f.entry;
    auto intersect = [&](BlockId a, BlockId b) -> BlockId {
        while (a != b) {
            while (rpo_num[a] > rpo_num[b]) {
                a = idom[a];
            }
            while (rpo_num[b] > rpo_num[a]) {
                b = idom[b];
            }
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockId b : rpo) {
            if (b == f.entry) {
                continue;
            }
            BlockId newidom = InvalidBlock;
            for (BlockId p : f.blocks[b].preds) {
                if (p < 0 || (size_t)p >= NB || rpo_num[p] < 0) {
                    continue;
                }
                if (idom[p] == InvalidBlock) {
                    continue;
                }
                newidom = (newidom == InvalidBlock) ? p : intersect(p, newidom);
            }
            if (newidom != InvalidBlock && idom[b] != newidom) {
                idom[b] = newidom;
                changed = true;
            }
        }
    }
    auto dominates = [&](BlockId a, BlockId b) -> bool {
        if (a == b) {
            return true;
        }
        for (BlockId x = b; x != InvalidBlock && x != f.entry; x = idom[x]) {
            if (idom[x] == x) {
                break;
            }
            if (idom[x] == a) {
                return true;
            }
        }
        return a == f.entry;
    };

    // natural-loop membership (back edge n->h where h dom n)
    std::vector<char> in_loop(NB, 0);
    for (BlockId n : rpo) {
        for (BlockId h : f.blocks[n].succs) {
            if (h < 0 || (size_t)h >= NB || rpo_num[h] < 0) {
                continue;
            }
            if (!dominates(h, n)) {
                continue; // not a back edge
            }
            in_loop[h] = 1;
            // collect loop body: nodes reaching n without passing through h
            std::vector<BlockId> work;
            in_loop[n] = 1;
            work.push_back(n);
            while (!work.empty()) {
                BlockId x = work.back();
                work.pop_back();
                for (BlockId p : f.blocks[x].preds) {
                    if (p < 0 || (size_t)p >= NB) {
                        continue;
                    }
                    if (!in_loop[p]) {
                        in_loop[p] = 1;
                        work.push_back(p);
                    }
                }
            }
        }
    }

    // value numbering
    std::unordered_map<std::string, int> vnint;
    std::vector<Deps> vndeps;
    auto mkvn = [&](const std::string &key, const Deps &d) -> int {
        auto it = vnint.find(key);
        if (it != vnint.end()) {
            return it->second;
        }
        int id = (int)vndeps.size();
        vnint[key] = id;
        vndeps.push_back(d);
        return id;
    };
    const size_t NI = f.insts.size();
    std::vector<int> vn(NI, -1);
    auto opvn = [&](ValueId v) -> int {
        if (v >= 0 && (size_t)v < NI && vn[v] >= 0) {
            return vn[v];
        }
        return mkvn("dangling:" + std::to_string((int)v), Deps{}); // conservative
    };
    for (ValueId id = 0; id < (ValueId)NI; id++) {
        const Inst &in = f.insts[id];
        Op op = in.op;
        Deps d;
        std::string key;
        auto uniq = [&]() { key = "uniq:" + std::to_string((int)id); };
        switch (op) {
            case Op::IConst:
                key = "i:" + std::to_string(in.imm_int);
                break;
            case Op::BConst:
                key = "b:" + std::to_string(in.imm_int);
                break;
            case Op::NConst:
                key = "none";
                break;
            case Op::FConst: {
                int64_t bits;
                memcpy(&bits, &in.imm_float, sizeof(bits));
                key = "f:" + std::to_string(bits);
                break;
            }
            case Op::LoadConst:
                key = "k:" + std::to_string(in.imm_u32);
                break;
            case Op::LoadSlot:
                key = "slot:" + std::to_string(in.imm_u32);
                d.add_slot(in.imm_u32);
                break;
            case Op::LoadGlobal:
                key = "glob:" + std::to_string(in.imm_u32);
                d.global = true;
                break;
            case Op::LoadCapture:
                key = "cap:" + std::to_string(in.imm_u32);
                d.capture = true;
                break;
            case Op::LoadProperty: {
                int o = in.operands.empty() ? -1 : opvn(in.operands[0]);
                key = "prop:" + std::to_string(in.imm_u32) + ":" + std::to_string(o);
                d.heap = true;
                if (!in.operands.empty()) {
                    d.merge(vndeps[opvn(in.operands[0])]);
                }
                break;
            }
            case Op::LoadIndex: {
                int a = in.operands.size() > 0 ? opvn(in.operands[0]) : -1;
                int b = in.operands.size() > 1 ? opvn(in.operands[1]) : -1;
                key = "idx:" + std::to_string(a) + ":" + std::to_string(b);
                d.heap = true;
                if (in.operands.size() > 0) {
                    d.merge(vndeps[opvn(in.operands[0])]);
                }
                if (in.operands.size() > 1) {
                    d.merge(vndeps[opvn(in.operands[1])]);
                }
                break;
            }
            case Op::Dup:
                if (!in.operands.empty()) {
                    int o = opvn(in.operands[0]);
                    vn[id] = o; // copy: same value number
                    continue;
                }
                uniq();
                break;
            default:
                if (classify(op) != GC_OTHER && in.result != InvalidValue) {
                    // pure value-producing op: key by op + normalized operands
                    std::vector<int> ov;
                    for (ValueId o : in.operands) {
                        int x = opvn(o);
                        ov.push_back(x);
                        d.merge(vndeps[x]);
                    }
                    if (commutative(op) && ov.size() == 2 && ov[0] > ov[1]) {
                        std::swap(ov[0], ov[1]);
                    }
                    key = "e" + std::to_string((int)op);
                    for (int x : ov) {
                        key += ":" + std::to_string(x);
                    }
                } else {
                    uniq(); // phi / call / store / make / terminator: opaque
                }
                break;
        }
        vn[id] = mkvn(key, d);
    }

    // candidate (value-producing, non-OTHER)
    std::unordered_map<int, int> vn2cid; // vn -> compact id
    std::vector<int> cid_class;          // compact id -> op class
    std::vector<int> cid_vn;             // compact id -> vn
    for (ValueId id = 0; id < (ValueId)NI; id++) {
        const Inst &in = f.insts[id];
        int c = classify(in.op);
        if (c == GC_OTHER || in.result == InvalidValue) {
            continue;
        }
        int v = vn[id];
        if (vn2cid.count(v)) {
            continue;
        }
        int cid = (int)cid_class.size();
        vn2cid[v] = cid;
        cid_class.push_back(c);
        cid_vn.push_back(v);
    }
    const size_t M = cid_class.size();
    if (M == 0) {
        return;
    }

    auto deps_of = [&](int cid) -> const Deps & { return vndeps[cid_vn[cid]]; };

    // kill effect of an instruction (which memory spaces / slot it invalidates).
    struct Kill {
        int slot = -1;
        bool g = false, h = false, c = false;
        bool any() const {
            return slot >= 0 || g || h || c;
        }
    };
    auto kill_of = [&](const Inst &in) -> Kill {
        Kill k;
        switch (in.op) {
            case Op::StoreSlot:
            case Op::StoreImmSlot:
            case Op::StoreBImmSlot:
            case Op::StoreNSlot:
            case Op::StoreCSlot:
            case Op::CopySlot:
                k.slot = (int)in.imm_u32;
                break;
            case Op::StoreGlobal:
                k.g = true;
                break;
            case Op::StoreCapture:
                k.c = true;
                break;
            case Op::StoreIndex:
            case Op::StoreProperty:
                k.h = true;
                break;
            case Op::Call:
            case Op::CallMethod:
                k.g = k.h = k.c = true;
                break;
            default:
                break;
        }
        return k;
    };
    auto killed = [&](int cid, const Kill &k) -> bool {
        const Deps &d = deps_of(cid);
        if (k.slot >= 0 && d.has_slot((uint32_t)k.slot)) {
            return true;
        }
        if (k.g && d.global) {
            return true;
        }
        if (k.h && d.heap) {
            return true;
        }
        if (k.c && d.capture) {
            return true;
        }
        return false;
    };

    // gen[c] = c computed in b with no later kill (available at exit).
    // kill[c] = c killed in b with no later recompute (must be dropped from IN).
    // Both derived from each expression's LAST event in the block, so the
    // transfer OUT = gen | (IN & !kill) is correct even for values only
    // available on entry that a store in b invalidates.
    std::vector<std::vector<char>> GEN(NB), KILLB(NB);
    for (size_t b = 0; b < NB; b++) {
        GEN[b].assign(M, 0);
        KILLB[b].assign(M, 0);
        if (rpo_num[b] < 0) {
            continue;
        }
        std::vector<signed char> state(M, 0); // last event: +1 computed, -1 killed
        auto body_and_term = [&](ValueId v) {
            if (v < 0) {
                return;
            }
            const Inst &in = f.insts[v];
            Kill k = kill_of(in);
            if (k.any()) {
                for (size_t c = 0; c < M; c++) {
                    if (killed((int)c, k)) {
                        state[c] = -1;
                    }
                }
            }
            int cls = classify(in.op);
            if (cls != GC_OTHER && in.result != InvalidValue) {
                auto it = vn2cid.find(vn[v]);
                if (it != vn2cid.end()) {
                    state[it->second] = +1;
                }
            }
        };
        for (ValueId v : f.blocks[b].insts) {
            body_and_term(v);
        }
        if (f.blocks[b].terminator != InvalidValue) {
            body_and_term(f.blocks[b].terminator);
        }
        for (size_t c = 0; c < M; c++) {
            GEN[b][c] = (state[c] == 1);
            KILLB[b][c] = (state[c] == -1);
        }
    }

    // availability fixpoint: intersection (must) & union (may)
    std::vector<std::vector<char>> IN_ALL(NB), OUT_ALL(NB), IN_ANY(NB), OUT_ANY(NB);
    for (size_t b = 0; b < NB; b++) {
        OUT_ALL[b].assign(M, (rpo_num[b] < 0) ? 0 : 1); // top = full set
        OUT_ANY[b].assign(M, 0);
        IN_ALL[b].assign(M, 0);
        IN_ANY[b].assign(M, 0);
    }
    for (int meet = 0; meet < 2; meet++) {
        bool intersect_meet = (meet == 0);
        auto &INx = intersect_meet ? IN_ALL : IN_ANY;
        auto &OUTx = intersect_meet ? OUT_ALL : OUT_ANY;
        for (bool changed = true; changed;) {
            changed = false;
            for (BlockId b : rpo) {
                std::vector<char> in(M, intersect_meet ? 1 : 0);
                bool first = true;
                if (b == f.entry) {
                    std::fill(in.begin(), in.end(), 0);
                } else {
                    for (BlockId p : f.blocks[b].preds) {
                        if (p < 0 || (size_t)p >= NB || rpo_num[p] < 0) {
                            continue;
                        };
                        for (size_t c = 0; c < M; c++) {
                            in[c] = intersect_meet ? (in[c] & OUTx[p][c]) : (in[c] | OUTx[p][c]);
                        }
                        first = false;
                    }
                    if (first) {
                        std::fill(in.begin(), in.end(), 0);
                    }
                }
                INx[b] = in;
                std::vector<char> out(M);
                for (size_t c = 0; c < M; c++) {
                    out[c] = GEN[b][c] | (in[c] & !KILLB[b][c]);
                }
                if (out != OUTx[b]) {
                    OUTx[b] = out;
                    changed = true;
                }
            }
        }
    }

    // count fully/partially redundant cross-block uses
    long fx = 0, fx_loop = 0, part = 0;
    long lc[GC_N] = { 0 }, lc_loop[GC_N] = { 0 };
    for (BlockId b : rpo) {
        std::vector<char> avail = IN_ALL[b]; // available on ALL paths into b
        std::vector<char> anyav = IN_ANY[b]; // available on SOME path
        std::vector<char> local(M, 0);       // computed earlier in THIS block
        auto walk = [&](ValueId v) {
            if (v < 0) {
                return;
            }
            const Inst &in = f.insts[v];
            Kill k = kill_of(in);
            if (k.any()) {
                for (size_t c = 0; c < M; c++) {
                    if (killed((int)c, k)) {
                        avail[c] = anyav[c] = local[c] = 0;
                    }
                }
            }
            int cls = classify(in.op);
            if (cls == GC_OTHER || in.result == InvalidValue) {
                return;
            }
            auto it = vn2cid.find(vn[v]);
            if (it == vn2cid.end()) {
                return;
            }
            int c = it->second;
            // fully redundant across blocks
            if (avail[c]) {
                fx++;
                lc[cls]++;
                if (in_loop[b]) {
                    fx_loop++;
                    lc_loop[cls]++;
                }
            } else if (anyav[c] && !local[c]) {
                part++; // partially redundant (needs PRE/hoist)
            }
            local[c] = 1;
            anyav[c] = 1;
        };
        for (ValueId v : f.blocks[b].insts) {
            walk(v);
        }
        if (f.blocks[b].terminator != InvalidValue) {
            walk(f.blocks[b].terminator);
        }
    }

    if (fx == 0 && part == 0) {
        return;
    }
    totals.funcs_hit++;
    totals.xred += fx;
    totals.xred_loop += fx_loop;
    totals.part += part;
    for (int c = 0; c < GC_N; c++) {
        totals.cls[c] += lc[c];
        totals.cls_loop[c] += lc_loop[c];
    }
    fprintf(stderr,
            "[GVN] %-24s#%-3u B=%zu | xred=%ld(loop=%ld) part=%ld |",
            (fn_name && *fn_name) ? fn_name : "<anon>",
            fn_idx,
            rpo.size(),
            fx,
            fx_loop,
            part);
    for (int c = 0; c < GC_N; c++) {
        if (lc[c]) {
            fprintf(stderr, " %s=%ld(l%ld)", gc_name(c), lc[c], lc_loop[c]);
        }
    }
    fprintf(stderr, "\n");
}

// Reuses the RPO + Cooper-Harvey-Kennedy dominator + natural-loop
// machinery of gvn_report, but instead of counting redundancy, it decides, per
// alias-free proven int-array slot, the OUTERMOST natural loop across which the
// slot's backing pointer is invariant and that has a dedicated preheader.
ArrayHeaderHoist plan_array_header_hoist(const Func &f, const IntArraySlots &int_arr_slots) {
    ArrayHeaderHoist plan;
    if (int_arr_slots.empty()) {
        return plan;
    }
    const size_t NB = f.blocks.size();
    if (NB == 0 || f.entry < 0 || (size_t)f.entry >= NB) {
        return plan;
    }

    // drop any proven slot that is copied to another slot.
    // analyze_int_array_slots treats `t = s` (StoreSlot @t = LoadSlot @s) and
    // CopySlot(src=s) as benign for s, so a proven slot may still have a second
    // live handle that could resize its backing while our cache is live.
    std::unordered_set<uint32_t> aliased;
    for (const Block &b : f.blocks) {
        for (ValueId v : b.insts) {
            const Inst &in = f.inst(v);
            if (in.op == Op::StoreSlot && !in.operands.empty()) {
                const Inst &rhs = f.inst(in.operands[0]);
                if (rhs.op == Op::LoadSlot && rhs.imm_u32 != in.imm_u32) {
                    aliased.insert(rhs.imm_u32);
                }
            } else if (in.op == Op::CopySlot) {
                uint32_t src = (uint32_t)in.imm_int;
                if (src != in.imm_u32) {
                    aliased.insert(src);
                }
            }
        }
    }
    std::unordered_set<uint32_t> safe_slots;
    for (uint32_t s : int_arr_slots) {
        if (!aliased.count(s)) {
            safe_slots.insert(s);
        }
    }
    if (safe_slots.empty()) {
        return plan;
    }

    // reverse-postorder over reachable blocks
    std::vector<int> rpo_num(NB, -1);
    std::vector<BlockId> order;
    {
        std::vector<char> seen(NB, 0);
        std::vector<std::pair<BlockId, size_t>> stk;
        stk.push_back({ f.entry, 0 });
        seen[f.entry] = 1;
        while (!stk.empty()) {
            BlockId b = stk.back().first;
            size_t &i = stk.back().second;
            const Block &B = f.blocks[b];
            if (i < B.succs.size()) {
                BlockId s = B.succs[i++];
                if (s >= 0 && (size_t)s < NB && !seen[s]) {
                    seen[s] = 1;
                    stk.push_back({ s, 0 });
                }
            } else {
                order.push_back(b);
                stk.pop_back();
            }
        }
    }
    std::vector<BlockId> rpo(order.rbegin(), order.rend());
    for (size_t i = 0; i < rpo.size(); i++) {
        rpo_num[rpo[i]] = (int)i;
    }

    // dominator tree (Cooper-Harvey-Kennedy)
    std::vector<BlockId> idom(NB, InvalidBlock);
    idom[f.entry] = f.entry;
    auto intersect = [&](BlockId a, BlockId b) -> BlockId {
        while (a != b) {
            while (rpo_num[a] > rpo_num[b]) {
                a = idom[a];
            }
            while (rpo_num[b] > rpo_num[a]) {
                b = idom[b];
            }
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockId b : rpo) {
            if (b == f.entry) {
                continue;
            }
            BlockId newidom = InvalidBlock;
            for (BlockId p : f.blocks[b].preds) {
                if (p < 0 || (size_t)p >= NB || rpo_num[p] < 0) {
                    continue;
                }
                if (idom[p] == InvalidBlock) {
                    continue;
                }
                newidom = (newidom == InvalidBlock) ? p : intersect(p, newidom);
            }
            if (newidom != InvalidBlock && idom[b] != newidom) {
                idom[b] = newidom;
                changed = true;
            }
        }
    }
    auto dominates = [&](BlockId a, BlockId b) -> bool {
        if (a == b) {
            return true;
        }
        for (BlockId x = b; x != InvalidBlock && x != f.entry; x = idom[x]) {
            if (idom[x] == x) {
                break;
            }
            if (idom[x] == a) {
                return true;
            }
        }
        return a == f.entry;
    };

    // natural loops: header h + body set (back edge n->h where h dom n)
    // Multiple back edges to the same header union into one loop.
    std::unordered_map<BlockId, std::vector<char>> loop_body; // header -> body mask
    for (BlockId n : rpo) {
        for (BlockId h : f.blocks[n].succs) {
            if (h < 0 || (size_t)h >= NB || rpo_num[h] < 0) {
                continue;
            }
            if (!dominates(h, n)) {
                continue; // not a back edge
            }
            auto &body = loop_body[h];
            if (body.empty()) {
                body.assign(NB, 0);
            }
            body[h] = 1;
            if (!body[n]) {
                body[n] = 1;
                std::vector<BlockId> work{ n };
                while (!work.empty()) {
                    BlockId x = work.back();
                    work.pop_back();
                    for (BlockId p : f.blocks[x].preds) {
                        if (p < 0 || (size_t)p >= NB) {
                            continue;
                        }
                        if (!body[p]) {
                            body[p] = 1;
                            work.push_back(p);
                        }
                    }
                }
            }
        }
    }
    if (loop_body.empty()) {
        return plan;
    }

    // within a block, does an instruction resize/rebind slot s?
    //   StoreSlot @s                (rebind: the slot may point elsewhere)
    //   StoreIndex(obj=LoadSlot@s)  (OOB miss escapes to jit_set_index -> resize)
    //   CallMethod(recv=LoadSlot@s) (push -> resize, any method)
    auto block_resizes = [&](const Block &b, uint32_t s) -> bool {
        for (ValueId v : b.insts) {
            const Inst &in = f.inst(v);
            if (in.op == Op::StoreSlot && in.imm_u32 == s) {
                return true;
            };

            if (in.op == Op::CopySlot && in.imm_u32 == s) {
                return true;
            };

            if ((in.op == Op::StoreImmSlot || in.op == Op::StoreBImmSlot ||
                 in.op == Op::StoreNSlot || in.op == Op::StoreCSlot) &&
                in.imm_u32 == s) {
                return true;
            }
            if (in.op == Op::StoreIndex && !in.operands.empty()) {
                const Inst &obj = f.inst(in.operands[0]);
                if (obj.op == Op::LoadSlot && obj.imm_u32 == s) {
                    return true;
                }
            }
            if (in.op == Op::CallMethod && !in.operands.empty()) {
                const Inst &recv = f.inst(in.operands[0]);
                if (recv.op == Op::LoadSlot && recv.imm_u32 == s) {
                    return true;
                }
            }
        }
        return false;
    };

    // Does block b read slot s via LoadIndex(obj=LoadSlot@s)?
    auto block_reads_index = [&](const Block &b, uint32_t s) -> bool {
        for (ValueId v : b.insts) {
            const Inst &in = f.inst(v);
            if (in.op == Op::LoadIndex && !in.operands.empty()) {
                const Inst &obj = f.inst(in.operands[0]);
                if (obj.op == Op::LoadSlot && obj.imm_u32 == s && in.type == Ty::Int48) {
                    return true;
                }
            }
        }
        return false;
    };

    // For each safe slot pick the OUTERMOST loop (largest body) that both contains
    // at least one qualifying LoadIndex of slot, is resize/rebind-free for slot in
    // every body block, and has a unique loop-external predecessor of its
    // header (the preheader) which is itself resize/rebind-free for slot.
    for (uint32_t slot : safe_slots) {
        BlockId best_hdr = InvalidBlock;
        BlockId best_pre = InvalidBlock;
        long best_size = -1;
        for (auto &kv : loop_body) {
            BlockId h = kv.first;
            const std::vector<char> &body = kv.second;
            bool reads = false;
            long bsize = 0;
            bool resizes = false;
            for (BlockId bb = 0; bb < (BlockId)NB; bb++) {
                if (!body[bb]) {
                    continue;
                }
                bsize++;
                if (block_resizes(f.blocks[bb], slot)) {
                    resizes = true;
                    break;
                }
                if (!reads && block_reads_index(f.blocks[bb], slot)) {
                    reads = true;
                }
            }
            if (resizes || !reads) {
                continue;
            }
            BlockId pre = InvalidBlock;
            bool unique_pre = true;

            for (BlockId p : f.blocks[h].preds) {
                if (p < 0 || (size_t)p >= NB) {
                    continue;
                }
                if (body[p]) {
                    continue; // back edge, inside loop
                }
                if (pre == InvalidBlock) {
                    pre = p;
                } else {
                    unique_pre = false;
                    break;
                }
            }

            if (!unique_pre || pre == InvalidBlock) {
                continue;
            }
            if (block_resizes(f.blocks[pre], slot)) {
                continue;
            }

            if (bsize > best_size) {
                best_size = bsize;
                best_hdr = h;
                best_pre = pre;
            }
        }
        if (best_hdr == InvalidBlock) {
            continue;
        }
        plan.materialize[best_pre].push_back(slot);
        const std::vector<char> &body = loop_body[best_hdr];
        for (BlockId bb = 0; bb < (BlockId)NB; bb++) {
            if (body[bb]) {
                plan.valid[bb].insert(slot);
            }
        }
        plan.slots.insert(slot);
    }
    return plan;
}

bool optimize(Func &f) {
    // Phi-branch threading runs first, exactly once, on the pristine build output:
    //      it is the only consumer of phi.operands[i]<->preds[i]
    bool any = thread_phi_branches(f);

    // Iterate to a fixpoint: each pass can expose work for the others
    //      (const-prop -> const-fold -> branch-fold -> jump-thread -> more dead/foldable code)
    for (int iter = 0; iter < 16; iter++) {
        bool changed = false;
        for (Block &b : f.blocks) {
            changed |= propagate_slot_consts(f, b);
            changed |= local_value_numbering(f, b);
            changed |= simplify_block(f, b);
            changed |= eliminate_slot_round_trips(f, b);
            changed |= fuse_slot_stores(f, b);
        }
        changed |= eliminate_dead_stores(f);
        bool preds_changed = false;
        for (Block &b : f.blocks) {
            preds_changed |= fold_branch(f, b);
        }
        if (preds_changed) {
            rebuild_preds(f);
            changed = true;
        }
        if (thread_jumps(f)) {
            rebuild_preds(f);
            changed = true;
        }
        if (merge_blocks(f)) {
            changed = true;
        }
        if (remove_unreachable_blocks(f)) {
            changed = true;
        }
        any |= changed;
        if (!changed) {
            break;
        }
    }
    return any;
}

// Map a Constant pool entry to an IR type
// STRING/FUNCTION -> Ty::Heap (boxed references, LoadGlobal lowering already produces a Value)
// NONE -> Ty::None
// INT -> Int48 (range-checked by the bytecode emitter for OP_LOAD_CONST)
// FLOAT -> Ty::Float
static Ty const_pool_type(const nari::bytecode::Constant &c) {
    using nari::bytecode::Constant;
    switch (c.type) {
        case Constant::Type::INT:
            return Ty::Int48;
        case Constant::Type::FLOAT:
            return Ty::Float;
        case Constant::Type::NONE:
            return Ty::None;
        case Constant::Type::STRING:
        case Constant::Type::FUNCTION:
            return Ty::Heap;
    }
    return Ty::Unknown;
}

// tracks a running "top-of-stack type" over a function's bytecode and records it at each OP_STORE_GLOBAL / OP_STORE_VAR
// handles `let X = <literal>`
struct GlobalStoreInfo {
    Ty value_ty = Ty::Bottom;
    bool poisoned = false; // multiple stores or unrecognized initializer
    uint32_t count = 0;
};

GlobalTypeMap analyze_const_globals(const nari::bytecode::Chunk &chunk) {
    using namespace nari::bytecode;
    GlobalTypeMap out;
    std::unordered_map<uint32_t, GlobalStoreInfo> infos;

    // Bytecode shorts are big-endian
    auto u16_at = [&](const ByteArray &code, size_t pc) -> uint16_t {
        return uint16_t((uint16_t(code[pc]) << 8) | uint16_t(code[pc + 1]));
    };

    for (const FunctionMeta &fn : chunk.functions) {
        const ByteArray &code = fn.code;
        // tos: type of value currently on top of abstract stack (Bottom = unknown / nothing tracked).
        Ty tos = Ty::Bottom;
        // local_ty[slot] = type if known, Bottom otherwise. Limited to function's locals.
        std::vector<Ty> local_ty(fn.var_names.size(), Ty::Bottom);

        auto clear_all = [&]() {
            tos = Ty::Bottom;
            std::fill(local_ty.begin(), local_ty.end(), Ty::Bottom);
        };

        size_t pc = 0;
        while (pc < code.size()) {
            OpCode op = OpCode(code[pc]);
            size_t op_pc = pc;
            size_t instruction_size = decoded_instruction_size(code, pc);
            if (instruction_size == 0) {
                clear_all();
                break;
            }
            if (op == OpCode::OP_MAKE_CLOSURE) {
                // MAKE_CLOSURE pushes a function value; treat as unknown tos.
                tos = Ty::Unknown;
            }
            switch (op) {
                case OpCode::OP_LOAD_CONST: {
                    uint16_t idx = u16_at(code, pc + 1);
                    if (idx < fn.constants.size()) {
                        tos = const_pool_type(fn.constants[idx]);
                    } else {
                        tos = Ty::Unknown;
                    }
                    break;
                }
                case OpCode::OP_LOAD_NONE:
                    tos = Ty::None;
                    break;
                case OpCode::OP_LOAD_TRUE:
                case OpCode::OP_LOAD_FALSE:
                    tos = Ty::Bool;
                    break;
                case OpCode::OP_LOAD_ZERO:
                case OpCode::OP_LOAD_ONE:
                    tos = Ty::Int48;
                    break;
                case OpCode::OP_LOAD_VAR: {
                    uint16_t slot = u16_at(code, pc + 1);
                    tos = (slot < local_ty.size()) ? local_ty[slot] : Ty::Bottom;
                    if (tos == Ty::Bottom) {
                        tos = Ty::Unknown;
                    }
                    break;
                }
                case OpCode::OP_STORE_VAR: {
                    uint16_t slot = u16_at(code, pc + 1);
                    // Record (peek without consuming)
                    if (slot < local_ty.size()) {
                        // subsequent store to same slot with a different type invalidates record
                        Ty new_ty = (tos == Ty::Bottom) ? Ty::Unknown : tos;
                        if (local_ty[slot] == Ty::Bottom) {
                            local_ty[slot] = new_ty;
                        } else if (local_ty[slot] != new_ty) {
                            local_ty[slot] = Ty::Unknown;
                        }
                    }
                    break;
                }
                case OpCode::OP_STORE_GLOBAL: {
                    uint16_t name = u16_at(code, pc + 1);
                    GlobalStoreInfo &gi = infos[name];
                    gi.count++;
                    if (gi.count > 1) {
                        gi.poisoned = true;
                    } else {
                        Ty t = (tos == Ty::Bottom) ? Ty::Unknown : tos;
                        if (t == Ty::Unknown) {
                            gi.poisoned = true;
                        } else {
                            gi.value_ty = t;
                        }
                    }
                    break;
                }
                case OpCode::OP_POP:
                    tos = Ty::Bottom;
                    break;
                case OpCode::OP_DUP:
                    // tos unchanged on top.
                    break;
                // Anything that branches, calls, returns, or otherwise hides dataflow, clear all tracked state.
                case OpCode::OP_JUMP:
                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE:
                case OpCode::OP_JUMP_IF_NONE:
                case OpCode::OP_CALL:
                case OpCode::OP_SELF_TAIL_CALL:
                case OpCode::OP_RETURN:
                case OpCode::OP_THROW:
                case OpCode::OP_SETUP_TRY:
                case OpCode::OP_POP_TRY:
                case OpCode::OP_BEGIN_CATCH:
                case OpCode::OP_BEGIN_FINALLY:
                    clear_all();
                    break;
                default:
                    // Unmodeled op: clear tos but keep local map (most ops
                    // don't store to locals). For safety on STORE_CAPTURE,
                    // SET_INDEX, etc., we just lose tos.
                    tos = Ty::Bottom;
                    break;
            }
            pc = op_pc + instruction_size;
        }
    }

    for (const auto &kv : infos) {
        const GlobalStoreInfo &gi = kv.second;
        if (!gi.poisoned && gi.count == 1 && gi.value_ty != Ty::Bottom &&
            gi.value_ty != Ty::Unknown) {
            out[kv.first] = gi.value_ty;
        }
    }

    if (getenv("NARI_DBG_GCT") != nullptr) {
        fprintf(stderr, "[GCT] %zu write-once globals typed\n", out.size());
        for (const auto &kv : out) {
            const char *name = "<oob>";
            if (kv.first < chunk.strings.size()) {
                name = chunk.strings[kv.first].c_str();
            }
            const char *tn = "?";
            switch (kv.second) {
                case Ty::Int48:
                    tn = "Int48";
                    break;
                case Ty::Float:
                    tn = "Float";
                    break;
                case Ty::Bool:
                    tn = "Bool";
                    break;
                case Ty::None:
                    tn = "None";
                    break;
                case Ty::Heap:
                    tn = "Heap";
                    break;
                default:
                    break;
            }
            fprintf(stderr, "[GCT]   #%u (%s) -> %s\n", kv.first, name, tn);
        }
    }

    return out;
}

// Identify slots whose runtime value is always an int-only array.
// roughly, every definition must be a MakeArray of int initializers (or empty) or a self
// round-trip, and every mutation (push / SetIndex) must store an int.
//
// Any other use escapes the contents and disqualifies the slot.
IntArraySlots int_array_candidates(const Func &f) {
    IntArraySlots out;
    if (f.num_slots == 0) {
        return out;
    }

    // Candidate set = slots written by at least one int-initialized MakeArray
    // and never written by anything else or a self round-trip (params excluded).
    // Every disqualification is unconditional
    std::vector<bool> candidate(f.num_slots, false);
    std::vector<bool> disqualified(f.num_slots, false);
    for (uint32_t s = 0; s < f.num_params && s < f.num_slots; s++) {
        disqualified[s] = true;
    }

    auto is_int_ty = [](Ty t) {
        return t == Ty::Int48;
    };

    // Collect StoreSlot definitions for each slot
    for (const Block &b : f.blocks) {
        for (ValueId v : b.insts) {
            const Inst &in = f.inst(v);
            if (in.op != Op::StoreSlot) {
                continue;
            }
            uint32_t s = in.imm_u32;
            if (s >= f.num_slots || disqualified[s]) {
                continue;
            }
            const Inst &val = f.inst(in.operands[0]);
            if (val.op == Op::MakeArray) {
                // Every initializer must be Int48-typed (or the array is empty).
                bool ok = true;
                for (ValueId iv : val.operands) {
                    if (!is_int_ty(f.inst(iv).type)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    candidate[s] = true;
                    continue;
                }
                disqualified[s] = true;
            } else if (val.op == Op::LoadSlot && val.imm_u32 == s) {
                // self round-trip (`grid = grid` no-op); ignore
                continue;
            } else if (val.op == Op::LoadSlot && val.imm_u32 < f.num_slots) {
                // cross-slot rebind: only allow if the source is also a
                // candidate int-array slot. We can't decide that here in a
                // single pass, disqualify just in case
                // but i'll revisit this later when multipass happens.
                disqualified[s] = true;
            } else {
                // Any other RHS (Call result, LoadGlobal, LoadIndex, etc.) could be a non-int array.
                disqualified[s] = true;
            }
        }
    }

    for (uint32_t s = 0; s < f.num_slots; s++) {
        if (candidate[s] && !disqualified[s]) {
            out.insert(s);
        }
    }
    return out;
}

IntArraySlots analyze_int_array_slots(const Func &f, uint32_t push_method_name_idx, uint32_t length_method_name_idx) {
    IntArraySlots out;
    if (f.num_slots == 0) {
        return out;
    }

    // whitelist `arr.length()` as a non-escaping pure read on an int-array candidate.
    const bool length_ok = length_method_name_idx != UINT32_MAX;

    // candidate set = slots that COULD hold an int-array based on their definitions alone
    IntArraySlots cand_set = int_array_candidates(f);
    std::vector<bool> candidate(f.num_slots, false);
    std::vector<bool> disqualified(f.num_slots, false);
    for (uint32_t s : cand_set) {
        candidate[s] = true;
    }
    for (uint32_t s = 0; s < f.num_slots; s++) {
        if (!candidate[s]) {
            disqualified[s] = true;
        }
    }

    auto is_int_ty = [](Ty t) {
        return t == Ty::Int48;
    };

    // walk every Inst's operands.
    // For each operand that is a LoadSlot of a candidate slot, decide whether the parent instruction is allowed.
    auto is_loadslot_of_candidate = [&](ValueId op, uint32_t *out_slot) -> bool {
        if (op == InvalidValue) {
            return false;
        }
        const Inst &src = f.inst(op);
        if (src.op != Op::LoadSlot) {
            return false;
        }
        if (src.imm_u32 >= f.num_slots) {
            return false;
        }
        if (!candidate[src.imm_u32] || disqualified[src.imm_u32]) {
            return false;
        }
        *out_slot = src.imm_u32;
        return true;
    };

    for (const Block &b : f.blocks) {
        for (ValueId v : b.insts) {
            const Inst &in = f.inst(v);
            // Examine each operand position by the parent's op.
            switch (in.op) {
                case Op::Pop:
                case Op::Dup:
                    // pure stack ops on the value don't escape the array
                    break;
                case Op::StoreSlot: {
                    // Already classified above. Re-validate: the RHS is the
                    // only operand; if it was a LoadSlot of a candidate, that
                    // candidate is being read - the read alone doesn't escape.
                    // We treat StoreSlot reading a candidate as benign.
                    break;
                }
                case Op::LoadIndex: {
                    // operand[0] = obj, operand[1] = idx. Reading is benign.
                    break;
                }
                case Op::StoreIndex: {
                    // operand[0]=obj, operand[1]=idx, operand[2]=val. If obj
                    // is candidate, the value stored must be Int48.
                    uint32_t s = 0;
                    if (is_loadslot_of_candidate(in.operands[0], &s)) {
                        const Inst &val = f.inst(in.operands[2]);
                        if (!is_int_ty(val.type)) {
                            disqualified[s] = true;
                        }
                    }
                    // operand[2] could itself be a LoadSlot of a candidate; if
                    // so it's just a read - benign.
                    break;
                }
                case Op::CallMethod: {
                    // operand[0] = receiver; rest = args. If receiver is a
                    // candidate, only two method calls keep it int-array-pure:
                    //   push(<Int48>)  (argc==1) - in-place int append,
                    //   length()       (argc==0) - pure read of the element
                    //                              count; stores nothing and
                    //                              leaks no reference, so it
                    //                              does not escape the array.
                    // Any other method conservatively escapes the receiver.
                    if (in.operands.empty()) {
                        break;
                    }
                    uint32_t s = 0;
                    bool recv_is_cand =
                        is_loadslot_of_candidate(in.operands[0], &s);
                    if (recv_is_cand) {
                        bool is_push = in.imm_u32 == push_method_name_idx &&
                                       in.imm_int == 1 &&
                                       in.operands.size() == 2;
                        bool is_length =
                            length_ok &&
                            in.imm_u32 == length_method_name_idx &&
                            in.imm_int == 0 && in.operands.size() == 1;
                        if (is_push) {
                            const Inst &arg = f.inst(in.operands[1]);
                            if (!is_int_ty(arg.type)) {
                                disqualified[s] = true;
                            }
                        } else if (is_length) {
                            // pure read: benign, keep the slot a candidate.
                        } else {
                            disqualified[s] = true;
                        }
                    }
                    // Any non-receiver argument that's a candidate -> escape.
                    for (size_t i = 1; i < in.operands.size(); i++) {
                        uint32_t s2 = 0;
                        if (is_loadslot_of_candidate(in.operands[i], &s2)) {
                            disqualified[s2] = true;
                        }
                    }
                    break;
                }
                case Op::Call:
                case Op::StoreGlobal:
                case Op::StoreCapture:
                case Op::StoreProperty:
                case Op::LoadProperty:
                case Op::MakeArray:
                case Op::MakeObject:
                case Op::StrConcat:
                case Op::FormatValue:
                case Op::IterArray:
                case Op::Return: {
                    // Any candidate flowing into these ops escapes (StoreCapture
                    // writes the value into a shared closure cell).
                    for (ValueId op : in.operands) {
                        uint32_t s = 0;
                        if (is_loadslot_of_candidate(op, &s)) {
                            disqualified[s] = true;
                        }
                    }
                    break;
                }
                default: {
                    // Conservative for any op we didn't enumerate: if it has a
                    // candidate operand, mark escape.
                    for (ValueId op : in.operands) {
                        uint32_t s = 0;
                        if (is_loadslot_of_candidate(op, &s)) {
                            disqualified[s] = true;
                        }
                    }
                    break;
                }
            }
        }
    }

    for (uint32_t s = 0; s < f.num_slots; s++) {
        if (candidate[s] && !disqualified[s]) {
            out.insert(s);
        }
    }

    if (getenv("NARI_DBG_IAS") != nullptr) {
        const char *fname = f.meta ? f.meta->name.c_str() : "<anon>";
        fprintf(stderr, "[IAS] %s: %zu int-array slots:", fname, out.size());
        for (uint32_t s : out) {
            fprintf(stderr, " @%u", s);
        }
        fprintf(stderr, "\n");
    }

    return out;
}

void infer_types(
    Func &f, std::vector<Ty> &slot_ty,
    const GlobalTypeMap *global_types,
    const IntArraySlots *int_array_slots,
    bool int48_params) {
    // Optimistic init: non-param slots start Bottom (refined upward by their
    // stores); params are dynamically typed at entry, so Unknown. Non-const value
    // types start Bottom; constants are fixed.
    // params seeded Int48 (entry guards enforce it).
    const Ty param_seed_ty = int48_params ? Ty::Int48 : Ty::Unknown;
    slot_ty.assign(f.num_slots, Ty::Bottom);
    for (uint32_t s = 0; s < f.num_params && s < f.num_slots; s++) {
        slot_ty[s] = param_seed_ty;
    }
    for (Inst &in : f.insts) {
        switch (in.op) {
            case Op::IConst:
                in.type = Ty::Int48;
                break;
            case Op::FConst:
                in.type = Ty::Float;
                break;
            case Op::BConst:
                in.type = Ty::Bool;
                break;
            case Op::NConst:
                in.type = Ty::None;
                break;
            default:
                in.type = Ty::Bottom;
                break;
        }
    }

    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 128) {
        changed = false;

        // value types from operands / slot types
        for (Inst &in : f.insts) {
            Ty nt = in.type;
            switch (in.op) {
                case Op::LoadSlot:
                    nt = (in.imm_u32 < slot_ty.size()) ? slot_ty[in.imm_u32] : Ty::Unknown;
                    break;
                case Op::LoadGlobal: {
                    Ty t = Ty::Unknown;
                    if (global_types) {
                        auto it = global_types->find(in.imm_u32);
                        if (it != global_types->end()) {
                            t = it->second;
                        }
                    }
                    nt = t;
                    break;
                }
                case Op::LoadConst:
                case Op::Call:
                case Op::CallMethod:
                case Op::LoadProperty:
                case Op::LoadCapture:
                case Op::MakeArray:
                case Op::MakeObject:
                case Op::StrConcat:
                case Op::FormatValue:
                case Op::IterArray:
                    nt = Ty::Unknown;
                    break;
                case Op::LoadIndex: {
                    // if the array operand is LoadSlot @s for a slot that analyze_int_array_slots()
                    // proved holds an int-only array, the element type is Int48. Otherwise Unknown.
                    nt = Ty::Unknown;
                    if (int_array_slots && !in.operands.empty()) {
                        const Inst &obj = f.inst(in.operands[0]);
                        if (obj.op == Op::LoadSlot &&
                            int_array_slots->count(obj.imm_u32)) {
                            nt = Ty::Int48;
                        }
                    }
                    break;
                }
                case Op::StoreIndex:
                    nt = f.inst(in.operands[2]).type;
                    break;
                case Op::StoreProperty:
                    nt = f.inst(in.operands[1]).type;
                    break;
                case Op::Dup:
                    nt = f.inst(in.operands[0]).type;
                    break;
                case Op::Not:
                    nt = Ty::Bool;
                    break;
                case Op::INeg:
                    nt = f.inst(in.operands[0]).type;
                    break;
                case Op::INot:
                case Op::IAnd:
                case Op::IOr:
                case Op::IXor:
                case Op::IShl:
                case Op::IShr:
                    nt = Ty::Int48;
                    break;
                case Op::DynAdd:
                case Op::DynSub:
                case Op::DynMul:
                case Op::DynMod:
                case Op::DynDiv:
                    nt = arith_type(in.op, f.inst(in.operands[0]).type,
                                    f.inst(in.operands[1]).type);
                    break;
                case Op::DynCmpLt:
                case Op::DynCmpLe:
                case Op::DynCmpGt:
                case Op::DynCmpGe:
                case Op::DynCmpEq:
                case Op::DynCmpNe:
                    nt = Ty::Bool;
                    break;
                case Op::Phi: {
                    // Type of a phi is the join of its operand types.
                    // (Operands may be a subset of predecessors when some are
                    // back-edges; that's fine - join over what's known is sound
                    // because back-edges into phi-bearing blocks were rejected
                    // by the builder.)
                    Ty acc = Ty::Bottom;
                    for (ValueId op : in.operands) {
                        if (op != InvalidValue) {
                            acc = join(acc, f.inst(op).type);
                        }
                    }
                    nt = acc;
                    break;
                }
                default:
                    break;
            }
            if (nt != in.type) {
                in.type = nt;
                changed = true;
            }
        }

        // 2) slot types = join of all values stored to them (params stay at
        // their entry seed: Unknown, or Int48 in speculative mode).
        // Iterate LIVE block instructions only: a store in an unreachable block (e.g.
        // one removed by dead-block elimination, whose Inst may linger orphaned in the
        // pool) never executes, so its value cannot determine the slot's runtime type.
        std::vector<Ty> ns(f.num_slots, Ty::Bottom);
        for (uint32_t s = 0; s < f.num_params && s < f.num_slots; s++) {
            ns[s] = param_seed_ty;
        }
        for (const Block &b : f.blocks) {
            for (ValueId v : b.insts) {
                const Inst &in = f.inst(v);
                // fused slot-store ops (StoreImmSlot/StoreBImmSlot/ StoreNSlot/StoreCSlot/CopySlot)
                // contribute the same slot-type information as the split IConst/BConst/.../StoreSlot form they replaced
                if (in.imm_u32 >= ns.size()) {
                    continue;
                }
                switch (in.op) {
                    case Op::StoreSlot:
                        ns[in.imm_u32] = join(ns[in.imm_u32], f.inst(in.operands[0]).type);
                        break;
                    case Op::StoreImmSlot:
                        ns[in.imm_u32] = join(ns[in.imm_u32], Ty::Int48);
                        break;
                    case Op::StoreBImmSlot:
                        ns[in.imm_u32] = join(ns[in.imm_u32], Ty::Bool);
                        break;
                    case Op::StoreNSlot:
                        ns[in.imm_u32] = join(ns[in.imm_u32], Ty::None);
                        break;
                    case Op::StoreCSlot:
                        // LoadConst had type Unknown in the original inference
                        // (constants can be any of int/float/string/none/func), preserve that type for parity.
                        ns[in.imm_u32] = join(ns[in.imm_u32], Ty::Unknown);
                        break;
                    case Op::CopySlot: {
                        // CopySlot @src -> @dst propagates the src slot type.
                        // The source slot is stored in in.imm_int (see fuse_slot_stores).
                        uint32_t src = (uint32_t)in.imm_int;
                        if (src < slot_ty.size()) {
                            ns[in.imm_u32] = join(ns[in.imm_u32], slot_ty[src]);
                        } else {
                            ns[in.imm_u32] = join(ns[in.imm_u32], Ty::Unknown);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        for (uint32_t s = 0; s < f.num_slots; s++) {
            if (ns[s] != slot_ty[s]) {
                slot_ty[s] = ns[s];
                changed = true;
            }
        }
    }

    // Any value/slot still Bottom is unreachable/uncomputed -> treat as Unknown so the lowering uses the safe dynamic path.
    for (Inst &in : f.insts) {
        if (in.type == Ty::Bottom) {
            in.type = Ty::Unknown;
        }
    }
    for (Ty &t : slot_ty) {
        if (t == Ty::Bottom) {
            t = Ty::Unknown;
        }
    }
}

bool specialize_types(Func &f) {
    bool changed = false;
    auto ty = [&](ValueId v) -> Ty {
        return v == InvalidValue ? Ty::Unknown : f.inst(v).type;
    };
    for (Inst &in : f.insts) {
        if (in.operands.size() < 2) {
            continue;
        }
        Ty a = ty(in.operands[0]);
        Ty b = ty(in.operands[1]);
        Op old = in.op;
        switch (in.op) {
            case Op::DynAdd:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::IAdd;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FAdd;
                }
                break;
            case Op::DynSub:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::ISub;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FSub;
                }
                break;
            case Op::DynMul:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::IMul;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FMul;
                }
                break;
            case Op::DynMod:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::IMod;
                }
                break;
            case Op::DynDiv:
                if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FDiv;
                }
                break;
            case Op::DynCmpLt:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::ICmpLt;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FCmpLt;
                }
                break;
            case Op::DynCmpLe:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::ICmpLe;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FCmpLe;
                }
                break;
            case Op::DynCmpGt:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::ICmpGt;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FCmpGt;
                }
                break;
            case Op::DynCmpGe:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::ICmpGe;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FCmpGe;
                }
                break;
            case Op::DynCmpEq:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::ICmpEq;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FCmpEq;
                }
                break;
            case Op::DynCmpNe:
                if (a == Ty::Int48 && b == Ty::Int48) {
                    in.op = Op::ICmpNe;
                } else if (a == Ty::Float && b == Ty::Float) {
                    in.op = Op::FCmpNe;
                }
                break;
            default:
                break;
        }
        changed |= in.op != old;
    }
    return changed;
}

// After redundant-pair folding, the only Not left feeding a Branch is `if (!x) ...`.
// Fold it into the terminator by swapping true/false targets.
bool fold_branch_not(Func &f) {
    // count uses of every ValueId across operands of all live insts.
    // We only fold when the Not in question has exactly one use,
    // otherwise it may also feed a Phi / StoreSlot / math op that needs the negated value.
    std::unordered_map<ValueId, int> uses;
    auto bump = [&](ValueId v) {
        if (v != InvalidValue) {
            uses[v]++;
        }
    };
    for (const Block &b : f.blocks) {
        for (ValueId p : b.phis) {
            for (ValueId op : f.inst(p).operands) {
                bump(op);
            }
        }
        for (ValueId i : b.insts) {
            for (ValueId op : f.inst(i).operands) {
                bump(op);
            }
        }
        if (b.terminator != InvalidValue) {
            for (ValueId op : f.inst(b.terminator).operands) {
                bump(op);
            }
        }
    }
    bool changed = false;
    for (Block &b : f.blocks) {
        if (b.terminator == InvalidValue || b.insts.empty()) {
            continue;
        }
        Inst &term = f.inst(b.terminator);
        if (term.op != Op::Branch || term.operands.empty()) {
            continue;
        }
        ValueId last_id = b.insts.back();
        if (term.operands[0] != last_id) {
            continue;
        }
        Inst &last = f.inst(last_id);
        if (last.op != Op::Not || last.operands.empty()) {
            continue;
        }
        // check that the Not feeds exactly one operand slot (the Branch condition we matched above).
        auto it = uses.find(last_id);
        if (it == uses.end() || it->second != 1) {
            continue;
        }
        ValueId x = last.operands[0];
        // drop the Not from the block body and rewrite the Branch.
        b.insts.pop_back();
        term.operands[0] = x;
        std::swap(term.target0, term.target1);
        // block successor list mirrors target0/target1 ordering.
        if (b.succs.size() >= 2) {
            std::swap(b.succs[0], b.succs[1]);
        }
        changed = true;
    }
    return changed;
}

bool fold_redundant_not(Func &f) {
    // build remap: pairs of ValueIds we drop -> their replacement (X)
    std::unordered_map<ValueId, ValueId> remap;
    for (Block &b : f.blocks) {
        std::vector<ValueId> out_body;
        out_body.reserve(b.insts.size());
        for (size_t i = 0; i < b.insts.size(); i++) {
            ValueId v = b.insts[i];
            // try to recognize [X, Not_a, Not_b] starting from `out_body.back()`
            if (i + 1 < b.insts.size() && !out_body.empty()) {
                const Inst &a = f.inst(v);
                const Inst &c = f.inst(b.insts[i + 1]);
                ValueId x = out_body.back();
                if (a.op == Op::Not && c.op == Op::Not &&
                    !a.operands.empty() && a.operands[0] == x &&
                    !c.operands.empty() && c.operands[0] == v &&
                    f.inst(x).type == Ty::Bool) {
                    // drop nots, redirect any user of either to X.
                    remap[v] = x;
                    remap[b.insts[i + 1]] = x;
                    i++; // skip Not_b too
                    continue;
                }
            }
            out_body.push_back(v);
        }
        b.insts = std::move(out_body);
    }
    if (remap.empty()) {
        return false;
    }
    auto rewrite = [&](ValueId &op) {
        // walk chains (in case a later pair's X was remapped).
        while (true) {
            auto it = remap.find(op);
            if (it == remap.end()) {
                break;
            }
            op = it->second;
        }
    };
    for (Inst &in : f.insts) {
        for (ValueId &op : in.operands) {
            if (op != InvalidValue) {
                rewrite(op);
            }
        }
    }
    return true;
}

// Mark StrConcat instructions whose left operand is a freshly-allocated,
// single-use temporary so codegen can append into the left's existing StringObj
// buffer instead of copying the prefix and allocating (see jit_str_concat_inplace).
bool mark_inplace_concat(Func &f) {
    // Global use-count over every operand slot of every live inst (same pattern as fold_branch_not).
    // A producer used exactly once is uniquely referenced.
    std::unordered_map<ValueId, int> uses;
    auto bump = [&](ValueId v) {
        if (v != InvalidValue) {
            uses[v]++;
        }
    };
    for (const Block &b : f.blocks) {
        for (ValueId p : b.phis) {
            for (ValueId op : f.inst(p).operands) {
                bump(op);
            }
        }
        for (ValueId i : b.insts) {
            for (ValueId op : f.inst(i).operands) {
                bump(op);
            }
        }
        if (b.terminator != InvalidValue) {
            for (ValueId op : f.inst(b.terminator).operands) {
                bump(op);
            }
        }
    }
    bool changed = false;
    for (Inst &in : f.insts) {
        if (in.op != Op::StrConcat || in.operands.empty()) {
            continue;
        }
        ValueId lhs = in.operands[0];
        if (lhs == InvalidValue) {
            continue;
        }
        // lhs must be a StrConcat result (=> fresh mutable StringObj) used only here.
        if (f.inst(lhs).op != Op::StrConcat) {
            continue;
        }
        auto it = uses.find(lhs);
        if (it == uses.end() || it->second != 1) {
            continue;
        }
        in.imm_u32 = 1;
        changed = true;
    }
    return changed;
}

// prove a builtin global (e.g. "to_string") is unshadowable in this chunk
uint32_t analyze_frozen_builtin(const nari::bytecode::Chunk &chunk, const char *name) {
    using namespace nari::bytecode;
    // locate the name's string-pool index. If it isn't in the pool at all,
    // no LoadGlobal can reference it, so there is nothing to fuse.
    uint32_t name_idx = UINT32_MAX;
    for (uint32_t i = 0; i < chunk.strings.size(); i++) {
        if (chunk.strings[i] == name) {
            name_idx = i;
            break;
        }
    }
    if (name_idx == UINT32_MAX) {
        return UINT32_MAX;
    }
    // check if a user function with this name shadows the builtin at load time.
    for (const FunctionMeta &fn : chunk.functions) {
        if (fn.name == name) {
            return UINT32_MAX;
        }
    }
    // check if any OP_STORE_GLOBAL to this name-index rebinds it at runtime.
    // Scan every function's full bytecode, any unknown opcode means we should bail just in case.
    auto u16_at = [&](const ByteArray &code, size_t pc) -> uint16_t {
        return uint16_t((uint16_t(code[pc]) << 8) | uint16_t(code[pc + 1]));
    };
    for (const FunctionMeta &fn : chunk.functions) {
        const ByteArray &code = fn.code;
        size_t pc = 0;
        while (pc < code.size()) {
            OpCode op = OpCode(code[pc]);
            size_t instruction_size = decoded_instruction_size(code, pc);
            if (instruction_size == 0) {
                return UINT32_MAX;
            }
            if (op == OpCode::OP_STORE_GLOBAL) {
                if (pc + 3 > code.size()) {
                    return UINT32_MAX;
                }
                if (u16_at(code, pc + 1) == name_idx) {
                    return UINT32_MAX;
                }
            }
            pc += instruction_size;
        }
    }
    return name_idx;
}

// fuse `s @ to_string(x)` -> `s @ x`, eliding the throwaway StringObj that builtin_toString allocates
bool fuse_tostring_concat(Func &f, uint32_t tostring_name_idx) {
    if (tostring_name_idx == UINT32_MAX) {
        return false;
    }
    // Global single-use count over every operand slot.
    // A call result used exactly once is uniquely referenced, so eliding it is invisible to the rest of the function.
    std::unordered_map<ValueId, int> uses;
    auto bump = [&](ValueId v) {
        if (v != InvalidValue) {
            uses[v]++;
        }
    };
    for (const Block &b : f.blocks) {
        for (ValueId p : b.phis) {
            for (ValueId op : f.inst(p).operands) {
                bump(op);
            }
        }
        for (ValueId i : b.insts) {
            for (ValueId op : f.inst(i).operands) {
                bump(op);
            }
        }
        if (b.terminator != InvalidValue) {
            for (ValueId op : f.inst(b.terminator).operands) {
                bump(op);
            }
        }
    }

    // A Call qualifies if: argc==1, callee (operand[0]) is a LoadGlobal of the
    // frozen "to_string" name used only by this call, and the call's result is used exactly once.
    auto is_fusable_call = [&](const Inst &call) -> bool {
        if (call.op != Op::Call || call.imm_u32 != 1 || call.operands.size() != 2) {
            return false;
        }
        ValueId callee = call.operands[0];
        if (callee == InvalidValue) {
            return false;
        }
        const Inst &cf = f.inst(callee);
        if (cf.op != Op::LoadGlobal || cf.imm_u32 != tostring_name_idx) {
            return false;
        }
        auto cu = uses.find(callee);
        if (cu == uses.end() || cu->second != 1) {
            return false; // shared callee: cannot drop its push safely
        }
        auto it = uses.find(call.result);
        return it != uses.end() && it->second == 1;
    };

    bool changed = false;
    for (Block &b : f.blocks) {
        // find fusable triples in this block.
        std::unordered_set<ValueId> fuse_call;         // committed call result ids to drop
        std::unordered_map<ValueId, ValueId> pend_arg; // pending callResult -> arg
        std::unordered_set<ValueId> drop_callee;       // callee LoadGlobal ids to drop
        std::unordered_map<ValueId, ValueId> retarget; // strConcat id -> new rhs (=arg)

        for (ValueId v : b.insts) {
            const Inst &in = f.inst(v);
            // A StrConcat consuming a pending fusable call's result commits it.
            if (in.op == Op::StrConcat && in.operands.size() == 2) {
                ValueId rhs = in.operands[1];
                auto pit = pend_arg.find(rhs);
                if (pit != pend_arg.end()) {
                    fuse_call.insert(rhs); // commit: drop the call
                    drop_callee.insert(f.inst(rhs).operands[0]); // drop its callee
                    retarget[v] = pit->second; // StrConcat RHS -> arg
                    pend_arg.erase(pit);
                    continue;
                }
            }
            if (is_fusable_call(in)) {
                pend_arg[in.result] = in.operands[1];
            }
        }
        // Any call left in pend_arg had no in-block StrConcat consumer,
        // leave it untouched, since fusing would unbalance the stack.
        if (fuse_call.empty()) {
            continue; // nothing committed in this block
        }

        // rebuild the instruction stream.
        // Skip committed calls and their sole-use callee LoadGlobals, retarget consumer StrConcats
        std::vector<ValueId> body;
        body.reserve(b.insts.size());
        for (ValueId v : b.insts) {
            if (fuse_call.count(v)) {
                continue; // drop the to_string Call
            }
            if (drop_callee.count(v)) {
                continue; // drop its callee LoadGlobal push
            }
            auto rit = retarget.find(v);
            if (rit != retarget.end()) {
                f.inst(v).operands[1] = rit->second;
            }
            body.push_back(v);
        }
        b.insts = std::move(body);
        changed = true;
    }
    return changed;
}

} // namespace ir
} // namespace jit
} // namespace nari
#endif // !DISABLE_JIT
