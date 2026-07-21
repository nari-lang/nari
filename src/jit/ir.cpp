// Mid-level optimizing IR - names + debug dump. See ir.h / docs/JIT_OPTIMIZING_PIPELINE.md.
#ifndef DISABLE_JIT
#include "ir.h"
#include <cstdio>

namespace nari {
namespace jit {
namespace ir {

const char *op_name(Op op) {
    switch (op) {
        case Op::IConst:
            return "iconst";
        case Op::FConst:
            return "fconst";
        case Op::BConst:
            return "bconst";
        case Op::NConst:
            return "none";
        case Op::LoadConst:
            return "load.const";
        case Op::IAdd:
            return "iadd";
        case Op::ISub:
            return "isub";
        case Op::IMul:
            return "imul";
        case Op::IMod:
            return "imod";
        case Op::INeg:
            return "ineg";
        case Op::IAnd:
            return "iand";
        case Op::IOr:
            return "ior";
        case Op::IXor:
            return "ixor";
        case Op::INot:
            return "inot";
        case Op::IShl:
            return "ishl";
        case Op::IShr:
            return "ishr";
        case Op::FAdd:
            return "fadd";
        case Op::FSub:
            return "fsub";
        case Op::FMul:
            return "fmul";
        case Op::FDiv:
            return "fdiv";
        case Op::Div:
            return "div";
        case Op::DynAdd:
            return "dyn.add";
        case Op::DynSub:
            return "dyn.sub";
        case Op::DynMul:
            return "dyn.mul";
        case Op::DynMod:
            return "dyn.mod";
        case Op::DynDiv:
            return "dyn.div";
        case Op::ICmpLt:
            return "icmp.lt";
        case Op::ICmpLe:
            return "icmp.le";
        case Op::ICmpGt:
            return "icmp.gt";
        case Op::ICmpGe:
            return "icmp.ge";
        case Op::ICmpEq:
            return "icmp.eq";
        case Op::ICmpNe:
            return "icmp.ne";
        case Op::FCmpLt:
            return "fcmp.lt";
        case Op::FCmpLe:
            return "fcmp.le";
        case Op::FCmpGt:
            return "fcmp.gt";
        case Op::FCmpGe:
            return "fcmp.ge";
        case Op::FCmpEq:
            return "fcmp.eq";
        case Op::FCmpNe:
            return "fcmp.ne";
        case Op::DynCmpLt:
            return "dyn.lt";
        case Op::DynCmpLe:
            return "dyn.le";
        case Op::DynCmpGt:
            return "dyn.gt";
        case Op::DynCmpGe:
            return "dyn.ge";
        case Op::DynCmpEq:
            return "dyn.eq";
        case Op::DynCmpNe:
            return "dyn.ne";
        case Op::Not:
            return "not";
        case Op::Box:
            return "box";
        case Op::Unbox:
            return "unbox";
        case Op::GuardInt48:
            return "guard.i48";
        case Op::LoadSlot:
            return "load.slot";
        case Op::StoreSlot:
            return "store.slot";
        case Op::LoadGlobal:
            return "load.global";
        case Op::StoreGlobal:
            return "store.global";
        case Op::LoadCapture:
            return "load.capture";
        case Op::StoreCapture:
            return "store.capture";
        case Op::Pop:
            return "pop";
        case Op::Dup:
            return "dup";
        case Op::StoreImmSlot:
            return "store.imm.slot";
        case Op::StoreBImmSlot:
            return "store.bimm.slot";
        case Op::StoreNSlot:
            return "store.n.slot";
        case Op::StoreCSlot:
            return "store.c.slot";
        case Op::CopySlot:
            return "copy.slot";
        case Op::MakeArray:
            return "make.array";
        case Op::MakeObject:
            return "make.object";
        case Op::StrConcat:
            return "str.concat";
        case Op::FormatValue:
            return "format";
        case Op::IterArray:
            return "iter.array";
        case Op::LoadIndex:
            return "load.index";
        case Op::StoreIndex:
            return "store.index";
        case Op::LoadProperty:
            return "load.prop";
        case Op::StoreProperty:
            return "store.prop";
        case Op::Call:
            return "call";
        case Op::CallMethod:
            return "call.method";
        case Op::Phi:
            return "phi";
        case Op::Jump:
            return "jump";
        case Op::Branch:
            return "branch";
        case Op::Return:
            return "return";
    }
    return "???";
}

static const char *ty_name(Ty t) {
    switch (t) {
        case Ty::Bottom:
            return "_";
        case Ty::Unknown:
            return "?";
        case Ty::Int48:
            return "i48";
        case Ty::Float:
            return "f64";
        case Ty::Number:
            return "num";
        case Ty::Bool:
            return "bool";
        case Ty::Heap:
            return "heap";
        case Ty::None:
            return "none";
    }
    return "?";
}

std::string dump(const Func &f) {
    std::string out;
    char line[256];
    auto emit_inst = [&](ValueId id) {
        const Inst &in = f.inst(id);
        if (in.result != InvalidValue) {
            snprintf(line, sizeof(line), "  v%d:%s = %s", in.result, ty_name(in.type), op_name(in.op));
        } else {
            snprintf(line, sizeof(line), "  %s", op_name(in.op));
        }
        out += line;
        for (ValueId o : in.operands) {
            snprintf(line, sizeof(line), " v%d", o);
            out += line;
        }
        if (in.op == Op::IConst || in.op == Op::BConst) {
            snprintf(line, sizeof(line), " #%lld", (long long)in.imm_int);
            out += line;
        } else if (in.op == Op::FConst) {
            snprintf(line, sizeof(line), " #%g", in.imm_float);
            out += line;
        } else if (in.op == Op::StoreImmSlot || in.op == Op::StoreBImmSlot) {
            snprintf(line, sizeof(line), " #%lld -> @%u", (long long)in.imm_int, in.imm_u32);
            out += line;
        } else if (in.op == Op::StoreNSlot) {
            snprintf(line, sizeof(line), " -> @%u", in.imm_u32);
            out += line;
        } else if (in.op == Op::StoreCSlot) {
            snprintf(line, sizeof(line), " const#%lld -> @%u", (long long)in.imm_int, in.imm_u32);
            out += line;
        } else if (in.op == Op::CopySlot) {
            snprintf(line, sizeof(line), " @%lld -> @%u", (long long)in.imm_int, in.imm_u32);
            out += line;
        } else if (in.op == Op::LoadSlot || in.op == Op::StoreSlot || in.op == Op::LoadGlobal ||
                   in.op == Op::StoreGlobal || in.op == Op::LoadCapture || in.op == Op::StoreCapture ||
                   in.op == Op::LoadConst || in.op == Op::MakeArray || in.op == Op::MakeObject ||
                   in.op == Op::FormatValue || in.op == Op::LoadProperty || in.op == Op::StoreProperty) {
            snprintf(line, sizeof(line), " @%u", in.imm_u32);
            out += line;
        } else if (in.op == Op::Call) {
            snprintf(line, sizeof(line), " argc=%u", in.imm_u32);
            out += line;
        } else if (in.op == Op::CallMethod) {
            snprintf(line, sizeof(line), " @%u argc=%lld", in.imm_u32, (long long)in.imm_int);
            out += line;
        } else if (in.op == Op::Jump) {
            snprintf(line, sizeof(line), " -> b%d", in.target0);
            out += line;
        } else if (in.op == Op::Branch) {
            snprintf(line, sizeof(line), " ? b%d : b%d", in.target0, in.target1);
            out += line;
        } else if (in.op == Op::StrConcat && in.imm_u32) {
            out += " [inplace]";
        }
        out += "\n";
    };
    for (const Block &b : f.blocks) {
        snprintf(line, sizeof(line), "b%d:", b.id);
        out += line;
        if (!b.preds.empty()) {
            out += "  ; preds";
            for (BlockId p : b.preds) {
                snprintf(line, sizeof(line), " b%d", p);
                out += line;
            }
        }
        out += "\n";
        for (ValueId p : b.phis) {
            emit_inst(p);
        }
        for (ValueId i : b.insts) {
            emit_inst(i);
        }
        if (b.terminator != InvalidValue) {
            emit_inst(b.terminator);
        }
    }
    return out;
}

} // namespace ir
} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
