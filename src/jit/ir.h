#pragma once
// Mid-level optimizing IR for the method-JIT pipeline:
//  bytecode -> CFG -> SSA IR -> opt passes -> AsmJIT.
//
// AsmJIT keeps doing register allocation, this IR is a higher level language that I can optimize easier.
// Handles are integer ids (indices into the Func's vectors), not pointers.
#ifndef DISABLE_JIT

#include <cstdint>
#include <string>
#include <vector>

namespace nari {
namespace bytecode {
struct FunctionMeta;
struct Chunk;
} // namespace bytecode

namespace jit {
namespace ir {

// SSA value / instruction / block handles. Invalid = "none".
using ValueId = int32_t;
using BlockId = int32_t;
inline constexpr ValueId InvalidValue = -1;
inline constexpr BlockId InvalidBlock = -1;

// Type lattice for an SSA value.
// Int48 is a Nari integer guaranteed in the int48 range (raw int64 in a gp reg).
// Number is a value that may overflow to float.
enum class Ty : uint8_t {
    Bottom = 0, // inference-internal: "no value computed yet"
    // infer_types() never leaves this in a value's type (-> Unknown).
    Unknown, // not yet known / dynamically typed Value
    Int48, // integer in [INT48_MIN, INT48_MAX] (unboxed raw int64)
    Float, // IEEE-754 double (unboxed in xmm)
    Number, // int-or-float (e.g. an int op whose overflow may promote to float)
    Bool,
    Heap, // heap pointer (string/array/object/function) - boxed Value
    None,
};

enum class Op : uint16_t {
    // constants
    IConst, // imm_int -> Int48
    FConst, // imm_float -> Float
    BConst, // imm_int (0/1) -> Bool
    NConst, // constant None
    LoadConst, // imm_u32 = constant pool index -> Value (strings/functions/etc.)

    // integer arithmetic (Int48 in, result may overflow: see Ty/guards)
    IAdd,
    ISub,
    IMul,
    IMod,
    INeg,
    IAnd,
    IOr,
    IXor,
    INot,
    IShl,
    IShr, // bitwise
    JsBitBinary, // imm_u32 = OP_JS_BIT_AND..OP_JS_USHR; JavaScript ToUint32 semantics
    JsBitNot,

    // float arithmetic
    FAdd,
    FSub,
    FMul,
    FDiv,

    // division (int/int -> Int or Float depending on divisibility)
    Div,

    // generic numeric op on dynamic Values (runtime type dispatch)
    // Used until the type lattice proves Int48/Float. Lowers to the existing
    // int-fast / float-fast / C-fallback sequence
    DynAdd,
    DynSub,
    DynMul,
    DynMod,
    DynDiv,
    Pow,

    // comparisons -> Bool
    ICmpLt,
    ICmpLe,
    ICmpGt,
    ICmpGe,
    ICmpEq,
    ICmpNe,
    FCmpLt,
    FCmpLe,
    FCmpGt,
    FCmpGe,
    FCmpEq,
    FCmpNe,
    DynCmpLt,
    DynCmpLe,
    DynCmpGt,
    DynCmpGe,
    DynCmpEq,
    DynCmpNe,
    DynStrictCmpEq,
    DynStrictCmpNe,

    // generic boolean op on dynamic Values
    Not,      // !operand using Nari truthiness -> Bool
    JsTruthy, // JavaScript truthiness conversion -> Bool
    IsNone,   // exact None test -> Bool

    // boxing / unboxing / guards (made explicit and lowered at codegen)
    Box,        // unboxed Int48/Float -> boxed Value
    Unbox,      // boxed Value -> unboxed (type per Ty)
    GuardInt48, // assert operand fits int48; side-exit/box-as-float otherwise

    // locals (replaced by SSA values + Phi after construction)
    LoadSlot,     // imm_int = slot index -> Value
    StoreSlot,    // operand[0] -> slot imm_int (no result)
    LoadGlobal,   // imm_int = string/name index -> Value
    StoreGlobal,  // operand[0] -> global name imm_int (no result)
    LoadCapture,  // imm_u32 = capture cell index -> Value (closure upvalue read)
    StoreCapture, // operand[0] -> capture cell imm_u32 (peek; no result)
    Pop,          // discard operand[0] (stack-machine bookkeeping; DCE'd in SSA)
    Dup,          // copy of operand[0] (lowers OP_DUP; a copy in SSA, copy-prop'd)

    // fused slot-store forms (no result, no operand stack traffic).
    StoreImmSlot,  // imm_int = int48 constant, imm_u32 = dst slot
    StoreBImmSlot, // imm_int = 0/1, imm_u32 = dst slot
    StoreNSlot,    // imm_u32 = dst slot (writes NaN-boxed none)
    StoreCSlot,    // imm_int = LoadConst pool index, imm_u32 = dst slot
    CopySlot,      // imm_int = src slot, imm_u32 = dst slot

    // aggregate / string helpers (stack-helper lowered for coverage)
    MakeArray,   // imm_u32 elements -> Value
    ArrayPush,   // target/value -> mutated target Value
    ArraySpread, // target/iterable -> mutated target Value
    MakeObject,  // imm_u32 pairs -> Value
    MakeClosure, // imm_u32 function index; capture descriptors remain at bytecode_pc
    StrConcat,   // lhs/rhs -> string-ish Value
    StrAppendSlot, // operand[0] appended to local imm_u32; updated slot -> Value
    FormatValue, // operand[0], imm_u32 format spec -> string Value
    IterArray,   // operand[0] iterable -> normalized array Value (for-in helper)

    // memory
    LoadIndex,     // arr=operand[0], idx=operand[1] -> Value
    StoreIndex,    // arr=operand[0], idx=operand[1], val=operand[2]
    LoadProperty,  // obj=operand[0], imm_u32=name index -> Value
    JsGetPropStatic, // obj=operand[0], imm_u32=literal key index -> Value
    StoreProperty, // obj=operand[0], val=operand[1], imm_u32=name index
    JsSetPropStatic, // obj=operand[0], val=operand[1], imm_u32=literal key index
    JsPostinc,       // obj=operand[0], imm_u32=literal key index -> old numeric value
    CloseUpvalues,   // imm_u32 = first local slot to close; no operands, no result

    // calls
    Call, // callee/args via operands + imm
    CallSpread, // callee/args-array -> Value; imm_u32 = callee label index
    CallMethod,

    // SSA / control flow
    Phi,    // merges predecessor values (operands parallel block preds)
    Jump,   // -> target block (Block::succ[0])
    Branch, // operand[0] cond; succ[0]=true, succ[1]=false
    SelfTailCall, // operands = new arguments; loops to the function entry
    Return, // operand[0] = return value (or none)
};

// One SSA instruction. `result` is its own ValueId (== index in Func::insts for
// real instructions); terminators / StoreSlot produce InvalidValue.
struct Inst {
    Op op;
    Ty type = Ty::Unknown;
    ValueId result = InvalidValue;
    std::vector<ValueId> operands;
    // immediates (only the field relevant to `op` is meaningful)
    int64_t imm_int = 0;
    double imm_float = 0.0;
    uint32_t imm_u32 = 0; // slot index / const index / arg count / name index
    // control-flow targets (Jump/Branch); Phi uses the parent block's pred list
    BlockId target0 = InvalidBlock;
    BlockId target1 = InvalidBlock;
    size_t bytecode_pc = 0; // provenance, for debugging/side-exit ip
};

struct Block {
    BlockId id = InvalidBlock;
    std::vector<ValueId> phis;         // indices into Func::insts (Op::Phi)
    std::vector<ValueId> insts;        // straight-line body (indices into Func::insts)
    ValueId terminator = InvalidValue; // Jump/Branch/Return
    std::vector<BlockId> preds;
    std::vector<BlockId> succs;
    size_t start_pc = 0; // bytecode offset of this block's first instruction
};

struct Func {
    std::vector<Inst> insts; // SSA value k is insts[k] (result == k)
    std::vector<Block> blocks;
    BlockId entry = InvalidBlock;
    uint32_t num_slots = 0; // local variable slots
    uint32_t num_params = 0;
    std::vector<uint8_t> captured_local_slots; // locals aliased by closures created in this function
    const bytecode::FunctionMeta *meta = nullptr;

    ValueId add_inst(Inst in) {
        ValueId id = (ValueId)insts.size();
        in.result = (in.op == Op::StoreSlot || in.op == Op::Jump || in.op == Op::Branch || in.op == Op::SelfTailCall || in.op == Op::Return ||
                     in.op == Op::StoreGlobal || in.op == Op::StoreCapture || in.op == Op::Pop || in.op == Op::StoreImmSlot ||
                     in.op == Op::CloseUpvalues ||
                     in.op == Op::StoreBImmSlot || in.op == Op::StoreNSlot || in.op == Op::StoreCSlot || in.op == Op::CopySlot)
                        ? InvalidValue
                        : id;
        insts.push_back(std::move(in));
        return id;
    }
    BlockId add_block(size_t start_pc) {
        BlockId id = (BlockId)blocks.size();
        Block b;
        b.id = id;
        b.start_pc = start_pc;
        blocks.push_back(std::move(b));
        return id;
    }
    Inst &inst(ValueId v) {
        return insts[v];
    }
    const Inst &inst(ValueId v) const {
        return insts[v];
    }
};

const char *op_name(Op op);
// Debug dump (gated by callers behind an env flag).
std::string dump(const Func &f);

} // namespace ir
} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
