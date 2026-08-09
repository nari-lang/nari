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
        case OpCode::OP_POW:
        case OpCode::OP_DIV:
        case OpCode::OP_NEG:
        case OpCode::OP_BIT_AND:
        case OpCode::OP_BIT_OR:
        case OpCode::OP_BIT_XOR:
        case OpCode::OP_BIT_NOT:
        case OpCode::OP_LSHIFT:
        case OpCode::OP_RSHIFT:
        case OpCode::OP_JS_BIT_AND:
        case OpCode::OP_JS_BIT_OR:
        case OpCode::OP_JS_BIT_XOR:
        case OpCode::OP_JS_BIT_NOT:
        case OpCode::OP_JS_SHL:
        case OpCode::OP_JS_SHR:
        case OpCode::OP_JS_USHR:
        case OpCode::OP_NOT:
        case OpCode::OP_JS_TRUTHY:
        case OpCode::OP_LT:
        case OpCode::OP_LE:
        case OpCode::OP_GT:
        case OpCode::OP_GE:
        case OpCode::OP_EQ:
        case OpCode::OP_NE:
        case OpCode::OP_STRICT_EQ:
        case OpCode::OP_STRICT_NE:
        case OpCode::OP_RETURN:
        case OpCode::OP_GET_INDEX:
        case OpCode::OP_SET_INDEX:
        case OpCode::OP_CALL:
        case OpCode::OP_CALL_SPREAD:
        case OpCode::OP_ARRAY_PUSH:
        case OpCode::OP_ARRAY_SPREAD:
        case OpCode::OP_SELF_TAIL_CALL:
            break;
        case OpCode::OP_STR_CONCAT:
        case OpCode::OP_STR_APPEND_VAR:
        case OpCode::OP_ITER_ARRAY:
        case OpCode::OP_LOAD_CONST:
        case OpCode::OP_LOAD_VAR:
        case OpCode::OP_STORE_VAR:
        case OpCode::OP_LOAD_GLOBAL:
        case OpCode::OP_JUMP:
        case OpCode::OP_JUMP_IF_FALSE:
        case OpCode::OP_JUMP_IF_TRUE:
        case OpCode::OP_JUMP_IF_NONE:
            break;
        case OpCode::OP_STORE_GLOBAL:
        case OpCode::OP_LOAD_CAPTURE:
        case OpCode::OP_STORE_CAPTURE:
        case OpCode::OP_GET_PROPERTY:
        case OpCode::OP_JS_GET_PROP_STATIC:
        case OpCode::OP_SET_PROPERTY:
        case OpCode::OP_JS_SET_PROP_STATIC:
        case OpCode::OP_JS_POSTINC:
        case OpCode::OP_CLOSE_UPVALUES:
        case OpCode::OP_FORMAT_VALUE:
        case OpCode::OP_MAKE_ARRAY:
        case OpCode::OP_MAKE_OBJECT:
        case OpCode::OP_MAKE_CLOSURE:
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
    const FunctionMeta &func_meta = chunk.functions[func_idx];
    const ByteArray &code = func_meta.code;
    static const bool kBuildReport = getenv("NARI_IR_BUILD_REPORT") != nullptr;
    if (code.empty()) {
        return false;
    }

    auto u16 = [&](size_t p) -> uint16_t { return (uint16_t)((code[p] << 8) | code[p + 1]); };
    auto jtarget = [&](size_t op_pc) -> size_t {
        int16_t off = (int16_t)u16(op_pc + 1);
        return (size_t)((ptrdiff_t)(op_pc + 3) + off);
    };

    // eligibility and block leaders
    std::set<size_t> leaders;
    leaders.insert(0);
    for (size_t pc = 0; pc < code.size();) {
        OpCode op = (OpCode)code[pc];
        int w = p1_width(op);
        if (op == OpCode::OP_MAKE_CLOSURE) {
            w = (int)nari::bytecode::decoded_instruction_size(code, pc);
        }
        if (w == 0) {
            if (kBuildReport) {
                fprintf(stderr, "[IR-BUILD] %s: non-P1 opcode %s at pc=%zu\n", func_meta.name.c_str(),
                        nari::bytecode::opcode_name(op), pc);
            }
            return false; // non-P1 opcode
        }
        if (op == OpCode::OP_JUMP || op == OpCode::OP_JUMP_IF_FALSE || op == OpCode::OP_JUMP_IF_TRUE ||
            op == OpCode::OP_JUMP_IF_NONE) {
            size_t target = jtarget(pc);
            if (target > code.size()) {
                return false;
            }
            leaders.insert(target);
            leaders.insert(pc + 3); // fall-through / post-jump
        } else if (op == OpCode::OP_RETURN || op == OpCode::OP_SELF_TAIL_CALL) {
            leaders.insert(pc + w); // unreachable-but-consistent boundary
        }
        pc += w;
    }

    // create blocks (one per in-range leader)
    out.meta = &func_meta;
    out.num_slots = (uint32_t)func_meta.var_names.size();
    out.num_params = func_meta.param_count;
    out.captured_local_slots.assign(out.num_slots, 0);
    std::map<size_t, BlockId> at; // leader pc -> block id
    std::vector<size_t> leader_vec(leaders.begin(), leaders.end());
    for (size_t leader : leader_vec) {
        if (leader < code.size()) {
            at[leader] = out.add_block(leader);
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

    // structural CFG (succs/preds)
    // we need predecessors known before emitting each block so step 2 can seed entry vstk
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
            if (op == OpCode::OP_MAKE_CLOSURE) {
                w = (int)nari::bytecode::decoded_instruction_size(code, pc);
            }
            if (w == 0) {
                return false; // pass 1 already filtered, defensive
            }
            if (op == OpCode::OP_JUMP) {
                size_t target = jtarget(pc);
                BlockId target_block = block_at(target);
                if (target_block == InvalidBlock) {
                    return false;
                }
                cfg_succs[bid].push_back(target_block);
                found_term = true;
                break;
            }
            if (op == OpCode::OP_JUMP_IF_FALSE || op == OpCode::OP_JUMP_IF_TRUE || op == OpCode::OP_JUMP_IF_NONE) {
                size_t target = jtarget(pc);
                BlockId tb = block_at(target);
                BlockId fall = block_at(pc + 3);
                if (tb == InvalidBlock || fall == InvalidBlock) {
                    return false;
                }
                cfg_succs[bid].push_back(fall);
                cfg_succs[bid].push_back(tb);
                found_term = true;
                break;
            }
            if (op == OpCode::OP_SELF_TAIL_CALL) {
                cfg_succs[bid].push_back(out.entry);
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
    for (size_t block = 0; block < cfg_succs.size(); block++) {
        for (BlockId successor : cfg_succs[block]) {
            cfg_preds[successor].push_back((BlockId)block);
        }
    }

    // per-block state for stack threading across edges.
    std::vector<std::vector<ValueId>> exit_vstk(out.blocks.size());
    std::vector<int> exit_depth(out.blocks.size(), -1);
    std::vector<int> entry_depth(out.blocks.size(), -1);
    entry_depth[out.entry] = 0;

    // emit each block
    auto emit_push = [&](Block &b, std::vector<ValueId> &vstk, Inst instruction) {
        ValueId val_id = out.add_inst(std::move(instruction));
        b.insts.push_back(val_id);
        vstk.push_back(val_id);
    };
    for (size_t li = 0; li < leader_vec.size(); li++) {
        size_t start = leader_vec[li];
        if (start >= code.size()) {
            continue;
        }
        size_t end = (li + 1 < leader_vec.size()) ? leader_vec[li + 1] : code.size();
        BlockId bid = at[start];
        Block &block = out.blocks[bid];
        std::vector<ValueId> vstk;

        // Seed entry vstk from predecessors.
        // forward preds (lower id) have already been processed, back-edge preds have not.
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
                    // unreachable predecessor (e.g. dead block), skip it.
                    continue;
                }
                if (depth < 0) {
                    depth = exit_depth[p];
                } else if (depth != exit_depth[p]) {
                    // predecessors leave different stack depths at this block's entry,
                    // this is a malformed CFG or unsupported shape, we should bail here.
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
                    // multi-pred merge: emit a Phi for each stack slot.
                    // Operand order matches preds
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
                        block.phis.push_back(pv_id);
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
            Inst instruction;
            instruction.op = op;
            instruction.operands = { lhs, rhs };
            instruction.bytecode_pc = op_pc;
            emit_push(block, vstk, std::move(instruction));
            return true;
        };

        auto unary_in_place = [&](Op op, size_t op_pc) -> bool {
            if (vstk.empty()) {
                return false;
            }
            Inst instruction;
            instruction.op = op;
            instruction.operands = { vstk.back() };
            instruction.bytecode_pc = op_pc;
            ValueId val_id = out.add_inst(std::move(instruction));
            block.insts.push_back(val_id);
            vstk.back() = val_id;
            return true;
        };

        while (pc < end && !terminated) {
            size_t op_pc = pc;
            OpCode op = (OpCode)code[pc++];
            switch (op) {
                case OpCode::OP_LOAD_CONST: {
                    uint16_t ci = u16(pc);
                    pc += 2;
                    if (ci >= func_meta.constants.size()) {
                        return false;
                    }
                    const Constant &c = func_meta.constants[ci];
                    Inst instruction;
                    instruction.bytecode_pc = op_pc;
                    if (c.type == Constant::Type::INT) {
                        instruction.op = Op::IConst;
                        instruction.type = Ty::Int48;
                        instruction.imm_int = c.as_int;
                    } else if (c.type == Constant::Type::FLOAT) {
                        instruction.op = Op::FConst;
                        instruction.type = Ty::Float;
                        instruction.imm_float = c.as_float;
                    } else {
                        instruction.op = Op::LoadConst;
                        instruction.imm_u32 = ci;
                    }
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_LOAD_ZERO:
                case OpCode::OP_LOAD_ONE: {
                    Inst instruction;
                    instruction.op = Op::IConst;
                    instruction.type = Ty::Int48;
                    instruction.imm_int = (op == OpCode::OP_LOAD_ONE) ? 1 : 0;
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_LOAD_TRUE:
                case OpCode::OP_LOAD_FALSE: {
                    Inst instruction;
                    instruction.op = Op::BConst;
                    instruction.type = Ty::Bool;
                    instruction.imm_int = (op == OpCode::OP_LOAD_TRUE) ? 1 : 0;
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_LOAD_NONE: {
                    Inst instruction;
                    instruction.op = Op::NConst;
                    instruction.type = Ty::None;
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_LOAD_VAR: {
                    uint16_t slot = u16(pc);
                    pc += 2;
                    Inst instruction;
                    instruction.op = Op::LoadSlot;
                    instruction.imm_u32 = slot;
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_LOAD_GLOBAL: {
                    uint16_t name = u16(pc);
                    pc += 2;
                    Inst instruction;
                    instruction.op = Op::LoadGlobal;
                    instruction.imm_u32 = name;
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_STORE_VAR: {
                    uint16_t slot = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst instruction;
                    instruction.op = Op::StoreSlot;
                    instruction.imm_u32 = slot;
                    instruction.operands = { vstk.back() }; // peek, no pop
                    instruction.bytecode_pc = op_pc;
                    ValueId val_id = out.add_inst(std::move(instruction));
                    block.insts.push_back(val_id);
                    break;
                }
                case OpCode::OP_STORE_GLOBAL: {
                    uint16_t name = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst instruction;
                    instruction.op = Op::StoreGlobal;
                    instruction.imm_u32 = name;
                    instruction.operands = { vstk.back() }; // peek, no pop
                    instruction.bytecode_pc = op_pc;
                    ValueId val_id = out.add_inst(std::move(instruction));
                    block.insts.push_back(val_id);
                    break;
                }
                case OpCode::OP_LOAD_CAPTURE: {
                    uint16_t idx = u16(pc);
                    pc += 2;
                    Inst instruction;
                    instruction.op = Op::LoadCapture;
                    instruction.imm_u32 = idx;
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_STORE_CAPTURE: {
                    uint16_t idx = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst instruction;
                    instruction.op = Op::StoreCapture;
                    instruction.imm_u32 = idx;
                    instruction.operands = { vstk.back() }; // peek, no pop
                    instruction.bytecode_pc = op_pc;
                    ValueId val_id = out.add_inst(std::move(instruction));
                    block.insts.push_back(val_id);
                    break;
                }
                case OpCode::OP_POP: {
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst instruction;
                    instruction.op = Op::Pop;
                    instruction.operands = { vstk.back() };
                    instruction.bytecode_pc = op_pc;
                    ValueId val_id = out.add_inst(std::move(instruction));
                    block.insts.push_back(val_id);
                    vstk.pop_back();
                    break;
                }
                case OpCode::OP_DUP: {
                    if (vstk.empty()) {
                        return false;
                    }
                    Inst instruction;
                    instruction.op = Op::Dup;
                    instruction.operands = { vstk.back() };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                case OpCode::OP_POW: {
                    if (!bin(Op::Pow, op_pc)) {
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
                case OpCode::OP_STR_APPEND_VAR: {
                    uint16_t slot = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    ValueId rhs = vstk.back();
                    vstk.pop_back();
                    Inst instruction;
                    instruction.op = Op::StrAppendSlot;
                    instruction.imm_u32 = slot;
                    instruction.operands = { rhs };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                case OpCode::OP_JS_BIT_AND:
                case OpCode::OP_JS_BIT_OR:
                case OpCode::OP_JS_BIT_XOR:
                case OpCode::OP_JS_SHL:
                case OpCode::OP_JS_SHR:
                case OpCode::OP_JS_USHR: {
                    if (!bin(Op::JsBitBinary, op_pc)) {
                        return false;
                    }
                    out.inst(block.insts.back()).imm_u32 = static_cast<uint32_t>(op);
                    break;
                }
                case OpCode::OP_JS_BIT_NOT: {
                    if (!unary_in_place(Op::JsBitNot, op_pc)) {
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
                case OpCode::OP_JS_TRUTHY: {
                    if (!unary_in_place(Op::JsTruthy, op_pc)) {
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
                case OpCode::OP_STRICT_EQ: {
                    if (!bin(Op::DynStrictCmpEq, op_pc)) {
                        return false;
                    }
                    break;
                }
                case OpCode::OP_STRICT_NE: {
                    if (!bin(Op::DynStrictCmpNe, op_pc)) {
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
                    Inst instruction;
                    instruction.op = Op::LoadIndex;
                    instruction.operands = { obj, idx };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                    Inst instruction;
                    instruction.op = Op::StoreIndex;
                    instruction.operands = { obj, idx, val };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                    Inst instruction;
                    instruction.op = Op::LoadProperty;
                    instruction.imm_u32 = name;
                    instruction.operands = { obj };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_CLOSE_UPVALUES: {
                    uint16_t first_slot = u16(pc);
                    pc += 2;
                    Inst instruction;
                    instruction.op = Op::CloseUpvalues;
                    instruction.imm_u32 = first_slot;
                    instruction.bytecode_pc = op_pc;
                    block.insts.push_back(out.add_inst(std::move(instruction)));
                    break;
                }
                case OpCode::OP_JS_GET_PROP_STATIC:
                case OpCode::OP_JS_POSTINC: {
                    uint16_t name = u16(pc);
                    pc += 2;
                    if (vstk.empty()) {
                        return false;
                    }
                    ValueId obj = vstk.back();
                    vstk.pop_back();
                    Inst instruction;
                    instruction.op =
                        op == OpCode::OP_JS_POSTINC ? Op::JsPostinc : Op::JsGetPropStatic;
                    instruction.imm_u32 = name;
                    instruction.operands = { obj };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_JS_SET_PROP_STATIC:
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
                    Inst instruction;
                    instruction.op =
                        op == OpCode::OP_JS_SET_PROP_STATIC ? Op::JsSetPropStatic : Op::StoreProperty;
                    instruction.imm_u32 = name;
                    instruction.operands = { obj, val };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                    Inst instruction;
                    instruction.op = Op::MakeArray;
                    instruction.imm_u32 = n;
                    instruction.operands = std::move(ops);
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_ARRAY_PUSH: {
                    if (vstk.size() < 2) {
                        return false;
                    }
                    ValueId value = vstk.back();
                    vstk.pop_back();
                    ValueId target = vstk.back();
                    vstk.pop_back();
                    Inst instruction;
                    instruction.op = Op::ArrayPush;
                    instruction.operands = { target, value };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_ARRAY_SPREAD: {
                    if (vstk.size() < 2) {
                        return false;
                    }
                    ValueId iterable = vstk.back();
                    vstk.pop_back();
                    ValueId target = vstk.back();
                    vstk.pop_back();
                    Inst instruction;
                    instruction.op = Op::ArraySpread;
                    instruction.operands = { target, iterable };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_CALL_SPREAD: {
                    uint16_t callee_label_idx = u16(pc);
                    pc += 2;
                    if (vstk.size() < 2) {
                        return false;
                    }
                    ValueId args = vstk.back();
                    vstk.pop_back();
                    ValueId callee = vstk.back();
                    vstk.pop_back();
                    Inst instruction;
                    instruction.op = Op::CallSpread;
                    instruction.imm_u32 = callee_label_idx;
                    instruction.operands = { callee, args };
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                    Inst instruction;
                    instruction.op = Op::MakeObject;
                    instruction.imm_u32 = n;
                    instruction.operands = std::move(ops);
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_MAKE_CLOSURE: {
                    uint16_t func_idx = u16(pc);
                    uint16_t capture_count = u16(pc + 2);
                    size_t descriptor = pc + 4;
                    for (uint16_t i = 0; i < capture_count; i++, descriptor += 3) {
                        uint8_t source = code[descriptor];
                        uint16_t idx = u16(descriptor + 1);
                        if (source == 0 && idx < out.captured_local_slots.size()) {
                            out.captured_local_slots[idx] = 1;
                        }
                    }
                    pc += 4 + (size_t)capture_count * 3;
                    Inst instruction;
                    instruction.op = Op::MakeClosure;
                    instruction.imm_u32 = func_idx;
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                    Inst instruction;
                    instruction.op = Op::Call;
                    instruction.imm_u32 = argc;
                    instruction.imm_int = callee_label;
                    instruction.operands = std::move(ops);
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
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
                    Inst instruction;
                    instruction.op = Op::CallMethod;
                    instruction.imm_u32 = method;
                    instruction.imm_int = argc;
                    instruction.operands = std::move(ops);
                    instruction.bytecode_pc = op_pc;
                    emit_push(block, vstk, std::move(instruction));
                    break;
                }
                case OpCode::OP_JUMP: {
                    size_t target = jtarget(op_pc);
                    pc = op_pc + 3;
                    BlockId tb = block_at(target);
                    if (tb == InvalidBlock) {
                        return false;
                    }
                    Inst instruction;
                    instruction.op = Op::Jump;
                    instruction.target0 = tb;
                    instruction.bytecode_pc = op_pc;
                    block.terminator = out.add_inst(std::move(instruction));
                    terminated = true;
                    break;
                }
                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE:
                case OpCode::OP_JUMP_IF_NONE: {
                    size_t target = jtarget(op_pc);
                    pc = op_pc + 3;
                    if (vstk.empty()) {
                        return false;
                    }
                    ValueId cond = vstk.back();
                    vstk.pop_back();
                    if (op == OpCode::OP_JUMP_IF_NONE) {
                        Inst test;
                        test.op = Op::IsNone;
                        test.operands = { cond };
                        test.bytecode_pc = op_pc;
                        cond = out.add_inst(std::move(test));
                        block.insts.push_back(cond);
                    }
                    BlockId fall = block_at(pc);
                    BlockId jump = block_at(target);
                    if (fall == InvalidBlock || jump == InvalidBlock) {
                        return false;
                    }
                    Inst instruction;
                    instruction.op = Op::Branch;
                    instruction.operands = { cond };
                    instruction.bytecode_pc = op_pc;
                    // target0 = taken when truthy, target1 = taken when falsy
                    if (op == OpCode::OP_JUMP_IF_FALSE) {
                        instruction.target0 = fall; // truthy -> fall through
                        instruction.target1 = jump; // falsy  -> jump target
                    } else {
                        instruction.target0 = jump; // truthy -> jump target
                        instruction.target1 = fall; // falsy  -> fall through
                    }
                    block.terminator = out.add_inst(std::move(instruction));
                    terminated = true;
                    break;
                }
                case OpCode::OP_RETURN: {
                    Inst instruction;
                    instruction.op = Op::Return;
                    instruction.bytecode_pc = op_pc;
                    if (!vstk.empty()) {
                        instruction.operands = { vstk.back() };
                        vstk.pop_back();
                    }
                    block.terminator = out.add_inst(std::move(instruction));
                    terminated = true;
                    break;
                }
                case OpCode::OP_SELF_TAIL_CALL: {
                    uint8_t argc = code[pc++];
                    if (vstk.size() < argc) {
                        return false;
                    }
                    size_t base = vstk.size() - argc;
                    Inst instruction;
                    instruction.op = Op::SelfTailCall;
                    instruction.imm_u32 = argc;
                    instruction.operands.assign(vstk.begin() + base, vstk.end());
                    instruction.target0 = out.entry;
                    instruction.bytecode_pc = op_pc;
                    vstk.resize(base);
                    block.terminator = out.add_inst(std::move(instruction));
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
                Inst instruction;
                instruction.op = Op::Return;
                block.terminator = out.add_inst(std::move(instruction));
            } else {
                Inst instruction;
                instruction.op = Op::Jump;
                instruction.target0 = nb;
                block.terminator = out.add_inst(std::move(instruction));
            }
        }

        // Record exit state. Return terminators consume nothing further; the
        // exit stack is the vstk as-is after the terminator processed (Return
        // already popped the return value above).
        exit_depth[bid] = (int)vstk.size();
        exit_vstk[bid] = vstk;

        // Validate back-edges: any already-processed successor (id <= bid)
        // must agree on the entry depth via this edge.
        for (BlockId successor : cfg_succs[bid]) {
            if ((size_t)successor <= (size_t)bid && entry_depth[successor] >= 0) {
                if (entry_depth[successor] != (int)vstk.size()) {
                    return false;
                }
                // depth-0 match is the only legal case
                if (entry_depth[successor] != 0) {
                    return false;
                }
            }
        }
    }

    // CFG edges (succs/preds)
    for (Block &block : out.blocks) {
        if (block.terminator == InvalidValue) {
            continue;
        }
        const Inst &term = out.inst(block.terminator);
        if (term.op == Op::Jump || term.op == Op::SelfTailCall) {
            block.succs.push_back(term.target0);
        } else if (term.op == Op::Branch) {
            block.succs.push_back(term.target0);
            block.succs.push_back(term.target1);
        }
    }
    for (Block &block : out.blocks) {
        for (BlockId successor : block.succs) {
            out.blocks[successor].preds.push_back(block.id);
        }
    }
    return true;
}

} // namespace ir
} // namespace jit
} // namespace nari
#endif // !DISABLE_JIT
