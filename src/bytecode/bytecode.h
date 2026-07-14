#pragma once

#include "core_types.h"
#include "parser_api.h"
#include "runtime.h"
#include <csetjmp>
#include <cstddef>
#include <cstdint>
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

#define OPCODE_LIST(X)              \
    X(LOAD_CONST, 2, false)          \
    X(LOAD_VAR, 2, false)            \
    X(STORE_VAR, 2, false)           \
    X(LOAD_GLOBAL, 2, false)         \
    X(STORE_GLOBAL, 2, false)        \
    X(POP, 0, false)                 \
    X(DUP, 0, false)                 \
    X(LOAD_NONE, 0, false)           \
    X(LOAD_TRUE, 0, false)           \
    X(LOAD_FALSE, 0, false)          \
    X(LOAD_ZERO, 0, false)           \
    X(LOAD_ONE, 0, false)            \
    X(ADD, 0, false)                 \
    X(SUB, 0, false)                 \
    X(MUL, 0, false)                 \
    X(DIV, 0, false)                 \
    X(MOD, 0, false)                 \
    X(POW, 0, false)                 \
    X(NEG, 0, false)                 \
    X(STR_CONCAT, 0, false)          \
    X(STR_APPEND_VAR, 2, false)      \
    X(STR_APPEND_GLOBAL, 2, false)   \
    X(BIT_AND, 0, false)             \
    X(BIT_OR, 0, false)              \
    X(BIT_XOR, 0, false)             \
    X(BIT_NOT, 0, false)             \
    X(LSHIFT, 0, false)              \
    X(RSHIFT, 0, false)              \
    X(NOT, 0, false)                 \
    X(EQ, 0, false)                  \
    X(NE, 0, false)                  \
    X(LT, 0, false)                  \
    X(LE, 0, false)                  \
    X(GT, 0, false)                  \
    X(GE, 0, false)                  \
    X(JUMP, 2, false)                \
    X(JUMP_IF_FALSE, 2, false)       \
    X(JUMP_IF_TRUE, 2, false)        \
    X(JUMP_IF_NONE, 2, false)        \
    X(CALL, 3, false)                \
    X(SELF_TAIL_CALL, 1, false)      \
    X(RETURN, 0, false)              \
    X(MAKE_CLOSURE, 3, true)         \
    X(SPAWN, 0, false)               \
    X(MAKE_ARRAY, 2, false)          \
    X(MAKE_OBJECT, 2, false)         \
    X(ARRAY_PUSH, 0, false)          \
    X(ARRAY_SPREAD, 0, false)        \
    X(OBJECT_SPREAD, 0, false)       \
    X(OBJECT_SET, 2, false)          \
    X(CALL_SPREAD, 2, false)         \
    X(MAKE_REGEX, 4, false)          \
    X(GET_INDEX, 0, false)           \
    X(SET_INDEX, 0, false)           \
    X(GET_PROPERTY, 2, false)        \
    X(SET_PROPERTY, 2, false)        \
    X(MAKE_ITERATOR, 0, false)       \
    X(ITER_NEXT, 0, false)           \
    X(MAKE_ITERATOR_KV, 0, false)    \
    X(ITER_NEXT_KV, 0, false)        \
    X(LOAD_CAPTURE, 2, false)        \
    X(STORE_CAPTURE, 2, false)       \
    X(THROW, 0, false)               \
    X(SETUP_TRY, 4, false)           \
    X(POP_TRY, 0, false)             \
    X(BEGIN_CATCH, 0, false)         \
    X(BEGIN_FINALLY, 0, false)       \
    X(NEW_INSTANCE, 3, false)        \
    X(LOAD_THIS, 0, false)           \
    X(CALL_METHOD, 3, false)         \
    X(CHECK_TYPE, 3, false)          \
    X(FORMAT_VALUE, 2, false)        \
    X(ITER_ARRAY, 0, false)

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
    size_t index = static_cast<size_t>(op);
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
// fixed header is 3 bytes (func_idx:u16 + capture_count:u8), followed by
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
        size += static_cast<size_t>(code[pc + 3]) * 3;
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
    std::string name;
    std::string source_file;            // original source filename (for error messages)
    std::vector<uint8_t> code;          // bytecode instructions
    std::vector<Constant> constants;    // constant pool
    std::vector<std::string> var_names; // local variable names
    std::vector<LineEntry> line_map;    // pc -> source line (sorted, sparse)
    uint8_t param_count;
    uint8_t capture_count;   // number of captured variables (for closures)
    int8_t rest_param_index; // index of rest param (-1 if none)
    bool is_lambda;
    bool strict_mode; // true when function was compiled under "use strict"
    int8_t return_vt; // JIT vt of return value: 0=unknown, 1=int, 2=float (set
    // for strict-mode annotated functions)

    FunctionMeta() : param_count(0), capture_count(0), rest_param_index(-1), is_lambda(false), strict_mode(false), return_vt(0) {
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

    // Skip the strict-mode CHECK_TYPE parameter preamble (if any).
    // Each typed param emits: LOAD_VAR(3) + CHECK_TYPE(4, ctx=0) + STORE_VAR(3) +
    // POP(1) = 11 bytes.
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
        if (end_off + 4 < func_code.size() &&
            (OpCode)func_code[end_off] == OpCode::OP_CHECK_TYPE &&
            func_code[end_off + 3] == 1 && // ctx=1 = return check
            (OpCode)func_code[end_off + 4] == OpCode::OP_RETURN) {
            return true;
        }
        return false;
    };

    // 2-arg body: LOAD_VAR(0) LOAD_VAR(1) op [CHECK_TYPE] RETURN
    if (base + 7 <= func_code.size() &&
        (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR &&
        func_code[base + 1] == 0 && func_code[base + 2] == 0 &&
        (OpCode)func_code[base + 3] == OpCode::OP_LOAD_VAR &&
        func_code[base + 4] == 0 && func_code[base + 5] == 1 &&
        is_return_end(base + 7)) {
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
    if (base + 7 <= func_code.size() &&
        (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR &&
        func_code[base + 1] == 0 && func_code[base + 2] == 0 &&
        (OpCode)func_code[base + 3] == OpCode::OP_LOAD_CONST &&
        is_return_end(base + 7)) {
        uint16_t cidx = (uint16_t(func_code[base + 4]) << 8) | func_code[base + 5];
        if (cidx < func_meta.constants.size() &&
            func_meta.constants[cidx].type == Constant::Type::INT) {
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
    if (base + 7 <= func_code.size() &&
        (OpCode)func_code[base + 0] == OpCode::OP_LOAD_CONST &&
        (OpCode)func_code[base + 3] == OpCode::OP_LOAD_VAR &&
        func_code[base + 4] == 0 && func_code[base + 5] == 0 &&
        is_return_end(base + 7)) {
        uint16_t cidx = (uint16_t(func_code[base + 1]) << 8) | func_code[base + 2];
        if (cidx < func_meta.constants.size() &&
            func_meta.constants[cidx].type == Constant::Type::INT) {
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
    if (func_code.size() >= 13 &&
        (OpCode)func_code[0] == OpCode::OP_LOAD_CAPTURE && func_code[1] == 0 &&
        func_code[2] == 0 && (OpCode)func_code[3] == OpCode::OP_LOAD_ONE &&
        (OpCode)func_code[4] == OpCode::OP_ADD &&
        (OpCode)func_code[5] == OpCode::OP_STORE_CAPTURE && func_code[6] == 0 &&
        func_code[7] == 0 && (OpCode)func_code[8] == OpCode::OP_POP &&
        (OpCode)func_code[9] == OpCode::OP_LOAD_CAPTURE && func_code[10] == 0 &&
        func_code[11] == 0 && is_return_end(12)) {
        return { JitInlineKind::ClosureInc, 1 };
    }
    // 0-arg closure that adds a constant to capture[0] and returns it.
    // Matches: LOAD_CAPTURE(0) LOAD_CONST(k) ADD STORE_CAPTURE(0) POP
    //  LOAD_CAPTURE(0) [CHECK_TYPE(ctx=1)] RETURN
    if (func_code.size() >= 15 &&
        (OpCode)func_code[0] == OpCode::OP_LOAD_CAPTURE && func_code[1] == 0 &&
        func_code[2] == 0 && (OpCode)func_code[3] == OpCode::OP_LOAD_CONST &&
        (OpCode)func_code[6] == OpCode::OP_ADD &&
        (OpCode)func_code[7] == OpCode::OP_STORE_CAPTURE && func_code[8] == 0 &&
        func_code[9] == 0 && (OpCode)func_code[10] == OpCode::OP_POP &&
        (OpCode)func_code[11] == OpCode::OP_LOAD_CAPTURE && func_code[12] == 0 &&
        func_code[13] == 0 && is_return_end(14)) {
        uint16_t cidx = (uint16_t(func_code[4]) << 8) | func_code[5];
        if (cidx < func_meta.constants.size() &&
            func_meta.constants[cidx].type == Constant::Type::INT) {
            return { JitInlineKind::ClosureAddConst, func_meta.constants[cidx].as_int };
        }
    }
    // 1-arg identity: LOAD_VAR(0) [CHECK_TYPE] RETURN  (4+ bytes)
    if (base + 3 < func_code.size() &&
        (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR &&
        func_code[base + 1] == 0 && func_code[base + 2] == 0 &&
        is_return_end(base + 3)) {
        return { JitInlineKind::Identity, 0 };
    }
    // 1-arg negate: LOAD_VAR(0) NEG [CHECK_TYPE] RETURN
    if (base + 4 < func_code.size() &&
        (OpCode)func_code[base + 0] == OpCode::OP_LOAD_VAR &&
        func_code[base + 1] == 0 && func_code[base + 2] == 0 &&
        (OpCode)func_code[base + 3] == OpCode::OP_NEG &&
        is_return_end(base + 4)) {
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
    std::vector<FunctionMeta> functions; // all functions in this chunk
    std::vector<std::string> strings;
    std::vector<TypeInfo> types; // FFI type decls
    uint32_t main_func_idx;

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

typedef std::unordered_map<uint16_t, std::shared_ptr<Value>> CaptureMap;

struct CallFrame {
    FunctionMeta *function;
    uint8_t *ip;
    size_t slot_base; // base index for locals in stack
    CapturesList captures;

    CallFrame() : function(nullptr), ip(nullptr), slot_base(0) {
    }

    // get or create upvalue cell for a local variable
    std::shared_ptr<Value> get_or_create_cell(uint16_t local_idx, const Value &val) {
        if (!open_upvalues) {
            open_upvalues = std::make_unique<CaptureMap>();
        }
        auto it = open_upvalues->find(local_idx);
        if (it != open_upvalues->end()) {
            return it->second;
        }
        auto cell = std::make_shared<Value>(val);
        (*open_upvalues)[local_idx] = cell;
        return cell;
    }

    // returns the number of open upvalue cells (0 when not allocated).
    size_t upvalue_count() const {
        return open_upvalues ? open_upvalues->size() : 0;
    }

    // lazily-allocated upvalue map, null for the common case (no closures in frame).
    std::unique_ptr<CaptureMap> open_upvalues;
};

class VM {
#ifndef DISABLE_JIT
    friend class nari::jit::AsmJITMethodCompiler;
#endif
    // JIT helpers and internal code access stack/frame directly
  public:
    std::vector<Value> stack;
    std::vector<CallFrame> frames;
    Chunk *chunk;

    CallFrame &current_frame() {
        return frames.back();
    }
    FunctionMeta *current_function() {
        return current_frame().function;
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
    // Safe-point usable from JIT helpers: JITted code never re-enters execute_instruction, so allocating helpers poll here.
    void jit_safepoint();
    // Cached object-property read for the method-JIT LoadProperty helper.
    // Mirrors the OP_GET_PROPERTY inline cache (prop_ic)
    Value jit_lookup_object_property(ObjectObj *oobj, uint16_t name_idx);
    void process_completed_io_for_jit();
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
    // jit_load_global shares this fast path so JITted code skips the get_global hash lookup, exactly
    // like the interpreter's OP_LOAD_GLOBAL.
  public:
    std::vector<Value> global_cache;
    std::vector<uint8_t> global_cache_valid;

  private:
    void rebuild_global_cache();

    // mark-sweep GC roots: gather every Value reachable from the
    // bytecode VM as a root, then collect. gc_stress is enabled via
    // NARI_GC_STRESS and forces a full collection at every instruction boundary
    bool gc_stress = false;
    // Allocator-paced precise collection at interpreter/JIT safe-points.
    // When the GC flags trigger_collection (allocation threshold reached), the next safe-point runs a full mark-sweep.
    bool gc_safepoints = false;
    void gc_collect_roots();

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
    // Resolves a method name to its runtime builtin member-function pointer once,
    // so repeated calls (e.g. str.startsWith / arr.push in a loop) skip the
    // per-call hash lookups in builtins.find + runtime->call_builtin.
    // state: 0 = unresolved, 1 = resolved to method_ic_fn[idx].
    // Public: the JIT's jit_call_method helper (a free function) shares this cache.
  public:
    std::vector<ScriptRuntime::BuiltinFn> method_ic_fn;
    std::vector<uint8_t> method_ic_state;

  private:
    // Per-site shape cache for OP_MAKE_OBJECT (object literals). Keyed by the
    // instruction address (bytecode is immutable after load). A literal builds
    // the same field-name set every time, so once we've seen the resulting
    // ObjectShape we can reuse it and fill fields by slot directly, skipping the
    // per-field intern + shape-transition hashing in set_field. Verified each
    // time by comparing keys (cheap string compare) so computed-key literals
    // still fall back correctly.
    std::unordered_map<const uint8_t *, const ObjectShape *> make_object_shape_cache;

  public:
    // Pop `size` key/value pairs and build an object, using the per-site shape
    // cache keyed by `site` (a stable bytecode address). Shared by the
    // interpreter's OP_MAKE_OBJECT and the JIT's jit_make_object_site helper
    // (a free function), so it must be public.
    Value make_object_cached(const uint8_t *site, uint32_t size);

    // Count a call to `func_idx` made from JIT-compiled code and compile it once
    // it crosses JIT_THRESHOLD. The interpreter's call path already does this, but
    // a function called only from JITted code (e.g. a pipeline callback) would
    // otherwise never be counted and stay interpreted forever.
    void note_jit_callee(uint32_t func_idx);

    // True if `func_idx` has a compiled trace running a profitable (long-running)
    // loop -- enough sampled entries with a high iterations/entry average. When so,
    // we SKIP method-compiling the function so it keeps using the register-flow
    // trace (which beats the method JIT on long single loops; see the trace-JIT
    // findings). Short re-entered loops never qualify, so they still method-compile.
    bool has_profitable_trace(uint32_t func_idx) const;

  private:
    // Property inline cache
    // Direct-mapped, keyed by instruction pointer's name_idx hash.
    //
    // Validity check is shape-pointer based, NOT object-pointer based.
    // ObjectShape pointers are uniqued in the global registry and never
    // freed. Two distinct ObjectObjs with the same fields-in-the-same-order
    // share the same shape pointer. Storing &fields[slot] directly is unsafe
    // because the underlying ObjectObj (and its std::vector<Value> heap buffer)
    // can be freed and re-allocated at the same address with the same
    // shape_version - a classic ABA that yielded use-after-free reads of the
    // freed vector buffer (e.g. regex.exec(s).value across repeated calls).
    // Caching the shape pointer + field slot index instead lets us recompute
    // &oobj->fields[slot] per access; the slot index is stable for any object
    // whose shape pointer matches.
    struct PropIC {
        const ObjectShape *shape; // cached shape pointer (uniqued, stable)
        uint16_t name_idx;        // string-table index at cache time (collision discriminator)
        uint32_t slot;            // field index within shape->names
    };
    static constexpr size_t PROP_IC_BITS = 4;
    static constexpr size_t PROP_IC_SIZE = 1u << PROP_IC_BITS;
    static constexpr size_t PROP_IC_MASK = PROP_IC_SIZE - 1;
    PropIC prop_ic[PROP_IC_SIZE] = {};

#ifndef DISABLE_JIT
    // Method JIT compilation tracking
    std::unordered_map<uint32_t, uint32_t> call_counts;
    static constexpr uint32_t JIT_THRESHOLD = 50;
    // Tracing JIT state
    jit::TraceRecording trace_recorder;
    // Trace profitability measurement: a compiled trace writes the number of loop
    // iterations it ran into trace_last_iters before returning.
    // The dispatcher accumulates per-(func_idx,anchor) totals so we can tell a profitable
    // long-running loop (high iters/entry) from a short re-entered one.
  public:
    uint64_t trace_last_iters = 0;

  private:
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> trace_iter_stats_;
    // Scratch populated during OP_GET_PROPERTY / OP_SET_PROPERTY execution while a trace is recording.
    uint32_t trace_prop_slot = 0;
    bool trace_prop_recordable = false;
    // Scratch populated during OP_GET_INDEX / OP_SET_INDEX while a trace is recording.
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

    // setjmp target for fatal runtime recovery.
    // C++ exceptions cannot unwind through JIT-generated code, so helpers longjmp out instead.
    std::jmp_buf *overflow_jmp = nullptr;

    // Depth counter for active JIT-compiled function calls on the C++ stack.
    // Incremented by JIT helper paths when they enter a JIT-compiled callee, decremented on exit.
    uint32_t jit_call_depth = 0;

    // JIT-only: raw borrowed captures pointer for the current closure call.
    // Set by jit_call_value to avoid shared_ptr ownership traffic
    std::vector<std::shared_ptr<Value>> *jit_captures_raw = nullptr;

    // check if there is an active try/catch handler
    bool has_try_handler() const {
        return !try_stack.empty();
    }
    // dispatch a throw to the nearest try/catch handler (returns false if uncaught)
    bool dispatch_throw(Value error);
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
    void call_user_function(uint32_t func_idx, const std::vector<Value> &args, const std::vector<Value> *captures = nullptr, CapturesList cell_captures = nullptr);
    // allocation-free variant: reads argc args from the top of the VM stack WITHOUT popping them.
    // The caller is responsible for popping args + func after this returns!!
    void call_user_function_stack(uint32_t func_idx, size_t args_base, size_t argc, CapturesList cell_captures = nullptr);

  public:
    VM(int argc = 0, char **argv = nullptr);
    ~VM();

    // run a compiled chunk
    bool run(Chunk *compiled_chunk);

    // synchronously execute a function to completion (for FFI callback re-entry)
    Value call_function_value_sync(const Value &func_val, const std::vector<Value> &args);

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

} // namespace bytecode
} // namespace nari
