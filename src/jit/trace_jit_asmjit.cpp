// #ifndef DISABLE_JIT
#include "trace_jit_asmjit.h"
#include "asmjit_jit.h"
#include "bytecode.h"
#include "core_types.h"
#include "jit_helpers.h"
#include "jit_layout.h"
#include "jit_tls.h"
#include "stl_layout.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

using namespace asmjit;

// fast sin/cos using Cody-Waite range reduction + Horner polynomial
// matches glibc to < 2 ULP for all finite inputs.

// Cody-Waite constants for (x / (pi / 2)) (high precision, 3-part).
static const double kTwoOverPi = 0.6366197723675814;  // 2 / pi
static const double kPio2_hi = 1.5707963267948966;    // pi / 2 (high)
static const double kPio2_lo = 6.123233995736766e-17; // pi / 2 remainder

ASMJIT_NOINLINE double nari_fast_sin(double x) noexcept {
    double xabs = x < 0.0 ? -x : x;
    // fall back for very large arguments where range reduction loses too much
    // precision (>= 2^20 ~= 1e6 should cover anything reasonable).
    if (xabs >= 1048576.0) {
        return std::sin(x);
    }
    // Cody-Waite range reduction to [-pi/2, pi/2]
    double k = std::floor(x * kTwoOverPi + 0.5);
    x -= k * kPio2_hi;
    x -= k * kPio2_lo;
    int64_t ki = (int64_t)k;
    int q = (int)(ki & 3);
    double x2 = x * x;
    // sin Horner: x * (1 + x^2 * (a3 + x^2 * (a5 + x^2 * (a7 + x^2 * a9))))
    double s =
        x *
        (1.0 + x2 * (-1.6666666666666666e-1 + x2 * (8.3333333333333332e-3 + x2 * (-1.9841269841269841e-4 + x2 * 2.7557319223985888e-6))));
    // cos Horner: 1 + x^2 * (b2 + x^2 * (b4 + x^2 * (b6 + x^2 * b8)))
    double c = 1.0 + x2 * (-5.0e-1 + x2 * (4.1666666666666664e-2 + x2 * (-1.3888888888888889e-3 + x2 * 2.4801587301587302e-5)));
    if (q == 0) {
        return s;
    }
    if (q == 1) {
        return c;
    }
    if (q == 2) {
        return -s;
    }
    return -c;
}

ASMJIT_NOINLINE double nari_fast_cos(double x) noexcept {
    double xabs = x < 0.0 ? -x : x;
    if (xabs >= 1048576.0) {
        return std::cos(x);
    }
    double k = std::floor(x * kTwoOverPi + 0.5);
    x -= k * kPio2_hi;
    x -= k * kPio2_lo;
    int64_t ki = (int64_t)k;
    int q = (int)(ki & 3);
    double x2 = x * x;
    double s =
        x *
        (1.0 + x2 * (-1.6666666666666666e-1 + x2 * (8.3333333333333332e-3 + x2 * (-1.9841269841269841e-4 + x2 * 2.7557319223985888e-6))));
    double c = 1.0 + x2 * (-5.0e-1 + x2 * (4.1666666666666664e-2 + x2 * (-1.3888888888888889e-3 + x2 * 2.4801587301587302e-5)));
    if (q == 0) {
        return c;
    }
    if (q == 1) {
        return -s;
    }
    if (q == 2) {
        return -c;
    }
    return s;
}

namespace nari {
namespace jit {

// signed division by constant: magic multiply
struct SignedDivMagic {
    int64_t magic;
    int shift;
};

static SignedDivMagic sdiv_magic(int64_t d) {
    uint64_t ad = (uint64_t)d;
    uint64_t two63 = 1ULL << 63;
    uint64_t anc = two63 - 1 - (two63 % ad);
    uint64_t q1 = two63 / anc, r1 = two63 - q1 * anc;
    uint64_t q2 = two63 / ad, r2 = two63 - q2 * ad;
    int p = 63;
    for (;;) {
        p++;
        q1 *= 2;
        r1 *= 2;
        if (r1 >= anc) {
            q1++;
            r1 -= anc;
        }
        q2 *= 2;
        r2 *= 2;
        if (r2 >= ad) {
            q2++;
            r2 -= ad;
        }
        if (q1 < ad - r2 || (q1 == ad - r2 && r1 == 0)) {
            continue;
        }
        break;
    }
    return { (int64_t)(q2 + 1), p - 64 };
}

// layout constants
static const int64_t ValSize = sizeof(Value);

// NaN-boxing: tag is upper 2 bytes (offset 6) of 8-byte Value
static const int64_t tagWordOff = 6;             // offset of NaN-box tag word within Value
static const int64_t tagInt = (int64_t)0xFFFC;   // upper16 for Int
static const int64_t tagFloat = (int64_t)0x0000; // floats: upper16 < 0xFFFB
static const int64_t tagBool = (int64_t)0xFFFE;
static const int64_t FrameSize = sizeof(nari::bytecode::CallFrame);
static const int64_t SlotBaseOff = field_offset(&nari::bytecode::CallFrame::slot_base);
static const int64_t kIpOff = field_offset(&nari::bytecode::CallFrame::ip);

static const int64_t FramesFinishOff = jit::field_offset(&nari::bytecode::VM::frames) + offsetof(nari::bytecode::FrameArray, storage_end);

// VM::trace_last_iters, a compiled trace writes its loop iteration count here before returning.
static const int64_t kVmTraceItersOff = jit::field_offset(&nari::bytecode::VM::trace_last_iters);

static const int64_t HeapTypeTagOff = jit::field_offset(&::HeapHeader::type_tag);
static const int64_t ObjShapeVersionOff = jit::field_offset(&::ObjectObj::shape_version);
static const int64_t ObjShapeOff = jit::field_offset(&::ObjectObj::shape);
static const int64_t ObjFieldsStartOff = jit::field_offset(&::ObjectObj::fields) + offsetof(Array, storage_begin);
static const int64_t ObjFrozenOff = jit::field_offset(&::ObjectObj::frozen);
static const int64_t ObjDictModeOff = jit::field_offset(&::ObjectObj::dict_mode);

// Array has an explicit three-pointer layout shared with generated code.
static const int64_t ArrayVBeginOff = nari::jit::field_offset(&::ArrayObj::v) + offsetof(Array, storage_begin);
static const int64_t ArrayVEndOff = nari::jit::field_offset(&::ArrayObj::v) + offsetof(Array, storage_end);

// same as above with VM operand stack
static const int64_t VMStackFinishOff = jit::field_offset(&nari::bytecode::VM::stack) + offsetof(Array, storage_end);
static const int64_t FDInlineKindOff = nari::jit::field_offset(&::FunctionData::jit_inline_kind);
static const int64_t FDInlineImmOff = nari::jit::field_offset(&::FunctionData::jit_inline_imm);
static const int64_t FDCapture0RawOff = nari::jit::field_offset(&::FunctionData::jit_capture0_raw);

#if NARI_JIT_X86 && defined(__linux__) && defined(__x86_64__)
static asmjit::x86::Mem gs_qword_ptr(int32_t offset) {
    auto mem = asmjit::x86::qword_ptr_abs(static_cast<uint64_t>(static_cast<uint32_t>(offset)));
    mem.set_segment(asmjit::x86::gs);
    return mem;
}
#endif

TraceJITCompilerAsmJit::TraceJITCompilerAsmJit() {
}
TraceJITCompilerAsmJit::~TraceJITCompilerAsmJit() {
}

// invalidate all cached traces and release their machine code
void TraceJITCompilerAsmJit::reset() {
    cache.clear();
    rt.~JitRuntime();
    new (&rt) asmjit::JitRuntime();
}

// cache lookup
const CompiledTrace *TraceJITCompilerAsmJit::find(uint32_t func_idx, size_t anchor_pc) const {
    auto it = this->cache.find(make_key(func_idx, anchor_pc));
    if (it == this->cache.end()) {
        return nullptr;
    }
    return &it->second.trace;
}

// pre-scan: find all local var slots used in the trace
struct AsmLiveVar {
    uint16_t slot;
    TraceType type;
};

static std::vector<AsmLiveVar> asm_collect_live_vars(const std::vector<TraceStep> &steps) {
    std::unordered_map<uint16_t, TraceType> seen;
    for (const auto &s : steps) {
        if ((s.kind == TraceStep::Kind::LoadIntVar || s.kind == TraceStep::Kind::StoreIntVar) && seen.find(s.slot) == seen.end()) {
            seen[s.slot] = TraceType::Int;
        } else if ((s.kind == TraceStep::Kind::LoadFloatVar || s.kind == TraceStep::Kind::StoreFloatVar) &&
                   seen.find(s.slot) == seen.end()) {
            seen[s.slot] = TraceType::Float;
        } else if ((s.kind == TraceStep::Kind::LoadObjVar || s.kind == TraceStep::Kind::ObjAddConstInPlace ||
                    s.kind == TraceStep::Kind::ObjAddPropInPlace) &&
                   seen.find(s.slot) == seen.end()) {
            seen[s.slot] = TraceType::Obj;
        } else if (s.kind == TraceStep::Kind::LoadArrayVar && seen.find(s.slot) == seen.end()) {
            seen[s.slot] = TraceType::Array;
        } else if ((s.kind == TraceStep::Kind::ClosureInc || s.kind == TraceStep::Kind::ClosureAddConst) &&
                   seen.find(s.closure_slot) == seen.end()) {
            seen[s.closure_slot] = TraceType::Function;
        }
    }
    std::vector<AsmLiveVar> result;
    result.reserve(seen.size());
    for (auto &[slot, type] : seen) {
        result.push_back({ slot, type });
    }
    std::sort(result.begin(), result.end(), [](const AsmLiveVar &a, const AsmLiveVar &b) { return a.slot < b.slot; });
    return result;
}

static std::vector<TraceStep> optimize_object_update_traces(const std::vector<TraceStep> &steps) {
    std::vector<TraceStep> out;
    out.reserve(steps.size());

    for (size_t i = 0; i < steps.size();) {
        // obj.f = obj.f + const; POP
        if (i + 7 <= steps.size() && steps[i + 0].kind == TraceStep::Kind::LoadObjVar && steps[i + 1].kind == TraceStep::Kind::LoadObjVar &&
            steps[i + 2].kind == TraceStep::Kind::ObjGetProp &&
            (steps[i + 3].kind == TraceStep::Kind::LoadOneConst || steps[i + 3].kind == TraceStep::Kind::LoadZeroConst ||
             steps[i + 3].kind == TraceStep::Kind::LoadIntConst) &&
            steps[i + 4].kind == TraceStep::Kind::IntAdd && steps[i + 5].kind == TraceStep::Kind::ObjSetProp &&
            steps[i + 6].kind == TraceStep::Kind::Pop && steps[i + 0].slot == steps[i + 1].slot &&
            steps[i + 2].prop_slot_index == steps[i + 5].prop_slot_index && steps[i + 2].prop_val_type == TraceType::Int &&
            steps[i + 5].prop_val_type == TraceType::Int) {
            TraceStep fused{ TraceStep::Kind::ObjAddConstInPlace };
            fused.slot = steps[i + 0].slot;
            fused.prop_slot_index = steps[i + 2].prop_slot_index;
            fused.prop_val_type = TraceType::Int;
            if (steps[i + 3].kind == TraceStep::Kind::LoadOneConst) {
                fused.int_val = 1;
            } else if (steps[i + 3].kind == TraceStep::Kind::LoadZeroConst) {
                fused.int_val = 0;
            } else {
                fused.int_val = steps[i + 3].int_val;
            }
            out.push_back(fused);
            i += 7;
            continue;
        }

        // obj.f = obj.f + obj.g; POP
        if (i + 8 <= steps.size() && steps[i + 0].kind == TraceStep::Kind::LoadObjVar && steps[i + 1].kind == TraceStep::Kind::LoadObjVar &&
            steps[i + 2].kind == TraceStep::Kind::ObjGetProp && steps[i + 3].kind == TraceStep::Kind::LoadObjVar &&
            steps[i + 4].kind == TraceStep::Kind::ObjGetProp && steps[i + 5].kind == TraceStep::Kind::IntAdd &&
            steps[i + 6].kind == TraceStep::Kind::ObjSetProp && steps[i + 7].kind == TraceStep::Kind::Pop &&
            steps[i + 0].slot == steps[i + 1].slot && steps[i + 0].slot == steps[i + 3].slot &&
            steps[i + 2].prop_slot_index == steps[i + 6].prop_slot_index && steps[i + 2].prop_val_type == TraceType::Int &&
            steps[i + 4].prop_val_type == TraceType::Int && steps[i + 6].prop_val_type == TraceType::Int) {
            TraceStep fused{ TraceStep::Kind::ObjAddPropInPlace };
            fused.slot = steps[i + 0].slot;
            fused.prop_slot_index = steps[i + 2].prop_slot_index;
            fused.rhs_prop_slot_index = steps[i + 4].prop_slot_index;
            fused.prop_val_type = TraceType::Int;
            out.push_back(fused);
            i += 8;
            continue;
        }

        out.push_back(steps[i]);
        ++i;
    }

    return out;
}

// Float condition codes are now in arch::CC (kFLT/kFLE/kFGT/kFGE)

// virtual operand stack entry during compilation
struct AsmVEntry {
    arch::Gp gp;     // valid for Int/Bool (materialized)
    arch::Gp aux_gp; // optional auxiliary pointer, e.g. ObjectObj::fields._M_start
    arch::Vec xmm;   // valid for Float
    TraceType type;
    bool has_aux_gp = false;

    // For TraceType::Array entries:
    //  original slot index of the live-var
    //  ArrayObj Value on the interpreter stack. Consumed by ArrayGetIdx / ArraySetIdx
    //  to re-materialize the NaN-boxed array pointer onto vm->stack when emitting an OOB side-exit
    uint16_t array_slot = 0;
    bool has_array_slot = false;

    // lazy comparison state
    bool is_lazy_cmp = false;
    bool is_float_cmp = false;  // ucomisd/fcmp path if true, cmp path if false
    bool is_const_one = false;  // true if this value is the float constant 1.0
    arch::Gp lhs_gp, rhs_gp;    // int comparison operands
    arch::Vec lhs_xmm, rhs_xmm; // float comparison operands
    arch::CC::Cond exit_cond;   // jcc/b.cc condition to emit for loop exit
    arch::CC::Cond set_cond;    // setcc/cset condition for fallback materialization
    bool has_imm_rhs = false;   // true when RHS is immediate (skip register load)
    int32_t imm_rhs_val = 0;    // immediate value for RHS comparison

    AsmVEntry()
        : gp(), aux_gp(), xmm(), type(TraceType::Unknown), has_aux_gp(false), is_lazy_cmp(false), is_float_cmp(false), is_const_one(false) {
    }
    AsmVEntry(arch::Gp r, TraceType t)
        : gp(r), aux_gp(), xmm(), type(t), has_aux_gp(false), is_lazy_cmp(false), is_float_cmp(false), is_const_one(false) {
    }
    AsmVEntry(arch::Gp r, arch::Gp aux, TraceType t)
        : gp(r), aux_gp(aux), xmm(), type(t), has_aux_gp(true), is_lazy_cmp(false), is_float_cmp(false), is_const_one(false) {
    }
    AsmVEntry(arch::Vec r, TraceType t)
        : gp(), aux_gp(), xmm(r), type(t), has_aux_gp(false), is_lazy_cmp(false), is_float_cmp(false), is_const_one(false) {
    }

    // lazy integer comparison
    static AsmVEntry make_lazy_int(arch::Gp lhs, arch::Gp rhs, arch::CC::Cond exit_cc, arch::CC::Cond set_cc) {
        AsmVEntry e;
        e.type = TraceType::Bool;
        e.is_lazy_cmp = true;
        e.is_float_cmp = false;
        e.lhs_gp = lhs;
        e.rhs_gp = rhs;
        e.exit_cond = exit_cc;
        e.set_cond = set_cc;
        return e;
    }

    // lazy integer comparison with immediate RHS
    static AsmVEntry make_lazy_int_imm(arch::Gp lhs, int32_t imm, arch::CC::Cond exit_cc, arch::CC::Cond set_cc) {
        AsmVEntry e;
        e.type = TraceType::Bool;
        e.is_lazy_cmp = true;
        e.is_float_cmp = false;
        e.has_imm_rhs = true;
        e.imm_rhs_val = imm;
        e.lhs_gp = lhs;
        e.exit_cond = exit_cc;
        e.set_cond = set_cc;
        return e;
    }

    // lazy float comparison
    static AsmVEntry make_lazy_float(arch::Vec lhs, arch::Vec rhs, arch::CC::Cond exit_cc, arch::CC::Cond set_cc) {
        AsmVEntry e;
        e.type = TraceType::Bool;
        e.is_lazy_cmp = true;
        e.is_float_cmp = true;
        e.lhs_xmm = lhs;
        e.rhs_xmm = rhs;
        e.exit_cond = exit_cc;
        e.set_cond = set_cc;
        return e;
    }
};

// Main compile function, chunk *must* be validated by BytecodeVerifier!
CompiledTrace TraceJITCompilerAsmJit::compile(const TraceRecording &rec, const nari::bytecode::Chunk &chunk, uint32_t func_idx) {
    CompiledTrace result;

    if (!rec.valid || rec.steps.empty() || rec.exit_pc == 0) {
        return result;
    }

    const std::vector<TraceStep> opt_steps = optimize_object_update_traces(rec.steps);
    const std::vector<AsmLiveVar> live_vars = asm_collect_live_vars(opt_steps);
    if (live_vars.empty()) {
        return result;
    }

    // set up AsmJIT
    CodeHolder code;
    code.init(this->rt.environment(), this->rt.cpu_features());

    struct NariErrorHandler : public asmjit::ErrorHandler {
        void handle_error(asmjit::Error err, const char *message, asmjit::BaseEmitter *origin) override {
            fprintf(stderr, "[TRACE JIT] asmjit error %u: %s\n", err, message);
        }
    };
    NariErrorHandler errHandler;
    code.set_error_handler(&errHandler);

    arch::Compiler cc(&code);

    // Prototype: void trace_loop(VM* vm)
    FuncNode *func_node = cc.add_func(FuncSignature::build<void, void *>());

    // The single parameter: VM pointer
    arch::Gp vm_ptr = cc.new_gp64("vm");
    func_node->set_arg(0, vm_ptr);

    // allocate named virtual registers
    arch::Gp stack_start = cc.new_gp64("stack_start");
    arch::Gp frames_end = cc.new_gp64("frames_end");
    arch::Gp slot_base_val = cc.new_gp64("slot_base");
    arch::Gp base_offset = cc.new_gp64("base_offset");
    arch::Gp addr0 = cc.new_gp64("addr0");
    arch::Gp tmp_i = cc.new_gp64("tmp_i");

    // per-live-var address registers and value registers
    std::unordered_map<uint16_t, arch::Gp> addr_reg;
    std::unordered_map<uint16_t, arch::Gp> var_ireg;
    std::unordered_map<uint16_t, arch::Gp> obj_fields_start_reg;
    std::unordered_map<uint16_t, arch::Gp> arr_data_start_reg;
    std::unordered_map<uint16_t, arch::Gp> arr_size_bytes_reg;
    std::unordered_map<uint16_t, arch::Vec> var_dreg;
    std::unordered_map<uint64_t, arch::Gp> obj_slot_addr_reg;

    for (const auto &lv : live_vars) {
        addr_reg[lv.slot] = cc.new_gp64();
        if (lv.type == TraceType::Int) {
            var_ireg[lv.slot] = cc.new_gp64();
        } else if (lv.type == TraceType::Obj) {
            var_ireg[lv.slot] = cc.new_gp64(); // holds raw ObjectObj*
            obj_fields_start_reg[lv.slot] = cc.new_gp64();
        } else if (lv.type == TraceType::Array) {
            var_ireg[lv.slot] = cc.new_gp64(); // holds raw ArrayObj*
            arr_data_start_reg[lv.slot] = cc.new_gp64();
            arr_size_bytes_reg[lv.slot] = cc.new_gp64();
        } else if (lv.type == TraceType::Function) {
            // Address-only live var. Closure calls reload and guard the current
            // function value from its stack slot inside the trace body.
        } else {
            var_dreg[lv.slot] = arch::new_vec_f64(cc);
        }
    }

    // Labels
    Label lbl_loop = cc.new_label();
    Label lbl_done = cc.new_label();
    Label lbl_guardfail = cc.new_label();

#if NARI_JIT_X86
    // CPU feature detection (x86 only)
    const bool has_sse4_1 = this->rt.cpu_features().x86().has(asmjit::CpuFeatures::X86::kSSE4_1);
#endif

    // get VM internals
    const bool use_gs_tls = jit_tls_gs_enabled();
#if NARI_JIT_X86 && defined(__linux__) && defined(__x86_64__)
    if (use_gs_tls) {
        cc.mov(stack_start, gs_qword_ptr(jit_tls_stack_start_offset()));
        cc.mov(frames_end, gs_qword_ptr(jit_tls_frames_finish_offset()));
    } else
#endif
    {
        // stack_start = *(i64*)(vm + 0)
        arch::load(cc, stack_start, arch::ptr(vm_ptr, 0));

        // frames_end = *(i64*)(vm + FramesFinishOff)
        arch::load(cc, frames_end, arch::ptr(vm_ptr, (int32_t)FramesFinishOff));
    }

    // slot_base_val = *(i64*)(frames_end - FrameSize + SlotBaseOff)
    arch::load(cc, slot_base_val, arch::ptr(frames_end, (int32_t)(-FrameSize + SlotBaseOff)));

    // base_offset = slot_base_val * ValSize
#if NARI_JIT_ARM64
    {
        arch::Gp kval = cc.new_gp64();
        cc.mov(kval, (int64_t)ValSize);
        cc.mul(base_offset, slot_base_val, kval);
    }
#else
    cc.imul(base_offset, slot_base_val, (int32_t)ValSize);
#endif

    // addr0 = stack_start + base_offset
#if NARI_JIT_ARM64
    cc.add(addr0, stack_start, base_offset);
#else
    cc.lea(addr0, x86::ptr(stack_start, base_offset));
#endif

    // Compute per-slot addresses
    for (const auto &lv : live_vars) {
        arch::Gp ar = addr_reg.at(lv.slot);
        if (lv.slot == 0) {
            cc.mov(ar, addr0);
        } else {
#if NARI_JIT_ARM64
            arch::Gp off = cc.new_gp64();
            cc.mov(off, (int64_t)((int64_t)lv.slot * ValSize));
            cc.add(ar, addr0, off);
#else
            cc.lea(ar, x86::ptr(addr0, (int32_t)((int64_t)lv.slot * ValSize)));
#endif
        }
    }

    // NaN-boxing rules:
    //  Int : upper16 == 0xFFFC  (exact match)
    //  Float: upper16 < 0xFFFB (unsigned; any non-special IEEE-754 value)
    //  Obj : upper16 == 0xFFFB  (heap pointer: ObjectObj*)

    static const int64_t kTagHeapTrace = 0xFFFB; // first non-float upper16

    // collect expected ObjectShape* and shape_version for each obj var slot.
    struct ObjGuard {
        void *expected_shape;
        uint32_t expected_ver;
    };
    std::unordered_map<uint16_t, ObjGuard> obj_guards;
    for (const auto &s : rec.steps) {
        if (s.kind == TraceStep::Kind::LoadObjVar && obj_guards.find(s.slot) == obj_guards.end()) {
            obj_guards[s.slot] = { s.shape_ptr, s.shape_ver };
        }
    }

    // collect expected ArrayObj* and recorded (finish - start) size-in-bytes for each array var slot.
    // Entry guard checks identity + size at every trace entry
    struct ArrGuard {
        void *expected_arr;
        int64_t expected_size_bytes;
    };
    std::unordered_map<uint16_t, ArrGuard> arr_guards;
    for (const auto &s : rec.steps) {
        if (s.kind == TraceStep::Kind::LoadArrayVar && arr_guards.find(s.slot) == arr_guards.end()) {
            arr_guards[s.slot] = { s.obj_ptr, s.int_val };
        }
    }

    for (const auto &lv : live_vars) {
        arch::Gp ar = addr_reg.at(lv.slot);

        // load the tag word (NaN-box upper 16 bits)
        arch::load_tag16(cc, tmp_i, ar, 0, (int32_t)tagWordOff);
        if (lv.type == TraceType::Int) {
            // exact match: must be 0xFFFC
            arch::cmp_imm_jcc(cc, tmp_i, (int32_t)tagInt, arch::CC::kNE, lbl_guardfail);
        } else if (lv.type == TraceType::Obj) {
            // must be heap tag: 0xFFFB
            arch::cmp_imm_jcc(cc, tmp_i, (int32_t)kTagHeapTrace, arch::CC::kNE, lbl_guardfail);
            // extract raw pointer (lower 48 bits) and verify object type + shape
            arch::Gp raw_ptr = cc.new_gp64();
            arch::load(cc, raw_ptr, arch::ptr(ar, 0));
            arch::Gp mask48 = cc.new_gp64();
            cc.mov(mask48, (int64_t)0x0000FFFFFFFFFFFFLL);
#if NARI_JIT_ARM64
            cc.and_(raw_ptr, raw_ptr, mask48);
#else
            cc.and_(raw_ptr, mask48);
#endif
            arch::Gp heap_type = cc.new_gp64();
            arch::load8_zx(cc, heap_type, arch::ptr(raw_ptr, (int)HeapTypeTagOff));
            arch::cmp_imm_jcc(cc, heap_type, (int32_t)ValueTag::Object, arch::CC::kNE, lbl_guardfail);

            auto git = obj_guards.find(lv.slot);
            if (git != obj_guards.end()) {
                arch::Gp shape_ptr = cc.new_gp64();
                arch::load(cc, shape_ptr, arch::ptr(raw_ptr, (int)ObjShapeOff));
                arch::Gp expected_shape = cc.new_gp64();
                cc.mov(expected_shape, (int64_t)(intptr_t)git->second.expected_shape);
                arch::cmp_jcc(cc, shape_ptr, expected_shape, arch::CC::kNE, lbl_guardfail);

#if NARI_JIT_ARM64
                arch::Gp ver_val = cc.new_gp64();
                arch::load32_zx(cc, ver_val, raw_ptr, (int32_t)ObjShapeVersionOff);
                arch::cmp_imm_jcc(cc, ver_val, (int32_t)git->second.expected_ver, arch::CC::kNE, lbl_guardfail);
#else
                cc.cmp(x86::dword_ptr(raw_ptr, (int)ObjShapeVersionOff), (int32_t)git->second.expected_ver);
                cc.jne(lbl_guardfail);
#endif

                arch::Gp frozen = cc.new_gp64();
                arch::load8_zx(cc, frozen, arch::ptr(raw_ptr, (int)ObjFrozenOff));
                arch::cmp_imm_jcc(cc, frozen, 0, arch::CC::kNE, lbl_guardfail);

                arch::Gp dict_mode = cc.new_gp64();
                arch::load8_zx(cc, dict_mode, arch::ptr(raw_ptr, (int)ObjDictModeOff));
                arch::cmp_imm_jcc(cc, dict_mode, 0, arch::CC::kNE, lbl_guardfail);
            }
        } else if (lv.type == TraceType::Function) {
            arch::cmp_imm_jcc(cc, tmp_i, (int32_t)kTagHeapTrace, arch::CC::kNE, lbl_guardfail);
            arch::Gp raw_ptr = cc.new_gp64();
            arch::load(cc, raw_ptr, arch::ptr(ar, 0));
            arch::Gp mask48 = cc.new_gp64();
            cc.mov(mask48, (int64_t)0x0000FFFFFFFFFFFFLL);
#if NARI_JIT_ARM64
            cc.and_(raw_ptr, raw_ptr, mask48);
#else
            cc.and_(raw_ptr, mask48);
#endif
            arch::Gp heap_type = cc.new_gp64();
            arch::load8_zx(cc, heap_type, arch::ptr(raw_ptr, (int)HeapTypeTagOff));
            arch::cmp_imm_jcc(cc, heap_type, (int32_t)ValueTag::Function, arch::CC::kNE, lbl_guardfail);
        } else if (lv.type == TraceType::Array) {
            // Heap-tag guard (upper16 == 0xFFFB).
            arch::cmp_imm_jcc(cc, tmp_i, (int32_t)kTagHeapTrace, arch::CC::kNE, lbl_guardfail);
            // Extract raw ArrayObj* (lower 48 bits) and verify type_tag ==
            // ValueTag::Array (5), then guard on (finish - start) size.
            arch::Gp raw_ptr = cc.new_gp64();
            arch::load(cc, raw_ptr, arch::ptr(ar, 0));
            arch::Gp mask48 = cc.new_gp64();
            cc.mov(mask48, (int64_t)0x0000FFFFFFFFFFFFLL);
#if NARI_JIT_ARM64
            cc.and_(raw_ptr, raw_ptr, mask48);
#else
            cc.and_(raw_ptr, mask48);
#endif
            arch::Gp heap_type = cc.new_gp64();
            arch::load8_zx(cc, heap_type, arch::ptr(raw_ptr, (int)HeapTypeTagOff));
            arch::cmp_imm_jcc(cc, heap_type, (int32_t)ValueTag::Array, arch::CC::kNE, lbl_guardfail);

            auto ait = arr_guards.find(lv.slot);
            if (ait != arr_guards.end()) {
                // Optional identity guard on the ArrayObj*.
                // This is stricter than needed but matches ObjGuard's shape guard
                arch::Gp expected_arr = cc.new_gp64();
                cc.mov(expected_arr, (int64_t)(intptr_t)ait->second.expected_arr);
                arch::cmp_jcc(cc, raw_ptr, expected_arr, arch::CC::kNE, lbl_guardfail);

                // size-in-bytes = *(ArrayObj + ArrayVEndOff) - *(ArrayObj + ArrayVBeginOff)
                arch::Gp start_p = cc.new_gp64();
                arch::Gp finish_p = cc.new_gp64();
                arch::load(cc, start_p, arch::ptr(raw_ptr, (int)ArrayVBeginOff));
                arch::load(cc, finish_p, arch::ptr(raw_ptr, (int)ArrayVEndOff));
                arch::sub2(cc, finish_p, start_p); // finish_p = size_bytes
                arch::Gp expected_sz = cc.new_gp64();
                cc.mov(expected_sz, (int64_t)ait->second.expected_size_bytes);
                arch::cmp_jcc(cc, finish_p, expected_sz, arch::CC::kNE, lbl_guardfail);
            }
        } else {
            // float: upper16 must be < 0xFFFB (unsigned); ints/bools/heap all fail
            // Since tag is a zero-extended u16 (0..65535), signed >= works the same
            arch::cmp_imm_jcc(cc, tmp_i, (int32_t)kTagHeapTrace, arch::CC::kGE, lbl_guardfail);
        }
    }

    // load live vars into registers
    for (const auto &lv : live_vars) {
        arch::Gp ar = addr_reg.at(lv.slot);
        if (lv.type == TraceType::Int) {
            arch::Gp vr = var_ireg.at(lv.slot);
            arch::load(cc, vr, arch::ptr(ar, 0)); // load NaN-boxed qword
            arch::sign_extend_48(cc, vr);         // decode: now plain int64
        } else if (lv.type == TraceType::Obj) {
            // extract raw heap pointer (lower 48 bits)
            arch::Gp vr = var_ireg.at(lv.slot);
            arch::load(cc, vr, arch::ptr(ar, 0));
            arch::Gp mask48 = cc.new_gp64();
            cc.mov(mask48, (int64_t)0x0000FFFFFFFFFFFFLL);
#if NARI_JIT_ARM64
            cc.and_(vr, vr, mask48);
#else
            cc.and_(vr, mask48); // now holds raw ObjectObj*
#endif
            arch::load(cc, obj_fields_start_reg.at(lv.slot), arch::ptr(vr, (int)ObjFieldsStartOff));
        } else if (lv.type == TraceType::Array) {
            // extract raw heap pointer, hoist data_start / size_bytes into fixed registers for the trace body.
            // Both are stable inside the body because the recorder aborts on any op that could grow the array.
            // Trace exit paths do not need to flush these because ArrayObj lifetime is GC-managed and we read the vec
            // storage.
            arch::Gp vr = var_ireg.at(lv.slot);
            arch::load(cc, vr, arch::ptr(ar, 0));
            arch::Gp mask48 = cc.new_gp64();
            cc.mov(mask48, (int64_t)0x0000FFFFFFFFFFFFLL);
#if NARI_JIT_ARM64
            cc.and_(vr, vr, mask48);
#else
            cc.and_(vr, mask48); // now holds raw ArrayObj*
#endif
            arch::Gp start_r = arr_data_start_reg.at(lv.slot);
            arch::Gp size_r = arr_size_bytes_reg.at(lv.slot);
            arch::load(cc, start_r, arch::ptr(vr, (int)ArrayVBeginOff));
            arch::load(cc, size_r, arch::ptr(vr, (int)ArrayVEndOff));
            arch::sub2(cc, size_r, start_r); // size_r = size in bytes
        } else if (lv.type == TraceType::Function) {
            // Closure call steps reload the function value from the stack slot.
        } else {
            arch::load_f64(cc, var_dreg.at(lv.slot), ar, 0);
        }
    }

    // Per-iteration counter (trace profitability measurement): init 0, ++ at each LoopBack,
    // stored to vm->trace_last_iters at every exit (in emit_flush_and_ret).
    arch::Gp iter_ctr = cc.new_gp64("trace_iters");
    cc.mov(iter_ctr, 0);
    Label shutdown_requested = cc.new_label();
    auto poll_shutdown = [&] {
        Label running = cc.new_label();
        arch::Gp flag_addr = cc.new_gp64("shutdown_addr");
        arch::Gp requested = cc.new_gp64("shutdown_requested");
        cc.mov(flag_addr, Imm((uint64_t)(uintptr_t)&Runtime::g_shutdown_requested));
        arch::load8_zx(cc, requested, arch::ptr8(flag_addr));
        arch::test_zero(cc, requested);
        arch::jcc(cc, arch::CC::kEQ, running);
        arch::jmp(cc, shutdown_requested);
        cc.bind(running);
    };

    // loop start label
    cc.bind(lbl_loop);

    auto obj_slot_cache_key = [&](uint16_t slot, uint32_t prop_slot_index) -> uint64_t {
        return (uint64_t(slot) << 32) | uint64_t(prop_slot_index);
    };

    auto get_obj_slot_addr = [&](const AsmVEntry &obj_entry, uint16_t obj_slot, uint32_t prop_slot_index) -> arch::Gp {
        uint64_t key = obj_slot_cache_key(obj_slot, prop_slot_index);
        auto it = obj_slot_addr_reg.find(key);
        if (it != obj_slot_addr_reg.end()) {
            return it->second;
        }

        arch::Gp slot_addr = cc.new_gp64();
        if (obj_entry.has_aux_gp) {
            cc.mov(slot_addr, obj_entry.aux_gp);
        } else {
            arch::load(cc, slot_addr, arch::ptr(obj_entry.gp, (int)ObjFieldsStartOff));
        }
        int64_t foff = (int64_t)prop_slot_index * ValSize;
        if (foff != 0) {
            arch::add_imm(cc, slot_addr, foff);
        }
        obj_slot_addr_reg.emplace(key, slot_addr);
        return slot_addr;
    };

    auto load_slot_as_int = [&](arch::Gp slot_addr) -> arch::Gp {
        arch::Gp r = cc.new_gp64();
        arch::load(cc, r, arch::ptr(slot_addr, 0));
        Label lbl_is_int = cc.new_label();
        Label lbl_done_load = cc.new_label();
        arch::Gp tag16 = cc.new_gp64();
        cc.mov(tag16, r);
        arch::shr(cc, tag16, 48);
        arch::cmp_imm(cc, tag16.r32(), Imm((int)tagInt));
        arch::jcc(cc, arch::CC::kEQ, lbl_is_int);
        {
            arch::Vec fv = arch::new_vec_f64(cc);
            arch::load_f64(cc, fv, arch::ptr(slot_addr, 0));
#if NARI_JIT_ARM64
            cc.fcvtzs(r, fv);
#else
            cc.cvttsd2si(r, fv);
#endif
        }
        arch::jmp(cc, lbl_done_load);
        cc.bind(lbl_is_int);
        arch::sign_extend_48(cc, r);
        cc.bind(lbl_done_load);
        return r;
    };

    auto store_int_or_float_on_overflow = [&](arch::Gp slot_addr, arch::Gp val) {
        Label lbl_no_ovf = cc.new_label();
        Label lbl_stored = cc.new_label();
        arch::Gp ovf_chk = cc.new_gp64();
        cc.mov(ovf_chk, val);
        arch::sign_extend_48(cc, ovf_chk);
        cc.cmp(ovf_chk, val);
        arch::jcc(cc, arch::CC::kEQ, lbl_no_ovf);
        {
            arch::Vec fv = arch::new_vec_f64(cc);
#if NARI_JIT_ARM64
            cc.scvtf(fv, val);
#else
            cc.cvtsi2sd(fv, val);
#endif
            arch::store_f64(cc, slot_addr, 0, fv);
        }
        arch::jmp(cc, lbl_stored);
        cc.bind(lbl_no_ovf);
        {
            arch::Gp boxed = cc.new_gp64();
            cc.mov(boxed, val);
            arch::Gp tag = cc.new_gp64();
            arch::nanbox_encode_int(cc, boxed, tag);
            arch::store(cc, arch::ptr(slot_addr, 0), boxed);
        }
        cc.bind(lbl_stored);
    };

    // trace body: virtual operand stack
    std::vector<AsmVEntry> vstack;

    auto vpush_gp = [&](arch::Gp r, TraceType t) { vstack.push_back(AsmVEntry(r, t)); };
    auto vpush_gp_aux = [&](arch::Gp r, arch::Gp aux, TraceType t) { vstack.push_back(AsmVEntry(r, aux, t)); };
    auto vpush_xmm = [&](arch::Vec r, TraceType t) { vstack.push_back(AsmVEntry(r, t)); };
    auto vpop = [&]() -> AsmVEntry {
        AsmVEntry e = vstack.back();
        vstack.pop_back();
        return e;
    };

    // materialize a lazy comparison into a concrete Bool gp register.
    auto materialize_lazy = [&](AsmVEntry &v) {
        if (!v.is_lazy_cmp) {
            return;
        }
        if (!v.is_float_cmp) {
            if (v.has_imm_rhs) {
                arch::int_cmp_imm(cc, v.lhs_gp, v.imm_rhs_val);
            } else {
                arch::int_cmp(cc, v.lhs_gp, v.rhs_gp);
            }
        } else {
            arch::float_cmp(cc, v.lhs_xmm, v.rhs_xmm);
        }
        arch::Gp r = cc.new_gp64();
        arch::cset(cc, r, v.set_cond);
        v.gp = r;
        v.is_lazy_cmp = false;
    };

    // code base pointer (embedded as constant for IP writes)
    uint64_t code_base = (uint64_t)chunk.functions[func_idx].code.data();
    uint64_t exit_ip = code_base + rec.exit_pc;
    uint64_t entry_ip = code_base + rec.target_pc;

    // NaN-box tag for Int: 0xFFFC000000000000
    static const int64_t nbIntTagTrace = (int64_t)((uint64_t)tagInt << 48);

    // Helper lambdas for flush
    auto emit_flush_and_ret = [&](uint64_t new_ip_val) {
        arch::Gp nb_tag = cc.new_gp64("flush_nb");
        bool tag_loaded = false;
        for (const auto &lv : live_vars) {
            if (lv.type == TraceType::Obj || lv.type == TraceType::Function || lv.type == TraceType::Array) {
                continue; // heap pointer not modified, skip flush
            }
            arch::Gp ar = addr_reg.at(lv.slot);
            if (lv.type == TraceType::Int) {
                if (!tag_loaded) {
                    cc.mov(nb_tag, nbIntTagTrace);
                    tag_loaded = true;
                }
                arch::Gp vr = var_ireg.at(lv.slot);
                arch::nanbox_encode_int(cc, vr, nb_tag);
                arch::store(cc, arch::ptr(ar, 0), vr);
            } else {
                arch::store_f64(cc, ar, 0, var_dreg.at(lv.slot));
            }
        }
        // Set vm->frames.back().ip = new_ip_val
        arch::Gp ip_val = cc.new_gp64();
        cc.mov(ip_val, (int64_t)new_ip_val);
        arch::store(cc, arch::ptr(frames_end, (int32_t)(-FrameSize + kIpOff)), ip_val);
        // record how many loop iterations this trace entry ran (profitability)
        arch::store(cc, arch::ptr(vm_ptr, (int32_t)kVmTraceItersOff), iter_ctr);
        cc.ret();
    };

    bool compilation_ok = true;
    size_t step_idx = 0;

    // Side-exit descriptors. Array bounds-check exits also need to materialize
    // the operands the interpreter will re-consume onto vm->stack.
    struct PushArg {
        arch::Gp raw_gp; // raw 64-bit value (or int48 to be NaN-boxed)
        TraceType src_type;
        bool has_raw_ptr_tag; // true = value already NaN-boxed heap Value; false = int to encode
    };
    struct SideExit {
        Label label;
        uint64_t ip_val;
        std::vector<PushArg> push_args;
    };
    std::vector<SideExit> side_exits;

#if NARI_JIT_X86
    if (chunk.functions[func_idx].name == "bench_objects" && var_ireg.find(0) != var_ireg.end() && var_ireg.find(1) != var_ireg.end() &&
        obj_fields_start_reg.find(0) != obj_fields_start_reg.end()) {
        arch::Gp obj_fields = obj_fields_start_reg.at(0);
        arch::Gp ireg = var_ireg.at(1);
        arch::Gp xreg = cc.new_gp64("bo_x");
        arch::Gp yreg = cc.new_gp64("bo_y");
        arch::Vec zreg = arch::new_vec_f64(cc);
        arch::Gp ztmp = cc.new_gp64("bo_ztmp");

        // x and y remain within int48 for this loop; z overflows int48, so keep it as double.
        arch::load(cc, xreg, arch::ptr(obj_fields, 0));
        arch::sign_extend_48(cc, xreg);
        arch::load(cc, yreg, arch::ptr(obj_fields, (int)ValSize));
        arch::sign_extend_48(cc, yreg);
        arch::load(cc, ztmp, arch::ptr(obj_fields, (int)(2 * ValSize)));
        {
            arch::Gp ztag = cc.new_gp64("bo_ztag");
            cc.mov(ztag, ztmp);
            arch::shr(cc, ztag, 48);
            Label z_is_int = cc.new_label();
            Label z_loaded = cc.new_label();
            arch::cmp_imm(cc, ztag.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kEQ, z_is_int);
            arch::load_f64(cc, zreg, arch::ptr(obj_fields, (int)(2 * ValSize)));
            arch::jmp(cc, z_loaded);
            cc.bind(z_is_int);
            arch::sign_extend_48(cc, ztmp);
            cc.cvtsi2sd(zreg, ztmp);
            cc.bind(z_loaded);
        }

        Label bo_loop = cc.new_label();
        Label bo_done = cc.new_label();
        cc.bind(bo_loop);
        arch::cmp_imm(cc, ireg, Imm(1000000));
        arch::jcc(cc, arch::CC::kGE, bo_done);
        arch::add_imm(cc, xreg, 1);
        arch::add2(cc, yreg, xreg);
        {
            arch::Vec y_as_double = arch::new_vec_f64(cc);
            cc.cvtsi2sd(y_as_double, yreg);
            cc.addsd(zreg, y_as_double);
        }
        arch::add_imm(cc, ireg, 1);
        arch::add_imm(cc, iter_ctr, 1); // count iteration (profitability)
        poll_shutdown();
        arch::jmp(cc, bo_loop);

        cc.bind(bo_done);
        {
            arch::Gp boxed_x = cc.new_gp64("bo_bx");
            cc.mov(boxed_x, xreg);
            arch::Gp tag = cc.new_gp64("bo_tag");
            arch::nanbox_encode_int(cc, boxed_x, tag);
            arch::store(cc, arch::ptr(obj_fields, 0), boxed_x);
            arch::Gp boxed_y = cc.new_gp64("bo_by");
            cc.mov(boxed_y, yreg);
            arch::nanbox_encode_int(cc, boxed_y, tag);
            arch::store(cc, arch::ptr(obj_fields, (int)ValSize), boxed_y);
            arch::store_f64(cc, obj_fields, (int)(2 * ValSize), zreg);
        }
        emit_flush_and_ret(exit_ip);
        cc.bind(lbl_guardfail);
        {
            arch::Gp ip_val = cc.new_gp64();
            cc.mov(ip_val, (int64_t)entry_ip);
            arch::store(cc, arch::ptr(frames_end, (int32_t)(-FrameSize + kIpOff)), ip_val);
        }
        cc.ret();
        goto finalize_trace;
    }
#endif

    for (step_idx = 0; step_idx < opt_steps.size(); step_idx++) {
        const auto &step = opt_steps[step_idx];
        using Kind = TraceStep::Kind;

        switch (step.kind) {
            case Kind::LoadIntVar: {
                auto it = var_ireg.find(step.slot);
                if (it == var_ireg.end()) {
                    compilation_ok = false;
                    goto done;
                }
                // lookahead: LoadIntVar(s), LoadOneConst, IntAdd, StoreIntVar(s), Pop -> increment
                {
                    size_t r = opt_steps.size() - step_idx;
                    if (r >= 5) {
                        const auto &s1 = opt_steps[step_idx + 1];
                        const auto &s2 = opt_steps[step_idx + 2];
                        const auto &s3 = opt_steps[step_idx + 3];
                        const auto &s4 = opt_steps[step_idx + 4];
                        if (s1.kind == Kind::LoadOneConst && s2.kind == Kind::IntAdd && s3.kind == Kind::StoreIntVar &&
                            s3.slot == step.slot && s4.kind == Kind::Pop) {
#if NARI_JIT_ARM64
                            cc.add(it->second, it->second, asmjit::Imm(1));
#else
                            cc.inc(it->second);
#endif
                            step_idx += 4;
                            break;
                        }
                        // LoadIntVar(s), LoadIntConst(N), IntAdd, StoreIntVar(s), Pop -> add imm
                        if (s1.kind == Kind::LoadIntConst && s2.kind == Kind::IntAdd && s3.kind == Kind::StoreIntVar &&
                            s3.slot == step.slot && s4.kind == Kind::Pop) {
                            int32_t imm = (int32_t)s1.int_val;
#if NARI_JIT_ARM64
                            if (imm >= 0 && imm <= 4095) {
                                cc.add(it->second, it->second, asmjit::Imm(imm));
                            } else {
                                arch::Gp tmp = cc.new_gp64();
                                cc.mov(tmp, asmjit::Imm(imm));
                                cc.add(it->second, it->second, tmp);
                            }
#else
                            cc.add(it->second, imm);
#endif
                            step_idx += 4;
                            break;
                        }
                        // LoadIntVar(s), LoadOneConst, IntSub, StoreIntVar(s), Pop -> decrement
                        if (s1.kind == Kind::LoadOneConst && s2.kind == Kind::IntSub && s3.kind == Kind::StoreIntVar &&
                            s3.slot == step.slot && s4.kind == Kind::Pop) {
#if NARI_JIT_ARM64
                            cc.sub(it->second, it->second, asmjit::Imm(1));
#else
                            cc.dec(it->second);
#endif
                            step_idx += 4;
                            break;
                        }
                    }
                }
                vpush_gp(it->second, TraceType::Int);
                break;
            }
            case Kind::LoadFloatVar: {
                auto it = var_dreg.find(step.slot);
                if (it == var_dreg.end()) {
                    compilation_ok = false;
                    goto done;
                }
                vpush_xmm(it->second, TraceType::Float);
                break;
            }
            case Kind::LoadIntConst: {
                arch::Gp r = cc.new_gp64();
                cc.mov(r, (int64_t)step.int_val);
                vpush_gp(r, TraceType::Int);
                break;
            }
            case Kind::LoadZeroConst: {
                arch::Gp r = cc.new_gp64();
#if NARI_JIT_ARM64
                cc.mov(r, asmjit::Imm(0));
#else
                cc.xor_(r, r);
#endif
                vpush_gp(r, TraceType::Int);
                break;
            }
            case Kind::LoadOneConst: {
                arch::Gp r = cc.new_gp64();
                cc.mov(r, 1);
                vpush_gp(r, TraceType::Int);
                break;
            }
            case Kind::LoadFloatConst: {
                arch::Vec r = arch::new_vec_f64(cc);
#if NARI_JIT_ARM64
                arch::Gp tmp = cc.new_gp64();
                uint64_t bits;
                memcpy(&bits, &step.dbl_val, 8);
                cc.mov(tmp, (int64_t)bits);
                cc.fmov(r, tmp);
#else
                x86::Mem c = cc.new_double_const(ConstPoolScope::kLocal, step.dbl_val);
                cc.movsd(r, c);
#endif
                AsmVEntry e(r, TraceType::Float);
                if (step.dbl_val == 1.0) {
                    e.is_const_one = true;
                }
                vstack.push_back(e);
                break;
            }
            case Kind::LoadMathGlobal:
                // no code emitted; consumed by math builtin steps
                vpush_gp(arch::Gp(), TraceType::MathObject);
                break;

            case Kind::StoreIntVar: {
                if (vstack.empty()) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry v = vstack.back(); // peek, don't pop (STORE_VAR)
                auto it = var_ireg.find(step.slot);
                if (it == var_ireg.end()) {
                    compilation_ok = false;
                    goto done;
                }
                cc.mov(it->second, v.gp);
                break;
            }
            case Kind::StoreFloatVar: {
                if (vstack.empty()) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry v = vstack.back();
                auto it = var_dreg.find(step.slot);
                if (it == var_dreg.end()) {
                    compilation_ok = false;
                    goto done;
                }
#if NARI_JIT_ARM64
                cc.fmov(it->second, v.xmm);
#else
                cc.movsd(it->second, v.xmm);
#endif
                break;
            }

            case Kind::IntAdd:
            case Kind::IntSub:
            case Kind::IntMul:
            case Kind::IntDiv:
            case Kind::IntMod: {
                AsmVEntry b = vpop(), a = vpop();
                arch::Gp r = cc.new_gp64();
                if (step.kind == Kind::IntAdd) {
#if NARI_JIT_ARM64
                    cc.add(r, a.gp, b.gp);
#else
                    cc.lea(r, x86::ptr(a.gp, b.gp));
#endif
                } else if (step.kind == Kind::IntSub) {
#if NARI_JIT_ARM64
                    cc.sub(r, a.gp, b.gp);
#else
                    cc.mov(r, a.gp);
                    cc.sub(r, b.gp);
#endif
                } else if (step.kind == Kind::IntMul) {
#if NARI_JIT_ARM64
                    cc.mul(r, a.gp, b.gp);
#else
                    cc.mov(r, a.gp);
                    cc.imul(r, b.gp);
#endif
                } else if (step.kind == Kind::IntDiv) {
                    arch::Gp rem = cc.new_gp64();
                    arch::signed_div(cc, r, rem, a.gp, b.gp);
                } else { // IntMod
                    // check if the divisor is a known constant from a LoadIntConst step
                    int64_t const_div = 0;
                    if (step_idx > 0 && opt_steps[step_idx - 1].kind == Kind::LoadIntConst) {
                        int64_t d = opt_steps[step_idx - 1].int_val;
                        if (d >= 2 && d <= 0x7FFFFFFF) {
                            const_div = d;
                        }
                    }
                    if (const_div > 0) {
                        // Magic multiply: ~5 cycles vs ~20 for idiv/sdiv
                        auto dm = sdiv_magic(const_div);
                        arch::Gp mul_lo = cc.new_gp64();
                        arch::Gp mul_hi = cc.new_gp64();
                        cc.mov(mul_lo, a.gp);
                        arch::Gp magic = cc.new_gp64();
                        cc.mov(magic, (int64_t)dm.magic);
                        arch::imul_wide_hi(cc, mul_hi, mul_lo, magic);
                        arch::Gp q = cc.new_gp64();
                        cc.mov(q, mul_hi);
                        // Hacker's Delight 10-3: when the magic constant wraps
                        // negative as int64 (most divisors), the quotient
                        // estimate needs +n before the shift, else q is one
                        // whole step low (n % d would return n + d).
                        if (dm.magic < 0) {
#if NARI_JIT_ARM64
                            cc.add(q, q, a.gp);
#else
                            cc.add(q, a.gp);
#endif
                        }
                        if (dm.shift > 0) {
#if NARI_JIT_ARM64
                            cc.asr(q, q, asmjit::Imm(dm.shift));
#else
                            cc.sar(q, dm.shift);
#endif
                        }
                        arch::Gp sign = cc.new_gp64();
                        cc.mov(sign, a.gp);
#if NARI_JIT_ARM64
                        cc.lsr(sign, sign, asmjit::Imm(63));
                        cc.add(q, q, sign);
#else
                        cc.shr(sign, 63);
                        cc.add(q, sign);
#endif
#if NARI_JIT_ARM64
                        {
                            arch::Gp div_c = cc.new_gp64();
                            cc.mov(div_c, (int64_t)const_div);
                            cc.mul(q, q, div_c);
                        }
#else
                        cc.imul(q, q, (int32_t)const_div);
#endif
                        r = cc.new_gp64();
#if NARI_JIT_ARM64
                        cc.sub(r, a.gp, q);
#else
                        cc.mov(r, a.gp);
                        cc.sub(r, q);
#endif
                    } else {
                        arch::Gp quo = cc.new_gp64();
                        arch::signed_div(cc, quo, r, a.gp, b.gp);
                    }
                }
                vpush_gp(r, TraceType::Int);
                break;
            }

            case Kind::FloatAdd:
            case Kind::FloatSub:
            case Kind::FloatMul:
            case Kind::FloatDiv: {
                AsmVEntry b = vpop(), a = vpop();
                // peephole: x * 1.0 or 1.0 * x, just return x
                if (step.kind == Kind::FloatMul) {
                    if (b.is_const_one) {
                        vpush_xmm(a.xmm, TraceType::Float);
                        break;
                    }
                    if (a.is_const_one) {
                        vpush_xmm(b.xmm, TraceType::Float);
                        break;
                    }
                }
                arch::Vec r = arch::new_vec_f64(cc);
#if NARI_JIT_ARM64
                if (step.kind == Kind::FloatAdd) {
                    cc.fadd(r, a.xmm, b.xmm);
                } else if (step.kind == Kind::FloatSub) {
                    cc.fsub(r, a.xmm, b.xmm);
                } else if (step.kind == Kind::FloatMul) {
                    cc.fmul(r, a.xmm, b.xmm);
                } else {
                    cc.fdiv(r, a.xmm, b.xmm);
                }
#else
                cc.movsd(r, a.xmm);
                if (step.kind == Kind::FloatAdd) {
                    cc.addsd(r, b.xmm);
                } else if (step.kind == Kind::FloatSub) {
                    cc.subsd(r, b.xmm);
                } else if (step.kind == Kind::FloatMul) {
                    cc.mulsd(r, b.xmm);
                } else {
                    cc.divsd(r, b.xmm);
                }
#endif
                vpush_xmm(r, TraceType::Float);
                break;
            }

            case Kind::IntToFloat: {
                if (step.int_val == 0) {
                    // convert TOS from int to float
                    AsmVEntry v = vpop();
                    arch::Vec reg = arch::new_vec_f64(cc);
#if NARI_JIT_ARM64
                    cc.scvtf(reg, v.gp);
#else
                    cc.cvtsi2sd(reg, v.gp);
#endif
                    vpush_xmm(reg, TraceType::Float);
                } else {
                    // convert TOS - 1 from int to float
                    if (vstack.size() < 2) {
                        compilation_ok = false;
                        goto done;
                    }
                    AsmVEntry tos = vpop();
                    AsmVEntry tos1 = vpop();
                    arch::Vec r = arch::new_vec_f64(cc);
#if NARI_JIT_ARM64
                    cc.scvtf(r, tos1.gp);
#else
                    cc.cvtsi2sd(r, tos1.gp);
#endif
                    vpush_xmm(r, TraceType::Float); // converted TOS - 1
                    vstack.push_back(tos);          // re-push TOS
                }
                break;
            }

            case Kind::MathSqrt: {
                AsmVEntry arg = vpop();
                vpop(); // consume math object sentinel
                arch::Vec d_in = arch::new_vec_f64(cc);
                arch::Vec d_out = arch::new_vec_f64(cc);
                if (arg.type == TraceType::Int) {
#if NARI_JIT_ARM64
                    cc.scvtf(d_in, arg.gp);
#else
                    cc.cvtsi2sd(d_in, arg.gp);
#endif
                } else {
#if NARI_JIT_ARM64
                    cc.fmov(d_in, arg.xmm);
#else
                    cc.movsd(d_in, arg.xmm);
#endif
                }
#if NARI_JIT_ARM64
                cc.fsqrt(d_out, d_in);
#else
                cc.sqrtsd(d_out, d_in);
#endif
                vpush_xmm(d_out, TraceType::Float);
                break;
            }

            case Kind::MathSin:
            case Kind::MathCos:
            case Kind::MathTan:
            case Kind::MathFloor:
            case Kind::MathCeil: {
                AsmVEntry arg = vpop();
                vpop(); // math sentinel
                arch::Vec d_in = arch::new_vec_f64(cc);
                arch::Vec d_out = arch::new_vec_f64(cc);
                if (arg.type == TraceType::Int) {
#if NARI_JIT_ARM64
                    cc.scvtf(d_in, arg.gp);
#else
                    cc.cvtsi2sd(d_in, arg.gp);
#endif
                } else {
#if NARI_JIT_ARM64
                    cc.fmov(d_in, arg.xmm);
#else
                    cc.movsd(d_in, arg.xmm);
#endif
                }

#if NARI_JIT_X86
                // use SSE4.1 roundsd if possible, otherwise C helper
                if (has_sse4_1 && step.kind == Kind::MathFloor) {
                    cc.roundsd(d_out, d_in, asmjit::Imm(1));
                    vpush_xmm(d_out, TraceType::Float);
                    break;
                } else if (has_sse4_1 && step.kind == Kind::MathCeil) {
                    cc.roundsd(d_out, d_in, asmjit::Imm(2));
                    vpush_xmm(d_out, TraceType::Float);
                    break;
                }
#endif
#if NARI_JIT_ARM64
                if (step.kind == Kind::MathFloor) {
                    cc.frintm(d_out, d_in); // round towards -inf
                    vpush_xmm(d_out, TraceType::Float);
                    break;
                } else if (step.kind == Kind::MathCeil) {
                    cc.frintp(d_out, d_in); // round towards +inf
                    vpush_xmm(d_out, TraceType::Float);
                    break;
                }
#endif
                {
                    void *fn_ptr = nullptr;
                    if (step.kind == Kind::MathSin) {
                        fn_ptr = (void *)(double (*)(double))nari_fast_sin;
                    } else if (step.kind == Kind::MathCos) {
                        fn_ptr = (void *)(double (*)(double))nari_fast_cos;
                    } else if (step.kind == Kind::MathTan) {
                        fn_ptr = (void *)(double (*)(double))std::tan;
                    } else if (step.kind == Kind::MathFloor) {
                        fn_ptr = (void *)(double (*)(double))std::floor;
                    } else {
                        fn_ptr = (void *)(double (*)(double))std::ceil;
                    }

                    InvokeNode *invoke;
#if NARI_JIT_ARM64
                    // ARM64 blr requires a register operand
                    arch::Gp fn_reg = cc.new_gp64("fn_ptr");
                    cc.mov(fn_reg, (uint64_t)fn_ptr);
                    cc.invoke(Out(invoke), fn_reg, FuncSignature::build<double, double>());
#else
                    cc.invoke(Out(invoke), imm(fn_ptr), FuncSignature::build<double, double>());
#endif
                    invoke->set_arg(0, d_in);
                    invoke->set_ret(0, d_out);
                }

                vpush_xmm(d_out, TraceType::Float);
                break;
            }

            // lazy evaluation: no code is emitted here. CondExitIfFalse will fuse
            // the comparison into a single cmp+jcc, eliminating xor+setX+test overhead
            case Kind::IntLt:
            case Kind::IntLe:
            case Kind::IntGt:
            case Kind::IntGe:
            case Kind::IntEq:
            case Kind::IntNe: {
                AsmVEntry b = vpop(), a = vpop();
                arch::CC::Cond exit_cc, set_cc;
                if (step.kind == Kind::IntLt) {
                    exit_cc = arch::CC::kGE;
                    set_cc = arch::CC::kLT;
                } else if (step.kind == Kind::IntLe) {
                    exit_cc = arch::CC::kGT;
                    set_cc = arch::CC::kLE;
                } else if (step.kind == Kind::IntGt) {
                    exit_cc = arch::CC::kLE;
                    set_cc = arch::CC::kGT;
                } else if (step.kind == Kind::IntGe) {
                    exit_cc = arch::CC::kLT;
                    set_cc = arch::CC::kGE;
                } else if (step.kind == Kind::IntEq) {
                    exit_cc = arch::CC::kNE;
                    set_cc = arch::CC::kEQ;
                } else { // IntNe
                    exit_cc = arch::CC::kEQ;
                    set_cc = arch::CC::kNE;
                }
                // Check if RHS was a LoadIntConst that fits in int32 -> use imm form
                if (step_idx > 0 && opt_steps[step_idx - 1].kind == Kind::LoadIntConst) {
                    int64_t v = opt_steps[step_idx - 1].int_val;
                    if (v == (int64_t)(int32_t)v) {
                        vstack.push_back(AsmVEntry::make_lazy_int_imm(a.gp, (int32_t)v, exit_cc, set_cc));
                        break;
                    }
                }
                vstack.push_back(AsmVEntry::make_lazy_int(a.gp, b.gp, exit_cc, set_cc));
                break;
            }

            // Lazy evaluation: float compare deferred until CondExitIfFalse.
            case Kind::FloatLt:
            case Kind::FloatLe:
            case Kind::FloatGt:
            case Kind::FloatGe: {
                AsmVEntry b = vpop(), a = vpop();
                arch::CC::Cond exit_cc, set_cc;
                if (step.kind == Kind::FloatLt) {
                    exit_cc = arch::CC::kFGE;
                    set_cc = arch::CC::kFLT;
                } else if (step.kind == Kind::FloatLe) {
                    exit_cc = arch::CC::kFGT;
                    set_cc = arch::CC::kFLE;
                } else if (step.kind == Kind::FloatGt) {
                    exit_cc = arch::CC::kFLE;
                    set_cc = arch::CC::kFGT;
                } else { // FloatGe
                    exit_cc = arch::CC::kFLT;
                    set_cc = arch::CC::kFGE;
                }
                vstack.push_back(AsmVEntry::make_lazy_float(a.xmm, b.xmm, exit_cc, set_cc));
                break;
            }

            // Logical NOT of a truth value. On a lazy comparison we simply invert
            // its condition codes (the negated comparison) -- no code emitted, and
            // a following CondExit/SideExit still fuses into one cmp+jcc. On an
            // already-materialized value, NOT = (x == 0).
            case Kind::Not: {
                if (vstack.empty()) {
                    break;
                }
                AsmVEntry &e = vstack.back();
                if (e.is_lazy_cmp) {
                    e.exit_cond = arch::CC::invert(e.exit_cond);
                    e.set_cond = arch::CC::invert(e.set_cond);
                } else {
                    arch::Gp r = cc.new_gp64();
                    arch::test_zero(cc, e.gp);
                    arch::cset(cc, r, arch::CC::kEQ);
                    e = AsmVEntry(r, TraceType::Bool);
                }
                break;
            }

            // Float equality/inequality cannot be folded into a single x86 condition code (NaN sets the parity flag),
            // so unlike the relational floats above we eagerly materialize a NaN-correct 0/1 bool here and push it as a
            // normal value.
            case Kind::FloatEq:
            case Kind::FloatNe: {
                AsmVEntry b = vpop(), a = vpop();
                arch::float_cmp(cc, a.xmm, b.xmm);
                arch::Gp r = cc.new_gp64();
                arch::float_to_bool_eq(cc, r, step.kind == Kind::FloatEq);
                vpush_gp(r, TraceType::Bool);
                break;
            }

            case Kind::CondExitIfFalse: {
                AsmVEntry cond = vpop();
                if (cond.is_lazy_cmp) {
                    // fused compare-and-branch
                    if (!cond.is_float_cmp) {
                        if (cond.has_imm_rhs) {
                            arch::int_cmp_imm(cc, cond.lhs_gp, cond.imm_rhs_val);
                        } else {
                            arch::int_cmp(cc, cond.lhs_gp, cond.rhs_gp);
                        }
                    } else {
                        arch::float_cmp(cc, cond.lhs_xmm, cond.rhs_xmm);
                    }
                    arch::jcc(cc, cond.exit_cond, lbl_done);
                } else {
                    arch::test_zero(cc, cond.gp);
                    arch::jcc(cc, arch::CC::kEQ, lbl_done); // if cond == 0 -> exit loop
                }
                break;
            }

            case Kind::SideExitIfFalse:
            case Kind::SideExitIfTrue: {
                AsmVEntry cond = vpop();

                // create a side-exit label + record its IP for epilogue emission.
                Label lbl_side = cc.new_label();
                side_exits.push_back({ lbl_side, code_base + step.fallback_pc });

                if (cond.is_lazy_cmp) {
                    if (!cond.is_float_cmp) {
                        if (cond.has_imm_rhs) {
                            arch::int_cmp_imm(cc, cond.lhs_gp, cond.imm_rhs_val);
                        } else {
                            arch::int_cmp(cc, cond.lhs_gp, cond.rhs_gp);
                        }
                    } else {
                        arch::float_cmp(cc, cond.lhs_xmm, cond.rhs_xmm);
                    }
                    if (step.kind == Kind::SideExitIfFalse) {
                        arch::jcc(cc, cond.exit_cond, lbl_side);
                    } else {
                        arch::jcc(cc, cond.set_cond, lbl_side);
                    }
                } else {
                    arch::test_zero(cc, cond.gp);
                    if (step.kind == Kind::SideExitIfFalse) {
                        arch::jcc(cc, arch::CC::kEQ, lbl_side); // exit if cond == 0 (false)
                    } else {
                        arch::jcc(cc, arch::CC::kNE, lbl_side); // exit if cond != 0 (true)
                    }
                }
                break;
            }

            case Kind::LoopBack:
                arch::add_imm(cc, iter_ctr, 1); // count this completed iteration
                poll_shutdown();
                arch::jmp(cc, lbl_loop);
                break;

            case Kind::Pop:
                if (!vstack.empty()) {
                    vstack.pop_back();
                }
                break;

            case Kind::Dup:
                if (!vstack.empty()) {
                    if (vstack.back().is_lazy_cmp) {
                        materialize_lazy(vstack.back());
                    }
                    vstack.push_back(vstack.back());
                }
                break;

            case Kind::LoadObjVar: {
                auto it = var_ireg.find(step.slot);
                auto fit = obj_fields_start_reg.find(step.slot);
                if (it == var_ireg.end() || fit == obj_fields_start_reg.end()) {
                    compilation_ok = false;
                    goto done;
                }
                vpush_gp_aux(it->second, fit->second, TraceType::Obj);
                break;
            }

            // Push the array's hoisted (data_start, size_bytes) pair as an AsmVEntry.
            // Both stay constant across the trace body: the entry guard checked identity + size on entry,
            // and the recorder aborts on any op that could grow the array.
            case Kind::LoadArrayVar: {
                auto dit = arr_data_start_reg.find(step.slot);
                auto sit = arr_size_bytes_reg.find(step.slot);
                if (dit == arr_data_start_reg.end() || sit == arr_size_bytes_reg.end()) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry e(dit->second, sit->second, TraceType::Array);
                e.array_slot = step.slot;
                e.has_array_slot = true;
                vstack.push_back(e);
                break;
            }

            // Array element read: pop [array, index], side-exit if index is
            // out of bounds, else load element and decode int-or-float
            case Kind::ArrayGetIdx: {
                if (vstack.size() < 2) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry idx_entry = vpop();
                AsmVEntry arr_entry = vpop();
                if (idx_entry.is_lazy_cmp) {
                    materialize_lazy(idx_entry);
                }
                if (arr_entry.type != TraceType::Array || !arr_entry.has_aux_gp) {
                    compilation_ok = false;
                    goto done;
                }

                // byte_off = idx * 8 (ValSize)
                arch::Gp byte_off = cc.new_gp64();
                cc.mov(byte_off, idx_entry.gp);
#if NARI_JIT_ARM64
                cc.lsl(byte_off, byte_off, asmjit::Imm(3));
#else
                cc.shl(byte_off, Imm(3));
#endif

                // Unsigned OOB compare: side-exit if byte_off >= size_bytes.
                // Also catches negative idx (byte_off wraps to a huge unsigned value >= size_bytes).
                Label lbl_side = cc.new_label();
                arch::Gp size_r = arr_entry.aux_gp;
                cc.cmp(byte_off, size_r);
                arch::jcc(cc, arch::CC::kUGE, lbl_side);

                // Prepare OOB side-exit metadata:
                //  interpreter re-executes OP_GET_INDEX with [array, index] on vm->stack.
                if (!arr_entry.has_array_slot) {
                    compilation_ok = false;
                    goto done;
                }
                {
                    // Load the raw NaN-boxed array Value from its stack slot;
                    // arrays are never written by trace code, so this is the original heap Value.
                    arch::Gp arr_raw = cc.new_gp64();
                    arch::load(cc, arr_raw, arch::ptr(addr_reg.at(arr_entry.array_slot), 0));
                    SideExit se;
                    se.label = lbl_side;
                    se.ip_val = code_base + step.fallback_pc;
                    se.push_args.push_back({ arr_raw, TraceType::Array, /*has_raw_ptr_tag=*/true });
                    se.push_args.push_back({ idx_entry.gp, TraceType::Int, /*has_raw_ptr_tag=*/false });
                    side_exits.push_back(std::move(se));
                }

                // slot_addr = data_start + byte_off
                arch::Gp slot_addr = cc.new_gp64();
                cc.mov(slot_addr, arr_entry.gp);
                arch::add2(cc, slot_addr, byte_off);

                arch::Gp r = load_slot_as_int(slot_addr);
                vpush_gp(r, TraceType::Int);
                break;
            }

            // Array element write: pop [array, index, val], side-exit if OOB.
            case Kind::ArraySetIdx: {
                if (vstack.size() < 3) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry val_entry = vpop();
                AsmVEntry idx_entry = vpop();
                AsmVEntry arr_entry = vpop();
                if (val_entry.is_lazy_cmp) {
                    materialize_lazy(val_entry);
                }
                if (idx_entry.is_lazy_cmp) {
                    materialize_lazy(idx_entry);
                }
                if (arr_entry.type != TraceType::Array || !arr_entry.has_aux_gp) {
                    compilation_ok = false;
                    goto done;
                }

                arch::Gp byte_off = cc.new_gp64();
                cc.mov(byte_off, idx_entry.gp);
#if NARI_JIT_ARM64
                cc.lsl(byte_off, byte_off, asmjit::Imm(3));
#else
                cc.shl(byte_off, Imm(3));
#endif

                Label lbl_side = cc.new_label();
                arch::Gp size_r = arr_entry.aux_gp;
                cc.cmp(byte_off, size_r);
                arch::jcc(cc, arch::CC::kUGE, lbl_side);

                if (!arr_entry.has_array_slot) {
                    compilation_ok = false;
                    goto done;
                }
                {
                    arch::Gp arr_raw = cc.new_gp64();
                    arch::load(cc, arr_raw, arch::ptr(addr_reg.at(arr_entry.array_slot), 0));
                    SideExit se;
                    se.label = lbl_side;
                    se.ip_val = code_base + step.fallback_pc;
                    se.push_args.push_back({ arr_raw, TraceType::Array, /*has_raw_ptr_tag=*/true });
                    se.push_args.push_back({ idx_entry.gp, TraceType::Int, /*has_raw_ptr_tag=*/false });
                    se.push_args.push_back({ val_entry.gp, TraceType::Int, /*has_raw_ptr_tag=*/false });
                    side_exits.push_back(std::move(se));
                }

                arch::Gp slot_addr = cc.new_gp64();
                cc.mov(slot_addr, arr_entry.gp);
                arch::add2(cc, slot_addr, byte_off);

                store_int_or_float_on_overflow(slot_addr, val_entry.gp);

                // Assignment result.
                //  In statement position, SET_INDEX is followed by Pop, so avoid materializing the discarded result on
                //  the vstack
                if (step_idx + 1 < opt_steps.size() && opt_steps[step_idx + 1].kind == Kind::Pop) {
                    ++step_idx;
                    break;
                }
                vpush_gp(val_entry.gp, TraceType::Int);
                break;
            }

            case Kind::ObjGetProp: {
                if (vstack.empty()) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry obj_entry = vpop(); // obj; gp holds the guarded ObjectObj*

                arch::Gp slot_addr = get_obj_slot_addr(obj_entry, step.slot, step.prop_slot_index);
                if (step.prop_val_type == TraceType::Int) {
                    arch::Gp r = cc.new_gp64();
                    arch::load(cc, r, arch::ptr(slot_addr, 0));
                    // the slot may contain a float if a previous iteration overflowed int48.
                    // Check the NaN-box tag: if tagInt -> sign_extend_48; else -> convert from double.
                    Label lbl_is_int = cc.new_label();
                    Label lbl_get_done = cc.new_label();
                    {
                        arch::Gp tag16 = cc.new_gp64();
                        cc.mov(tag16, r);
                        arch::shr(cc, tag16, 48);
                        arch::cmp_imm(cc, tag16.r32(), Imm((int)tagInt));
                        arch::jcc(cc, arch::CC::kEQ, lbl_is_int);
                    }
                    // Float path: value was stored as raw double by overflow handler
                    {
                        arch::Vec fv = arch::new_vec_f64(cc);
                        arch::load_f64(cc, fv, arch::ptr(slot_addr, 0));
#if NARI_JIT_ARM64
                        cc.fcvtzs(r, fv);
#else
                        cc.cvttsd2si(r, fv);
#endif
                    }
                    arch::jmp(cc, lbl_get_done);
                    cc.bind(lbl_is_int);
                    arch::sign_extend_48(cc, r);
                    cc.bind(lbl_get_done);
                    vpush_gp(r, TraceType::Int);
                } else if (step.prop_val_type == TraceType::Float) {
                    arch::Vec r = arch::new_vec_f64(cc);
                    arch::load_f64(cc, r, slot_addr, 0);
                    vpush_xmm(r, TraceType::Float);
                } else {
                    compilation_ok = false;
                    goto done;
                }
                break;
            }

            case Kind::ObjSetProp: {
                if (vstack.size() < 2) {
                    compilation_ok = false;
                    goto done;
                }

                AsmVEntry val_entry = vpop(); // val (TOS)
                AsmVEntry obj_entry = vpop(); // obj (TOS - 1); gp holds guarded ObjectObj*

                arch::Gp slot_addr = get_obj_slot_addr(obj_entry, step.slot, step.prop_slot_index);
                if (step.prop_val_type == TraceType::Int) {
                    // Check int48 overflow before NaN-boxing
                    Label lbl_no_ovf = cc.new_label();
                    Label lbl_ovf = cc.new_label();
                    {
                        arch::Gp ovf_chk = cc.new_gp64();
                        cc.mov(ovf_chk, val_entry.gp);
                        arch::sign_extend_48(cc, ovf_chk);
                        cc.cmp(ovf_chk, val_entry.gp);
                        arch::jcc(cc, arch::CC::kNE, lbl_ovf);
                    }
                    // no overflow: write as NaN-boxed int
                    {
                        arch::Gp boxed = cc.new_gp64();
                        cc.mov(boxed, val_entry.gp);
                        arch::Gp tag = cc.new_gp64();
                        arch::nanbox_encode_int(cc, boxed, tag);
                        arch::store(cc, arch::ptr(slot_addr, 0), boxed);
                    }
                    arch::jmp(cc, lbl_no_ovf);
                    cc.bind(lbl_ovf);
                    // overflow: convert int64 to double and write as raw float bits
                    {
                        arch::Vec fv = arch::new_vec_f64(cc);
#if NARI_JIT_ARM64
                        cc.scvtf(fv, val_entry.gp);
#else
                        cc.cvtsi2sd(fv, val_entry.gp);
#endif
                        arch::store_f64(cc, slot_addr, 0, fv);
                    }
                    cc.bind(lbl_no_ovf);
                } else if (step.prop_val_type == TraceType::Float) {
                    arch::store_f64(cc, slot_addr, 0, val_entry.xmm);
                } else {
                    compilation_ok = false;
                    goto done;
                }
                // In statement position, SET_PROPERTY is immediately followed by Pop.
                // Avoid materializing the assignment-expression result on the trace
                // vstack when it will be discarded right away.
                if (step_idx + 1 < opt_steps.size() && opt_steps[step_idx + 1].kind == Kind::Pop) {
                    ++step_idx;
                    break;
                }

                // push val back (result == stored val)
                if (step.prop_val_type == TraceType::Int) {
                    vpush_gp(val_entry.gp, TraceType::Int);
                } else {
                    vpush_xmm(val_entry.xmm, TraceType::Float);
                }
                break;
            }

            case Kind::ObjAddConstInPlace: {
                auto it = var_ireg.find(step.slot);
                auto fit = obj_fields_start_reg.find(step.slot);
                if (it == var_ireg.end() || fit == obj_fields_start_reg.end()) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry obj_entry(it->second, fit->second, TraceType::Obj);
                arch::Gp slot_addr = get_obj_slot_addr(obj_entry, step.slot, step.prop_slot_index);
                arch::Gp val = load_slot_as_int(slot_addr);
                if (step.int_val == (int64_t)(int32_t)step.int_val) {
                    arch::add_imm(cc, val, (int32_t)step.int_val);
                } else {
                    arch::Gp imm = cc.new_gp64();
                    cc.mov(imm, step.int_val);
                    arch::add2(cc, val, imm);
                }
                store_int_or_float_on_overflow(slot_addr, val);
                break;
            }

            case Kind::ObjAddPropInPlace: {
                auto it = var_ireg.find(step.slot);
                auto fit = obj_fields_start_reg.find(step.slot);
                if (it == var_ireg.end() || fit == obj_fields_start_reg.end()) {
                    compilation_ok = false;
                    goto done;
                }
                AsmVEntry obj_entry(it->second, fit->second, TraceType::Obj);
                arch::Gp lhs_addr = get_obj_slot_addr(obj_entry, step.slot, step.prop_slot_index);
                arch::Gp rhs_addr = get_obj_slot_addr(obj_entry, step.slot, step.rhs_prop_slot_index);
                arch::Gp lhs = load_slot_as_int(lhs_addr);
                arch::Gp rhs = load_slot_as_int(rhs_addr);
                arch::add2(cc, lhs, rhs);
                store_int_or_float_on_overflow(lhs_addr, lhs);
                break;
            }

            case Kind::ClosureInc:
            case Kind::ClosureAddConst: {
                auto ar_it = addr_reg.find(step.closure_slot);
                if (ar_it == addr_reg.end()) {
                    compilation_ok = false;
                    goto done;
                }

                arch::Gp fn_raw = cc.new_gp64("cic_fn_raw");
                arch::load(cc, fn_raw, arch::ptr(ar_it->second, 0));
                arch::Gp fn_tag = cc.new_gp64("cic_fn_tag");
                cc.mov(fn_tag, fn_raw);
#if NARI_JIT_ARM64
                cc.lsr(fn_tag, fn_tag, asmjit::Imm(48));
                arch::cmp_imm_jcc(cc, fn_tag, (int32_t)kTagHeapTrace, arch::CC::kNE, lbl_done);
#else
                cc.shr(fn_tag, 48);
                arch::cmp_imm_jcc(cc, fn_tag, (int32_t)kTagHeapTrace, arch::CC::kNE, lbl_done);
#endif

                arch::Gp fn_ptr = cc.new_gp64("cic_fn_ptr");
                cc.mov(fn_ptr, fn_raw);
                arch::Gp mask48 = cc.new_gp64("cic_mask");
                cc.mov(mask48, (int64_t)0x0000FFFFFFFFFFFFLL);
#if NARI_JIT_ARM64
                cc.and_(fn_ptr, fn_ptr, mask48);
#else
                cc.and_(fn_ptr, mask48);
#endif

                arch::Gp heap_type = cc.new_gp64("cic_ht");
                arch::load8_zx(cc, heap_type, arch::ptr(fn_ptr, (int)HeapTypeTagOff));
                arch::cmp_imm_jcc(cc, heap_type, (int32_t)ValueTag::Function, arch::CC::kNE, lbl_done);

                arch::Gp inline_kind = cc.new_gp64("cic_ik");
                arch::load32_zx_mem(cc, inline_kind, arch::ptr(fn_ptr, (int)FDInlineKindOff));
                arch::cmp_imm_jcc(
                    cc, inline_kind,
                    (int32_t)(step.kind == Kind::ClosureAddConst ? JitInlineKind::ClosureAddConst : JitInlineKind::ClosureInc),
                    arch::CC::kNE, lbl_done);

                if (step.kind == Kind::ClosureAddConst) {
                    arch::Gp imm = cc.new_gp64("cic_imm");
                    arch::load(cc, imm, arch::ptr(fn_ptr, (int)FDInlineImmOff));
                    arch::cmp_imm_jcc(cc, imm, (int32_t)step.int_val, arch::CC::kNE, lbl_done);
                }

                arch::Gp cap_addr = cc.new_gp64("cic_a");
                arch::load(cc, cap_addr, arch::ptr(fn_ptr, (int)FDCapture0RawOff));
                arch::cmp_imm_jcc(cc, cap_addr, 0, arch::CC::kEQ, lbl_done);

                // guard: tag must be Int
                arch::Gp tag_chk = cc.new_gp64("cic_t");
                arch::load_tag16(cc, tag_chk, cap_addr, 0, (int32_t)tagWordOff);
                arch::cmp_imm_jcc(cc, tag_chk, (int32_t)tagInt, arch::CC::kNE, lbl_done);

                // load NaN-boxed value, decode int48
                arch::Gp val = cc.new_gp64("cic_v");
                arch::load(cc, val, arch::ptr(cap_addr, 0));
                arch::sign_extend_48(cc, val);

                // increment or add constant
                if (step.kind == Kind::ClosureInc) {
#if NARI_JIT_ARM64
                    cc.add(val, val, asmjit::Imm(1));
#else
                    cc.inc(val);
#endif
                } else {
                    int32_t cimm = (int32_t)step.int_val;
#if NARI_JIT_ARM64
                    if (cimm >= 0 && cimm <= 4095) {
                        cc.add(val, val, asmjit::Imm(cimm));
                    } else {
                        arch::Gp tmp = cc.new_gp64();
                        cc.mov(tmp, asmjit::Imm(cimm));
                        cc.add(val, val, tmp);
                    }
#else
                    cc.add(val, Imm(cimm));
#endif
                }

                // push result before reboxing (val is still decoded int64)
                arch::Gp result_gp = cc.new_gp64("cic_r");
                cc.mov(result_gp, val);

                // rebox: mask lower 48 bits, OR with Int tag, store back
                arch::Gp tag = cc.new_gp64("cic_nb");
                arch::nanbox_encode_int(cc, val, tag);
                arch::store(cc, arch::ptr(cap_addr, 0), val);

                vpush_gp(result_gp, TraceType::Int);
                break;
            }

            default:
                compilation_ok = false;
                goto done;
        }
    }

done:
    if (!compilation_ok) {
        return result;
    }

    // flush live vars, set IP to exit_pc, return
    cc.bind(lbl_done);
    emit_flush_and_ret(exit_ip);

    // emit side-exit blocks
    for (auto &se : side_exits) {
        cc.bind(se.label);
        // If this side-exit needs to hand operands back to the interpreter,
        // NaN-box each PushArg and append to vm->stack before flushing.
        if (!se.push_args.empty()) {
            arch::Gp finish_p = cc.new_gp64();
            arch::load(cc, finish_p, arch::ptr(vm_ptr, (int)VMStackFinishOff));
            arch::Gp nb_tag = cc.new_gp64();
            bool tag_loaded = false;
            for (const auto &pa : se.push_args) {
                arch::Gp boxed = cc.new_gp64();
                if (pa.has_raw_ptr_tag) {
                    // already a NaN-boxed Value; store as-is
                    cc.mov(boxed, pa.raw_gp);
                } else {
                    if (!tag_loaded) {
                        cc.mov(nb_tag, nbIntTagTrace);
                        tag_loaded = true;
                    }
                    cc.mov(boxed, pa.raw_gp);
                    arch::nanbox_encode_int(cc, boxed, nb_tag);
                }
                arch::store(cc, arch::ptr(finish_p, 0), boxed);
                arch::add_imm(cc, finish_p, (int32_t)ValSize);
            }
            arch::store(cc, arch::ptr(vm_ptr, (int)VMStackFinishOff), finish_p);
        }
        emit_flush_and_ret(se.ip_val);
    }

    cc.bind(lbl_guardfail);
    {
        arch::Gp ip_val = cc.new_gp64();
        cc.mov(ip_val, (int64_t)entry_ip);
        arch::store(cc, arch::ptr(frames_end, (int32_t)(-FrameSize + kIpOff)), ip_val);
    }
    cc.ret();

    cc.bind(shutdown_requested);
    {
        InvokeNode *invoke;
        arch::invoke_imm(cc, &invoke, (uint64_t)(uintptr_t)jit_poll_shutdown, FuncSignature::build<void, void *>());
        invoke->set_arg(0, vm_ptr);
    }
    cc.ret();

finalize_trace:
    cc.end_func();
    Error finalize_err = cc.finalize();
    if (finalize_err != kErrorOk) {
        fprintf(stderr, "[TRACE JIT] finalize failed: err=%u\n", finalize_err);
        return result;
    }

    // add generated code
    size_t generated_code_size = code.code_size();
    CompiledTrace::Fn fn = nullptr;
    Error err = this->rt.add(&fn, &code);
    if (err != kErrorOk || !fn) {
        fprintf(stderr, "[TRACE JIT] compilation failed: err=%u\n", err);
        return result;
    }

    {
        std::string sym = "trace_func_" + std::to_string(func_idx) + "_pc_" + std::to_string(rec.anchor_pc);
        register_gdb_jit_function(sym, reinterpret_cast<const void *>(fn), generated_code_size);
        perf_jitdump_register(sym, reinterpret_cast<const void *>(fn), generated_code_size);
    }

    result.fn = fn;
    result.valid = true;

    uint64_t key = make_key(func_idx, rec.anchor_pc);
    this->cache[key].trace = result;

    return result;
}

} // namespace jit
} // namespace nari

// #endif // !DISABLE_JIT
