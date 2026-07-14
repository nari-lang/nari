// Bytecode -> SSA IR builder.
#ifndef DISABLE_JIT
#include "ir_build.h"
#include "bytecode.h"
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace nari {
namespace jit {
namespace ir {

using nari::bytecode::Chunk;
using nari::bytecode::Constant;
using nari::bytecode::FunctionMeta;
using nari::bytecode::OpCode;

// Instruction width (bytes) for the P1 opcode subset; 0 = not eligible.
static int p1_width(OpCode op) {
    switch (op) {
        case OpCode::OP_LOAD_ZERO:
        case OpCode::OP_LOAD_ONE:
        case OpCode::OP_LOAD_NONE:
        case OpCode::OP_LOAD_TRUE:
        case OpCode::OP_LOAD_FALSE:
        case OpCode::OP_POP:
        case OpCode::OP_DUP:
        case OpCode::OP_ADD:
        case OpCode::OP_SUB:
        case OpCode::OP_MUL:
        case OpCode::OP_MOD:
        case OpCode::OP_DIV:
        case OpCode::OP_NEG:
        case OpCode::OP_BIT_AND:
        case OpCode::OP_BIT_OR:
        case OpCode::OP_BIT_XOR:
        case OpCode::OP_BIT_NOT:
        case OpCode::OP_LSHIFT:
        case OpCode::OP_RSHIFT:
        case OpCode::OP_NOT:
        case OpCode::OP_LT:
        case OpCode::OP_LE:
        case OpCode::OP_GT:
        case OpCode::OP_GE:
        case OpCode::OP_EQ:
        case OpCode::OP_NE:
        case OpCode::OP_RETURN:
        case OpCode::OP_GET_INDEX:
        case OpCode::OP_SET_INDEX:
            break;
        case OpCode::OP_CALL:
            break;
        case OpCode::OP_STR_CONCAT:
        case OpCode::OP_ITER_ARRAY:
            break;
        case OpCode::OP_LOAD_CONST:
        case OpCode::OP_LOAD_VAR:
        case OpCode::OP_STORE_VAR:
        case OpCode::OP_LOAD_GLOBAL:
        case OpCode::OP_JUMP:
        case OpCode::OP_JUMP_IF_FALSE:
        case OpCode::OP_JUMP_IF_TRUE:
            break;
        case OpCode::OP_STORE_GLOBAL:
        case OpCode::OP_LOAD_CAPTURE:
        case OpCode::OP_STORE_CAPTURE:
        case OpCode::OP_GET_PROPERTY:
        case OpCode::OP_SET_PROPERTY:
        case OpCode::OP_FORMAT_VALUE:
        case OpCode::OP_MAKE_ARRAY:
        case OpCode::OP_MAKE_OBJECT:
            break;
        case OpCode::OP_CALL_METHOD:
            break;
        default:
            return 0; // outside the P1 subset
    }
    return nari::bytecode::opcode_fixed_size(op);
}

bool build(const Chunk &chunk, uint32_t func_idx, Func &out) {
    if (func_idx >= chunk.functions.size()) {
        return false;
    }
    const FunctionMeta &fm = chunk.functions[func_idx];
    const std::vector<uint8_t> &code = fm.code;
    if (code.empty()) {
        return false;
    }

    auto u16 = [&](size_t p) -> uint16_t {
        return (uint16_t)((code[p] << 8) | code[p + 1]);
    };
    auto jtarget = [&](size_t op_pc) -> size_t {
        int16_t off = (int16_t)u16(op_pc + 1);
        return (size_t)((ptrdiff_t)(op_pc + 3) + off);
    };

    static const bool kBuildReport = getenv("NARI_IR_BUILD_REPORT") != nullptr;

    // --- pass 1: eligibility + block leaders -------------------------------
    std::set<size_t> leaders;
    leaders.insert(0);
    for (size_t pc = 0; pc < code.size();) {
        OpCode op = (OpCode)code[pc];
        int w = p1_width(op);
        if (w == 0) {
            if (kBuildReport) {
                fprintf(stderr, "[IR-BUILD] %s: non-P1 opcode %s at pc=%zu\n",
                        fm.name.c_str(), nari::bytecode::opcode_name(op), pc);
            }
            return false; // non-P1 opcode
        }
        if (op == OpCode::OP_JUMP || op == OpCode::OP_JUMP_IF_FALSE ||
            op == OpCode::OP_JUMP_IF_TRUE) {
            size_t tgt = jtarget(pc);
            if (tgt > code.size()) {
                return false;
            }
            leaders.insert(tgt);
            leaders.insert(pc + 3); // fall-through / post-jump
        } else if (op == OpCode::OP_RETURN) {
            leaders.insert(pc + 1); // unreachable-but-consistent boundary
        }
        pc += w;
    }

    // --- create blocks (one per in-range leader) ---------------------------
    out.meta = &fm;
    out.num_slots = (uint32_t)fm.var_names.size();
    out.num_params = fm.param_count;
    std::map<size_t, BlockId> at; // leader pc -> block id
    std::vector<size_t> leader_vec(leaders.begin(), leaders.end());
    for (size_t L : leader_vec) {
        if (L < code.size()) {
            at[L] = out.add_block(L);
        }
    }
    if (!at.count(0)) {
        return false;
    }
    out.entry = at[0];

    auto block_at = [&](size_t pc) -> BlockId {
        auto it = at.find(pc);
        return it == at.end() ? InvalidBlock : it->second;
    };

    // --- pass 1.5: structural CFG (succs/preds) ---------------------------
    // We need predecessors known before emitting each block so pass 2 can seed
    // entry vstk (phi at merges, inherited from single fwd pred otherwise).
    // We use local arrays here; b.succs/b.preds remain populated at the end
    // from the actual emitted terminators (kept unchanged for downstream passes).
    std::vector<std::vector<BlockId>> cfg_succs(out.blocks.size());
    std::vector<std::vector<BlockId>> cfg_preds(out.blocks.size());
    for (size_t li = 0; li < leader_vec.size(); li++) {
        size_t start = leader_vec[li];
        if (start >= code.size()) {
            continue;
        }
        size_t end = (li + 1 < leader_vec.size()) ? leader_vec[li + 1] : code.size();
        BlockId bid = at[start];
        size_t pc = start;
        bool found_term = false;
        while (pc < end) {
            OpCode op = (OpCode)code[pc];
            int w = p1_width(op);
            if (w == 0) {
                return false; // pass 1 already filtered, defensive
            }
            if (op == OpCode::OP_JUMP) {
                size_t tgt = jtarget(pc);
                BlockId tb = block_at(tgt);
                if (tb == InvalidBlock) {
                    return false;
                }
                cfg_succs[bid].push_back(tb);
                found_term = true;
                break;
            }
            if (op == OpCode::OP_JUMP_IF_FALSE || op == OpCode::OP_JUMP_IF_TRUE) {
                size_t tgt = jtarget(pc);
                BlockId tb = block_at(tgt);
                BlockId fall = block_at(pc + 3);
                if (tb == InvalidBlock || fall == InvalidBlock) {
                    return false;
                }
                cfg_succs[bid].push_back(fall);
                cfg_succs[bid].push_back(tb);
                found_term = true;
                break;
            }
            if (op == OpCode::OP_RETURN) {
                found_term = true;
                break;
            }
            pc += w;
        }
        if (!found_term) {
            BlockId nb = block_at(end);
            if (nb != InvalidBlock) {
                cfg_succs[bid].push_back(nb);
            }
        }
    }
    for (size_t b = 0; b < cfg_succs.size(); b++) {
        for (BlockId s : cfg_succs[b]) {
            cfg_preds[s].push_back((BlockId)b);
        }
    }

    // Per-block state for stack threading across edges.
    std::vector<std::vector<ValueId>> exit_vstk(out.blocks.size());
    std::vector<int> exit_depth(out.blocks.size(), -1);
    std::vector<int> entry_depth(out.blocks.size(), -1);
    entry_depth[out.entry] = 0;

    // --- pass 2: emit each block -------------------------------------------
    auto emit_push = [&](Block &b, std::vector<ValueId> &vstk, Inst in) {
        ValueId v = out.add_inst(std::move(in));
        b.insts.push_back(v);
        vstk.push_back(v);
    };
    for (size_t li = 0; li < leader_vec.size(); li++) {
        size_t start = leader_vec[li];
        if (start >= code.size()) {
            continue;
        }
        size_t end = (li + 1 < leader_vec.size()) ? leader_vec[li + 1] : code.size();
        BlockId bid = at[start];
        Block &b = out.blocks[bid];
        std::vector<ValueId> vstk;

        // Seed entry vstk from predecessors. Forward preds (lower id) have
        // already been processed; back-edge preds have not. Blocks are emitted
        // in pc order, so all forward predecessors of a forward merge are
        // available. Loop headers (any back-edge pred) MUST have entry depth 0:
        // back-edges carrying values are rejected as too risky.
        {
            const auto &preds = cfg_preds[bid];
            int depth = -1;
            bool has_back_edge = false;
            for (BlockId p : preds) {
                if ((size_t)p >= (size_t)bid) {
                    has_back_edge = true;
                    continue;
                }
                if (exit_depth[p] < 0) {
                    // Unreachable predecessor (e.g. dead block) - skip.
                    continue;
                }
                if (depth < 0) {
                    depth = exit_depth[p];
                } else if (depth != exit_depth[p]) {
                    // Disagreement: predecessors leave different stack depths at
                    // this block's entry - this is a malformed CFG or unsupported
                    // shape (e.g. exception flow). Bail.
                    if (kBuildReport) {
                        fprintf(stderr, "[IR-BUILD] %s: pred depth mismatch at block start pc=%zu (%d vs %d)\n",
                                fm.name.c_str(), start, depth, exit_depth[p]);
                    }
                    return false;
                }
            }
            if (bid == out.entry) {
                depth = 0;
            } else if (depth < 0) {
                // No reachable forward predecessor - this block is either the
                // entry, unreachable, or a pure loop header with only back-edges.
                // Loop headers must be stack-neutral; assume depth 0.
                depth = 0;
            }
            if (has_back_edge && depth != 0) {
                if (kBuildReport) {
                    fprintf(stderr, "[IR-BUILD] %s: loop header has entry depth %d at pc=%zu (back-edge requires 0)\n",
                            fm.name.c_str(), depth, start);
                }
                return false;
            }
            entry_depth[bid] = depth;

            if (depth > 0) {
                // Decide single-pred inherit vs multi-pred phi.
                int fwd_pred_count = 0;
                BlockId only_fwd = InvalidBlock;
                for (BlockId p : preds) {
                    if ((size_t)p < (size_t)bid && exit_depth[p] >= 0) {
                        fwd_pred_count++;
                        only_fwd = p;
                    }
                }
                if (fwd_pred_count == 1) {
                    // Inherit predecessor's exit vstk (top `depth` entries).
                    const auto &pv = exit_vstk[only_fwd];
                    if ((int)pv.size() < depth) {
                        return false;
                    }
                    vstk.assign(pv.end() - depth, pv.end());
                } else {
                    // Multi-pred merge: emit a Phi for each stack slot.
                    // Operand order matches preds (the order matters only for
                    // the type-join in infer_types, not for codegen).
                    for (int d = 0; d < depth; d++) {
                        Inst phi;
                        phi.op = Op::Phi;
                        phi.bytecode_pc = start;
                        for (BlockId p : preds) {
                            if ((size_t)p < (size_t)bid && exit_depth[p] >= 0) {
                                const auto &pv = exit_vstk[p];
                                int idx = (int)pv.size() - depth + d;
                                if (idx < 0) {
                                    return false;
                                }
                                phi.operands.push_back(pv[idx]);
                            }
                        }
                        ValueId pv_id = out.add_inst(std::move(phi));
                        b.phis.push_back(pv_id);
                        vstk.push_back(pv_id);
                    }
                }
            }
        }

        size_t pc = start;
        bool terminated = false;

        auto bin = [&](Op op, size_t op_pc) -> bool {
            if (vstk.size() < 2) {
                return false;
            }
            ValueId rhs = vstk.back();
            vstk.pop_back();
            ValueId lhs = vstk.back();
            vstk.pop_back();
            Inst in;
            in.op = op;
            in.operands = { lhs, rhs };
            in.bytecode_pc = op_pc;
            emit_push(b, vstk, std::move(in));
            return true;
        };

        auto unary_in_place = [&](Op op, size_t op_pc) -> bool {
            if (vstk.empty()) {
                return false;
            }
            Inst in;
            in.op = op;
            in.operands = { vstk.back() };
            in.bytecode_pc = op_pc;
            ValueId v = out.add_inst(std::move(in));
            b.insts.push_back(v);
            vstk.back() = v;
            return true;
        };

        while (pc < end && !terminated) {
            size_t op_pc = pc;
            OpCode op = (OpCode)code[pc++];
            switch (op) {
                case OpCode::OP_LOAD_CONST: {
                    uint16_t ci = u16(pc);
                    pc += 2;
                    if (ci >= fm.constants.size()) {
                        return false;
                    }
                    const Constant &c = fm.constants[ci];
                    Inst in;
                    in.bytecode_pc = op_pc;
                    if (c.type == Constant::Type::INT) {
                        in.op = Op::IConst;
                        in.type = Ty::Int48;
                        in.imm_int = c.as_int;
                    } else if (c.type == Constant::Type::FLOAT) {
                        in.op = Op::FConst;
                        in.type = Ty::Float;
                        in.imm_float = c.as_float;
                    } else {
                        in.op = Op::LoadConst;
                        in.imm_u32 = ci;
                    }
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_LOAD_ZERO:
                case OpCode::OP_LOAD_ONE: {
                    Inst in;
                    in.op = Op::IConst;
                    in.type = Ty::Int48;
                    in.imm_int = (op == OpCode::OP_LOAD_ONE) ? 1 : 0;
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_LOAD_TRUE:
                case OpCode::OP_LOAD_FALSE: {
                    Inst in;
                    in.op = Op::BConst;
                    in.type = Ty::Bool;
                    in.imm_int = (op == OpCode::OP_LOAD_TRUE) ? 1 : 0;
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_LOAD_NONE: {
                    Inst in;
                    in.op = Op::NConst;
                    in.type = Ty::None;
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_LOAD_VAR: {
                    uint16_t slot = u16(pc);
                    pc += 2;
                    Inst in;
                    in.op = Op::LoadSlot;
                    in.imm_u32 = slot;
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_LOAD_GLOBAL: {
                    uint16_t name = u16(pc);
                    pc += 2;
                    Inst in;
                    in.op = Op::LoadGlobal;
                    in.imm_u32 = name;
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_STORE_VAR: {
                    uint16_t slot = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst in;
                    in.op = Op::StoreSlot;
                    in.imm_u32 = slot;
                    in.operands = { vstk.back() }; // peek, no pop
                    in.bytecode_pc = op_pc;
                    ValueId v = out.add_inst(std::move(in));
                    b.insts.push_back(v);
                    break;
                }
                case OpCode::OP_STORE_GLOBAL: {
                    uint16_t name = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst in;
                    in.op = Op::StoreGlobal;
                    in.imm_u32 = name;
                    in.operands = { vstk.back() }; // peek, no pop
                    in.bytecode_pc = op_pc;
                    ValueId v = out.add_inst(std::move(in));
                    b.insts.push_back(v);
                    break;
                }
                case OpCode::OP_LOAD_CAPTURE: {
                    uint16_t idx = u16(pc);
                    pc += 2;
                    Inst in;
                    in.op = Op::LoadCapture;
                    in.imm_u32 = idx;
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_STORE_CAPTURE: {
                    uint16_t idx = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst in;
                    in.op = Op::StoreCapture;
                    in.imm_u32 = idx;
                    in.operands = { vstk.back() }; // peek, no pop
                    in.bytecode_pc = op_pc;
                    ValueId v = out.add_inst(std::move(in));
                    b.insts.push_back(v);
                    break;
                }
                case OpCode::OP_POP: {
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst in;
                    in.op = Op::Pop;
                    in.operands = { vstk.back() };
                    in.bytecode_pc = op_pc;
                    ValueId v = out.add_inst(std::move(in));
                    b.insts.push_back(v);
                    vstk.pop_back();
                    break;
                }
                case OpCode::OP_DUP: {
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst in;
                    in.op = Op::Dup;
                    in.operands = { vstk.back() };
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_ADD: {
                    if (!bin(Op::DynAdd, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_SUB: {
                    if (!bin(Op::DynSub, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_MUL: {
                    if (!bin(Op::DynMul, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_MOD: {
                    if (!bin(Op::DynMod, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_DIV: {
                    if (!bin(Op::DynDiv, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_STR_CONCAT: {
                    if (!bin(Op::StrConcat, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_NEG: {
                    if (!unary_in_place(Op::INeg, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_BIT_AND: {
                    if (!bin(Op::IAnd, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_BIT_OR: {
                    if (!bin(Op::IOr, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_BIT_XOR: {
                    if (!bin(Op::IXor, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_BIT_NOT: {
                    if (!unary_in_place(Op::INot, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_LSHIFT: {
                    if (!bin(Op::IShl, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_RSHIFT: {
                    if (!bin(Op::IShr, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_NOT: {
                    if (!unary_in_place(Op::Not, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_LT: {
                    if (!bin(Op::DynCmpLt, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_LE: {
                    if (!bin(Op::DynCmpLe, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_GT: {
                    if (!bin(Op::DynCmpGt, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_GE: {
                    if (!bin(Op::DynCmpGe, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_EQ: {
                    if (!bin(Op::DynCmpEq, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_NE: {
                    if (!bin(Op::DynCmpNe, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_GET_INDEX: {
                    if (vstk.size() < 2) {
                        return false;
                    }
                    ValueId idx = vstk.back();
                    vstk.pop_back();
                    ValueId obj = vstk.back();
                    vstk.pop_back();
                    Inst in;
                    in.op = Op::LoadIndex;
                    in.operands = { obj, idx };
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_SET_INDEX: {
                    if (vstk.size() < 3) {
                        return false;
                    }
                    ValueId val = vstk.back();
                    vstk.pop_back();
                    ValueId idx = vstk.back();
                    vstk.pop_back();
                    ValueId obj = vstk.back();
                    vstk.pop_back();
                    Inst in;
                    in.op = Op::StoreIndex;
                    in.operands = { obj, idx, val };
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_GET_PROPERTY: {
                    uint16_t name = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    ValueId obj = vstk.back();
                    vstk.pop_back();
                    Inst in;
                    in.op = Op::LoadProperty;
                    in.imm_u32 = name;
                    in.operands = { obj };
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_SET_PROPERTY: {
                    uint16_t name = u16(pc);
                    pc += 2;
                    if (vstk.size() < 2) {
                        return false;
                    }
                    ValueId val = vstk.back();
                    vstk.pop_back();
                    ValueId obj = vstk.back();
                    vstk.pop_back();
                    Inst in;
                    in.op = Op::StoreProperty;
                    in.imm_u32 = name;
                    in.operands = { obj, val };
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_MAKE_ARRAY: {
                    uint16_t n = u16(pc);
                    pc += 2;
                    if (vstk.size() < n) {
                        return false;
                    }
                    std::vector<ValueId> ops;
                    ops.reserve(n);
                    size_t base = vstk.size() - n;
                    for (size_t i = base; i < vstk.size(); i++) {
                        ops.push_back(vstk[i]);
                    }
                    vstk.resize(base);
                    Inst in;
                    in.op = Op::MakeArray;
                    in.imm_u32 = n;
                    in.operands = std::move(ops);
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_MAKE_OBJECT: {
                    uint16_t n = u16(pc);
                    pc += 2;
                    size_t count = (size_t)n * 2;
                    if (vstk.size() < count) {
                        return false;
                    }
                    std::vector<ValueId> ops;
                    ops.reserve(count);
                    size_t base = vstk.size() - count;
                    for (size_t i = base; i < vstk.size(); i++) {
                        ops.push_back(vstk[i]);
                    }
                    vstk.resize(base);
                    Inst in;
                    in.op = Op::MakeObject;
                    in.imm_u32 = n;
                    in.operands = std::move(ops);
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_FORMAT_VALUE: {
                    uint16_t spec = u16(pc);
                    pc += 2;
                    if (!unary_in_place(Op::FormatValue, op_pc)) {
                        return false;
                    }
                    out.inst(vstk.back()).imm_u32 = spec;
                    break;
                }
                case OpCode::OP_ITER_ARRAY: {
                    if (!unary_in_place(Op::IterArray, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_CALL: {
                    uint8_t argc = code[pc++];
                    uint16_t callee_label = u16(pc);
                    pc += 2;
                    if (vstk.size() < (size_t)argc + 1) {
                        return false;
                    }
                    std::vector<ValueId> ops;
                    ops.reserve((size_t)argc + 1);
                    size_t base = vstk.size() - ((size_t)argc + 1);
                    for (size_t i = base; i < vstk.size(); i++) {
                        ops.push_back(vstk[i]);
                    }
                    vstk.resize(base);
                    Inst in;
                    in.op = Op::Call;
                    in.imm_u32 = argc;
                    in.imm_int = callee_label;
                    in.operands = std::move(ops);
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_CALL_METHOD: {
                    uint16_t method = u16(pc);
                    pc += 2;
                    uint8_t argc = code[pc++];
                    if (vstk.size() < (size_t)argc + 1) {
                        return false;
                    }
                    std::vector<ValueId> ops;
                    ops.reserve((size_t)argc + 1);
                    size_t base = vstk.size() - ((size_t)argc + 1);
                    for (size_t i = base; i < vstk.size(); i++) {
                        ops.push_back(vstk[i]);
                    }
                    vstk.resize(base);
                    Inst in;
                    in.op = Op::CallMethod;
                    in.imm_u32 = method;
                    in.imm_int = argc;
                    in.operands = std::move(ops);
                    in.bytecode_pc = op_pc;
                    emit_push(b, vstk, std::move(in));
                    break;
                }
                case OpCode::OP_JUMP: {
                    size_t tgt = jtarget(op_pc);
                    pc = op_pc + 3;
                    BlockId tb = block_at(tgt);
                    if (tb == InvalidBlock) {
                        return false;
                    }
                    Inst in;
                    in.op = Op::Jump;
                    in.target0 = tb;
                    in.bytecode_pc = op_pc;
                    b.terminator = out.add_inst(std::move(in));
                    terminated = true;
                    break;
                }
                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE: {
                    size_t tgt = jtarget(op_pc);
                    pc = op_pc + 3;
                    if (vstk.empty()) {
                        return false;
                    }
                    ValueId cond = vstk.back();
                    vstk.pop_back();
                    BlockId fall = block_at(pc);
                    BlockId jump = block_at(tgt);
                    if (fall == InvalidBlock || jump == InvalidBlock) {
                        return false;
                    }
                    Inst in;
                    in.op = Op::Branch;
                    in.operands = { cond };
                    in.bytecode_pc = op_pc;
                    // target0 = taken when truthy, target1 = taken when falsy
                    if (op == OpCode::OP_JUMP_IF_FALSE) {
                        in.target0 = fall; // truthy -> fall through
                        in.target1 = jump; // falsy  -> jump target
                    } else {
                        in.target0 = jump; // truthy -> jump target
                        in.target1 = fall; // falsy  -> fall through
                    }
                    b.terminator = out.add_inst(std::move(in));
                    terminated = true;
                    break;
                }
                case OpCode::OP_RETURN: {
                    Inst in;
                    in.op = Op::Return;
                    in.bytecode_pc = op_pc;
                    if (!vstk.empty()) {
                        in.operands = { vstk.back() };
                        vstk.pop_back();
                    }
                    b.terminator = out.add_inst(std::move(in));
                    terminated = true;
                    break;
                }
                default:
                    return false;
            }
        }

        if (!terminated) {
            // fell off the block end -> implicit jump to the next block
            BlockId nb = block_at(end);
            if (nb == InvalidBlock) {
                // no successor block: implicit `return none`
                Inst in;
                in.op = Op::Return;
                b.terminator = out.add_inst(std::move(in));
            } else {
                Inst in;
                in.op = Op::Jump;
                in.target0 = nb;
                b.terminator = out.add_inst(std::move(in));
            }
        }

        // Record exit state. Return terminators consume nothing further; the
        // exit stack is the vstk as-is after the terminator processed (Return
        // already popped the return value above).
        exit_depth[bid] = (int)vstk.size();
        exit_vstk[bid] = vstk;

        // Validate back-edges: any already-processed successor (id <= bid)
        // must agree on the entry depth via this edge.
        for (BlockId s : cfg_succs[bid]) {
            if ((size_t)s <= (size_t)bid && entry_depth[s] >= 0) {
                if (entry_depth[s] != (int)vstk.size()) {
                    if (kBuildReport) {
                        fprintf(stderr, "[IR-BUILD] %s: back-edge depth %zu -> %d mismatch (block %d -> %d)\n",
                                fm.name.c_str(), vstk.size(), entry_depth[s], (int)bid, (int)s);
                    }
                    return false;
                }
                // Back-edges to a non-zero entry depth would require phi
                // operands from this block - but we've already emitted the
                // header's phis. We require loop headers to have depth 0
                // (enforced at header entry), so depth-0 match is the only
                // legal case here.
                if (entry_depth[s] != 0) {
                    return false;
                }
            }
        }
    }

    // --- CFG edges (succs/preds) -------------------------------------------
    for (Block &b : out.blocks) {
        if (b.terminator == InvalidValue) {
            continue;
        }
        const Inst &t = out.inst(b.terminator);
        if (t.op == Op::Jump) {
            b.succs.push_back(t.target0);
        } else if (t.op == Op::Branch) {
            b.succs.push_back(t.target0);
            b.succs.push_back(t.target1);
        }
    }
    for (Block &b : out.blocks) {
        for (BlockId s : b.succs) {
            out.blocks[s].preds.push_back(b.id);
        }
    }
    return true;
}

} // namespace ir
} // namespace jit
} // namespace nari
#endif // !DISABLE_JIT
