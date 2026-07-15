#pragma once

#include "bytecode.h"
#include <cstdint>
#include <cstdio>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace nari {
namespace bytecode {

// Bytecode verifier: range-checks every index/slot/jump target in a decoded
// Chunk and runs a conservative stack-height dataflow pass to catch obvious underflow.

// NOTE: does NOT check runtime types, full exception invariants, or that all paths reach RETURN/THROW.
class BytecodeVerifier {
  public:
    static bool verify(const Chunk &chunk) {
        const size_t n_funcs = chunk.functions.size();
        const size_t n_strs = chunk.strings.size();

        if (chunk.main_func_idx >= n_funcs) {
            fail("main_func_idx out of range");
            return false;
        }

        for (size_t fi = 0; fi < n_funcs; fi++) {
            if (!verify_function(chunk, chunk.functions[fi], n_strs, n_funcs, fi)) {
                return false;
            }
        }
        return true;
    }

  private:
    struct InsnInfo {
        size_t pc = 0;
        OpCode op = OpCode::OP_LOAD_NONE;
        uint8_t op_byte = 0;
        size_t operand_pc = 0;
        size_t size = 1;
        int16_t jump_a = 0;
        int16_t jump_b = 0;
        uint16_t u16_a = 0;
        uint8_t u8_a = 0;
    };

    static void fail(const std::string &msg) {
        fprintf(stderr, "bytecode verifier: %s\n", msg.c_str());
    }

    static void fail(const std::string &fn, size_t pc, const std::string &msg) {
        fprintf(stderr, "bytecode verifier: %s@%zu: %s\n", fn.c_str(), pc, msg.c_str());
    }

    static uint16_t read_u16_be(const uint8_t *p) {
        return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
    }

    static int16_t read_i16_be(const uint8_t *p) {
        return static_cast<int16_t>(read_u16_be(p));
    }

    static bool is_instruction_boundary(size_t pc, const std::unordered_map<size_t, size_t> &pc_to_index, size_t code_size) {
        return pc == code_size || pc_to_index.find(pc) != pc_to_index.end();
    }

    static int64_t jump_target_after_operand(size_t operand_pc, size_t operand_size, int16_t rel) {
        return static_cast<int64_t>(operand_pc) + static_cast<int64_t>(operand_size) + static_cast<int64_t>(rel);
    }

    static bool add_successor(const std::string &where, const std::vector<InsnInfo> &insns,
                              const std::unordered_map<size_t, size_t> &pc_to_index,
                              size_t code_size, std::queue<size_t> &work,
                              std::vector<int> &height_at, size_t target_pc,
                              int out_height, size_t from_pc) {
        if (target_pc == code_size) {
            return true;
        }
        auto it = pc_to_index.find(target_pc);
        if (it == pc_to_index.end()) {
            fail(where, from_pc, "control-flow target is not an instruction boundary");
            return false;
        }
        size_t target_idx = it->second;
        int &known = height_at[target_idx];
        if (known == -1) {
            known = out_height;
            work.push(target_idx);
            return true;
        }
        if (known != out_height) {
            fail(where, from_pc, "inconsistent stack height at control-flow join targeting pc " + std::to_string(target_pc));
            return false;
        }
        (void)insns;
        return true;
    }

    static bool stack_effect(const InsnInfo &insn, int in_height, int &out_height,
                             const std::string &where) {
        auto require = [&](int n) -> bool {
            if (in_height < n) {
                fail(where, insn.pc, "stack underflow");
                return false;
            }
            return true;
        };
        auto set_delta = [&](int pop_count, int push_count) -> bool {
            if (!require(pop_count)) {
                return false;
            }
            out_height = in_height - pop_count + push_count;
            return true;
        };

        switch (insn.op) {
            case OpCode::OP_LOAD_CONST:
            case OpCode::OP_LOAD_VAR:
            case OpCode::OP_LOAD_GLOBAL:
            case OpCode::OP_LOAD_CAPTURE:
            case OpCode::OP_LOAD_NONE:
            case OpCode::OP_LOAD_TRUE:
            case OpCode::OP_LOAD_FALSE:
            case OpCode::OP_LOAD_ZERO:
            case OpCode::OP_LOAD_ONE:
            case OpCode::OP_LOAD_THIS:
            case OpCode::OP_MAKE_REGEX:
                return set_delta(0, 1);

            case OpCode::OP_STORE_VAR:
            case OpCode::OP_STORE_GLOBAL:
            case OpCode::OP_STORE_CAPTURE:
            case OpCode::OP_STR_APPEND_VAR:
            case OpCode::OP_STR_APPEND_GLOBAL:
            case OpCode::OP_CHECK_TYPE:
                return set_delta(1, 1); // peek/validate/store/append, value remains on stack

            case OpCode::OP_POP:
            case OpCode::OP_THROW:
                return set_delta(1, 0);

            case OpCode::OP_DUP:
                return set_delta(1, 2);

            case OpCode::OP_NEG:
            case OpCode::OP_FORMAT_VALUE:
            case OpCode::OP_NOT:
            case OpCode::OP_BIT_NOT:
            case OpCode::OP_SPAWN:
            case OpCode::OP_GET_PROPERTY:
            case OpCode::OP_ITER_ARRAY:
                return set_delta(1, 1);

            case OpCode::OP_MAKE_ITERATOR:
            case OpCode::OP_MAKE_ITERATOR_KV:
                return set_delta(1, 2);

            case OpCode::OP_ADD:
            case OpCode::OP_SUB:
            case OpCode::OP_MUL:
            case OpCode::OP_DIV:
            case OpCode::OP_MOD:
            case OpCode::OP_POW:
            case OpCode::OP_STR_CONCAT:
            case OpCode::OP_BIT_AND:
            case OpCode::OP_BIT_OR:
            case OpCode::OP_BIT_XOR:
            case OpCode::OP_LSHIFT:
            case OpCode::OP_RSHIFT:
            case OpCode::OP_EQ:
            case OpCode::OP_NE:
            case OpCode::OP_LT:
            case OpCode::OP_LE:
            case OpCode::OP_GT:
            case OpCode::OP_GE:
            case OpCode::OP_GET_INDEX:
            case OpCode::OP_ARRAY_PUSH:
            case OpCode::OP_ARRAY_SPREAD:
            case OpCode::OP_OBJECT_SPREAD:
            case OpCode::OP_OBJECT_SET:
                return set_delta(2, 1);

            case OpCode::OP_SET_INDEX:
                return set_delta(3, 1);

            case OpCode::OP_SET_PROPERTY:
                return set_delta(2, 1);

            case OpCode::OP_MAKE_ARRAY:
                return set_delta(static_cast<int>(insn.u16_a), 1);

            case OpCode::OP_MAKE_OBJECT:
                return set_delta(static_cast<int>(insn.u16_a) * 2, 1);

            case OpCode::OP_CALL:
                return set_delta(static_cast<int>(insn.u8_a) + 1, 1);

            case OpCode::OP_SELF_TAIL_CALL:
                return set_delta(static_cast<int>(insn.u8_a), 0);

            case OpCode::OP_NEW_INSTANCE:
                return set_delta(static_cast<int>(insn.u8_a), 1);

            case OpCode::OP_CALL_METHOD:
                return set_delta(static_cast<int>(insn.u8_a) + 1, 1);

            case OpCode::OP_CALL_SPREAD:
                return set_delta(2, 1);

            case OpCode::OP_MAKE_CLOSURE:
                return set_delta(0, 1);

            case OpCode::OP_ITER_NEXT:
            case OpCode::OP_ITER_NEXT_KV:
                fail(where, insn.pc, "internal: iterator opcodes require branch-sensitive stack analysis");
                return false;

            case OpCode::OP_JUMP_IF_FALSE:
            case OpCode::OP_JUMP_IF_TRUE:
            case OpCode::OP_JUMP_IF_NONE:
                return set_delta(1, 0);

            case OpCode::OP_JUMP:
            case OpCode::OP_SETUP_TRY:
            case OpCode::OP_POP_TRY:
            case OpCode::OP_BEGIN_CATCH:
            case OpCode::OP_BEGIN_FINALLY:
                out_height = in_height;
                return true;

            case OpCode::OP_RETURN:
                return set_delta(1, 0);
        }
        fail(where, insn.pc, "unknown opcode in stack analysis");
        return false;
    }

    static bool verify_stack_flow(const std::string &where, const std::vector<InsnInfo> &insns,
                                  const std::unordered_map<size_t, size_t> &pc_to_index,
                                  size_t code_size) {
        if (insns.empty()) {
            return true;
        }

        std::vector<int> height_at(insns.size(), -1);
        std::queue<size_t> work;
        height_at[0] = 0;
        work.push(0);

        while (!work.empty()) {
            size_t idx = work.front();
            work.pop();

            const InsnInfo &insn = insns[idx];
            int in_height = height_at[idx];

            if (insn.op == OpCode::OP_ITER_NEXT || insn.op == OpCode::OP_ITER_NEXT_KV) {
                if (in_height < 2) {
                    fail(where, insn.pc, "stack underflow");
                    return false;
                }
                if (idx + 1 >= insns.size()) {
                    fail(where, insn.pc, "iterator opcode must be followed by a conditional jump");
                    return false;
                }
                const InsnInfo &branch = insns[idx + 1];
                if (branch.pc != insn.pc + insn.size ||
                    (branch.op != OpCode::OP_JUMP_IF_FALSE && branch.op != OpCode::OP_JUMP_IF_TRUE && branch.op != OpCode::OP_JUMP_IF_NONE)) {
                    fail(where, insn.pc, "iterator opcode must be followed by a conditional jump");
                    return false;
                }

                const int false_height = in_height;
                const int true_height = in_height + (insn.op == OpCode::OP_ITER_NEXT ? 1 : 2);
                int64_t target = jump_target_after_operand(branch.operand_pc, 2, branch.jump_a);
                const size_t after_branch_pc = branch.pc + branch.size;

                if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, static_cast<size_t>(target), false_height, branch.pc)) {
                    return false;
                }
                if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, after_branch_pc, true_height, branch.pc)) {
                    return false;
                }
                continue;
            }

            int out_height = in_height;
            if (!stack_effect(insn, in_height, out_height, where)) {
                return false;
            }
            if (out_height < 0) {
                fail(where, insn.pc, "negative stack height");
                return false;
            }

            const size_t next_pc = insn.pc + insn.size;
            switch (insn.op) {
                case OpCode::OP_RETURN:
                case OpCode::OP_THROW:
                case OpCode::OP_SELF_TAIL_CALL:
                    break;

                case OpCode::OP_JUMP: {
                    int64_t target = jump_target_after_operand(insn.operand_pc, 2, insn.jump_a);
                    if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, static_cast<size_t>(target), out_height, insn.pc)) {
                        return false;
                    }
                    break;
                }

                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE:
                case OpCode::OP_JUMP_IF_NONE: {
                    int64_t target = jump_target_after_operand(insn.operand_pc, 2, insn.jump_a);
                    if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, static_cast<size_t>(target), out_height, insn.pc)) {
                        return false;
                    }
                    if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, next_pc, out_height, insn.pc)) {
                        return false;
                    }
                    break;
                }

                case OpCode::OP_SETUP_TRY: {
                    // Normal fallthrough keeps stack height, EH entry gets one pushed error value from dispatch_throw().
                    // Finally entry is conservatively treated as the same stack height as normal fallthrough.
                    int64_t catch_target = jump_target_after_operand(insn.operand_pc, 4, insn.jump_a);
                    int64_t finally_target = jump_target_after_operand(insn.operand_pc, 4, insn.jump_b);
                    if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, next_pc, out_height, insn.pc)) {
                        return false;
                    }
                    if (catch_target != static_cast<int64_t>(next_pc) && catch_target != static_cast<int64_t>(code_size)) {
                        if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, static_cast<size_t>(catch_target), out_height + 1, insn.pc)) {
                            return false;
                        }
                    }
                    if (finally_target != static_cast<int64_t>(next_pc) && finally_target != static_cast<int64_t>(code_size)) {
                        if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, static_cast<size_t>(finally_target), out_height, insn.pc)) {
                            return false;
                        }
                    }
                    break;
                }

                default:
                    if (!add_successor(where, insns, pc_to_index, code_size, work, height_at, next_pc, out_height, insn.pc)) {
                        return false;
                    }
                    break;
            }
        }

        return true;
    }

    static bool verify_function(const Chunk &chunk, const FunctionMeta &fn, size_t n_strs, size_t n_funcs, size_t fn_idx) {
        const ByteArray &code = fn.code;
        const size_t n_code = code.size();
        const size_t n_consts = fn.constants.size();
        const size_t n_locals = fn.var_names.size();
        const size_t n_caps = fn.capture_count;
        const std::string where = fn.name.empty() ? std::string("<function #") + std::to_string(fn_idx) + ">" : fn.name;

        for (size_t i = 0; i < n_consts; i++) {
            const Constant &c = fn.constants[i];
            if (c.type == Constant::Type::STRING && c.string_idx >= n_strs) {
                fail(where + ": constant[" + std::to_string(i) + "] has out-of-range string_idx");
                return false;
            }
            if (c.type == Constant::Type::FUNCTION && c.func_idx >= n_funcs) {
                fail(where + ": constant[" + std::to_string(i) + "] has out-of-range func_idx");
                return false;
            }
        }

        std::vector<InsnInfo> insns;
        std::unordered_map<size_t, size_t> pc_to_index;

        size_t pc = 0;
        while (pc < n_code) {
            InsnInfo info;
            info.pc = pc;
            info.op_byte = code[pc];
            info.op = static_cast<OpCode>(info.op_byte);
            pc++;
            info.operand_pc = pc;

            const int osz = opcode_operand_size(info.op);
            if (osz < 0) {
                fail(where, info.pc, "unknown opcode " + std::to_string(static_cast<int>(info.op_byte)));
                return false;
            }
            if (pc + static_cast<size_t>(osz) > n_code) {
                fail(where, info.pc, "operand bytes truncated by end of code");
                return false;
            }

            switch (info.op) {
                case OpCode::OP_LOAD_CONST: {
                    uint16_t idx = read_u16_be(&code[pc]);
                    info.u16_a = idx;
                    if (idx >= n_consts) {
                        fail(where, info.pc, "OP_LOAD_CONST: constant index out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_LOAD_VAR:
                case OpCode::OP_STORE_VAR:
                case OpCode::OP_STR_APPEND_VAR: {
                    uint16_t idx = read_u16_be(&code[pc]);
                    info.u16_a = idx;
                    if (idx >= n_locals) {
                        fail(where, info.pc, "local variable index out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_LOAD_GLOBAL:
                case OpCode::OP_STORE_GLOBAL:
                case OpCode::OP_STR_APPEND_GLOBAL:
                case OpCode::OP_OBJECT_SET:
                case OpCode::OP_GET_PROPERTY:
                case OpCode::OP_SET_PROPERTY: {
                    uint16_t idx = read_u16_be(&code[pc]);
                    info.u16_a = idx;
                    if (idx >= n_strs) {
                        fail(where, info.pc, "name/string index out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_LOAD_CAPTURE:
                case OpCode::OP_STORE_CAPTURE: {
                    uint16_t idx = read_u16_be(&code[pc]);
                    info.u16_a = idx;
                    if (idx >= n_caps) {
                        fail(where, info.pc, "capture index out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_MAKE_ARRAY:
                case OpCode::OP_MAKE_OBJECT: {
                    info.u16_a = read_u16_be(&code[pc]);
                    break;
                }
                case OpCode::OP_SELF_TAIL_CALL: {
                    info.u8_a = code[pc];
                    break;
                }
                case OpCode::OP_CALL: {
                    info.u8_a = code[pc];
                    uint16_t label_idx = read_u16_be(&code[pc + 1]);
                    if (label_idx >= n_strs) {
                        fail(where, info.pc, "OP_CALL: callee label index out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_CALL_SPREAD: {
                    uint16_t label_idx = read_u16_be(&code[pc]);
                    if (label_idx >= n_strs) {
                        fail(where, info.pc, "OP_CALL_SPREAD: callee label index out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_NEW_INSTANCE:
                case OpCode::OP_CALL_METHOD: {
                    uint16_t nidx = read_u16_be(&code[pc]);
                    info.u16_a = nidx;
                    info.u8_a = code[pc + 2];
                    if (nidx >= n_strs) {
                        fail(where, info.pc, "name index out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_JUMP:
                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE:
                case OpCode::OP_JUMP_IF_NONE: {
                    info.jump_a = read_i16_be(&code[pc]);
                    int64_t target = jump_target_after_operand(pc, 2, info.jump_a);
                    if (target < 0 || target > static_cast<int64_t>(n_code)) {
                        fail(where, info.pc, "jump target outside function body");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_SETUP_TRY: {
                    info.jump_a = read_i16_be(&code[pc]);
                    info.jump_b = read_i16_be(&code[pc + 2]);
                    int64_t catch_tgt = jump_target_after_operand(pc, 4, info.jump_a);
                    int64_t finally_tgt = jump_target_after_operand(pc, 4, info.jump_b);
                    if (catch_tgt < 0 || catch_tgt > static_cast<int64_t>(n_code)) {
                        fail(where, info.pc, "OP_SETUP_TRY: catch target out of range");
                        return false;
                    }
                    if (finally_tgt < 0 || finally_tgt > static_cast<int64_t>(n_code)) {
                        fail(where, info.pc, "OP_SETUP_TRY: finally target out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_MAKE_CLOSURE: {
                    uint16_t fidx = read_u16_be(&code[pc]);
                    info.u16_a = fidx;
                    if (fidx >= n_funcs) {
                        fail(where, info.pc, "OP_MAKE_CLOSURE: func_idx out of range");
                        return false;
                    }
                    uint8_t cap_count = code[pc + 2];
                    info.u8_a = cap_count;
                    if (pc + 3 + static_cast<size_t>(cap_count) * 3 > n_code) {
                        fail(where, info.pc, "OP_MAKE_CLOSURE: capture descriptors truncated");
                        return false;
                    }
                    for (uint8_t ci = 0; ci < cap_count; ci++) {
                        uint8_t source = code[pc + 3 + ci * 3];
                        uint16_t cidx = read_u16_be(&code[pc + 3 + ci * 3 + 1]);
                        if (source == 0) {
                            if (cidx >= n_locals) {
                                fail(where, info.pc, "OP_MAKE_CLOSURE: capture source=local idx out of range");
                                return false;
                            }
                        } else if (source == 1) {
                            if (cidx >= n_caps) {
                                fail(where, info.pc, "OP_MAKE_CLOSURE: capture source=capture idx out of range");
                                return false;
                            }
                        } else if (source == 2) {
                            if (cidx >= n_strs) {
                                fail(where, info.pc, "OP_MAKE_CLOSURE: capture global string idx out of range");
                                return false;
                            }
                        } else {
                            fail(where, info.pc, "OP_MAKE_CLOSURE: unknown capture source");
                            return false;
                        }
                    }
                    info.size = 1 + 3 + static_cast<size_t>(cap_count) * 3;
                    pc += 3 + static_cast<size_t>(cap_count) * 3;
                    pc_to_index[info.pc] = insns.size();
                    insns.push_back(info);
                    continue;
                }
                case OpCode::OP_MAKE_REGEX: {
                    uint16_t pat = read_u16_be(&code[pc]);
                    uint16_t flg = read_u16_be(&code[pc + 2]);
                    if (pat >= n_strs || flg >= n_strs) {
                        fail(where, info.pc, "OP_MAKE_REGEX: pattern/flags string idx out of range");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_CHECK_TYPE: {
                    uint16_t tidx = read_u16_be(&code[pc]);
                    uint8_t ctx = code[pc + 2];
                    info.u16_a = tidx;
                    info.u8_a = ctx;
                    if (tidx >= n_strs) {
                        fail(where, info.pc, "OP_CHECK_TYPE: type name idx out of range");
                        return false;
                    }
                    if (ctx > 2) {
                        fail(where, info.pc, "OP_CHECK_TYPE: context byte out of range (0-2)");
                        return false;
                    }
                    break;
                }
                default:
                    break;
            }

            info.size = 1 + static_cast<size_t>(osz);
            pc += static_cast<size_t>(osz);
            pc_to_index[info.pc] = insns.size();
            insns.push_back(info);
        }

        if (pc != n_code) {
            fail(where, pc, "internal: pc walked past code end");
            return false;
        }

        for (const auto &insn : insns) {
            switch (insn.op) {
                case OpCode::OP_JUMP:
                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE:
                case OpCode::OP_JUMP_IF_NONE: {
                    size_t target = static_cast<size_t>(jump_target_after_operand(insn.operand_pc, 2, insn.jump_a));
                    if (!is_instruction_boundary(target, pc_to_index, n_code)) {
                        fail(where, insn.pc, "jump target is not an instruction boundary");
                        return false;
                    }
                    break;
                }
                case OpCode::OP_SETUP_TRY: {
                    size_t catch_target = static_cast<size_t>(jump_target_after_operand(insn.operand_pc, 4, insn.jump_a));
                    size_t finally_target = static_cast<size_t>(jump_target_after_operand(insn.operand_pc, 4, insn.jump_b));
                    if (!is_instruction_boundary(catch_target, pc_to_index, n_code)) {
                        fail(where, insn.pc, "OP_SETUP_TRY: catch target is not an instruction boundary");
                        return false;
                    }
                    if (!is_instruction_boundary(finally_target, pc_to_index, n_code)) {
                        fail(where, insn.pc, "OP_SETUP_TRY: finally target is not an instruction boundary");
                        return false;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        return verify_stack_flow(where, insns, pc_to_index, n_code);
    }
};

} // namespace bytecode
} // namespace nari
