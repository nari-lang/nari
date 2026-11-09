// Block-level IR optimization passes (split out of ir_opt.cpp).
#ifndef DISABLE_JIT
#include "ir_opt_internal.h"

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
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nari {
namespace jit {
namespace ir {

bool is_int(Ty t) {
    return t == Ty::Int48;
}
bool is_float(Ty t) {
    return t == Ty::Float;
}
bool numeric(Ty t) {
    return t == Ty::Int48 || t == Ty::Float || t == Ty::Number;
}

// Lattice join. Bottom is the identity (no info yet); Unknown is absorbing (top).
// Numeric types merge to Number rather than collapsing to Unknown, so a loop that
// mixes int and float stays numeric.
Ty join(Ty a, Ty b) {
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
Ty arith_type(Op op, Ty a, Ty b) {
    if (a == Ty::Bottom || b == Ty::Bottom) {
        return Ty::Bottom;
    }
    switch (op) {
        case Op::DynAdd:
        case Op::DynSub:
        case Op::DynMul:
        case Op::DynMod:
            if (is_int(a) && is_int(b)) {
                // Add/sub/mul can promote to float. Modulo stays an Int48.
                return op == Op::DynMod ? Ty::Int48 : Ty::Number;
            }
            if ((is_float(a) || is_float(b)) && numeric(a) && numeric(b)) {
                return Ty::Float;
            }
            // Number is a boxed numeric result (for example an integer add
            // that may overflow to float). Keep subsequent numeric operations
            // numeric; widening it to Unknown poisons loop counters and then
            // every float expression that depends on them.
            return (numeric(a) && numeric(b)) ? Ty::Number : Ty::Unknown;
        case Op::DynDiv:
            return (numeric(a) && numeric(b)) ? Ty::Number : Ty::Unknown;
        default:
            return Ty::Unknown;
    }
}

static constexpr int64_t kInt48Max = (1LL << 47) - 1;
static constexpr int64_t kInt48Min = -(1LL << 47);

bool fits_int48_i64(int64_t v) {
    return v >= kInt48Min && v <= kInt48Max;
}

static bool multiply_fits_int48(int64_t a, int64_t b, int64_t &out) {
    if (!fits_int48_i64(a) || !fits_int48_i64(b)) {
        return false;
    }
    const uint64_t magnitude_a = a < 0 ? static_cast<uint64_t>(-a) : static_cast<uint64_t>(a);
    const uint64_t magnitude_b = b < 0 ? static_cast<uint64_t>(-b) : static_cast<uint64_t>(b);
    const uint64_t limit = (a < 0) != (b < 0) ? (uint64_t{1} << 47) : ((uint64_t{1} << 47) - 1);
    if (magnitude_b != 0 && magnitude_a > limit / magnitude_b) {
        return false;
    }
    out = a * b; // product is now proven to fit Int48, hence also int64_t
    return true;
}

static bool reaches_without(const Func &f, BlockId start, BlockId target, BlockId excluded) {
    if (start == excluded || start < 0 || static_cast<size_t>(start) >= f.blocks.size()) {
        return false;
    }
    std::vector<uint8_t> seen(f.blocks.size());
    std::vector<BlockId> pending{start};
    while (!pending.empty()) {
        BlockId b = pending.back();
        pending.pop_back();
        if (b == excluded || b < 0 || static_cast<size_t>(b) >= seen.size() || seen[b]) {
            continue;
        }
        if (b == target) {
            return true;
        }
        seen[b] = 1;
        for (BlockId next : f.blocks[b].succs) {
            pending.push_back(next);
        }
    }
    return false;
}

void mark_bounded_counter_adds(Func &f, const GlobalIntMap &global_ints, uint32_t length_method_name_idx) {
    if (f.entry == InvalidBlock || f.blocks.empty() || f.num_slots > 256 ||
        f.blocks.size() > 512 || f.insts.size() > 4096) {
        return;
    }
    std::unordered_map<ValueId, int64_t> fresh_lengths;
    if (length_method_name_idx != UINT32_MAX) {
        for (const Block &b : f.blocks) {
            for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                ValueId id = b.insts[pos];
                Inst &call = f.inst(id);
                if (call.op != Op::CallMethod || call.imm_u32 != length_method_name_idx || call.operands.size() != 1) {
                    continue;
                }
                ValueId array_id = call.operands[0];
                const Inst &array = f.inst(array_id);
                if (array.op != Op::MakeArray) {
                    continue;
                }
                size_t source_pos = 0;
                while (source_pos < pos && b.insts[source_pos] != array_id) {
                    ++source_pos;
                }
                if (source_pos == pos) {
                    continue;
                }
                // The receiver is still the fresh array: between its creation
                // and length(), permit only binding it to a non-captured local.
                bool untouched = true;
                for (size_t i = source_pos + 1; i < pos; ++i) {
                    const Inst &between = f.inst(b.insts[i]);
                    if (between.op != Op::StoreSlot || between.operands.size() != 1 ||
                        between.operands[0] != array_id || between.imm_u32 >= f.num_slots ||
                        (between.imm_u32 < f.captured_local_slots.size() && f.captured_local_slots[between.imm_u32])) {
                        untouched = false;
                        break;
                    }
                }
                if (untouched) {
                    call.proven_int48 = true;
                    fresh_lengths.emplace(id, static_cast<int64_t>(array.imm_u32));
                }
            }
        }
    }
    std::vector<std::optional<int64_t>> fixed_lengths(f.num_slots);
    std::vector<unsigned> length_writes(f.num_slots, 0);
    for (const Block &b : f.blocks) {
        for (size_t pos = 0; pos < b.insts.size(); ++pos) {
            const Inst &in = f.inst(b.insts[pos]);
            if (in.imm_u32 >= f.num_slots) {
                continue;
            }
            if (in.op == Op::StoreSlot || in.op == Op::StoreImmSlot || in.op == Op::StoreBImmSlot ||
                in.op == Op::StoreNSlot || in.op == Op::StoreCSlot || in.op == Op::CopySlot) {
                uint32_t slot = in.imm_u32;
                ++length_writes[slot];
                if (b.id != f.entry || in.op != Op::StoreSlot || in.operands.size() != 1 ||
                    slot < f.num_params || (slot < f.captured_local_slots.size() && f.captured_local_slots[slot])) {
                    continue;
                }
                auto length = fresh_lengths.find(in.operands[0]);
                if (length == fresh_lengths.end()) {
                    continue;
                }
                bool earlier_read = false;
                for (size_t i = 0; i < pos; ++i) {
                    const Inst &previous = f.inst(b.insts[i]);
                    earlier_read |= previous.op == Op::LoadSlot && previous.imm_u32 == slot;
                }
                if (!earlier_read) {
                    fixed_lengths[slot] = length->second;
                }
            }
        }
    }
    for (uint32_t slot = 0; slot < f.num_slots; ++slot) {
        if (length_writes[slot] != 1) {
            fixed_lengths[slot].reset();
        }
    }
    struct Increment {
        BlockId block;
        ValueId value;
        int64_t delta;
    };
    struct Candidate {
        bool bad = false;
        BlockId init_block = InvalidBlock;
        int64_t init_value = 0;
        int64_t max_value = 0;
        std::vector<Increment> increments;
        std::vector<BlockId> loads;
    };
    std::vector<Candidate> slots(f.num_slots);
    auto load_from_slot = [&](ValueId value, uint32_t slot) {
        if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
            return false;
        }
        const Inst *source = &f.inst(value);
        if (source->op == Op::Dup && source->operands.size() == 1) {
            source = &f.inst(source->operands[0]);
        }
        return source->op == Op::LoadSlot && source->imm_u32 == slot;
    };
    auto positive_test = [&](const Inst &branch, const Inst *&comparison, BlockId &true_target) {
        if (branch.op != Op::Branch || branch.operands.size() != 1) {
            return false;
        }
        comparison = &f.inst(branch.operands[0]);
        bool inverted = false;
        for (int depth = 0; depth < 8 && comparison->op == Op::Not && comparison->operands.size() == 1; ++depth) {
            comparison = &f.inst(comparison->operands[0]);
            inverted = !inverted;
        }
        true_target = inverted ? branch.target1 : branch.target0;
        return true_target != InvalidBlock && (comparison->op == Op::DynCmpLt || comparison->op == Op::DynCmpLe);
    };
    for (const Block &b : f.blocks) {
        for (ValueId id : b.insts) {
            const Inst &in = f.inst(id);
            if (in.op == Op::LoadSlot && in.imm_u32 < slots.size()) {
                slots[in.imm_u32].loads.push_back(b.id);
            }
            if (in.imm_u32 >= slots.size()) {
                continue;
            }
            Candidate &c = slots[in.imm_u32];
            if (in.op == Op::StoreImmSlot || (in.op == Op::StoreSlot && in.operands.size() == 1 &&
                                              f.inst(in.operands[0]).op == Op::IConst)) {
                const int64_t value = in.op == Op::StoreImmSlot ? in.imm_int : f.inst(in.operands[0]).imm_int;
                if (c.init_block != InvalidBlock || !fits_int48_i64(value)) {
                    c.bad = true;
                } else {
                    c.init_block = b.id;
                    c.init_value = value;
                    c.max_value = value;
                }
            } else if (in.op == Op::StoreSlot && in.operands.size() == 1) {
                ValueId value = in.operands[0];
                const Inst &add = f.inst(value);
                if (add.op != Op::DynAdd || add.operands.size() != 2 ||
                    !load_from_slot(add.operands[0], in.imm_u32) || f.inst(add.operands[1]).op != Op::IConst) {
                    c.bad = true;
                } else {
                    const int64_t delta = f.inst(add.operands[1]).imm_int;
                    if (delta <= 0 || delta > INT32_MAX) {
                        c.bad = true;
                    } else {
                        c.increments.push_back({b.id, value, delta});
                    }
                }
            } else if (in.op == Op::StoreBImmSlot || in.op == Op::StoreNSlot || in.op == Op::StoreCSlot ||
                       in.op == Op::CopySlot) {
                c.bad = true;
            }
        }
    }
    struct Range {
        int64_t lo;
        int64_t hi;
    };
    std::vector<std::optional<Range>> slot_ranges(f.num_slots);
    for (uint32_t slot = 0; slot < slots.size(); ++slot) {
        Candidate &c = slots[slot];
        if (c.bad || c.init_block == InvalidBlock || c.increments.size() != 1 ||
            (slot < f.captured_local_slots.size() && f.captured_local_slots[slot])) {
            continue;
        }
        // The initializer must execute before every read, including the loop
        // guard itself. Require strict block dominance for a simple proof.
        bool initialized = true;
        for (BlockId load : c.loads) {
            if (load == c.init_block || reaches_without(f, f.entry, load, c.init_block)) {
                initialized = false;
                break;
            }
        }
        if (!initialized) {
            continue;
        }
        bool safe = true;
        for (const Increment &inc : c.increments) {
            bool guarded = false;
            for (const Block &guard : f.blocks) {
                const Inst &branch = f.inst(guard.terminator);
                const Inst *cmp = nullptr;
                BlockId true_target = InvalidBlock;
                if (!positive_test(branch, cmp, true_target) || reaches_without(f, f.entry, inc.block, true_target)) {
                    continue;
                }
                if (cmp->operands.size() != 2 || !load_from_slot(cmp->operands[0], slot)) {
                    continue;
                }
                const Inst &limit_inst = f.inst(cmp->operands[1]);
                std::optional<int64_t> known_limit;
                if (limit_inst.op == Op::IConst) {
                    known_limit = limit_inst.imm_int;
                } else if (limit_inst.op == Op::LoadSlot && limit_inst.imm_u32 < fixed_lengths.size()) {
                    known_limit = fixed_lengths[limit_inst.imm_u32];
                }
                if (!known_limit) {
                    continue;
                }
                const int64_t limit = *known_limit;
                if (limit > kInt48Max - inc.delta) {
                    continue;
                }
                // Every repetition of this increment must recheck the guard.
                bool bypass = false;
                for (BlockId next : f.blocks[inc.block].succs) {
                    bypass |= reaches_without(f, next, inc.block, guard.id);
                }
                if (!bypass) {
                    guarded = true;
                    c.max_value = std::max(c.max_value, limit + inc.delta);
                    const int64_t span = limit - c.init_value + (cmp->op == Op::DynCmpLe ? 1 : 0);
                    const BlockId false_target = true_target == branch.target0 ? branch.target1 : branch.target0;
                    if (span >= 0 && span <= 16 * inc.delta &&
                        !reaches_without(f, true_target, guard.id, inc.block) &&
                        !reaches_without(f, false_target, guard.id, inc.block)) {
                        f.blocks[guard.id].short_bounded_loop = true;
                    }
                    break;
                }
            }
            if (!guarded) {
                safe = false;
                break;
            }
        }
        if (safe) {
            slot_ranges[slot] = Range{c.init_value, c.max_value};
            for (const Increment &inc : c.increments) {
                f.inst(inc.value).proven_int48 = true;
            }
        }
    }
    // Propagate conservative intervals through expressions built from bounded
    // counters and immutable slots. This proves grid indexes without assuming
    // that arbitrary arithmetic stays inside the tagged integer range.
    std::function<std::optional<Range>(ValueId, unsigned)> range_of = [&](ValueId id, unsigned depth) -> std::optional<Range> {
        if (id < 0 || static_cast<size_t>(id) >= f.insts.size() || depth > 32) {
            return std::nullopt;
        }
        const Inst &in = f.inst(id);
        if (in.op == Op::IConst) {
            return fits_int48_i64(in.imm_int) ? std::optional<Range>(Range{in.imm_int, in.imm_int}) : std::nullopt;
        }
        if (in.op == Op::LoadSlot) {
            if (in.imm_u32 >= slot_ranges.size()) {
                return std::nullopt;
            }
            if (slot_ranges[in.imm_u32]) {
                return slot_ranges[in.imm_u32];
            }
            if (fixed_lengths[in.imm_u32]) {
                const int64_t n = *fixed_lengths[in.imm_u32];
                return Range{n, n};
            }
            return std::nullopt;
        }
        if (in.op == Op::CallMethod) {
            auto length = fresh_lengths.find(id);
            if (length != fresh_lengths.end()) {
                return Range{length->second, length->second};
            }
        }
        if (in.op == Op::LoadGlobal) {
            auto global = global_ints.find(in.imm_u32);
            return global != global_ints.end() ? std::optional<Range>(Range{global->second, global->second}) : std::nullopt;
        }
        if (in.op == Op::Dup && in.operands.size() == 1) {
            return range_of(in.operands[0], depth + 1);
        }
        if ((in.op != Op::DynAdd && in.op != Op::DynSub && in.op != Op::DynMul &&
             in.op != Op::DynMod && in.op != Op::IMod) || in.operands.size() != 2) {
            return std::nullopt;
        }
        auto lhs = range_of(in.operands[0], depth + 1);
        auto rhs = range_of(in.operands[1], depth + 1);
        if (!lhs || !rhs) {
            return std::nullopt;
        }
        if (in.op == Op::DynMod || in.op == Op::IMod) {
            if (rhs->lo != rhs->hi || rhs->lo == 0) {
                return std::nullopt;
            }
            const int64_t bound = rhs->lo < 0 ? -rhs->lo : rhs->lo;
            // Integer remainder has the dividend's sign and magnitude below
            // the divisor's. This holds for either divisor sign.
            return Range{lhs->lo >= 0 ? 0 : std::max(lhs->lo, 1 - bound),
                         lhs->hi <= 0 ? 0 : std::min(lhs->hi, bound - 1)};
        }
        int64_t lo = 0, hi = 0;
        if (in.op == Op::DynAdd) {
            lo = lhs->lo + rhs->lo;
            hi = lhs->hi + rhs->hi;
        } else if (in.op == Op::DynSub) {
            lo = lhs->lo - rhs->hi;
            hi = lhs->hi - rhs->lo;
        } else {
            int64_t products[4];
            if (!multiply_fits_int48(lhs->lo, rhs->lo, products[0]) ||
                !multiply_fits_int48(lhs->lo, rhs->hi, products[1]) ||
                !multiply_fits_int48(lhs->hi, rhs->lo, products[2]) ||
                !multiply_fits_int48(lhs->hi, rhs->hi, products[3])) {
                return std::nullopt;
            }
            lo = *std::min_element(std::begin(products), std::end(products));
            hi = *std::max_element(std::begin(products), std::end(products));
        }
        if (lo < kInt48Min || hi > kInt48Max) {
            return std::nullopt;
        }
        return Range{lo, hi};
    };
    struct Write {
        BlockId block = InvalidBlock;
        size_t position = 0;
        ValueId value = InvalidValue;
        int count = 0;
    };
    std::vector<Write> writes(f.num_slots);
    for (const Block &b : f.blocks) {
        for (size_t pos = 0; pos < b.insts.size(); ++pos) {
            const Inst &in = f.inst(b.insts[pos]);
            if (in.imm_u32 >= writes.size()) {
                continue;
            }
            if (in.op == Op::StoreSlot || in.op == Op::StoreImmSlot || in.op == Op::StoreBImmSlot ||
                in.op == Op::StoreNSlot || in.op == Op::StoreCSlot || in.op == Op::CopySlot) {
                Write &w = writes[in.imm_u32];
                ++w.count;
                w.block = b.id;
                w.position = pos;
                w.value = b.insts[pos];
            }
        }
    }
    // A common integer recurrence is x = (x * multiplier) % modulus. If the
    // write-once modulus and multiplier bound the full product to Int48, the
    // recurrence stays integer for every iteration, including the first.
    for (uint32_t slot = 0; slot < writes.size(); ++slot) {
        const Candidate &c = slots[slot];
        const Write &w = writes[slot];
        if (slot_ranges[slot] || c.init_block == InvalidBlock || w.count != 2 || slot < f.num_params ||
            (slot < f.captured_local_slots.size() && f.captured_local_slots[slot])) {
            continue;
        }
        const Inst &store = f.inst(w.value);
        if (store.op != Op::StoreSlot || store.operands.size() != 1) {
            continue;
        }
        Inst &mod = f.inst(store.operands[0]);
        if (mod.op != Op::DynMod || mod.operands.size() != 2) {
            continue;
        }
        Inst &mul = f.inst(mod.operands[0]);
        if (mul.op != Op::DynMul || mul.operands.size() != 2 || !load_from_slot(mul.operands[0], slot) ||
            f.inst(mul.operands[1]).op != Op::IConst) {
            continue;
        }
        const Inst &modulus_inst = f.inst(mod.operands[1]);
        std::optional<int64_t> modulus;
        if (modulus_inst.op == Op::IConst) {
            modulus = modulus_inst.imm_int;
        } else if (modulus_inst.op == Op::LoadGlobal) {
            auto found = global_ints.find(modulus_inst.imm_u32);
            if (found != global_ints.end()) {
                modulus = found->second;
            }
        }
        const int64_t multiplier = f.inst(mul.operands[1]).imm_int;
        int64_t max_product = 0;
        if (!modulus || *modulus <= 0 || multiplier < 0 || c.init_value < 0 || c.init_value >= *modulus ||
            !multiply_fits_int48(*modulus - 1, multiplier, max_product)) {
            continue;
        }
        bool initialized = true;
        for (BlockId load : c.loads) {
            if (load == c.init_block || reaches_without(f, f.entry, load, c.init_block)) {
                initialized = false;
                break;
            }
        }
        if (initialized) {
            slot_ranges[slot] = Range{0, *modulus - 1};
            mul.proven_int48 = true;
            mod.proven_int48 = true;
        }
    }
    for (size_t round = 0; round < f.num_slots; ++round) {
        bool changed = false;
        for (uint32_t slot = 0; slot < writes.size(); ++slot) {
            const Write &w = writes[slot];
            if (slot_ranges[slot] || w.count != 1 || slot < f.num_params ||
                (slot < f.captured_local_slots.size() && f.captured_local_slots[slot])) {
                continue;
            }
            const Inst &store = f.inst(w.value);
            std::optional<Range> value;
            if (store.op == Op::StoreImmSlot && fits_int48_i64(store.imm_int)) {
                value = Range{store.imm_int, store.imm_int};
            } else if (store.op == Op::StoreSlot && store.operands.size() == 1) {
                value = range_of(store.operands[0], 0);
            }
            if (!value) {
                continue;
            }
            bool dominates_reads = true;
            for (const Block &b : f.blocks) {
                for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                    const Inst &in = f.inst(b.insts[pos]);
                    if (in.op != Op::LoadSlot || in.imm_u32 != slot) {
                        continue;
                    }
                    if ((b.id == w.block && pos <= w.position) ||
                        (b.id != w.block && reaches_without(f, f.entry, b.id, w.block))) {
                        dominates_reads = false;
                    }
                }
            }
            if (dominates_reads) {
                slot_ranges[slot] = value;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    // Repeat with guarded recurrences whose step or loop bound is a previously
    // bounded slot. Examples include a doubling merge width, a row offset that
    // advances by that width, and merge cursors bounded by derived endpoints.
    // A slot qualifies only when every write is accounted for and every update
    // must pass through its own bound check before it can execute again.
    struct LocatedWrite {
        BlockId block;
        size_t position;
        ValueId id;
    };
    std::vector<std::vector<LocatedWrite>> all_writes(f.num_slots);
    for (const Block &b : f.blocks) {
        for (size_t pos = 0; pos < b.insts.size(); ++pos) {
            const Inst &in = f.inst(b.insts[pos]);
            if (in.imm_u32 < all_writes.size() &&
                (in.op == Op::StoreSlot || in.op == Op::StoreImmSlot || in.op == Op::CopySlot ||
                 in.op == Op::StoreBImmSlot || in.op == Op::StoreNSlot || in.op == Op::StoreCSlot)) {
                all_writes[in.imm_u32].push_back({b.id, pos, b.insts[pos]});
            }
        }
    }
    for (size_t round = 0; round < f.num_slots; ++round) {
        bool changed = false;
        for (uint32_t slot = 0; slot < all_writes.size(); ++slot) {
            if (slot_ranges[slot] || slot < f.num_params || all_writes[slot].empty() ||
                (slot < f.captured_local_slots.size() && f.captured_local_slots[slot])) {
                continue;
            }
            std::optional<Range> initial;
            std::vector<LocatedWrite> updates;
            std::vector<LocatedWrite> initial_writes;
            bool valid = true;
            for (const LocatedWrite &w : all_writes[slot]) {
                const Inst &store = f.inst(w.id);
                std::optional<Range> value;
                if (store.op == Op::StoreImmSlot && fits_int48_i64(store.imm_int)) {
                    value = Range{store.imm_int, store.imm_int};
                } else if (store.op == Op::CopySlot && store.imm_int >= 0 &&
                           static_cast<size_t>(store.imm_int) < slot_ranges.size()) {
                    value = slot_ranges[static_cast<size_t>(store.imm_int)];
                } else if (store.op == Op::StoreSlot && store.operands.size() == 1) {
                    const Inst &source = f.inst(store.operands[0]);
                    if ((source.op == Op::DynAdd || source.op == Op::DynMul) && source.operands.size() == 2 &&
                        load_from_slot(source.operands[0], slot)) {
                        updates.push_back(w);
                        continue;
                    }
                    value = range_of(store.operands[0], 0);
                }
                if (!value) {
                    valid = false;
                    break;
                }
                initial_writes.push_back(w);
                if (!initial) {
                    initial = value;
                } else {
                    initial->lo = std::min(initial->lo, value->lo);
                    initial->hi = std::max(initial->hi, value->hi);
                }
            }
            if (!valid || !initial) {
                continue;
            }
            bool initialized = false;
            for (const LocatedWrite &w : initial_writes) {
                bool dominates = true;
                for (const Block &b : f.blocks) {
                    for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                        const Inst &in = f.inst(b.insts[pos]);
                        if (in.op == Op::LoadSlot && in.imm_u32 == slot &&
                            ((b.id == w.block && pos <= w.position) ||
                             (b.id != w.block && reaches_without(f, f.entry, b.id, w.block)))) {
                            dominates = false;
                        }
                    }
                }
                if (dominates) {
                    initialized = true;
                    break;
                }
            }
            if (!initialized) {
                continue;
            }
            int64_t upper = initial->hi;
            for (const LocatedWrite &update : updates) {
                const Inst &arithmetic = f.inst(f.inst(update.id).operands[0]);
                auto step = range_of(arithmetic.operands[1], 0);
                if (!step || initial->lo < 0 ||
                    (arithmetic.op == Op::DynAdd && step->lo < 0) ||
                    (arithmetic.op == Op::DynMul && step->lo < 1)) {
                    valid = false;
                    break;
                }
                bool guarded = false;
                for (const Block &guard : f.blocks) {
                    const Inst &branch = f.inst(guard.terminator);
                    const Inst *cmp = nullptr;
                    BlockId true_target = InvalidBlock;
                    if (!positive_test(branch, cmp, true_target) || reaches_without(f, f.entry, update.block, true_target)) {
                        continue;
                    }
                    if (cmp->operands.size() != 2 || !load_from_slot(cmp->operands[0], slot)) {
                        continue;
                    }
                    auto bound = range_of(cmp->operands[1], 0);
                    if (!bound || bound->hi < 0) {
                        continue;
                    }
                    const int64_t old_max = cmp->op == Op::DynCmpLt ? bound->hi - 1 : bound->hi;
                    if (old_max < 0) {
                        continue;
                    }
                    int64_t result_max = 0;
                    if (arithmetic.op == Op::DynAdd) {
                        if (step->hi > kInt48Max || old_max > kInt48Max - step->hi ||
                            initial->hi > kInt48Max - step->hi) {
                            continue;
                        }
                        result_max = old_max + step->hi;
                    } else {
                        int64_t initial_product = 0;
                        if (!multiply_fits_int48(old_max, step->hi, result_max) ||
                            !multiply_fits_int48(initial->hi, step->hi, initial_product)) {
                            continue;
                        }
                    }
                    bool bypass = false;
                    for (const LocatedWrite &other : updates) {
                        if (other.id != update.id && other.block == update.block) {
                            bypass = true;
                        }
                        for (BlockId next : f.blocks[other.block].succs) {
                            bypass |= reaches_without(f, next, update.block, guard.id);
                        }
                    }
                    if (!bypass) {
                        upper = std::max(upper, result_max);
                        guarded = true;
                        break;
                    }
                }
                if (!guarded) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                slot_ranges[slot] = Range{initial->lo, upper};
                for (const LocatedWrite &update : updates) {
                    f.inst(f.inst(update.id).operands[0]).proven_int48 = true;
                }
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    for (const Block &b : f.blocks) {
        for (ValueId id : b.insts) {
            Inst &in = f.inst(id);
            if ((in.op == Op::DynAdd || in.op == Op::DynSub || in.op == Op::DynMul) && range_of(id, 0)) {
                in.proven_int48 = true;
            }
        }
    }
}

// Recognize reads of a private, fixed-size integer array in a guarded stride
// loop. For example, `while (pc < program.length()) { program[pc];
// program[pc + 1]; pc += 2; }` is in bounds when the program length is a
// multiple of two. Keeping this separate from integer-element analysis is
// important: an integer-only array can still return None outside its bounds.
void mark_fixed_array_bounded_reads(Func &f, const IntArraySlots &int_array_slots, uint32_t length_method_name_idx) {
    if (int_array_slots.empty() || length_method_name_idx == UINT32_MAX || f.entry == InvalidBlock ||
        f.num_slots > 256 || f.blocks.size() > 512 || f.insts.size() > 4096) {
        return;
    }
    struct Write {
        ValueId id = InvalidValue;
        BlockId block = InvalidBlock;
        size_t position = 0;
        unsigned count = 0;
    };
    std::vector<Write> writes(f.num_slots);
    for (const Block &b : f.blocks) {
        for (size_t pos = 0; pos < b.insts.size(); ++pos) {
            ValueId id = b.insts[pos];
            const Inst &in = f.inst(id);
            if (in.imm_u32 < writes.size() &&
                (in.op == Op::StoreSlot || in.op == Op::StoreImmSlot || in.op == Op::StoreBImmSlot ||
                 in.op == Op::StoreNSlot || in.op == Op::StoreCSlot || in.op == Op::CopySlot)) {
                Write &w = writes[in.imm_u32];
                w.id = id;
                w.block = b.id;
                w.position = pos;
                ++w.count;
            }
        }
    }
    auto uncaptured_local = [&](uint32_t slot) {
        return slot < f.num_slots && slot >= f.num_params &&
               (slot >= f.captured_local_slots.size() || !f.captured_local_slots[slot]);
    };
    struct ArrayInfo {
        ValueId maker = InvalidValue;
        int64_t length = 0;
    };
    std::vector<std::optional<ArrayInfo>> arrays(f.num_slots);
    for (uint32_t slot : int_array_slots) {
        if (!uncaptured_local(slot) || writes[slot].count != 1 || writes[slot].block != f.entry) {
            continue;
        }
        const Inst &store = f.inst(writes[slot].id);
        if (store.op != Op::StoreSlot || store.operands.size() != 1) {
            continue;
        }
        ValueId maker = store.operands[0];
        const Inst &array = f.inst(maker);
        if (array.op != Op::MakeArray || array.imm_u32 == 0) {
            continue;
        }
        bool private_array = true;
        for (const Block &b : f.blocks) {
            for (ValueId phi_id : b.phis) {
                for (ValueId value : f.inst(phi_id).operands) {
                    if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
                        private_array = false;
                        break;
                    }
                    const Inst &source = f.inst(value);
                    if (value == maker || (source.op == Op::LoadSlot && source.imm_u32 == slot)) {
                        private_array = false;
                    }
                }
            }
            if (!private_array) {
                break;
            }
            for (size_t pos = 0; pos < b.insts.size() && private_array; ++pos) {
                const Inst &in = f.inst(b.insts[pos]);
                if (in.op == Op::LoadSlot && in.imm_u32 == slot && b.id == f.entry && pos <= writes[slot].position) {
                    private_array = false;
                    break;
                }
                for (size_t arg = 0; arg < in.operands.size(); ++arg) {
                    ValueId value = in.operands[arg];
                    if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
                        private_array = false;
                        break;
                    }
                    const Inst &source = f.inst(value);
                    if (value != maker && !(source.op == Op::LoadSlot && source.imm_u32 == slot)) {
                        continue;
                    }
                    const bool binding = in.op == Op::StoreSlot && in.imm_u32 == slot && arg == 0 && value == maker;
                    const bool reading = in.op == Op::LoadIndex && arg == 0;
                    const bool length = in.op == Op::CallMethod && in.imm_u32 == length_method_name_idx &&
                                        in.imm_int == 0 && in.operands.size() == 1 && arg == 0;
                    const bool discard = in.op == Op::Pop && arg == 0;
                    if (!binding && !reading && !length && !discard) {
                        private_array = false;
                        break;
                    }
                }
            }
            if (!private_array) {
                break;
            }
            if (b.terminator != InvalidValue) {
                const Inst &term = f.inst(b.terminator);
                for (ValueId value : term.operands) {
                    if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
                        private_array = false;
                        break;
                    }
                    const Inst &source = f.inst(value);
                    if (value == maker || (source.op == Op::LoadSlot && source.imm_u32 == slot)) {
                        private_array = false;
                    }
                }
            }
        }
        if (private_array) {
            arrays[slot] = ArrayInfo{maker, static_cast<int64_t>(array.imm_u32)};
        }
    }
    struct LengthInfo {
        uint32_t array_slot;
        int64_t length;
    };
    std::vector<std::optional<LengthInfo>> lengths(f.num_slots);
    for (uint32_t slot = 0; slot < f.num_slots; ++slot) {
        if (!uncaptured_local(slot) || writes[slot].count != 1 || writes[slot].block != f.entry) {
            continue;
        }
        const Inst &store = f.inst(writes[slot].id);
        if (store.op != Op::StoreSlot || store.operands.size() != 1) {
            continue;
        }
        const Inst &call = f.inst(store.operands[0]);
        if (call.op != Op::CallMethod || call.imm_u32 != length_method_name_idx || call.imm_int != 0 ||
            call.operands.size() != 1) {
            continue;
        }
        for (uint32_t array_slot = 0; array_slot < arrays.size(); ++array_slot) {
            if (!arrays[array_slot]) {
                continue;
            }
            const Inst &receiver = f.inst(call.operands[0]);
            if (call.operands[0] == arrays[array_slot]->maker ||
                (receiver.op == Op::LoadSlot && receiver.imm_u32 == array_slot)) {
                lengths[slot] = LengthInfo{array_slot, arrays[array_slot]->length};
                break;
            }
        }
    }
    auto from_slot = [&](ValueId value, uint32_t slot) {
        const Inst *source = &f.inst(value);
        if (source->op == Op::Dup && source->operands.size() == 1) {
            source = &f.inst(source->operands[0]);
        }
        return source->op == Op::LoadSlot && source->imm_u32 == slot;
    };
    for (const Block &guard : f.blocks) {
        if (guard.terminator == InvalidValue) {
            continue;
        }
        const Inst &branch = f.inst(guard.terminator);
        if (branch.op != Op::Branch || branch.operands.size() != 1 || branch.target0 == InvalidBlock ||
            branch.target1 == InvalidBlock) {
            continue;
        }
        const Inst &cmp = f.inst(branch.operands[0]);
        if ((cmp.op != Op::DynCmpLt && cmp.op != Op::ICmpLt) || cmp.operands.size() != 2) {
            continue;
        }
        const Inst &counter = f.inst(cmp.operands[0]);
        const Inst &limit = f.inst(cmp.operands[1]);
        if (counter.op != Op::LoadSlot || limit.op != Op::LoadSlot || counter.imm_u32 >= writes.size() ||
            limit.imm_u32 >= lengths.size() || !lengths[limit.imm_u32]) {
            continue;
        }
        const uint32_t counter_slot = counter.imm_u32;
        const LengthInfo info = *lengths[limit.imm_u32];
        const BlockId body_id = branch.target0;
        if (body_id < 0 || static_cast<size_t>(body_id) >= f.blocks.size() ||
            !uncaptured_local(counter_slot) || writes[counter_slot].count != 2 ||
            reaches_without(f, f.entry, body_id, guard.id) ||
            reaches_without(f, branch.target1, body_id, guard.id)) {
            continue;
        }
        const Block &body = f.blocks[body_id];
        size_t increment_pos = body.insts.size();
        int64_t stride = 0;
        BlockId init_block = InvalidBlock;
        bool valid_writes = true;
        for (const Block &b : f.blocks) {
            for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                const Inst &in = f.inst(b.insts[pos]);
                if (in.imm_u32 != counter_slot ||
                    (in.op != Op::StoreSlot && in.op != Op::StoreImmSlot && in.op != Op::StoreBImmSlot &&
                     in.op != Op::StoreNSlot && in.op != Op::StoreCSlot && in.op != Op::CopySlot)) {
                    continue;
                }
                if (in.op == Op::StoreImmSlot && in.imm_int == 0 && init_block == InvalidBlock && b.id != body_id) {
                    init_block = b.id;
                } else if (in.op == Op::StoreSlot && in.operands.size() == 1 && b.id == body_id &&
                           increment_pos == body.insts.size()) {
                    const Inst &add = f.inst(in.operands[0]);
                    if (add.op != Op::DynAdd || add.operands.size() != 2 ||
                        !from_slot(add.operands[0], counter_slot) || f.inst(add.operands[1]).op != Op::IConst) {
                        valid_writes = false;
                        break;
                    }
                    stride = f.inst(add.operands[1]).imm_int;
                    increment_pos = pos;
                } else if (in.op == Op::StoreSlot && in.operands.size() == 1 &&
                           f.inst(in.operands[0]).op == Op::IConst && f.inst(in.operands[0]).imm_int == 0 &&
                           init_block == InvalidBlock && b.id != body_id) {
                    init_block = b.id;
                } else {
                    valid_writes = false;
                    break;
                }
            }
            if (!valid_writes) {
                break;
            }
        }
        if (!valid_writes || init_block == InvalidBlock || increment_pos == body.insts.size() ||
            stride <= 0 || info.length % stride != 0 ||
            reaches_without(f, f.entry, guard.id, init_block)) {
            continue;
        }
        bool rechecks = true;
        for (BlockId next : body.succs) {
            if (reaches_without(f, next, body_id, guard.id)) {
                rechecks = false;
                break;
            }
        }
        if (!rechecks) {
            continue;
        }
        for (size_t pos = 0; pos < increment_pos; ++pos) {
            Inst &read = f.inst(body.insts[pos]);
            if (read.op != Op::LoadIndex || read.operands.size() != 2) {
                continue;
            }
            const Inst &receiver = f.inst(read.operands[0]);
            if (receiver.op != Op::LoadSlot || receiver.imm_u32 != info.array_slot) {
                continue;
            }
            ValueId key = read.operands[1];
            const Inst &index = f.inst(key);
            int64_t offset = 0;
            if (!from_slot(key, counter_slot)) {
                if (index.op != Op::DynAdd || index.operands.size() != 2 ||
                    !from_slot(index.operands[0], counter_slot) || f.inst(index.operands[1]).op != Op::IConst) {
                    continue;
                }
                offset = f.inst(index.operands[1]).imm_int;
            }
            if (offset >= 0 && offset < stride) {
                read.proven_inbounds = true;
            }
        }
    }


}

// A loop that appends once per counter value can establish a minimum array
// length for subsequent loops. Indexed writes may extend that array but never
// shrink it. Reject aliases and any other method call on the array so that a
// later read can rely on that minimum length.
IntArraySlots mark_filled_array_bounded_reads(Func &f, uint32_t push_method_name_idx) {
    if (push_method_name_idx == UINT32_MAX || f.entry == InvalidBlock || f.num_slots > 256 ||
        f.blocks.size() > 512 || f.insts.size() > 4096) {
        return {};
    }
    struct Write {
        ValueId id = InvalidValue;
        BlockId block = InvalidBlock;
        size_t position = 0;
        unsigned count = 0;
    };
    std::vector<Write> writes(f.num_slots);
    for (const Block &b : f.blocks) {
        for (size_t pos = 0; pos < b.insts.size(); ++pos) {
            const Inst &in = f.inst(b.insts[pos]);
            if (in.imm_u32 < writes.size() &&
                (in.op == Op::StoreSlot || in.op == Op::StoreImmSlot || in.op == Op::StoreBImmSlot ||
                 in.op == Op::StoreNSlot || in.op == Op::StoreCSlot || in.op == Op::CopySlot)) {
                Write &w = writes[in.imm_u32];
                w.id = b.insts[pos];
                w.block = b.id;
                w.position = pos;
                ++w.count;
            }
        }
    }
    auto private_slot = [&](uint32_t slot) {
        return slot < f.num_slots && slot >= f.num_params &&
               (slot >= f.captured_local_slots.size() || !f.captured_local_slots[slot]);
    };
    auto from_slot = [&](ValueId value, uint32_t slot) {
        if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
            return false;
        }
        const Inst *source = &f.inst(value);
        if (source->op == Op::Dup && source->operands.size() == 1) {
            source = &f.inst(source->operands[0]);
        }
        return source->op == Op::LoadSlot && source->imm_u32 == slot;
    };
    struct Filled {
        int64_t min_length;
        BlockId after;
    };
    std::vector<std::optional<Filled>> filled(f.num_slots);
    for (uint32_t slot = 0; slot < f.num_slots; ++slot) {
        if (!private_slot(slot) || writes[slot].count != 1) {
            continue;
        }
        const Inst &store = f.inst(writes[slot].id);
        if (store.op != Op::StoreSlot || store.operands.size() != 1) {
            continue;
        }
        ValueId maker = store.operands[0];
        const Inst &array = f.inst(maker);
        if (array.op != Op::MakeArray || array.imm_u32 != 0) {
            continue;
        }
        bool private_array = true;
        unsigned pushes = 0;
        BlockId push_block = InvalidBlock;
        ValueId push_id = InvalidValue;
        for (const Block &b : f.blocks) {
            for (ValueId phi_id : b.phis) {
                for (ValueId value : f.inst(phi_id).operands) {
                    if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
                        private_array = false;
                        break;
                    }
                    const Inst &source = f.inst(value);
                    if (value == maker || (source.op == Op::LoadSlot && source.imm_u32 == slot)) {
                        private_array = false;
                    }
                }
            }
            for (size_t pos = 0; pos < b.insts.size() && private_array; ++pos) {
                ValueId id = b.insts[pos];
                const Inst &in = f.inst(id);
                if (in.op == Op::LoadSlot && in.imm_u32 == slot && b.id == writes[slot].block &&
                    pos <= writes[slot].position) {
                    private_array = false;
                    break;
                }
                for (size_t arg = 0; arg < in.operands.size(); ++arg) {
                    ValueId value = in.operands[arg];
                    if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
                        private_array = false;
                        break;
                    }
                    const Inst &source = f.inst(value);
                    if (value != maker && !(source.op == Op::LoadSlot && source.imm_u32 == slot)) {
                        continue;
                    }
                    const bool binding = in.op == Op::StoreSlot && in.imm_u32 == slot && arg == 0 && value == maker;
                    const bool read = in.op == Op::LoadIndex && arg == 0;
                    const bool write = in.op == Op::StoreIndex && arg == 0;
                    const bool push = in.op == Op::CallMethod && in.imm_u32 == push_method_name_idx &&
                                      in.imm_int == 1 && in.operands.size() == 2 && arg == 0;
                    const bool discard = in.op == Op::Pop && arg == 0;
                    if (!binding && !read && !write && !push && !discard) {
                        private_array = false;
                        break;
                    }
                    if (push) {
                        ++pushes;
                        push_block = b.id;
                        push_id = id;
                    }
                }
            }
            if (b.terminator != InvalidValue) {
                for (ValueId value : f.inst(b.terminator).operands) {
                    if (value < 0 || static_cast<size_t>(value) >= f.insts.size()) {
                        private_array = false;
                        break;
                    }
                    const Inst &source = f.inst(value);
                    if (value == maker || (source.op == Op::LoadSlot && source.imm_u32 == slot)) {
                        private_array = false;
                    }
                }
            }
            if (!private_array) {
                break;
            }
        }
        if (!private_array || pushes != 1 || push_block == InvalidBlock) {
            continue;
        }
        for (const Block &guard : f.blocks) {
            if (guard.terminator == InvalidValue) {
                continue;
            }
            const Inst &branch = f.inst(guard.terminator);
            if (branch.op != Op::Branch || branch.operands.size() != 1 || branch.target0 != push_block ||
                branch.target1 < 0 || static_cast<size_t>(branch.target1) >= f.blocks.size()) {
                continue;
            }
            const Inst &cmp = f.inst(branch.operands[0]);
            if (cmp.op != Op::DynCmpLt || cmp.operands.size() != 2 || f.inst(cmp.operands[0]).op != Op::LoadSlot ||
                f.inst(cmp.operands[1]).op != Op::IConst) {
                continue;
            }
            const uint32_t counter = f.inst(cmp.operands[0]).imm_u32;
            const int64_t limit = f.inst(cmp.operands[1]).imm_int;
            if (!private_slot(counter) || limit <= 0 || limit > kInt48Max - 1 || writes[counter].count != 2 ||
                f.blocks[branch.target1].preds.size() != 1 || f.blocks[branch.target1].preds[0] != guard.id ||
                reaches_without(f, f.entry, guard.id, writes[slot].block)) {
                continue;
            }
            const Block &body = f.blocks[push_block];
            if (body.terminator == InvalidValue || f.inst(body.terminator).op != Op::Jump ||
                f.inst(body.terminator).target0 != guard.id) {
                continue;
            }
            BlockId init_block = InvalidBlock;
            size_t push_pos = body.insts.size();
            size_t increment_pos = body.insts.size();
            bool valid = true;
            for (const Block &b : f.blocks) {
                for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                    ValueId id = b.insts[pos];
                    const Inst &in = f.inst(id);
                    if (id == push_id) {
                        push_pos = pos;
                    }
                    if (in.imm_u32 != counter ||
                        (in.op != Op::StoreSlot && in.op != Op::StoreImmSlot && in.op != Op::StoreBImmSlot &&
                         in.op != Op::StoreNSlot && in.op != Op::StoreCSlot && in.op != Op::CopySlot)) {
                        continue;
                    }
                    if (in.op == Op::StoreImmSlot && in.imm_int == 0 && init_block == InvalidBlock && b.id != body.id) {
                        init_block = b.id;
                    } else if (in.op == Op::StoreSlot && in.operands.size() == 1 && b.id == body.id &&
                               increment_pos == body.insts.size()) {
                        const Inst &add = f.inst(in.operands[0]);
                        if (add.op != Op::DynAdd || add.operands.size() != 2 ||
                            !from_slot(add.operands[0], counter) || f.inst(add.operands[1]).op != Op::IConst ||
                            f.inst(add.operands[1]).imm_int != 1) {
                            valid = false;
                            break;
                        }
                        increment_pos = pos;
                    } else {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    break;
                }
            }
            if (!valid || init_block == InvalidBlock || push_pos >= increment_pos ||
                reaches_without(f, f.entry, guard.id, init_block) ||
                reaches_without(f, f.entry, push_block, guard.id)) {
                continue;
            }
            filled[slot] = Filled{limit, branch.target1};
            break;
        }
    }

    // Direct counter reads in the true successor of a bounds guard.
    for (const Block &guard : f.blocks) {
        if (guard.terminator == InvalidValue) {
            continue;
        }
        const Inst &branch = f.inst(guard.terminator);
        if (branch.op != Op::Branch || branch.operands.size() != 1 || branch.target0 < 0 ||
            static_cast<size_t>(branch.target0) >= f.blocks.size()) {
            continue;
        }
        const Inst &cmp = f.inst(branch.operands[0]);
        if (cmp.op != Op::DynCmpLt || cmp.operands.size() != 2 || f.inst(cmp.operands[0]).op != Op::LoadSlot ||
            f.inst(cmp.operands[1]).op != Op::IConst) {
            continue;
        }
        const uint32_t counter = f.inst(cmp.operands[0]).imm_u32;
        const int64_t limit = f.inst(cmp.operands[1]).imm_int;
        if (!private_slot(counter) || limit <= 0 || writes[counter].count != 2 ||
            reaches_without(f, f.entry, branch.target0, guard.id) ||
            reaches_without(f, branch.target1, branch.target0, guard.id)) {
            continue;
        }
        const Block &body = f.blocks[branch.target0];
        BlockId init_block = InvalidBlock;
        size_t increment_pos = body.insts.size();
        int64_t stride = 0;
        bool valid = true;
        for (const Block &b : f.blocks) {
            for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                const Inst &in = f.inst(b.insts[pos]);
                if (in.imm_u32 != counter ||
                    (in.op != Op::StoreSlot && in.op != Op::StoreImmSlot && in.op != Op::StoreBImmSlot &&
                     in.op != Op::StoreNSlot && in.op != Op::StoreCSlot && in.op != Op::CopySlot)) {
                    continue;
                }
                if (in.op == Op::StoreImmSlot && in.imm_int == 0 && init_block == InvalidBlock && b.id != body.id) {
                    init_block = b.id;
                } else if (in.op == Op::StoreSlot && in.operands.size() == 1 && b.id == body.id &&
                           increment_pos == body.insts.size()) {
                    const Inst &add = f.inst(in.operands[0]);
                    if (add.op != Op::DynAdd || add.operands.size() != 2 ||
                        !from_slot(add.operands[0], counter) || f.inst(add.operands[1]).op != Op::IConst) {
                        valid = false;
                        break;
                    }
                    stride = f.inst(add.operands[1]).imm_int;
                    increment_pos = pos;
                } else {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                break;
            }
        }
        if (!valid || init_block == InvalidBlock || increment_pos == body.insts.size() || stride <= 0 ||
            limit % stride != 0 || reaches_without(f, f.entry, guard.id, init_block)) {
            continue;
        }
        bool rechecks = true;
        for (BlockId next : body.succs) {
            rechecks &= !reaches_without(f, next, body.id, guard.id);
        }
        if (!rechecks) {
            continue;
        }
        for (size_t pos = 0; pos < increment_pos; ++pos) {
            Inst &read = f.inst(body.insts[pos]);
            if (read.op != Op::LoadIndex || read.operands.size() != 2) {
                continue;
            }
            const Inst &recv = f.inst(read.operands[0]);
            if (recv.op != Op::LoadSlot || recv.imm_u32 >= filled.size() || !filled[recv.imm_u32] ||
                filled[recv.imm_u32]->min_length < limit ||
                reaches_without(f, f.entry, body.id, filled[recv.imm_u32]->after)) {
                continue;
            }
            int64_t offset = 0;
            if (!from_slot(read.operands[1], counter)) {
                const Inst &key = f.inst(read.operands[1]);
                if (key.op != Op::DynAdd || key.operands.size() != 2 ||
                    !from_slot(key.operands[0], counter) || f.inst(key.operands[1]).op != Op::IConst) {
                    continue;
                }
                offset = f.inst(key.operands[1]).imm_int;
            }
            if (offset >= 0 && offset < stride) {
                read.proven_inbounds = true;
            }
        }
    }
    for (const Block &b : f.blocks) {
        for (ValueId id : b.insts) {
            Inst &store = f.inst(id);
            if (store.op != Op::StoreIndex || store.operands.empty()) {
                continue;
            }
            const Inst &receiver = f.inst(store.operands[0]);
            if (receiver.op == Op::LoadSlot && receiver.imm_u32 < filled.size() && filled[receiver.imm_u32] &&
                !reaches_without(f, f.entry, b.id, filled[receiver.imm_u32]->after)) {
                store.proven_array_receiver = true;
            }
        }
    }
    // A row-major index is in bounds when both coordinates have explicit
    // nonnegative and upper-bound guards that every path to the read crosses.
    // Require single-write coordinate slots so the checked values cannot be
    // changed between those branches and the load.
    struct CoordBounds {
        bool nonnegative = false;
        std::optional<int64_t> upper;
    };
    auto nonnegative_counter = [&](uint32_t slot, BlockId read_block) {
        if (!private_slot(slot) || writes[slot].count != 2) {
            return false;
        }
        BlockId initializer = InvalidBlock;
        BlockId increment = InvalidBlock;
        for (const Block &block : f.blocks) {
            for (ValueId id : block.insts) {
                const Inst &store = f.inst(id);
                if (store.imm_u32 != slot ||
                    (store.op != Op::StoreSlot && store.op != Op::StoreImmSlot && store.op != Op::StoreBImmSlot &&
                     store.op != Op::StoreNSlot && store.op != Op::StoreCSlot && store.op != Op::CopySlot)) {
                    continue;
                }
                if (store.op == Op::StoreImmSlot && store.imm_int == 0 && initializer == InvalidBlock) {
                    initializer = block.id;
                    continue;
                }
                if (store.op != Op::StoreSlot || store.operands.size() != 1 || increment != InvalidBlock) {
                    return false;
                }
                const Inst &add = f.inst(store.operands[0]);
                if (add.op != Op::DynAdd || add.operands.size() != 2 || !from_slot(add.operands[0], slot) ||
                    f.inst(add.operands[1]).op != Op::IConst || f.inst(add.operands[1]).imm_int <= 0 ||
                    !add.proven_int48) {
                    return false;
                }
                increment = block.id;
            }
        }
        return initializer != InvalidBlock && increment != InvalidBlock && initializer != increment &&
               !reaches_without(f, f.entry, increment, initializer) &&
               !reaches_without(f, f.entry, read_block, initializer);
    };
    for (const Block &b : f.blocks) {
        for (size_t read_pos = 0; read_pos < b.insts.size(); ++read_pos) {
            ValueId id = b.insts[read_pos];
            Inst &read = f.inst(id);
            if (read.op != Op::LoadIndex || read.operands.size() != 2) {
                continue;
            }
            const Inst &receiver = f.inst(read.operands[0]);
            if (receiver.op != Op::LoadSlot || receiver.imm_u32 >= filled.size() || !filled[receiver.imm_u32] ||
                reaches_without(f, f.entry, b.id, filled[receiver.imm_u32]->after)) {
                continue;
            }
            const Inst &index = f.inst(read.operands[1]);
            if ((index.op != Op::DynAdd && index.op != Op::IAdd) || index.operands.size() != 2) {
                continue;
            }
            const Inst &product = f.inst(index.operands[0]);
            const Inst &column = f.inst(index.operands[1]);
            if ((product.op != Op::DynMul && product.op != Op::IMul) || product.operands.size() != 2 ||
                column.op != Op::LoadSlot) {
                continue;
            }
            const Inst &row = f.inst(product.operands[0]);
            const Inst &stride_inst = f.inst(product.operands[1]);
            if (row.op != Op::LoadSlot || stride_inst.op != Op::IConst || stride_inst.imm_int <= 0 ||
                row.type != Ty::Int48 || column.type != Ty::Int48 ||
                !private_slot(row.imm_u32) || !private_slot(column.imm_u32) ||
                (writes[row.imm_u32].count != 1 && !nonnegative_counter(row.imm_u32, b.id)) ||
                (writes[column.imm_u32].count != 1 && !nonnegative_counter(column.imm_u32, b.id))) {
                continue;
            }
            const int64_t stride = stride_inst.imm_int;
            CoordBounds row_bounds, col_bounds;
            if (writes[row.imm_u32].count == 2) {
                row_bounds.nonnegative = true;
            }
            if (writes[column.imm_u32].count == 2) {
                col_bounds.nonnegative = true;
            }
            for (const Block &guard : f.blocks) {
                if (guard.terminator == InvalidValue) {
                    continue;
                }
                const Inst &branch = f.inst(guard.terminator);
                if (branch.op != Op::Branch || branch.operands.size() != 1 || branch.target0 == InvalidBlock ||
                    branch.target1 == InvalidBlock ||
                    reaches_without(f, f.entry, b.id, branch.target0) ||
                    reaches_without(f, branch.target1, b.id, guard.id)) {
                    continue;
                }
                const Inst *cmp = &f.inst(branch.operands[0]);
                bool inverted = false;
                for (unsigned depth = 0; depth < 8 && cmp->op == Op::Not && cmp->operands.size() == 1; ++depth) {
                    cmp = &f.inst(cmp->operands[0]);
                    inverted = !inverted;
                }
                if (cmp->operands.size() != 2 || f.inst(cmp->operands[0]).op != Op::LoadSlot ||
                    f.inst(cmp->operands[1]).op != Op::IConst) {
                    continue;
                }
                const uint32_t checked_slot = f.inst(cmp->operands[0]).imm_u32;
                CoordBounds *bounds = checked_slot == row.imm_u32 ? &row_bounds :
                                      checked_slot == column.imm_u32 ? &col_bounds : nullptr;
                if (!bounds) {
                    continue;
                }
                size_t comparison_pos = 0;
                for (size_t pos = 0; pos < guard.insts.size(); ++pos) {
                    if (&f.inst(guard.insts[pos]) == cmp) {
                        comparison_pos = pos;
                        break;
                    }
                }
                bool changed_after_check = false;
                for (const Block &write_block : f.blocks) {
                    for (size_t pos = 0; pos < write_block.insts.size(); ++pos) {
                        const Inst &write = f.inst(write_block.insts[pos]);
                        if (write.imm_u32 != checked_slot ||
                            (write.op != Op::StoreSlot && write.op != Op::StoreImmSlot &&
                             write.op != Op::StoreBImmSlot && write.op != Op::StoreNSlot &&
                             write.op != Op::StoreCSlot && write.op != Op::CopySlot)) {
                            continue;
                        }
                        if (write_block.id == guard.id) {
                            changed_after_check |= pos >= comparison_pos;
                        } else if (write_block.id == b.id) {
                            changed_after_check |= pos < read_pos;
                        } else if (reaches_without(f, branch.target0, write_block.id, guard.id) &&
                                   reaches_without(f, write_block.id, b.id, guard.id)) {
                            changed_after_check = true;
                        }
                    }
                }
                if (changed_after_check) {
                    continue;
                }
                const int64_t limit = f.inst(cmp->operands[1]).imm_int;
                const bool ge = cmp->op == Op::DynCmpGe || cmp->op == Op::ICmpGe;
                const bool lt = cmp->op == Op::DynCmpLt || cmp->op == Op::ICmpLt;
                if (((!inverted && ge) || (inverted && lt)) && limit == 0) {
                    bounds->nonnegative = true;
                } else if (((!inverted && lt) || (inverted && ge)) && limit > 0) {
                    if (!bounds->upper || limit < *bounds->upper) {
                        bounds->upper = limit;
                    }
                }
            }
            if (row_bounds.nonnegative && col_bounds.nonnegative && row_bounds.upper && col_bounds.upper &&
                *col_bounds.upper <= stride &&
                *row_bounds.upper <= filled[receiver.imm_u32]->min_length / stride) {
                read.proven_inbounds = true;
            }
        }
    }
    IntArraySlots result;
    for (uint32_t slot = 0; slot < filled.size(); ++slot) {
        if (filled[slot]) {
            result.insert(slot);
        }
    }
    return result;
}

// Prove a small integer accumulator when every added element is in [-1, 1]
// and exhaustive control-flow exploration finds at most 32 additions between
// resets. The exploration tracks only simple constant-start, unit-step loop
// counters; all other branches are treated as nondeterministic. This keeps the
// proof valid even when the loop body contains data-dependent branches.
void mark_bounded_binary_accumulators(Func &f, const IntArraySlots &filled_array_slots, uint32_t push_method_name_idx) {
    if (filled_array_slots.empty() || f.entry == InvalidBlock || f.num_slots > 256 ||
        f.blocks.size() > 512 || f.insts.size() > 4096) {
        return;
    }
    auto is_write = [](Op op) {
        return op == Op::StoreSlot || op == Op::StoreImmSlot || op == Op::StoreBImmSlot ||
               op == Op::StoreNSlot || op == Op::StoreCSlot || op == Op::CopySlot;
    };
    auto from_slot = [&](ValueId id, uint32_t slot) {
        if (id < 0 || static_cast<size_t>(id) >= f.insts.size()) return false;
        const Inst *in = &f.inst(id);
        if (in->op == Op::Dup && in->operands.size() == 1) in = &f.inst(in->operands[0]);
        return in->op == Op::LoadSlot && in->imm_u32 == slot;
    };
    struct Write { BlockId block; size_t pos; ValueId id; };
    std::vector<std::vector<Write>> writes(f.num_slots);
    for (const Block &b : f.blocks) {
        for (size_t pos = 0; pos < b.insts.size(); ++pos) {
            const Inst &in = f.inst(b.insts[pos]);
            if (is_write(in.op) && in.imm_u32 < writes.size()) writes[in.imm_u32].push_back({b.id, pos, b.insts[pos]});
        }
    }
    auto private_slot = [&](uint32_t slot) {
        return slot < f.num_slots && slot >= f.num_params &&
               (slot >= f.captured_local_slots.size() || !f.captured_local_slots[slot]);
    };
    std::vector<bool> binary_scalar(f.num_slots, false);
    for (uint32_t slot = 0; slot < f.num_slots; ++slot) {
        if (!private_slot(slot) || writes[slot].empty()) continue;
        bool valid = true;
        for (const Write &w : writes[slot]) {
            const Inst &store = f.inst(w.id);
            if (store.op == Op::StoreImmSlot) {
                valid &= store.imm_int >= -1 && store.imm_int <= 1;
            } else if (store.op == Op::StoreSlot && store.operands.size() == 1) {
                const Inst &value = f.inst(store.operands[0]);
                valid &= value.op == Op::IConst && value.imm_int >= -1 && value.imm_int <= 1;
            } else {
                valid = false;
            }
        }
        if (!valid) continue;
        bool initialized = false;
        for (const Write &w : writes[slot]) {
            bool dominates = true;
            for (const Block &b : f.blocks) {
                for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                    const Inst &in = f.inst(b.insts[pos]);
                    if (in.op == Op::LoadSlot && in.imm_u32 == slot &&
                        ((b.id == w.block && pos <= w.pos) ||
                         (b.id != w.block && reaches_without(f, f.entry, b.id, w.block)))) {
                        dominates = false;
                    }
                }
            }
            if (dominates) { initialized = true; break; }
        }
        binary_scalar[slot] = initialized;
    }
    IntArraySlots binary_arrays = filled_array_slots;
    auto binary_value = [&](ValueId id, const IntArraySlots &arrays) {
        if (id < 0 || static_cast<size_t>(id) >= f.insts.size()) return false;
        const Inst *in = &f.inst(id);
        if (in->op == Op::Dup && in->operands.size() == 1) in = &f.inst(in->operands[0]);
        if (in->op == Op::IConst) return in->imm_int >= -1 && in->imm_int <= 1;
        if (in->op == Op::LoadSlot) return in->imm_u32 < binary_scalar.size() && binary_scalar[in->imm_u32];
        if ((in->op == Op::IMod || in->op == Op::DynMod) && in->operands.size() == 2 &&
            f.inst(in->operands[0]).type == Ty::Int48) {
            const Inst &divisor = f.inst(in->operands[1]);
            return divisor.op == Op::IConst && divisor.imm_int == 2;
        }
        if (in->op == Op::LoadIndex && in->type == Ty::Int48 && in->proven_inbounds && in->operands.size() == 2) {
            const Inst &receiver = f.inst(in->operands[0]);
            return receiver.op == Op::LoadSlot && arrays.count(receiver.imm_u32) != 0;
        }
        return false;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = binary_arrays.begin(); it != binary_arrays.end();) {
            const uint32_t slot = *it;
            bool valid = true;
            for (const Block &b : f.blocks) {
                for (ValueId id : b.insts) {
                    const Inst &in = f.inst(id);
                    if (in.operands.empty() || !from_slot(in.operands[0], slot)) continue;
                    if (in.op == Op::StoreIndex) {
                        valid &= in.operands.size() == 3 && binary_value(in.operands[2], binary_arrays);
                    } else if (in.op == Op::CallMethod && in.imm_u32 == push_method_name_idx && in.imm_int == 1) {
                        valid &= in.operands.size() == 2 && binary_value(in.operands[1], binary_arrays);
                    }
                }
            }
            if (!valid) { it = binary_arrays.erase(it); changed = true; }
            else ++it;
        }
    }
    if (binary_arrays.empty()) return;

    struct Counter { uint32_t slot; BlockId init; BlockId guard; BlockId body; };
    std::vector<Counter> counters;
    for (uint32_t slot = 0; slot < f.num_slots; ++slot) {
        if (!private_slot(slot) || writes[slot].size() != 2) continue;
        BlockId init = InvalidBlock, increment = InvalidBlock;
        int64_t start = 0;
        for (const Write &w : writes[slot]) {
            const Inst &store = f.inst(w.id);
            if (store.op == Op::StoreImmSlot && store.imm_int >= -8 && store.imm_int <= 8) {
                init = w.block;
                start = store.imm_int;
            } else if (store.op == Op::StoreSlot && store.operands.size() == 1) {
                const Inst &add = f.inst(store.operands[0]);
                if (add.op == Op::DynAdd && add.operands.size() == 2 &&
                    from_slot(add.operands[0], slot) && f.inst(add.operands[1]).op == Op::IConst &&
                    f.inst(add.operands[1]).imm_int == 1 && add.proven_int48) increment = w.block;
            }
        }
        if (init == InvalidBlock || increment == InvalidBlock || init == increment) continue;
        for (const Block &b : f.blocks) {
            if (b.terminator == InvalidValue) continue;
            const Inst &branch = f.inst(b.terminator);
            if (branch.op != Op::Branch || branch.operands.size() != 1 || branch.target0 == InvalidBlock ||
                branch.target1 == InvalidBlock) continue;
            const Inst &cmp = f.inst(branch.operands[0]);
            if ((cmp.op != Op::DynCmpLe && cmp.op != Op::DynCmpLt) || cmp.operands.size() != 2 ||
                !from_slot(cmp.operands[0], slot) || f.inst(cmp.operands[1]).op != Op::IConst) continue;
            const int64_t limit = f.inst(cmp.operands[1]).imm_int;
            if (b.id == init || b.id == increment || limit < start || limit > 8 || limit - start > 16 ||
                reaches_without(f, f.entry, b.id, init) ||
                reaches_without(f, branch.target0, b.id, increment)) continue;
            counters.push_back({slot, init, b.id, branch.target0});
            break;
        }
    }
    if (counters.size() < 2) return;

    for (uint32_t slot = 0; slot < f.num_slots; ++slot) {
        if (!private_slot(slot) || writes[slot].size() != 2) continue;
        const Write *reset = nullptr, *update = nullptr;
        Inst *add = nullptr;
        for (const Write &w : writes[slot]) {
            const Inst &store = f.inst(w.id);
            if (store.op == Op::StoreImmSlot && store.imm_int == 0) reset = &w;
            else if (store.op == Op::StoreSlot && store.operands.size() == 1) {
                Inst &candidate = f.inst(store.operands[0]);
                if (candidate.op == Op::DynAdd && candidate.operands.size() == 2 &&
                    from_slot(candidate.operands[0], slot) && binary_value(candidate.operands[1], binary_arrays)) {
                    update = &w;
                    add = &candidate;
                }
            }
        }
        if (!reset || !update || !add || reset->block == update->block ||
            reaches_without(f, f.entry, update->block, reset->block)) continue;
        // All reads of the accumulator must follow its reset.
        bool initialized = true;
        for (const Block &b : f.blocks) {
            for (size_t pos = 0; pos < b.insts.size(); ++pos) {
                const Inst &in = f.inst(b.insts[pos]);
                if (in.op == Op::LoadSlot && in.imm_u32 == slot &&
                    ((b.id == reset->block && pos <= reset->pos) ||
                     (b.id != reset->block && reaches_without(f, f.entry, b.id, reset->block)))) initialized = false;
            }
        }
        if (!initialized) continue;
        for (const Counter &outer : counters) {
            if (outer.init != reset->block || reaches_without(f, f.entry, update->block, outer.body)) continue;
            for (const Counter &inner : counters) {
                if (inner.slot == outer.slot || inner.init != outer.body ||
                    reaches_without(f, f.entry, update->block, inner.body)) continue;
                // The inner body cannot restart its counter, or reach the
                // outer guard, without passing through the outer increment.
                BlockId outer_inc = InvalidBlock;
                for (const Write &w : writes[outer.slot]) if (w.block != outer.init) outer_inc = w.block;
                if (outer_inc == InvalidBlock ||
                    reaches_without(f, inner.body, inner.init, outer_inc) ||
                    reaches_without(f, inner.body, outer.guard, outer_inc)) continue;

                struct State { BlockId block; int outer; int inner; int additions; bool first; };
                std::vector<State> pending{{reset->block, 100, 100, 0, true}};
                std::set<std::tuple<BlockId, int, int, int, bool>> seen;
                bool safe = true;
                while (!pending.empty() && safe) {
                    State s = pending.back(); pending.pop_back();
                    if (s.block == reset->block && !s.first) continue;
                    if (s.block < 0 || static_cast<size_t>(s.block) >= f.blocks.size() ||
                        !seen.emplace(s.block, s.outer, s.inner, s.additions, s.first).second) continue;
                    if (seen.size() > 20000) { safe = false; break; }
                    const Block &b = f.blocks[s.block];
                    for (ValueId id : b.insts) {
                        const Inst &in = f.inst(id);
                        if (in.op == Op::StoreImmSlot) {
                            if (in.imm_u32 == outer.slot) s.outer = static_cast<int>(in.imm_int);
                            if (in.imm_u32 == inner.slot) s.inner = static_cast<int>(in.imm_int);
                        } else if (in.op == Op::StoreSlot && in.operands.size() == 1) {
                            if (in.imm_u32 == outer.slot) ++s.outer;
                            if (in.imm_u32 == inner.slot) ++s.inner;
                        }
                        if (id == update->id && ++s.additions > 32) { safe = false; break; }
                    }
                    if (!safe) break;
                    if (s.outer < -16 || s.outer > 100 || s.inner < -16 || s.inner > 100) { safe = false; break; }
                    if (b.terminator == InvalidValue) continue;
                    const Inst &term = f.inst(b.terminator);
                    if (term.op == Op::Jump) {
                        pending.push_back({term.target0, s.outer, s.inner, s.additions, false});
                    } else if (term.op == Op::Branch) {
                        int decision = -1;
                        if (s.block == outer.guard || s.block == inner.guard) {
                            const Inst &cmp = f.inst(term.operands[0]);
                            const int value = s.block == outer.guard ? s.outer : s.inner;
                            const int limit = static_cast<int>(f.inst(cmp.operands[1]).imm_int);
                            if (value == 100) { safe = false; break; }
                            decision = cmp.op == Op::DynCmpLe ? value <= limit : value < limit;
                        }
                        if (decision != 0) pending.push_back({term.target0, s.outer, s.inner, s.additions, false});
                        if (decision != 1) pending.push_back({term.target1, s.outer, s.inner, s.additions, false});
                    } else if (term.op != Op::Return) {
                        safe = false;
                    }
                }
                if (safe) { add->proven_int48 = true; break; }
            }
            if (add->proven_int48) break;
        }
    }
}

bool is_const(Op op) {
    return op == Op::IConst || op == Op::FConst || op == Op::BConst || op == Op::NConst;
}

bool is_pure_stack_value(Op op) {
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

bool is_foldable_binary(Op op) {
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

bool is_nonfolding_binary(Op op) {
    switch (op) {
        case Op::IAnd:
        case Op::IOr:
        case Op::IXor:
        case Op::IShl:
        case Op::IShr:
        case Op::JsBitBinary:
        case Op::StrConcat:
        case Op::Pow:
            return true;
        default:
            return false;
    }
}

bool numeric_const(const Inst &in) {
    return in.op == Op::IConst || in.op == Op::FConst;
}

double const_number(const Inst &in) {
    return in.op == Op::IConst ? static_cast<double>(in.imm_int) : in.imm_float;
}

bool truthy_const(const Inst &in) {
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

Inst make_int_const(int64_t v, size_t pc) {
    Inst out;
    out.op = Op::IConst;
    out.type = Ty::Int48;
    out.imm_int = v;
    out.bytecode_pc = pc;
    return out;
}

Inst make_float_const(double v, size_t pc) {
    Inst out;
    out.op = Op::FConst;
    out.type = Ty::Float;
    out.imm_float = v;
    out.bytecode_pc = pc;
    return out;
}

Inst make_bool_const(bool v, size_t pc) {
    Inst out;
    out.op = Op::BConst;
    out.type = Ty::Bool;
    out.imm_int = v ? 1 : 0;
    out.bytecode_pc = pc;
    return out;
}

// `overflowed` = exact integer result didn't fit int64 (so certainly not int48)
Inst number_result_checked(bool overflowed, int64_t v, double fallback, size_t pc) {
    if (!overflowed && fits_int48_i64(v)) {
        return make_int_const(v, pc);
    }
    return make_float_const(fallback, pc);
}

bool fold_binary_const(Op op, const Inst &lhs, const Inst &rhs, size_t pc, Inst &out) {
    switch (op) {
        case Op::DynAdd:
            if (lhs.op == Op::IConst && rhs.op == Op::IConst) {
                int64_t r;
                bool ovf = add_overflow_i64(lhs.imm_int, rhs.imm_int, &r);
                out = number_result_checked(ovf, r, static_cast<double>(lhs.imm_int) + static_cast<double>(rhs.imm_int), pc);
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
                out = number_result_checked(ovf, r, static_cast<double>(lhs.imm_int) - static_cast<double>(rhs.imm_int), pc);
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
                    out = make_float_const(static_cast<double>(lhs.imm_int) * static_cast<double>(rhs.imm_int), pc);
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
                } else if (lhs.op == Op::IConst && rhs.op == Op::IConst && lhs.imm_int % rhs.imm_int == 0) {
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

bool simplify_block(Func &f, Block &b) {
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
            case Op::MakeClosure:
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
            case Op::StrAppendSlot:
                if (stack.empty()) {
                    body.push_back(v);
                    break;
                }
                stack.pop_back();
                push_existing(v, false);
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
            case Op::JsTruthy:
            case Op::IsNone:
            case Op::INeg:
            case Op::INot:
            case Op::JsBitNot:
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
                size_t n = static_cast<size_t>(in.imm_u32) + 1;
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
            case Op::CallSpread:
                if (stack.size() < 2) {
                    body.push_back(v);
                    stack.clear();
                    break;
                }
                stack.pop_back();
                stack.pop_back();
                push_existing(v, false);
                break;
            case Op::CallMethod: {
                size_t n = static_cast<size_t>(in.imm_int) + 1;
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
            case Op::ArrayPush:
            case Op::ArraySpread:
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
                size_t n = in.op == Op::MakeArray ? static_cast<size_t>(in.imm_u32) : static_cast<size_t>(in.imm_u32) * 2;
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
                    bool operands_are_trailing =
                        body.size() >= 2 && body[body.size() - 2] == lhs.value && body[body.size() - 1] == rhs.value;
                    if (is_foldable_binary(in.op) && lhs.removable && rhs.removable && operands_are_trailing &&
                        fold_binary_const(in.op, f.inst(lhs.value), f.inst(rhs.value), in.bytecode_pc, folded)) {
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

bool eliminate_slot_round_trips(Func &f, Block &b) {
    bool changed = false;
    std::vector<ValueId> body;
    std::unordered_map<ValueId, ValueId> remap;
    body.reserve(b.insts.size());

    for (size_t i = 0; i < b.insts.size();) {
        if (i + 2 < b.insts.size()) {
            const Inst &a = f.inst(b.insts[i]);
            const Inst &mid = f.inst(b.insts[i + 1]);
            const Inst &c = f.inst(b.insts[i + 2]);

            // StoreSlot leaves the stored value on the VM stack. A following
            // Pop + LoadSlot of the same slot can be dropped and the original
            // stored value remains on top of the stack for the consumer.
            if (a.op == Op::StoreSlot && mid.op == Op::Pop && c.op == Op::LoadSlot && a.imm_u32 == c.imm_u32) {
                body.push_back(b.insts[i]);
                remap[b.insts[i + 2]] = a.operands[0];
                i += 3;
                changed = true;
                continue;
            }

            // `x = x` used as a statement lowers to LoadSlot, StoreSlot, Pop.
            // The slot and final stack are unchanged, so all three helpers are
            // dead. Keep this exact and local to avoid crossing side effects.
            if (a.op == Op::LoadSlot && mid.op == Op::StoreSlot && c.op == Op::Pop && a.imm_u32 == mid.imm_u32) {
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
        for (Inst &in : f.insts) {
            for (ValueId &operand : in.operands) {
                auto it = remap.find(operand);
                if (it != remap.end()) {
                    operand = it->second;
                }
            }
        }
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
//   LoadSlot v @Y; StoreSlot v @X; Pop v ->  CopySlot @Y -> @X   (X != Y, X == Y is eliminated by eliminate_slot_round_trips)
bool slot_aliased(const Func &f, uint32_t slot) {
    return slot < f.captured_local_slots.size() && f.captured_local_slots[slot] != 0;
}

bool fuse_slot_stores(Func &f, Block &b) {
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

            bool prod_is_const =
                (a.op == Op::IConst || a.op == Op::BConst || a.op == Op::NConst || a.op == Op::LoadConst || a.op == Op::LoadSlot);

            // the fused forms (StoreImmSlot/StoreCSlot/CopySlot/...) write raw slot
            // memory and would silently skip the cell write-through
            if (prod_is_const && mid.op == Op::StoreSlot && c.op == Op::Pop && !mid.operands.empty() && mid.operands[0] == prod_id &&
                !c.operands.empty() && c.operands[0] == prod_id && !slot_aliased(f, mid.imm_u32) && no_other_users(prod_id, i + 3)) {
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
                        fused.imm_int = static_cast<int64_t>(prod_imm_u32); // const pool idx
                        break;
                    case Op::LoadSlot:
                        // Skip the copy-to-self case: eliminate_slot_round_trips
                        // handles LoadSlot @X; StoreSlot @X; Pop as a no-op.
                        if (prod_imm_u32 == dst_slot) {
                            goto no_fuse;
                        }
                        // CopySlot reads the source slot raw, bypassing its cell
                        if (slot_aliased(f, prod_imm_u32)) {
                            goto no_fuse;
                        }
                        fused.op = Op::CopySlot;
                        fused.imm_int = static_cast<int64_t>(prod_imm_u32); // src slot
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

bool fold_branch(Func &f, Block &b) {
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

BlockId jump_thread_target(const Func &f, BlockId target) {
    BlockId cur = target;
    for (size_t depth = 0; depth < f.blocks.size(); depth++) {
        if (cur < 0 || static_cast<size_t>(cur) >= f.blocks.size()) {
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

bool thread_jumps(Func &f) {
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
bool propagate_slot_consts(Func &f, Block &b) {
    bool changed = false;
    std::unordered_map<uint32_t, ValueId> known; // slot -> const-valued inst id
    for (ValueId v : b.insts) {
        Inst &in = f.inst(v);
        if (in.op == Op::LoadSlot) {
            if (slot_aliased(f, in.imm_u32)) {
                continue; // a call may have rewritten the cell; the load must stand
            }
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
            if (sv != InvalidValue && is_const(f.inst(sv).op) && !slot_aliased(f, in.imm_u32)) {
                known[in.imm_u32] = sv;
            } else {
                known.erase(in.imm_u32);
            }
        }
        // No other op modifies a slot; LoadSlot reads only.
    }
    return changed;
}

// Dead store elimination: a StoreSlot to a slot that is never read (no LoadSlot anywhere)
//  and is not captured by a closure is unobservable, since a non-aliased slot is purely function-local.
//
// StoreSlot is stack-neutral (it peeks), so dropping it preserves the value stack, the peeked value stays on the stack.
// Composes with const-prop and DCE (which then removes the now-unused value + its Pop).
bool eliminate_dead_stores(Func &f) {
    std::vector<bool> loaded(f.num_slots, false);
    // An aliased slot's store also writes its upvalue cell, which a closure can read
    // even if this function never loads the slot again, so it is never unobservable.
    for (uint32_t sl = 0; sl < f.num_slots; sl++) {
        if (slot_aliased(f, sl)) {
            loaded[sl] = true;
        }
    }
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

void rebuild_preds(Func &f);

// Local value numbering (within-block CSE). Simulates the block's operand stack,
// giving each value an exact integer value-number.
// When a freshly pushed value equals the entry directly below it on the stack (i.e.
// a sub-expression was recomputed immediately after an identical one), its whole
// span is replaced by a single Dup of that lower value.
bool local_value_numbering(Func &f, Block &b) {
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
                push_leaf(v, getvn("ld:" + std::to_string(in.imm_u32) + ":" + std::to_string(slot_ver(in.imm_u32))));
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
            case Op::StrAppendSlot:
                slot_version[in.imm_u32] = slot_ver(in.imm_u32) + 1;
                vnmap.clear();
                body.push_back(v);
                if (st.empty()) {
                    st.clear();
                } else {
                    E rhs = st.back();
                    st.pop_back();
                    in.operands = { rhs.vid };
                    st.push_back({ getvn("strapp:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
                }
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
            case Op::JsTruthy:
            case Op::IsNone:
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
                int vn = getvn(std::to_string(static_cast<int>(in.op)) + ":" + std::to_string(a.vn));
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
            case Op::DynCmpNe:
            case Op::DynStrictCmpEq:
            case Op::DynStrictCmpNe: {
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
                if ((in.op == Op::DynAdd || in.op == Op::DynMul || in.op == Op::DynStrictCmpEq || in.op == Op::DynStrictCmpNe) && o1 > o2) {
                    std::swap(o1, o2); // commutative: normalize operand order
                }
                int vn = getvn(std::to_string(static_cast<int>(in.op)) + ":" + std::to_string(o1) + ":" + std::to_string(o2));
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
            case Op::Pow:
            case Op::IAnd:
            case Op::IOr:
            case Op::IXor:
            case Op::IShl:
            case Op::IShr:
            case Op::JsBitBinary: {
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
                int vn = getvn(std::to_string(static_cast<int>(in.op)) + ":" + std::to_string(o1) + ":" + std::to_string(o2));
                body.push_back(v);
                st.push_back({ vn, v, lhs.body_pos });
                break;
            }
            case Op::Call: {
                size_t n = static_cast<size_t>(in.imm_u32) + 1;
                body.push_back(v);
                if (st.size() < n) {
                    st.clear();
                } else {
                    for (size_t i = 0; i < n; i++) {
                        st.pop_back();
                    }
                    st.push_back({ getvn("call:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
                }
                break;
            }
            case Op::CallSpread:
                body.push_back(v);
                if (st.size() < 2) {
                    st.clear();
                } else {
                    E args = st.back();
                    st.pop_back();
                    E callee = st.back();
                    st.pop_back();
                    in.operands = { callee.vid, args.vid };
                    vnmap.clear();
                    st.push_back({ getvn("calls:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
                }
                break;
            case Op::CallMethod: {
                size_t n = static_cast<size_t>(in.imm_int) + 1;
                body.push_back(v);
                if (st.size() < n) {
                    st.clear();
                } else {
                    for (size_t i = 0; i < n; i++) {
                        st.pop_back();
                    }
                    st.push_back({ getvn("callm:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
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
                    st.push_back({ getvn("idx:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
                }
                break;
            case Op::ArrayPush:
            case Op::ArraySpread:
                body.push_back(v);
                if (st.size() < 2) {
                    st.clear();
                } else {
                    E iterable = st.back();
                    st.pop_back();
                    E target = st.back();
                    st.pop_back();
                    in.operands = { target.vid, iterable.vid };
                    vnmap.clear();
                    st.push_back({ getvn("arraymut:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
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
                size_t n = in.op == Op::MakeArray ? static_cast<size_t>(in.imm_u32) : static_cast<size_t>(in.imm_u32) * 2;
                body.push_back(v);
                if (st.size() < n) {
                    st.clear();
                } else {
                    for (size_t i = 0; i < n; i++) {
                        st.pop_back();
                    }
                    st.push_back({ getvn("agg:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
                }
                break;
            }
            case Op::MakeClosure:
                body.push_back(v);
                st.push_back({ getvn("closure:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
                break;
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
                    st.push_back({ getvn("iter:" + std::to_string(static_cast<int>(v))), v, body.size() - 1 });
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
bool thread_phi_branches(Func &f) {
    bool any = false;
    for (size_t guard = 0; guard <= f.blocks.size(); guard++) {
        rebuild_preds(f);
        bool changed = false;
        for (size_t bi = 0; bi < f.blocks.size(); bi++) {
            if (static_cast<BlockId>(bi) == f.entry) {
                continue;
            }
            Block &B = f.blocks[bi];
            if (B.preds.empty() || !B.insts.empty() || B.phis.size() != 1 || B.terminator == InvalidValue) {
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
            if (T < 0 || F < 0 || static_cast<size_t>(T) >= f.blocks.size() || static_cast<size_t>(F) >= f.blocks.size() ||
                T == static_cast<BlockId>(bi) || F == static_cast<BlockId>(bi)) {
                continue;
            }
            if (!f.blocks[T].phis.empty() || !f.blocks[F].phis.empty()) {
                continue; // targets would gain preds needing phi fixup
            }
            // Every predecessor must reach B via an unconditional Jump.
            bool preds_ok = true;
            for (BlockId p : B.preds) {
                if (p < 0 || static_cast<size_t>(p) >= f.blocks.size()) {
                    preds_ok = false;
                    break;
                }
                const Block &P = f.blocks[p];
                if (P.terminator == InvalidValue) {
                    preds_ok = false;
                    break;
                }
                const Inst &pt = f.inst(P.terminator);
                if (pt.op != Op::Jump || pt.target0 != static_cast<BlockId>(bi)) {
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
bool merge_blocks(Func &f) {
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
        if (bid < 0 || static_cast<size_t>(bid) >= f.blocks.size() || bid == static_cast<BlockId>(ai) || bid == f.entry || removed[bid]) {
            continue;
        }
        Block &B = f.blocks[bid];
        if (B.preds.size() != 1 || B.preds[0] != static_cast<BlockId>(ai) || !B.phis.empty()) {
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
// Compacts f.blocks and remaps every BlockId reference: Func::entry, each Block::id/succs/preds, and Jump/Branch
// terminator targets.
bool remove_unreachable_blocks(Func &f) {
    if (f.blocks.empty() || f.entry < 0 || static_cast<size_t>(f.entry) >= f.blocks.size()) {
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
            if (s >= 0 && static_cast<size_t>(s) < reach.size() && !reach[s]) {
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
            if (s >= 0 && static_cast<size_t>(s) < f.blocks.size()) {
                f.blocks[s].preds.push_back(b.id);
            }
        }
    }
    return true;
}

void rebuild_preds(Func &f) {
    for (Block &b : f.blocks) {
        b.preds.clear();
    }
    for (Block &b : f.blocks) {
        for (BlockId s : b.succs) {
            if (s >= 0 && static_cast<size_t>(s) < f.blocks.size()) {
                f.blocks[s].preds.push_back(b.id);
            }
        }
    }
}

} // namespace ir
} // namespace jit
} // namespace nari
#endif // !DISABLE_JIT
