#pragma once

#include "compiler_support.h" // NARI_UNLIKELY
#include "core_types.h"
#include "int_overflow.h" // mul_overflow_i48 and friends
#include "parser_api.h"
#include "runtime.h"
#include <cmath> // std::nan, std::fmod
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#ifndef DISABLE_JIT
#include "trace_jit.h"
#endif

namespace nari {
struct Function;
namespace jit {
class MethodJITBase;
class AsmJITMethodCompiler;
} // namespace jit
} // namespace nari

namespace nari {
namespace bytecode {

#define OPCODE_LIST(X)                                                                                                                     \
    X(LOAD_CONST, 2, false)                                                                                                                \
    X(LOAD_VAR, 2, false)                                                                                                                  \
    X(STORE_VAR, 2, false)                                                                                                                 \
    X(LOAD_GLOBAL, 2, false)                                                                                                               \
    X(STORE_GLOBAL, 2, false)                                                                                                              \
    X(POP, 0, false)                                                                                                                       \
    X(DUP, 0, false)                                                                                                                       \
    X(LOAD_NONE, 0, false)                                                                                                                 \
    X(LOAD_TRUE, 0, false)                                                                                                                 \
    X(LOAD_FALSE, 0, false)                                                                                                                \
    X(LOAD_ZERO, 0, false)                                                                                                                 \
    X(LOAD_ONE, 0, false)                                                                                                                  \
    X(ADD, 0, false)                                                                                                                       \
    X(SUB, 0, false)                                                                                                                       \
    X(MUL, 0, false)                                                                                                                       \
    X(DIV, 0, false)                                                                                                                       \
    X(MOD, 0, false)                                                                                                                       \
    X(POW, 0, false)                                                                                                                       \
    X(NEG, 0, false)                                                                                                                       \
    X(STR_CONCAT, 0, false)                                                                                                                \
    X(STR_APPEND_VAR, 2, false)                                                                                                            \
    X(STR_APPEND_GLOBAL, 2, false)                                                                                                         \
    X(BIT_AND, 0, false)                                                                                                                   \
    X(BIT_OR, 0, false)                                                                                                                    \
    X(BIT_XOR, 0, false)                                                                                                                   \
    X(BIT_NOT, 0, false)                                                                                                                   \
    X(LSHIFT, 0, false)                                                                                                                    \
    X(RSHIFT, 0, false)                                                                                                                    \
    X(NOT, 0, false)                                                                                                                       \
    X(EQ, 0, false)                                                                                                                        \
    X(NE, 0, false)                                                                                                                        \
    X(LT, 0, false)                                                                                                                        \
    X(LE, 0, false)                                                                                                                        \
    X(GT, 0, false)                                                                                                                        \
    X(GE, 0, false)                                                                                                                        \
    X(JUMP, 2, false)                                                                                                                      \
    X(JUMP_IF_FALSE, 2, false)                                                                                                             \
    X(JUMP_IF_TRUE, 2, false)                                                                                                              \
    X(JUMP_IF_NONE, 2, false)                                                                                                              \
    X(CALL, 3, false)                                                                                                                      \
    X(SELF_TAIL_CALL, 1, false)                                                                                                            \
    X(RETURN, 0, false)                                                                                                                    \
    X(MAKE_CLOSURE, 4, true)                                                                                                               \
    X(SPAWN, 0, false)                                                                                                                     \
    X(MAKE_ARRAY, 2, false)                                                                                                                \
    X(MAKE_OBJECT, 2, false)                                                                                                               \
    X(ARRAY_PUSH, 0, false)                                                                                                                \
    X(ARRAY_SPREAD, 0, false)                                                                                                              \
    X(OBJECT_SPREAD, 0, false)                                                                                                             \
    X(OBJECT_SET, 2, false)                                                                                                                \
    X(CALL_SPREAD, 2, false)                                                                                                               \
    X(MAKE_REGEX, 4, false)                                                                                                                \
    X(GET_INDEX, 0, false)                                                                                                                 \
    X(SET_INDEX, 0, false)                                                                                                                 \
    X(GET_PROPERTY, 2, false)                                                                                                              \
    X(JS_GET_PROP_STATIC, 2, false)                                                                                                        \
    X(SET_PROPERTY, 2, false)                                                                                                              \
    X(MAKE_ITERATOR, 0, false)                                                                                                             \
    X(ITER_NEXT, 0, false)                                                                                                                 \
    X(MAKE_ITERATOR_KV, 0, false)                                                                                                          \
    X(ITER_NEXT_KV, 0, false)                                                                                                              \
    X(LOAD_CAPTURE, 2, false)                                                                                                              \
    X(STORE_CAPTURE, 2, false)                                                                                                             \
    X(THROW, 0, false)                                                                                                                     \
    X(SETUP_TRY, 4, false)                                                                                                                 \
    X(POP_TRY, 0, false)                                                                                                                   \
    X(BEGIN_CATCH, 0, false)                                                                                                               \
    X(BEGIN_FINALLY, 0, false)                                                                                                             \
    X(NEW_INSTANCE, 3, false)                                                                                                              \
    X(LOAD_THIS, 0, false)                                                                                                                 \
    X(CALL_METHOD, 3, false)                                                                                                               \
    X(CHECK_TYPE, 3, false)                                                                                                                \
    X(FORMAT_VALUE, 2, false)                                                                                                              \
    X(ITER_ARRAY, 0, false)                                                                                                                \
    X(JS_TRUTHY, 0, false)                                                                                                                 \
    X(JS_BIT_AND, 0, false)                                                                                                                \
    X(JS_BIT_OR, 0, false)                                                                                                                 \
    X(JS_BIT_XOR, 0, false)                                                                                                                \
    X(JS_BIT_NOT, 0, false)                                                                                                                \
    X(JS_SHL, 0, false)                                                                                                                    \
    X(JS_SHR, 0, false)                                                                                                                    \
    X(JS_USHR, 0, false)                                                                                                                   \
    X(STRICT_EQ, 0, false)                                                                                                                 \
    X(STRICT_NE, 0, false)                                                                                                                 \
    /* appended last on purpose: opcode numbers are serialized into .naric files, */                                                       \
    /* so new opcodes go at the end or every existing compiled artifact breaks.   */                                                       \
    X(JS_SET_PROP_STATIC, 2, false)                                                                                                        \
    X(JS_POSTINC, 2, false)                                                                                                                \
    /* CLOSE_UPVALUES <first_slot>: drop this frame's open upvalue cells for locals   */                                                   \
    /* at or above first_slot */                                                                                                           \
    X(CLOSE_UPVALUES, 2, false)

enum class OpCode : uint8_t {
#define X(name, operands, variable) OP_##name,
    OPCODE_LIST(X)
#undef X
};

struct OpcodeInfo {
    const char *name;
    uint8_t operand_size;
    bool variable_size;
};

inline constexpr OpcodeInfo OPCODE_INFO[] = {
#define X(name, operands, variable) { #name, operands, variable },
    OPCODE_LIST(X)
#undef X
};

inline constexpr const OpcodeInfo *opcode_info(OpCode op) {
    size_t index = (size_t)op;
    return index < sizeof(OPCODE_INFO) / sizeof(OPCODE_INFO[0]) ? &OPCODE_INFO[index] : nullptr;
}

inline const char *opcode_name(OpCode op) {
    const OpcodeInfo *info = opcode_info(op);
    if (info) {
        return info->name;
    }
    return "???";
}

// Number of fixed operand bytes after the opcode byte.
// Returns -1 for unknown opcodes. OP_MAKE_CLOSURE is variable-length: the
// fixed header is 4 bytes (func_idx:u16 + capture_count:u16), followed by
// capture_count * 3 bytes.
inline int opcode_operand_size(OpCode op) {
    const OpcodeInfo *info = opcode_info(op);
    return info ? info->operand_size : -1;
}

inline size_t opcode_fixed_size(OpCode op) {
    int operands = opcode_operand_size(op);
    return operands < 0 ? 0 : 1 + static_cast<size_t>(operands);
}

template <typename Bytecode>
inline size_t decoded_instruction_size(const Bytecode &code, size_t pc) {
    if (pc >= code.size()) {
        return 0;
    }

    const OpcodeInfo *info = opcode_info(static_cast<OpCode>(code[pc]));
    if (!info) {
        return 0;
    }

    size_t size = 1 + info->operand_size;
    if (size > code.size() - pc) {
        return 0;
    }
    if (info->variable_size) {
        size += static_cast<size_t>((static_cast<uint16_t>(code[pc + 3]) << 8) | code[pc + 4]) * 3;
        if (size > code.size() - pc) {
            return 0;
        }
    }
    return size;
}

struct Constant {
    enum class Type {
        NONE,
        INT,
        FLOAT,
        STRING,
        FUNCTION
    };

    Type type;
    union {
        int64_t as_int;
        double as_float;
        uint32_t string_idx; // index into string table
        uint32_t func_idx;   // index into function table
    };

    Constant() : type(Type::NONE), as_int(0) {
    }

    static Constant make_none() {
        Constant c;
        c.type = Type::NONE;
        return c;
    }

    static Constant make_int(int64_t val) {
        Constant c;
        // Values are NaN-boxed with a 48-bit int payload, so an INT constant outside int48 cannot be materialized.
        // so, promote to float if out of range
        if (!Value::fits_int48(val)) {
            return make_float(static_cast<double>(val));
        }
        c.type = Type::INT;
        c.as_int = val;
        return c;
    }

    static Constant make_float(double val) {
        Constant c;
        c.type = Type::FLOAT;
        c.as_float = val;
        return c;
    }

    static Constant make_string(uint32_t idx) {
        Constant c;
        c.type = Type::STRING;
        c.string_idx = idx;
        return c;
    }

    static Constant make_function(uint32_t idx) {
        Constant c;
        c.type = Type::FUNCTION;
        c.func_idx = idx;
        return c;
    }
};

// Maps a bytecode offset to the source line that generated it.
// Entries are sorted by pc_offset and emitted sparsely (one per statement).
struct LineEntry {
    uint32_t pc_offset;
    int32_t line;
};

struct FunctionMeta {
    // index of this meta within Chunk::functions; set during registration. Replaces
    // pointer arithmetic off functions.data(), which a deque cannot provide.
    uint32_t self_idx = 0;
    std::string name;
    std::string source_file;            // original source filename (for error messages)
    ByteArray code;                     // bytecode instructions
    std::vector<Constant> constants;    // constant pool
    std::vector<std::string> var_names; // local variable names
    std::vector<LineEntry> line_map;    // pc -> source line (sorted, sparse)
    uint8_t param_count;
    uint16_t capture_count;  // number of captured variables (for closures)
    int8_t rest_param_index; // index of rest param (-1 if none)
    bool is_lambda;
    bool js_undefined_params; // omitted parameters use the global JavaScript undefined singleton
    bool strict_mode;         // true when function was compiled under "use strict"
    int8_t return_vt;         // JIT vt of return value: 0=unknown, 1=int, 2=float (set
    // for strict-mode annotated functions)
    JitInlineKind jit_inline_kind = JitInlineKind::None;
    int64_t jit_inline_imm = 0;

    FunctionMeta()
        : param_count(0), capture_count(0), rest_param_index(-1), is_lambda(false), js_undefined_params(false), strict_mode(false),
          return_vt(0) {
    }

    // Return the source line number for a given bytecode offset.
    // Returns the line of the last entry whose pc_offset <= the given offset.
    int resolve_line(size_t pc_offset) const {
        if (line_map.empty()) {
            return 0;
        }
        int line = line_map[0].line;
        for (const auto &e : line_map) {
            if (e.pc_offset > pc_offset) {
                break;
            }
            line = e.line;
        }
        return line;
    }
};

// Result of scanning a FunctionMeta for an inlineable body pattern.
// `imm` holds the constant operand for MulConst/AddConst/SubConst and
// ClosureAddConst; it is unused (0) for the other kinds.
struct InlineClassification {
    JitInlineKind kind;
    int64_t imm;
};

// scan a FunctionMeta body for a trivially inlineable pattern. See
// JitInlineKind in core_types.h for the catalogue of recognised shapes.
inline InlineClassification jit_classify_inline(const FunctionMeta &func_meta) {
    const auto &func_code = func_meta.code;

    // Skip the strict-mode CHECK_TYPE parameter preamble if it exists.
    size_t base = 0;
    if (func_meta.strict_mode) {
        size_t pc = 0;
        while (pc + 11 <= func_code.size()) {
            if ((OpCode)func_code[pc] != OpCode::OP_LOAD_VAR) {
                break;
            }
            if ((OpCode)func_code[pc + 3] != OpCode::OP_CHECK_TYPE) {
                break;
            }
            if (func_code[pc + 6] != 0) {
                break; // ctx=0
            }
            if ((OpCode)func_code[pc + 7] != OpCode::OP_STORE_VAR) {
                break;
            }
            if ((OpCode)func_code[pc + 10] != OpCode::OP_POP) {
                break;
            }
            pc += 11;
        }
        base = pc;
    }

    // Check if offset `end_off` is a valid inlineable body terminator:
    // bare RETURN, or CHECK_TYPE(ctx=1, 4 bytes) then RETURN.
    auto is_return_end = [&](size_t end_off) -> bool {
        if (end_off >= func_code.size()) {
            return false;
        }
        if ((OpCode)func_code[end_off] == OpCode::OP_RETURN) {
            return true;
        }
        if (end_off + 4 < func_code.size() && (OpCode)func_code[end_off] == OpCode::OP_CHECK_TYPE &&
            func_code[end_off + 3] == 1 && // ctx=1 = return check
            (OpCode)func_code[end_off + 4] == OpCode::OP_RETURN) {
            return true;
        }
        return false;
    };

    // 0-arg closure getter: LOAD_CAPTURE(0) [CHECK_TYPE] RETURN.
    if (func_meta.param_count == 0 && base + 3 < func_code.size() && (OpCode)func_code[base] == OpCode::OP_LOAD_CAPTURE &&
        func_code[base + 1] == 0 && func_code[base + 2] == 0 && is_return_end(base + 3)) {
        return { JitInlineKind::Capture0, 0 };
    }

    // 2-arg body: LOAD_VAR(0) LOAD_VAR(1) op [CHECK_TYPE] RETURN
    if (base + 7 <= func_code.size() && (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR && func_code[base + 1] == 0 &&
        func_code[base + 2] == 0 && (OpCode)func_code[base + 3] == OpCode::OP_LOAD_VAR && func_code[base + 4] == 0 &&
        func_code[base + 5] == 1 && is_return_end(base + 7)) {
        switch ((OpCode)func_code[base + 6]) {
            case OpCode::OP_ADD:
                return { JitInlineKind::IntAdd, 0 };
            case OpCode::OP_SUB:
                return { JitInlineKind::IntSub, 0 };
            case OpCode::OP_MUL:
                return { JitInlineKind::IntMul, 0 };
            case OpCode::OP_LT:
                return { JitInlineKind::LT, 0 };
            case OpCode::OP_LE:
                return { JitInlineKind::LE, 0 };
            case OpCode::OP_GT:
                return { JitInlineKind::GT, 0 };
            case OpCode::OP_GE:
                return { JitInlineKind::GE, 0 };
            case OpCode::OP_EQ:
                return { JitInlineKind::EQ, 0 };
            case OpCode::OP_NE:
                return { JitInlineKind::NE, 0 };
            default:
                break;
        }
    }
    // 1-arg const: LOAD_VAR(0) LOAD_CONST(k) op [CHECK_TYPE] RETURN
    if (base + 7 <= func_code.size() && (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR && func_code[base + 1] == 0 &&
        func_code[base + 2] == 0 && (OpCode)func_code[base + 3] == OpCode::OP_LOAD_CONST && is_return_end(base + 7)) {
        uint16_t cidx = (uint16_t(func_code[base + 4]) << 8) | func_code[base + 5];
        if (cidx < func_meta.constants.size() && func_meta.constants[cidx].type == Constant::Type::INT) {
            int64_t imm = func_meta.constants[cidx].as_int;
            switch ((OpCode)func_code[base + 6]) {
                case OpCode::OP_MUL:
                    return { JitInlineKind::MulConst, imm };
                case OpCode::OP_ADD:
                    return { JitInlineKind::AddConst, imm };
                case OpCode::OP_SUB:
                    return { JitInlineKind::SubConst, imm };
                default:
                    break;
            }
        }
    }
    // 1-arg const (commutative): LOAD_CONST(k) LOAD_VAR(0) op [CHECK_TYPE] RETURN
    if (base + 7 <= func_code.size() && (OpCode)func_code[base + 0] == OpCode::OP_LOAD_CONST &&
        (OpCode)func_code[base + 3] == OpCode::OP_LOAD_VAR && func_code[base + 4] == 0 && func_code[base + 5] == 0 &&
        is_return_end(base + 7)) {
        uint16_t cidx = (uint16_t(func_code[base + 1]) << 8) | func_code[base + 2];
        if (cidx < func_meta.constants.size() && func_meta.constants[cidx].type == Constant::Type::INT) {
            int64_t imm = func_meta.constants[cidx].as_int;
            switch ((OpCode)func_code[base + 6]) {
                case OpCode::OP_MUL:
                    return { JitInlineKind::MulConst, imm };
                case OpCode::OP_ADD:
                    return { JitInlineKind::AddConst, imm };
                default:
                    break;
            }
        }
    }
    // 0-arg closure that increments capture[0] by 1 and returns it.
    // Matches: LOAD_CAPTURE(0) LOAD_ONE ADD STORE_CAPTURE(0) POP
    //  LOAD_CAPTURE(0) [CHECK_TYPE(ctx=1)] RETURN
    // (13 or 17 bytes; bytecode compiler may append LOAD_NONE+RETURN as
    // unreachable)
    if (func_code.size() >= 13 && (OpCode)func_code[0] == OpCode::OP_LOAD_CAPTURE && func_code[1] == 0 && func_code[2] == 0 &&
        (OpCode)func_code[3] == OpCode::OP_LOAD_ONE && (OpCode)func_code[4] == OpCode::OP_ADD &&
        (OpCode)func_code[5] == OpCode::OP_STORE_CAPTURE && func_code[6] == 0 && func_code[7] == 0 &&
        (OpCode)func_code[8] == OpCode::OP_POP && (OpCode)func_code[9] == OpCode::OP_LOAD_CAPTURE && func_code[10] == 0 &&
        func_code[11] == 0 && is_return_end(12)) {
        return { JitInlineKind::ClosureInc, 1 };
    }
    // 0-arg closure that adds a constant to capture[0] and returns it.
    // Matches: LOAD_CAPTURE(0) LOAD_CONST(k) ADD STORE_CAPTURE(0) POP
    //  LOAD_CAPTURE(0) [CHECK_TYPE(ctx=1)] RETURN
    if (func_code.size() >= 15 && (OpCode)func_code[0] == OpCode::OP_LOAD_CAPTURE && func_code[1] == 0 && func_code[2] == 0 &&
        (OpCode)func_code[3] == OpCode::OP_LOAD_CONST && (OpCode)func_code[6] == OpCode::OP_ADD &&
        (OpCode)func_code[7] == OpCode::OP_STORE_CAPTURE && func_code[8] == 0 && func_code[9] == 0 &&
        (OpCode)func_code[10] == OpCode::OP_POP && (OpCode)func_code[11] == OpCode::OP_LOAD_CAPTURE && func_code[12] == 0 &&
        func_code[13] == 0 && is_return_end(14)) {
        uint16_t cidx = (uint16_t(func_code[4]) << 8) | func_code[5];
        if (cidx < func_meta.constants.size() && func_meta.constants[cidx].type == Constant::Type::INT) {
            return { JitInlineKind::ClosureAddConst, func_meta.constants[cidx].as_int };
        }
    }
    // 1-arg identity: LOAD_VAR(0) [CHECK_TYPE] RETURN  (4+ bytes)
    if (base + 3 < func_code.size() && (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR && func_code[base + 1] == 0 &&
        func_code[base + 2] == 0 && is_return_end(base + 3)) {
        return { JitInlineKind::Identity, 0 };
    }
    // 1-arg negate: LOAD_VAR(0) NEG [CHECK_TYPE] RETURN
    if (base + 4 < func_code.size() && (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR && func_code[base + 1] == 0 &&
        func_code[base + 2] == 0 && (OpCode)func_code[base + 3] == OpCode::OP_NEG && is_return_end(base + 4)) {
        return { JitInlineKind::Negate, 0 };
    }
    return { JitInlineKind::None, 0 };
}

// serialized type field for FFI struct support
struct TypeFieldInfo {
    std::string name;
    std::string type_name;
    bool is_array;
    uint64_t fixed_array_count;

    TypeFieldInfo() : is_array(false), fixed_array_count(0) {
    }
    TypeFieldInfo(std::string n, std::string t, bool arr = false, uint64_t fixed_count = 0)
        : name(std::move(n)), type_name(std::move(t)), is_array(arr), fixed_array_count(fixed_count) {
    }
};

struct TypeInfo {
    std::string name;
    bool is_union = false;
    std::vector<TypeFieldInfo> fields;
    std::string alias_target; // non-empty if this is a type alias
};

// bytecode chunk (compilation unit, script, or module)
struct Chunk {
    // deque, not vector: eval() appends functions to a chunk that is already running,
    // and live CallFrames hold FunctionMeta* into this container. deque::push_back
    // never invalidates references to existing elements; vector reallocation would
    // dangle the currently-executing frame's `function` pointer.
    std::deque<FunctionMeta> functions; // all functions in this chunk
    std::vector<std::string> strings;
    std::vector<TypeInfo> types; // FFI type decls
    uint32_t main_func_idx;

    // Compiled class methods and initializers. Keyed by AST pointer, not name:
    // ClassDecls live in the parser registry for the whole process, and
    // bc_find_method already hands back the *defining* class's ClassMethod, so a
    // pointer key gets inherited-method dispatch for free with no name mangling.
    // Not serialized: .naric has no class support (see BytecodeSerializer).
    std::unordered_map<const nari::ClassMethod *, uint32_t> method_func_idx;
    std::unordered_map<const nari::ClassDecl *, uint32_t> class_init_idx;        // field defaults + ctor forward
    std::unordered_map<const nari::ClassDecl *, uint32_t> class_static_init_idx; // static field defaults

    // Lazily-materialized shared immutable Value for each string-table entry, so a
    // string-literal LOAD_CONST in a loop reuses one StringObj instead of
    // re-allocating + copying every iteration. Indexed by string-table index.
    std::vector<Value> const_string_cache;
    std::vector<uint8_t> const_string_valid;

    Chunk() : main_func_idx(0) {
    }

    // Shared immutable Value for string-table entry `sidx` (lazily materialized).
    // LOAD_CONST of a string literal reuses this instead of re-allocating.
    const Value &get_const_string(uint32_t sidx) {
        if (const_string_valid.size() != strings.size()) {
            const_string_cache.assign(strings.size(), Value());
            const_string_valid.assign(strings.size(), 0);
        }
        if (!const_string_valid[sidx]) {
            const_string_cache[sidx] = Value::make_const_string(strings[sidx]);
            const_string_valid[sidx] = 1;
        }
        return const_string_cache[sidx];
    }

    uint32_t add_string(const std::string &str) {
        auto it = string_index.find(str);
        if (it != string_index.end()) {
            return it->second;
        }
        uint32_t idx = strings.size();
        strings.push_back(str);
        string_index[str] = idx;
        return idx;
    }

  private:
    std::unordered_map<std::string, uint32_t> string_index; // dedup index
};

struct TryHandler {
    size_t catch_ip;    // IP offset for catch block
    size_t finally_ip;  // IP offset for finally block
    size_t stack_depth; // stack depth when try was entered
    size_t frame_depth; // frame depth when try was entered
};

// Frames hold a handful of open upvalue cells at most, so a flat vector with a
// linear scan beats a hash table: no bucket array, no per-entry node alloc.
// Kept behind the same unique_ptr so CallFrame's layout and null checks are unchanged.
typedef std::vector<std::pair<uint16_t, CellRef>> CaptureMap;

struct CallFrame {
    FunctionMeta *function;
    uint8_t *ip;
    size_t slot_base; // base index for locals in stack
    CapturesList captures;
    Value closure_root;
    Value receiver;

    CallFrame()
        : function(nullptr), ip(nullptr), slot_base(0), closure_root(Value::none()), receiver(Value::none()),
          inline_upvalue_idx(UINT16_MAX) {
    }

    // get or create upvalue cell for a local variable
    CellRef get_or_create_cell(uint16_t local_idx, const Value &val) {
        if (inline_upvalue_idx == local_idx) {
            return inline_upvalue;
        }
        if (inline_upvalue_idx == UINT16_MAX) {
            inline_upvalue_idx = local_idx;
            inline_upvalue = CellRef::make(val);
            return inline_upvalue;
        }
        if (!open_upvalues) {
            open_upvalues = std::make_unique<CaptureMap>();
        }
        for (auto &entry : *open_upvalues) {
            if (entry.first == local_idx) {
                return entry.second;
            }
        }
        auto cell = CellRef::make(val);
        open_upvalues->emplace_back(local_idx, cell);
        return cell;
    }

    // Drops open upvalue cells for locals at or above `first_slot`. Emitted at the end of
    // a loop iteration so the next iteration's closures capture fresh cells: without this,
    // cells are cached per slot for the whole frame and every iteration shares one cell.
    // single_capture_cache needs no invalidation here: jit_make_closure/OP_MAKE_CLOSURE
    // validate it by cell identity, so a reissued fresh cell misses and rebuilds.
    void close_upvalues_from(uint16_t first_slot) {
        if (inline_upvalue_idx != UINT16_MAX && inline_upvalue_idx >= first_slot) {
            inline_upvalue_idx = UINT16_MAX;
            inline_upvalue.reset();
        }
        if (open_upvalues && !open_upvalues->empty()) {
            for (size_t i = open_upvalues->size(); i-- > 0;) {
                if ((*open_upvalues)[i].first >= first_slot) {
                    open_upvalues->erase(open_upvalues->begin() + (ptrdiff_t)i);
                }
            }
        }
    }

    Value *find_open_upvalue(uint16_t local_idx) {
        if (inline_upvalue_idx == local_idx) {
            return inline_upvalue.get();
        }
        if (open_upvalues) {
            for (const auto &entry : *open_upvalues) {
                if (entry.first == local_idx) {
                    return entry.second.get();
                }
            }
        }
        return nullptr;
    }

    // returns the number of open upvalue cells (0 when not allocated).
    size_t upvalue_count() const {
        return (inline_upvalue ? 1 : 0) + (open_upvalues ? open_upvalues->size() : 0);
    }

    uint16_t inline_upvalue_idx;
    CellRef inline_upvalue;

    // lazily-allocated upvalue map, null for the common case (no closures in frame).
    std::unique_ptr<CaptureMap> open_upvalues;

    // Consecutive closures commonly capture the same single cell.
    // The frame already owns that cell, so it can retain and reuse the environment too.
    CapturesList single_capture_cache;
};

using FrameArray = OwnedArray<CallFrame>;
static_assert(std::is_standard_layout<FrameArray>::value, "FrameArray layout is part of the JIT ABI");

struct JsGetPropStaticIC {
    const ObjectShape *shape = nullptr;
    uint32_t slot = 0;
    uint64_t lazy_mask = 0;
    const ObjectShape *shape2 = nullptr;
    uint32_t slot2 = 0;
    uint64_t lazy_mask2 = 0;
};
using JsGetPropStaticICArray = OwnedArray<JsGetPropStaticIC>;
static_assert(std::is_standard_layout<JsGetPropStaticIC>::value, "JsGetPropStaticIC layout is part of the JIT ABI");
static_assert(std::is_standard_layout<JsGetPropStaticICArray>::value, "JsGetPropStaticICArray layout is part of the JIT ABI");

class VM {
#ifndef DISABLE_JIT
    friend class nari::jit::AsmJITMethodCompiler;
#endif
    // JIT helpers and internal code access stack/frame directly
  public:
    Array stack;
    FrameArray frames;
    Chunk *chunk;
    CallFrame &current_frame() {
        return frames.back();
    }
    FunctionMeta *current_function() {
        return current_frame().function;
    }
    CallFrame &push_jit_frame(FunctionMeta *function, size_t slot_base, const Value &closure_root) {
        if (NARI_UNLIKELY(frames.storage_end == frames.storage_capacity)) {
            frames.emplace_back();
        } else {
            CallFrame *frame = frames.storage_end++;
            if (NARI_UNLIKELY(frame->captures || frame->inline_upvalue || frame->open_upvalues || frame->single_capture_cache)) {
                frame->captures.reset();
                frame->inline_upvalue.reset();
                frame->inline_upvalue_idx = UINT16_MAX;
                frame->open_upvalues.reset();
                frame->single_capture_cache.reset();
            }
        }
        CallFrame &frame = frames.back();
        frame.function = function;
        frame.ip = function->code.data();
        frame.slot_base = slot_base;
        frame.closure_root = closure_root;
        frame.receiver = Value::none();
        return frame;
    }
    void push(const Value &val) {
        stack.push_back(val);
    }
    void push(Value &&val) {
        stack.push_back(std::move(val));
    }
    Value pop() {
        Value val = std::move(stack.back());
        stack.pop_back();
        return val;
    }
    // Safe-point usable from JIT helpers: JITted code never re-enters execute_instruction, so allocating helpers poll
    // here.
    void jit_safepoint();
    // Cached object-property read for the method-JIT LoadProperty helper.
    Value jit_lookup_object_property(ObjectObj *oobj, uint16_t name_idx);
    uint32_t field_id_for_name(uint16_t name_idx);
    uint32_t getter_field_id_for_name(uint16_t name_idx);
    uint32_t setter_field_id_for_name(uint16_t name_idx);
    void process_completed_io_for_jit();
    void run_timer_loop();
    void ensure_static_fields_inited_for_jit(const std::string &class_name, const nari::ClassDecl *class_decl);
    Value &peek(size_t offset = 0) {
        return stack[stack.size() - 1 - offset];
    }

  private:
    ValuesList globals;
    std::vector<TryHandler> try_stack;

    // Static class fields initialization helper
    void ensure_static_fields_inited(const std::string &class_name, const nari::ClassDecl *class_decl);

    // indexed globals cache (keyed by bytecode name_idx).
    // jit_load_global shares this fast path so JITted code skips the get_global hash lookup
  public:
    Array global_cache;
    ByteArray global_cache_valid;
    Value js_undefined_value;
    // Cached `globals` entry for jsrt's __js_this_cell. __js_invoke reads it on
    // every JS method call (1.39M times on tsc) and each read was a std::string
    // hash plus a hashtable probe. unordered_map is node-based, so a pointer to
    // the mapped Value stays valid across rehash and across set_global's
    // in-place reassignment, which is what makes caching the pointer safe.
    const Value *js_this_cell_cached = nullptr;
    const Value &js_this_cell();
    JsGetPropStaticICArray js_get_prop_static_ic;
    std::vector<uint32_t> getter_property_field_ids;
    std::vector<uint32_t> setter_property_field_ids;

  private:
    void rebuild_global_cache();

    // gather every Value reachable from the bytecode VM as a root, then collect
    bool gc_stress = false;
    // allocator-paced precise collection
    bool gc_safepoints = false;
    void gc_collect_roots();

    // Opt-in profiling for ranking bytecode that remains in the interpreter.
    bool profile_interpreter = false;
    bool interpreter_profile_reported = false;
    std::vector<uint64_t> interpreted_instruction_counts;
    void report_interpreter_profile();

    // C++ locals that hold heap Values across a *nested* VM execution (e.g. OP_SPAWN running a task, callbacks, etc)
    // are not on the operand stack while that nested code runs,
    // so we mark them to prevent them from being unintentionally swept by the GC.
    std::vector<const Value *> gc_temp_roots;
    struct TempRootScope {
        VM &vm;
        size_t base;
        explicit TempRootScope(VM &v) : vm(v), base(v.gc_temp_roots.size()) {
        }
        void add(const Value *p) {
            vm.gc_temp_roots.push_back(p);
        }
        ~TempRootScope() {
            vm.gc_temp_roots.resize(base);
        }
        TempRootScope(const TempRootScope &) = delete;
        TempRootScope &operator=(const TempRootScope &) = delete;
    };

    // Builtin-method inline cache, keyed by CALL_METHOD's name_idx (string idx).
    // Resolves a method name to its runtime builtin member-function pointer once, repeated calls skip the hash lookup.
  public:
    std::vector<ScriptRuntime::BuiltinFn> method_ic_fn;
    std::vector<uint8_t> method_ic_state;
    std::vector<uint32_t> property_field_ids;
    std::vector<uint32_t> method_field_ids;
    std::vector<const ObjectShape *> method_ic_shapes;
    std::vector<uint32_t> method_ic_slots;
    std::vector<const ObjectShape *> method_ic_shapes2;
    std::vector<uint32_t> method_ic_slots2;

  private:
    // Per-site shape cache for OP_MAKE_OBJECT (object literals). Keyed by the instruction address
    std::unordered_map<const uint8_t *, const ObjectShape *> make_object_shape_cache;

  public:
    // Pop `size` key/value pairs and build an object, using the per-site shape cache keyed by `site`
    Value make_object_cached(const uint8_t *site, uint32_t size);

    // Count a call to `func_idx` made from JIT-compiled code and compile it once it crosses JIT_THRESHOLD.
    void note_jit_callee(uint32_t func_idx);

    // True if `func_idx` has a compiled trace running a profitable (long-running) loop
    bool has_profitable_trace(uint32_t func_idx) const;

  private:
    // Property inline cache
    // Direct-mapped, keyed by instruction pointer's name_idx hash.
    // Each entry is a shape pointer + name_idx + slot index.
    struct PropIC {
        const ObjectShape *shape; // cached shape pointer (uniqued, stable)
        uint16_t name_idx;        // string-table index at cache time (collision discriminator)
        uint32_t slot;            // field index within shape->field_ids
    };
    static constexpr size_t PROP_IC_BITS = 4;
    static constexpr size_t PROP_IC_SIZE = 1u << PROP_IC_BITS;
    static constexpr size_t PROP_IC_MASK = PROP_IC_SIZE - 1;
    PropIC prop_ic[PROP_IC_SIZE] = {};

#ifndef DISABLE_JIT
    // Method JIT compilation tracking
    std::unordered_map<uint32_t, uint32_t> call_counts;
    // Tuned on the tsc workload (1280 JITted functions, startup-dominated):
    // interpretation is far more expensive per call than compilation, so compiling
    // earlier than the old 200 is a net win. Measured instruction counts, 3
    // interleaved rounds each: T=20 48.35B, T=35 47.84B, T=50 47.88B, T=75 47.90B,
    // T=120 48.25B, T=200 48.80B, T=1000 56.6B, T=5000 79.7B, T=20000 122.6B.
    // 35..75 is a flat optimum; 50 sits mid-plateau rather than on its edge.
    static constexpr uint32_t JIT_THRESHOLD = 50;
    // Tracing JIT state
    jit::TraceRecording trace_recorder;
    // Trace profitability measurement: a compiled trace writes the number of loop
    // iterations it ran into trace_last_iters before returning.
  public:
    uint64_t trace_last_iters = 0;

  private:
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> trace_iter_stats_;
    // scratch populated during OP_GET_PROPERTY / OP_SET_PROPERTY execution while a trace is recording.
    uint32_t trace_prop_slot = 0;
    bool trace_prop_recordable = false;
    // scratch populated during OP_GET_INDEX / OP_SET_INDEX while a trace is recording.
    void *trace_arr_ptr = nullptr;
    size_t trace_arr_size_bytes = 0;
    bool trace_arr_recordable = false;
    std::unordered_map<uint64_t, uint32_t> edge_heat_;
    static constexpr uint32_t TRACE_HOT_THRESHOLD = 8;
    void trace_record_step(OpCode op, uint8_t *insn_base);
#endif

  public:
    // class instance tracking (for 'this' in methods).
    // MUST be null-initialized: gc_collect_roots reads it on every collection (to root the current receiver)
    ClassInstancePtr current_instance = nullptr;
    std::string current_class_name;

    // runtime error flag, set by JIT helpers or execute_instruction on unrecoverable error.
    bool has_error = false;

    struct NativeCatchBoundary {
        bool caught = false;
        Value error;
    };
    std::vector<NativeCatchBoundary> native_catch_stack;

    // setjmp target for fatal runtime recovery
    std::jmp_buf *overflow_jmp = nullptr;

    // depth counter for active JIT-compiled function calls on the C++ stack
    uint32_t jit_call_depth = 0;

    // JIT-only: borrowed capture views for the current closure call.
    std::vector<CellRef> *jit_captures_raw = nullptr;
    Value *jit_capture0_raw = nullptr;
    Value *jit_capture1_raw = nullptr;
    Value *jit_capture2_raw = nullptr;

    void set_jit_captures_raw(std::vector<CellRef> *captures) {
        jit_captures_raw = captures;
        jit_capture0_raw = captures && !captures->empty() ? (*captures)[0].get() : nullptr;
        jit_capture1_raw = captures && captures->size() > 1 ? (*captures)[1].get() : nullptr;
        jit_capture2_raw = captures && captures->size() > 2 ? (*captures)[2].get() : nullptr;
    }

    // check if there is an active try/catch handler
    bool has_try_handler() const {
        return !try_stack.empty();
    }
    // dispatch a throw to the nearest try/catch handler (returns false if uncaught)
    bool dispatch_throw(Value error);
    // Capture a pending script panic at the innermost __nari_catch boundary.
    bool capture_native_throw(Value error);
    Value call_function_value_catching(const Value &func_val);
    // report an uncatchable runtime panic and terminate execution.
    void runtime_panic(Value error);
    // poll pending I/O completions (usable from JIT helpers)
    void poll_io();

  private:
    uint8_t *&ip() {
        return current_frame().ip;
    }

    // instruction helpers
    uint8_t read_byte() {
        return *ip()++;
    }
    uint16_t read_short() {
        uint16_t val = (ip()[0] << 8) | ip()[1];
        ip() += 2;
        return val;
    }
    int16_t read_signed_short() {
        return static_cast<int16_t>(read_short());
    }

  public:
    // these need to be accessible from JIT helpers
    ValuesList builtins;
    std::unordered_map<std::string, uint32_t> func_indices;
    std::unique_ptr<ScriptRuntime> runtime;

    // execution
    bool execute_instruction();
    Value call_builtin(const std::string &name, const std::vector<Value> &args);
    Value call_builtin(const std::string &name, const Value *argv, size_t argc);
    Value call_builtin_member(ScriptRuntime::BuiltinFn fn, const Value *argv, size_t argc);
    // pushes a builtin's return value, but first promotes any pending
    // ScriptRuntime throw_flag into a bytecode-VM throw via dispatch_throw, false when the throw is uncaught.
    bool push_builtin_result(Value result);
    void call_user_function(
        uint32_t func_idx, const std::vector<Value> &args, const std::vector<Value> *captures = nullptr,
        const CapturesList &cell_captures = {}, const Value *receiver = nullptr
    );
    // allocation-free variant: reads argc args from the top of the VM stack WITHOUT popping them.
    // The caller is responsible for popping args + func after this returns!!
    void call_user_function_stack(
        uint32_t func_idx, size_t args_base, size_t argc, const CapturesList &cell_captures = {}, const Value *receiver = nullptr
    );
    // span variant for runtime re-entry (delegate traps, FFI callbacks)
    void call_user_function_span(
        uint32_t func_idx, const Value *args, size_t argc, const CapturesList &cell_captures = {}, const Value *receiver = nullptr
    );

  public:
    VM(int argc = 0, char **argv = nullptr);
    ~VM();

    // run a compiled chunk
    bool run(Chunk *compiled_chunk);

    // synchronously execute a function to completion
    Value call_function_value_sync(const Value &func_val, const std::vector<Value> &args, const Value *receiver = nullptr);
    // span variant: same semantics without making a vector
    Value call_function_value_span(const Value &func_val, const Value *args, size_t argc, const Value *receiver = nullptr);
    // synchronously run a compiled class function (field/ctor init, method, static init) to completion
    Value call_class_function_sync(uint32_t func_idx, const Value *args, size_t argc, const Value *receiver = nullptr);
    // compile `funcs` into the running chunk and run `entry_name` (backs the eval() builtin)
    Value eval_compile_run(const FuncList &funcs, const std::string &entry_name);
    // name -> index registration for chunk functions [from, size); also seeds JIT metadata
    void register_chunk_functions(size_t from);

    // create and return a new class instance
    Value instantiate_class(const std::string &class_name, std::vector<Value> args);

    // call a named method on a class instance
    Value call_class_method(ClassInstance *instance, const std::string &method_name, std::vector<Value> args);

    // access globals, mainly used for builtins
    void set_global(const std::string &name, const Value &val);
    const Value &get_global(const std::string &name);

    // builtin registration
    void register_builtin(const std::string &name);
    void register_all_builtins();
};

// compile AST to bytecode
Chunk *compile_bytecode(const FuncList &functions);

// Result of appending code to a live chunk (see compile_bytecode_append).
struct AppendedCode {
    uint32_t entry_idx = UINT32_MAX;     // function to run, or UINT32_MAX for declarations-only
    std::vector<uint32_t> toplevel_idxs; // module top-levels from eval'd imports, in source order
};

// Compile extra functions into an already-running chunk, for eval().
// Safe because calls resolve by name at runtime (OP_CALL carries a name label)
AppendedCode compile_bytecode_append(Chunk *existing, const FuncList &functions, const std::string &entry_name);

// Print a full disassembly of a compiled chunk (used by naric --dump and the
// NARI_DUMP_CHUNK runtime debug hook).
void dump_chunk(const Chunk &chunk);

// collect all fields in MRO order, parent fields, then child fields.
inline void bc_collect_all_fields(const nari::ClassDecl *class_decl, std::vector<const nari::ClassField *> &all_fields) {
    if (!class_decl) {
        return;
    }
    if (!class_decl->parent_name.empty()) {
        const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
        if (parent) {
            bc_collect_all_fields(parent, all_fields);
        }
    }
    for (const auto &field : class_decl->fields) {
        if (!field.is_static) {
            all_fields.push_back(&field);
        }
    }
}

// search class hierarchy for a method
inline const nari::ClassMethod *bc_find_method(const nari::ClassDecl *class_decl, const std::string &method_name) {
    if (!class_decl) {
        return nullptr;
    }

    for (const auto &m : class_decl->methods) {
        if (m.name == method_name) {
            return &m;
        }
    }
    if (!class_decl->parent_name.empty()) {
        const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
        if (parent) {
            return bc_find_method(parent, method_name);
        }
    }
    return nullptr;
}

// Shared VM op bodies. These live here, not in jit_helpers.h, because the interpreter needs them in EVERY build config
extern "C" {

// Defined at the bottom of bytecode.cpp (not jit_helpers.cpp -- the interpreter
// runs these opcodes in every build config, including -Ddisable_jit=true).
void jit_js_get_prop_static(VM *vm, uint32_t name_idx);
void jit_js_set_prop_static(VM *vm, uint32_t name_idx);
void jit_js_postinc(VM *vm, uint32_t name_idx);

// Shared arithmetic op bodies.
// These are the single definition used by both the bytecode interpreter and JIT-emitted code
inline void jit_add(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        a.set_int_checked(a.get_int() + b.get_int());
    } else if (a.is_string() || b.is_string()) {
        a = Value::make_string(a.to_string() + b.to_string());
    } else {
        a.set_float(a.as_number() + b.as_number());
    }
    vm->stack.pop_back();
}

inline void jit_sub(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        a.set_int_checked(a.get_int() - b.get_int());
    } else {
        a.set_float(a.as_number() - b.as_number());
    }
    vm->stack.pop_back();
}

inline void jit_mul(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        // fused check: product outside int48 promotes to float in one branch
        int64_t product;
        if (NARI_UNLIKELY(mul_overflow_i48(a.get_int(), b.get_int(), &product))) {
            a.set_float(a.as_number() * b.as_number());
        } else {
            a.set_int(product);
        }
    } else {
        a.set_float(a.as_number() * b.as_number());
    }
    vm->stack.pop_back();
}

inline void jit_div(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    double bn = b.as_number();
    if (bn == 0.0) {
        a.set_float(std::nan(""));
    } else if (a.is_int() && b.is_int()) {
        int64_t av = a.get_int(), bv = b.get_int();
        if (av == INT64_MIN && bv == -1) {
            a.set_float(-static_cast<double>(INT64_MIN));
        } else if (av % bv == 0) {
            a.set_int(av / bv);
        } else {
            a.set_float(a.as_number() / bn);
        }
    } else {
        a.set_float(a.as_number() / bn);
    }
    vm->stack.pop_back();
}

inline void jit_mod(VM *vm) {
    Value &b = vm->peek(0);
    Value &a = vm->peek(1);
    if (a.is_int() && b.is_int()) {
        int64_t bv = b.get_int();
        if (bv == 0) {
            a.set_float(std::nan(""));
        } else {
            int64_t av = a.get_int();
            if (av == INT64_MIN && bv == -1) {
                a.set_float(0.0);
            } else {
                a.set_int(av % bv);
            }
        }
    } else {
        a.set_float(std::fmod(a.as_number(), b.as_number()));
    }
    vm->stack.pop_back();
}

inline void jit_neg(VM *vm) {
    Value &a = vm->peek(0);
    if (a.is_int()) {
        // -INT48_MIN doesn't fit in int48; set_int_checked promotes to
        // float instead of silently wrapping. -get_int() itself is in-range
        // because get_int() returns an int48 (so its negation fits int64).
        a.set_int_checked(-a.get_int());
    } else {
        a.set_float(-a.as_number());
    }
}

// Shared by bytecode.cpp OP_FORMAT_VALUE and jit_format_value(); the bound
// check was duplicated in both. Returns true if __format_value ran -- the only
// case needing jit_abort_on_runtime_error(), which stays in the JIT wrapper
// because the interpreter must NOT longjmp out of its dispatch loop.
inline bool jit_format_value_body(VM *vm, uint32_t spec_idx) {
    Value value = vm->pop();
    if (spec_idx == 0xFFFF || spec_idx >= vm->chunk->strings.size()) {
        vm->push(Value::make_string(value.to_string()));
        return false;
    }
    Value args[2] = { value, Value::make_string(vm->chunk->strings[spec_idx]) };
    vm->push(vm->call_builtin("__format_value", args, 2));
    return true;
}

} // extern "C"

} // namespace bytecode
} // namespace nari
