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
    static bool verify(const Chunk &chunk);

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

    static void fail(const std::string &msg);

    static void fail(const std::string &fn, size_t pc, const std::string &msg);

    static uint16_t read_u16_be(const uint8_t *p);

    static int16_t read_i16_be(const uint8_t *p);

    // pc -> instruction index, as a flat table indexed by byte offset rather than a hash map.
    using PcIndex = std::vector<uint32_t>;
    static constexpr uint32_t kNoInsn = UINT32_MAX;

    static bool is_instruction_boundary(size_t pc, const PcIndex &pc_to_index, size_t code_size);

    static int64_t jump_target_after_operand(size_t operand_pc, size_t operand_size, int16_t rel);

    static bool add_successor(
        const std::string &where, const std::vector<InsnInfo> &insns, const PcIndex &pc_to_index, size_t code_size,
        std::queue<size_t> &work, std::vector<int> &height_at, size_t target_pc, int out_height, size_t from_pc
    );

    static bool stack_effect(const InsnInfo &insn, int in_height, int &out_height, const std::string &where);

    static bool
    verify_stack_flow(const std::string &where, const std::vector<InsnInfo> &insns, const PcIndex &pc_to_index, size_t code_size);

    static bool verify_function(const Chunk &chunk, const FunctionMeta &fn, size_t n_strs, size_t n_funcs, size_t fn_idx);
};

} // namespace bytecode
} // namespace nari
