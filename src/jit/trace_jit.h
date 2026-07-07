#pragma once
// Forward declarations and shared trace types for the trace JIT.
#ifndef DISABLE_JIT

namespace nari {
namespace bytecode {
class VM;
struct Chunk;
struct FunctionMeta;
} // namespace bytecode
} // namespace nari

#include "core_types.h" // JitInlineKind
#include <cstdint>
#include <vector>

namespace nari {
namespace jit {

// Observed operand type during trace recording
enum class TraceType : uint8_t {
    Unknown = 0,
    Int = 1,        // ValueTag::Int  (tagInt  = 2)
    Float = 2,      // ValueTag::Float (tagFloat = 3)
    Bool = 3,       // ValueTag::Bool  (tagBool  = 4)
    MathObject = 4, // the stdlib 'math' object; allows inlining builtins
    Obj = 5,        // heap ObjectObj pointer (for property access traces)
    Function = 6,   // heap FunctionData pointer (for closure-call traces)
    Array = 7,      // heap ArrayObj pointer (for element-access traces)
};

// one recorded step from a single hot loop iteration
struct TraceStep {
    enum class Kind : uint8_t {
        LoadIntVar,
        LoadFloatVar,
        StoreIntVar,
        StoreFloatVar,
        LoadIntConst,
        LoadFloatConst,
        LoadZeroConst,
        LoadOneConst,
        LoadMathGlobal,
        IntAdd,
        IntSub,
        IntMul,
        IntDiv,
        IntMod,
        FloatAdd,
        FloatSub,
        FloatMul,
        FloatDiv,
        IntToFloat,
        // math builtins
        MathSqrt,
        MathSin,
        MathCos,
        MathFloor,
        MathCeil,
        MathTan,
        // comparisons (result pushed as Bool / int bit)
        IntLt,
        IntLe,
        IntGt,
        IntGe,
        IntEq,
        IntNe,
        FloatLt,
        FloatLe,
        FloatGt,
        FloatGe,
        FloatEq,
        FloatNe,
        // logical NOT of a Bool/Int truth value (negates a comparison result).
        // On a lazy comparison this just inverts its condition codes.
        Not,
        // conditional loop exit: JUMP_IF_FALSE was NOT taken (condition true).
        // compiled as: if cond == 0 -> flush + return (loop done).
        CondExitIfFalse,
        // body branch guard: condition was TRUE during recording (JUMP_IF_FALSE not taken).
        // At runtime, if condition is FALSE -> side-exit to fallback_ip.
        SideExitIfFalse,
        // body branch guard: condition was FALSE during recording (JUMP_IF_FALSE taken).
        // At runtime, if condition is TRUE -> side-exit to fallback_ip.
        SideExitIfTrue,
        // backward jump = end of one loop iteration
        LoopBack,
        // pop top of stack (mirrors OP_POP after OP_STORE_VAR etc.)
        Pop,
        // duplicate top of stack (mirrors OP_DUP)
        Dup,
        // load an ObjectObj* from local var into trace
        LoadObjVar,
        // pop Obj, read Int/Float from obj->fields[prop_slot_index], push result.
        ObjGetProp,
        // pop val, pop Obj, write val to obj->fields[prop_slot_index], push val
        ObjSetProp,
        // fused complete statement windows: obj.f += const / obj.f += obj.g
        ObjAddConstInPlace,
        ObjAddPropInPlace,
        // closure call (local var): inline ClosureInc / ClosureAddConst
        // capture_ptr points to the captured Value cell; int_val = constant for AddConst
        ClosureInc,
        ClosureAddConst,
        // load an ArrayObj* from a local var into the trace. int_val holds the
        // recorded (finish - start) size-in-bytes for the entry guard. obj_ptr
        // is the recorded ArrayObj*.
        LoadArrayVar,
        // stack: [..., Array, Int] -> [..., Int]. Bounds-side-exit on OOB.
        // prop_val_type = element type (Int only for the MVP).
        ArrayGetIdx,
        // stack: [..., Array, Int, val] -> [..., val]. Bounds-side-exit on OOB.
        // Trace body cannot grow the array; growth is caught by the entry
        // size-guard on the next entry (recorder aborts on push / OOB store).
        ArraySetIdx,
    };

    Kind kind;
    uint16_t slot = 0;    // for Load/StoreVar: local slot index
    int64_t int_val = 0;  // for LoadIntConst
    double dbl_val = 0.0; // for LoadFloatConst

    // property access IC data
    uint32_t prop_slot_index = 0; // field index within the object's shape (shape-pointer scheme).
    // Stable for any object whose shape matches the entry guard;
    // the slot address is recomputed each access
    void *obj_ptr = nullptr; // recorded ObjectObj* (for entry guard)
    void *shape_ptr = nullptr; // recorded ObjectShape* (for object property guards)
    uint32_t shape_ver = 0; // recorded shape_version (for entry guard)
    TraceType prop_val_type = TraceType::Unknown; // type of the property value

    // side-exit PC for body branch guards (SideExitIfFalse / SideExitIfTrue)
    size_t fallback_pc = 0;

    // closure call: pinned capture cell pointer (for ClosureInc / ClosureAddConst)
    void *capture_ptr = nullptr;
    uint16_t closure_slot = 0; // local slot holding the closure value
    uint32_t rhs_prop_slot_index = 0; // second property slot for fused object updates
};

// trace recorder state
struct TraceRecording {
    bool pending_record = false; // set true at hot back-edge -> start next iteration
    bool recording = false; // actively collecting steps
    bool aborted = false; // unsupported opcode seen -> skip compilation

    uint32_t func_idx = 0;
    size_t anchor_pc = 0; // PC of the backward JUMP opcode
    size_t target_pc = 0; // where the backward jump lands (loop header)
    size_t exit_pc = 0; // where JUMP_IF_FALSE exits to (loop done)

    std::vector<TraceStep> steps;
    std::vector<TraceType> type_vstack; // parallel type stack used during recording

    bool valid = false; // true when fully recorded (exit_pc known, LoopBack seen)

    // set to callee FunctionMeta when a LOAD_GLOBAL of an inlineable function is deferred
    bytecode::FunctionMeta *pending_inline_func = nullptr;

    // deferred local closure call state (set by LOAD_VAR when value is a closure)
    void *pending_closure_capture0 = nullptr; // Value* to pinned capture[0]
    uint16_t pending_closure_slot = 0; // local slot holding the closure value
    JitInlineKind pending_closure_kind = JitInlineKind::None; // ClosureInc or ClosureAddConst
    int64_t pending_closure_imm = 0; // constant for ClosureAddConst

    // depth counter for inlined function calls whose body is being executed
    // by the interpreter while the trace recorder skips those instructions
    int inline_depth = 0;

    void reset() {
        pending_record = recording = aborted = false;
        valid = false;
        steps.clear();
        type_vstack.clear();
        exit_pc = 0;
        pending_inline_func = nullptr;
        pending_closure_capture0 = nullptr;
        pending_closure_slot = 0;
        pending_closure_kind = JitInlineKind::None;
        pending_closure_imm = 0;
        inline_depth = 0;
    }
};

// compiled trace
struct CompiledTrace {
    using Fn = void (*)(bytecode::VM *);
    Fn fn = nullptr;
    bool valid = false;
};

// TraceJIT base interface
class TraceJITBase {
  public:
    virtual ~TraceJITBase() = default;
    virtual CompiledTrace compile(const TraceRecording &rec, const bytecode::Chunk &chunk, uint32_t func_idx) = 0;
    virtual const CompiledTrace *find(uint32_t func_idx, size_t anchor_pc) const = 0;
    // invalidate all cached compiled traces
    virtual void reset() = 0;
};

extern TraceJITBase *g_trace_jit;
void init_trace_jit();
void shutdown_trace_jit();

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
