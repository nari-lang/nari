#ifndef DISABLE_JIT
#include "asmjit_dump.h"
#include "asmjit_jit.h"
#include "asmjit_jit_async.h"
#include "core_types.h"
#include "ir.h"
#include "ir_build.h"
#include "ir_opt.h"
#include "jit_arch.h"
#include "jit_helpers.h"
#include "jit_layout.h"
#include "stl_layout.h"
#include "util.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "ir_emit.h"

using namespace asmjit;
using namespace nari::bytecode;

namespace nari {
namespace jit {

// Register-tier fast path: every op keeps its value in a vreg across the whole
// function. Moved verbatim from AsmJITMethodCompiler::ir_compile (asmjit_jit.cpp).
RegTierResult AsmJITMethodCompiler::compile_reg_tier(IrEmitContext &ctx) {
    auto &chunk = ctx.chunk;
    auto &chunk_idx = ctx.chunk_idx;
    auto &spec = ctx.spec;
    auto &spec_fallback = ctx.spec_fallback;
    auto &irFuncs = ctx.irFuncs;
    auto &slot_types = ctx.slot_types;
    auto &int_arr_slots = ctx.int_arr_slots;
    auto &code_holder = ctx.code_holder;
    auto &asm_logger = ctx.asm_logger;
    auto &cc = ctx.cc;
    auto &vm_reg = ctx.vm_reg;
    auto &vm_arg_scratch = ctx.vm_arg_scratch;
    auto &shutdown_requested = ctx.shutdown_requested;
    if ((irFuncs.num_params == 0 || spec) && getenv("NARI_DISABLE_REG_JIT") == nullptr) {
        bool elig = true;
        static const bool dbg_elig = getenv("NARI_REG_TIER_DEBUG") != nullptr;
        // Register tier for array-bearing functions
        const char *reject = nullptr;
        auto ty_str = [](ir::Ty t) -> const char * {
            switch (t) {
                case ir::Ty::Bottom:
                    return "Bottom";
                case ir::Ty::Unknown:
                    return "Unknown";
                case ir::Ty::Int48:
                    return "Int48";
                case ir::Ty::Float:
                    return "Float";
                case ir::Ty::Number:
                    return "Number";
                case ir::Ty::Bool:
                    return "Bool";
                case ir::Ty::Heap:
                    return "Heap";
                case ir::Ty::None:
                    return "None";
            }
            return "?";
        };
        auto raw_rep = [](ir::Ty t) { return t == ir::Ty::Unknown || t == ir::Ty::Heap || t == ir::Ty::Number; };
        auto slot_ok = [&](ir::Ty t) {
            if (t == ir::Ty::Int48 || t == ir::Ty::Bool) {
                return true;
            }
            if (t == ir::Ty::Float) {
                return true;
            }
            if (raw_rep(t)) {
                return true;
            }
            return false;
        };
        bool needs_escape_infra = false; // entry stack-reserve + raw machinery
        bool has_raw_slots = false;      // slot_base hoist for write-through
        std::vector<ir::BlockId> value_block(irFuncs.insts.size(), ir::InvalidBlock);
        for (const ir::Block &b : irFuncs.blocks) {
            for (ir::ValueId v : b.phis) {
                value_block[v] = b.id;
            }
            for (ir::ValueId v : b.insts) {
                value_block[v] = b.id;
            }
        }
        for (ir::Ty t : slot_types) {
            if (!slot_ok(t)) {
                elig = false;
                if (!reject) {
                    static char buf[64];
                    snprintf(buf, sizeof(buf), "slot ty=%s", ty_str(t));
                    reject = buf;
                }
            } else if (raw_rep(t)) {
                has_raw_slots = true;
                needs_escape_infra = true;
            }
        }
        for (const ir::Block &b : irFuncs.blocks) {
            if (!b.phis.empty()) {
                for (ir::ValueId pv : b.phis) {
                    const ir::Inst &pin = irFuncs.inst(pv);
                    if (pin.type != ir::Ty::Int48 && pin.type != ir::Ty::Bool && pin.type != ir::Ty::None) {
                        elig = false;
                        if (!reject) {
                            reject = "phi type non-int/bool/none";
                        }
                        break;
                    }
                }
                if (!elig) {
                    break;
                }
            }
            // Operand-type helper: with Unknown/Heap values now admitted register-arith ops
            // must PROVE their operands are raw-untagged (Int48/Bool)
            auto operands_raw_int = [&](const ir::Inst &in) {
                for (ir::ValueId ov : in.operands) {
                    ir::Ty t = irFuncs.inst(ov).type;
                    if (t != ir::Ty::Int48 && t != ir::Ty::Bool) {
                        return false;
                    }
                }
                return true;
            };
            for (ir::ValueId v : b.insts) {
                const ir::Inst &in = irFuncs.inst(v);
                for (ir::ValueId operand : in.operands) {
                    // value_block[operand] == InvalidBlock means the defining instruction is in
                    // no block: ir::optimize() detached it and left this consumer's operand list
                    // pointing at the dead ValueId. Lowering consumes operands positionally off
                    // the compile-time stack (MakeArray, for one, pops imm_u32 elements and never
                    // reads in.operands), so a stale reference cannot affect codegen. Only a def
                    // in a different *live* block is genuine unsupported cross-block dataflow.
                    if (operand < 0 || static_cast<size_t>(operand) >= value_block.size() ||
                        (value_block[operand] != b.id && value_block[operand] != ir::InvalidBlock)) {
                        elig = false;
                        if (!reject) {
                            reject = "cross-block operand without phi";
                        }
                        break;
                    }
                }
                // Result-type rule: register-arith results must be raw-representable (Int48/Bool/None)
                bool any_result_ty = false;
                switch (in.op) {
                    case ir::Op::IConst:
                    case ir::Op::BConst:
                    case ir::Op::NConst:
                    case ir::Op::StoreSlot:
                    case ir::Op::Pop:
                        break;
                    case ir::Op::LoadSlot:
                    case ir::Op::Dup:
                        any_result_ty = true; // rep follows slot/source type
                        break;
                    case ir::Op::DynAdd:
                    case ir::Op::DynSub:
                        any_result_ty = true;
                        needs_escape_infra = true;
                        break;
                    case ir::Op::DynCmpLt:
                    case ir::Op::DynCmpLe:
                    case ir::Op::DynCmpGt:
                    case ir::Op::DynCmpGe:
                    case ir::Op::DynCmpEq:
                    case ir::Op::DynCmpNe:
                    case ir::Op::DynStrictCmpEq:
                    case ir::Op::DynStrictCmpNe:
                        // a Dyn op with a non-raw operand (e.g. the Unknown result of a Call)
                        // is lowered via ESCAPE to the general-path helper
                        if (!operands_raw_int(in)) {
                            any_result_ty = true;
                            needs_escape_infra = true;
                        }
                        break;
                    case ir::Op::IAdd:
                    case ir::Op::ISub:
                    case ir::Op::IMul:
                        if (!operands_raw_int(in) || (in.type != ir::Ty::Number && !(in.type == ir::Ty::Int48 && in.proven_int48))) {
                            elig = false;
                            if (!reject) {
                                reject = "checked integer operation has invalid type";
                            }
                        }
                        any_result_ty = true;
                        needs_escape_infra = true;
                        break;
                    case ir::Op::ICmpLt:
                    case ir::Op::ICmpLe:
                    case ir::Op::ICmpGt:
                    case ir::Op::ICmpGe:
                    case ir::Op::ICmpEq:
                    case ir::Op::ICmpNe:
                        // Typed forms are only produced with proven-int
                        // operands; non-raw here means an inference bug.
                        if (!operands_raw_int(in)) {
                            elig = false;
                            if (!reject) {
                                reject = "typed arith/cmp operand non-raw";
                            }
                        }
                        break;
                    // fused slot-store forms
                    case ir::Op::StoreImmSlot:
                    case ir::Op::StoreBImmSlot:
                    case ir::Op::StoreNSlot:
                        break;
                    case ir::Op::CopySlot:
                        // Representation conversion raw->untagged would need
                        // a type-directed unbox; type joins make it
                        // impossible (dst ty >= src ty), but keep the guard.
                        if (in.imm_int >= 0 && static_cast<size_t>(in.imm_int) < slot_types.size() &&
                            raw_rep(slot_types[static_cast<uint32_t>(in.imm_int)]) && !raw_rep(slot_types[in.imm_u32])) {
                            elig = false;
                            if (!reject) {
                                reject = "copyslot raw->untagged";
                            }
                        }
                        // a Float xmm dst reads slot_vec[src], so src
                        // must also be an xmm Float slot.
                        if (in.imm_u32 < slot_types.size() && slot_types[in.imm_u32] == ir::Ty::Float &&
                            !(in.imm_int >= 0 && static_cast<size_t>(in.imm_int) < slot_types.size() &&
                              slot_types[static_cast<uint32_t>(in.imm_int)] == ir::Ty::Float)) {
                            elig = false;
                            if (!reject) {
                                reject = "copyslot float dst non-float src";
                            }
                        }
                        break;
                    case ir::Op::Not:
                        // Lowered as xor 1: valid ONLY for a Bool operand.
                        if (in.operands.empty() || irFuncs.inst(in.operands[0]).type != ir::Ty::Bool) {
                            elig = false;
                            if (!reject) {
                                reject = "Not on non-bool";
                            }
                        }
                        break;
                    case ir::Op::JsTruthy:
                        if (in.operands.empty() ||
                            (irFuncs.inst(in.operands[0]).type != ir::Ty::Bool && irFuncs.inst(in.operands[0]).type != ir::Ty::Int48 &&
                             irFuncs.inst(in.operands[0]).type != ir::Ty::None)) {
                            needs_escape_infra = true;
                        }
                        break;
                    case ir::Op::IsNone:
                        break;
                    // lowered inline when proven (array index ops / imod / load.global)
                    case ir::Op::IMod:
                        if (!operands_raw_int(in)) {
                            elig = false;
                            if (!reject) {
                                reject = "imod operand non-raw";
                            }
                        }
                        needs_escape_infra = true;
                        break;
                    case ir::Op::LoadIndex:
                        // A read can return None for a negative or past-end index.
                        // Until a path-sensitive bounds proof exists, the register
                        // tier must keep its result boxed and dynamically typed.
                        if (in.type == ir::Ty::Int48 &&
                            (!in.proven_inbounds || in.operands.size() < 2 ||
                             irFuncs.inst(in.operands[1]).type != ir::Ty::Int48)) {
                            elig = false;
                            if (!reject) {
                                reject = "array read inferred Int48 without bounds proof";
                            }
                        }
                        any_result_ty = true;
                        needs_escape_infra = true;
                        break;
                    case ir::Op::StoreIndex:
                    case ir::Op::MakeArray:
                    case ir::Op::ArrayPush:
                    case ir::Op::ArraySpread:
                    case ir::Op::StrAppendSlot:
                    case ir::Op::Call:
                    case ir::Op::CallSpread:
                    case ir::Op::CallMethod:
                    case ir::Op::LoadGlobal:
                        any_result_ty = true;
                        needs_escape_infra = true;
                        break;
                    case ir::Op::FConst:
                        any_result_ty = true; // result rep is Float (raw bits)
                        break;
                    case ir::Op::FAdd:
                    case ir::Op::FSub:
                    case ir::Op::FMul:
                    case ir::Op::FDiv:
                        if (in.operands.size() != 2 || irFuncs.inst(in.operands[0]).type != ir::Ty::Float ||
                            irFuncs.inst(in.operands[1]).type != ir::Ty::Float) {
                            elig = false;
                            if (!reject) {
                                reject = "typed float arith operand non-float";
                            }
                        }
                        any_result_ty = true; // result rep is Float (raw bits)
                        break;
                    // typed float compare
                    case ir::Op::FCmpEq:
                    case ir::Op::FCmpNe:
                    case ir::Op::FCmpLt:
                    case ir::Op::FCmpLe:
                    case ir::Op::FCmpGt:
                    case ir::Op::FCmpGe:
                        if (in.operands.size() != 2 || irFuncs.inst(in.operands[0]).type != ir::Ty::Float ||
                            irFuncs.inst(in.operands[1]).type != ir::Ty::Float) {
                            elig = false;
                            if (!reject) {
                                reject = "typed float cmp operand non-float";
                            }
                        }
                        break;
                    case ir::Op::DynMul:
                    case ir::Op::DynDiv:
                        any_result_ty = true;
                        needs_escape_infra = true;
                        break;
                    case ir::Op::DynMod:
                        any_result_ty = true;
                        needs_escape_infra = true;
                        break;
                    default:
                        elig = false;
                        if (!reject) {
                            static char buf[64];
                            snprintf(buf, sizeof(buf), "op %d", static_cast<int>(in.op));
                            reject = buf;
                        }
                        break;
                }
                if (!any_result_ty && in.result != ir::InvalidValue && in.type != ir::Ty::Int48 && in.type != ir::Ty::Bool &&
                    in.type != ir::Ty::None) {
                    elig = false;
                    if (!reject) {
                        reject = "result type non-int/bool/none";
                    }
                }
            }
            if (b.terminator != ir::InvalidValue) {
                const ir::Inst &tin = irFuncs.inst(b.terminator);
                for (ir::ValueId operand : tin.operands) {
                    if (operand < 0 || static_cast<size_t>(operand) >= value_block.size() ||
                        (value_block[operand] != b.id && value_block[operand] != ir::InvalidBlock)) {
                        elig = false;
                        if (!reject) {
                            reject = "cross-block terminator operand without phi";
                        }
                        break;
                    }
                }
                ir::Op to = tin.op;
                if (to != ir::Op::Jump && to != ir::Op::Branch && to != ir::Op::Return) {
                    elig = false;
                    if (!reject) {
                        reject = "non-J/B/R terminator";
                    }
                }
                if (to == ir::Op::Branch && !tin.operands.empty()) {
                    ir::Ty ct = irFuncs.inst(tin.operands[0]).type;
                    if (ct != ir::Ty::Bool && ct != ir::Ty::Int48) {
                        needs_escape_infra = true;
                    }
                }
                if (to == ir::Op::Return) {
                    needs_escape_infra = true;
                }
            }
        }
        if (dbg_elig && !elig) {
            fprintf(
                stderr, "[REG-TIER] %-30s reject: %s\n",
                chunk.functions[chunk_idx].name.empty() ? "<anon>" : chunk.functions[chunk_idx].name.c_str(), reject ? reject : "?"
            );
        }

        if (elig) {
            std::vector<arch::Gp> slot_reg(irFuncs.num_slots);
            // parallel XMM home for Float slots.
            // slot_reg[s] stays default-constructed for xmm slots
            std::vector<arch::Vec> slot_vec(irFuncs.num_slots);
            auto is_xmm_slot = [&](uint32_t s) { return s < slot_types.size() && slot_types[s] == ir::Ty::Float; };
            for (uint32_t s = 0; s < irFuncs.num_slots; s++) {
                if (is_xmm_slot(s)) {
                    slot_vec[s] = arch::new_vec_f64(cc, "slot_vec");
                    // Param xmm slots are defined by the entry load below;
                    // others get a zero write so the allocator sees a def.
                    if (!(spec && s < irFuncs.num_params)) {
                        arch::vec_zero(cc, slot_vec[s]);
                    }
                    continue;
                }
                slot_reg[s] = cc.new_gp64();
                // param slots are defined by the entry loads below (which dominate every use),
                // so no placeholder init needed
                if (spec && s < irFuncs.num_params) {
                    continue;
                }
                if (s < slot_types.size() && raw_rep(slot_types[s])) {
                    cc.mov(slot_reg[s], Imm(static_cast<int64_t>(nb.none_tag)));
                } else {
                    cc.mov(slot_reg[s], Imm(0)); // defined for allocator
                }
            }
            if (needs_escape_infra) {
                const uint32_t stack_bound = static_cast<uint32_t>(irFuncs.insts.size()) + 8;
                Label rz_ok = cc.new_label();
                arch::Gp rz_need = cc.new_gp64("rrz_need");
                arch::Gp rz_cap = cc.new_gp64("rrz_cap");
                arch::load(cc, rz_need, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
                arch::load(cc, rz_cap, arch::ptr(vm_reg, static_cast<int>(VMStackCapacityOff)));
                arch::add_imm(cc, rz_need, static_cast<int64_t>(stack_bound) * static_cast<int64_t>(ValSize));
                cc.cmp(rz_need, rz_cap);
                arch::jcc(cc, arch::CC::kULT, rz_ok);
                call_u32(ctx, reinterpret_cast<const void *>(jit_reserve), stack_bound);
                cc.bind(rz_ok);
            }
            // (b) slot_base*8 hoist for raw-slot write-through (same
            // invariance argument as the general path's slot-CSE: slot_base
            // is set once at frame init; IR functions can't have upvalues).
            // The byte OFFSET survives stack reallocs; _M_start is reloaded
            // fresh at each write-through.
            arch::Gp r_sbb = cc.new_gp64("r_slot_base_bytes");
            if (has_raw_slots || spec) {
                arch::Gp fr = cc.new_gp64("r_frame");
                arch::load(cc, fr, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)));
                arch::sub_imm(cc, fr, static_cast<int>(FrameSize));
                arch::load(cc, r_sbb, arch::ptr(fr, static_cast<int>(SlotBaseOff)));
                arch::shl(cc, r_sbb, r_sbb, 3);
            }
            // load params from frame slot memory into their slot registers.
            //
            // Int48-typed param slots (the speculation) get a NaN-box tag guard,
            // non-int argument gets general path.
            Label spec_fail = cc.new_label();
            bool spec_fail_used = false;
            if (spec) {
                arch::Gp pbase = cc.new_gp64("r_param_base");
                arch::load(cc, pbase, arch::ptr(vm_reg, static_cast<int>(VMStackStartOff)));
                arch::add2(cc, pbase, r_sbb);
                for (uint32_t s = 0; s < irFuncs.num_params && s < irFuncs.num_slots; s++) {
                    const ir::Ty pt = s < slot_types.size() ? slot_types[s] : ir::Ty::Unknown;
                    arch::load(cc, slot_reg[s], arch::ptr(pbase, static_cast<int>((static_cast<int64_t>(s) * ValSize))));
                    if (pt == ir::Ty::Int48) {
                        arch::Gp ptag = cc.new_gp64("r_param_tag");
                        arch::load16_zx(cc, ptag, arch::ptr16(pbase, static_cast<int>((static_cast<int64_t>(s) * ValSize + tag.word_off))));
                        arch::cmp_imm(cc, ptag.r32(), Imm(static_cast<int>(tag._int)));
                        arch::jcc(cc, arch::CC::kNE, spec_fail);
                        spec_fail_used = true;
                        arch::sign_extend_48(cc, slot_reg[s]);
                    }
                    // raw_rep (Unknown/Heap): keep boxed bits as loaded.
                }
            }
            // Write-through: raw-rep slot registers stay authoritative for
            // reads, but every store also lands in frame slot memory so the
            // GC root scan (vm->stack walk) sees live heap values held by
            // this frame. GC is non-moving, so the register copy stays valid
            // across collections.
            auto slot_write_through = [&](uint32_t s) {
                arch::Gp addr = cc.new_gp64("r_wt");
                arch::load(cc, addr, arch::ptr(vm_reg, static_cast<int>(VMStackStartOff)));
                arch::add2(cc, addr, r_sbb);
                arch::store(cc, arch::ptr(addr, static_cast<int>(s) * static_cast<int>(ValSize)), slot_reg[s]);
            };
            // one stable Gp per phi value, written by each pred at the
            // pred's exit (before the jmp/jcc) and read at the successor block's
            // entry as if it were on the operand stack
            std::unordered_map<ir::ValueId, arch::Gp> phi_reg;
            for (const ir::Block &b : irFuncs.blocks) {
                for (ir::ValueId pv : b.phis) {
                    arch::Gp g = cc.new_gp64();
                    cc.mov(g, Imm(0)); // defined for allocator before first cross-edge write
                    phi_reg.emplace(pv, g);
                }
            }
            std::vector<asmjit::Label> rlabels(irFuncs.blocks.size());
            for (auto &L : rlabels) {
                L = cc.new_label();
            }
            // reachable set from entry
            std::vector<bool> reach(irFuncs.blocks.size(), false);
            if (irFuncs.entry >= 0 && static_cast<size_t>(irFuncs.entry) < irFuncs.blocks.size()) {
                std::vector<ir::BlockId> stk{ irFuncs.entry };
                reach[irFuncs.entry] = true;
                while (!stk.empty()) {
                    ir::BlockId bb = stk.back();
                    stk.pop_back();
                    for (ir::BlockId s : irFuncs.blocks[bb].succs) {
                        if (s >= 0 && static_cast<size_t>(s) < reach.size() && !reach[s]) {
                            reach[s] = true;
                            stk.push_back(s);
                        }
                    }
                }
            }
            // Array-header hoist plan (LICM of the invariant array base/size out
            // of a read loop; see plan_array_header_hoist in ir_opt.h). Allocate
            // one function-scoped GP per cached slot for the base pointer and one
            // for the element count, materialized once at the preheader and read
            // at every LoadIndex in the loop body.
            ir::ArrayHeaderHoist hoist_plan = ir::plan_array_header_hoist(irFuncs, int_arr_slots);
            std::vector<arch::Gp> hdr_base(irFuncs.num_slots);
            std::vector<arch::Gp> hdr_size(irFuncs.num_slots);
            for (uint32_t s : hoist_plan.slots) {
                if (s >= irFuncs.num_slots) {
                    continue;
                }
                hdr_base[s] = cc.new_gp64("hdr_base");
                hdr_size[s] = cc.new_gp64("hdr_size");
                cc.mov(hdr_base[s], Imm(0)); // defined for allocator
                cc.mov(hdr_size[s], Imm(0));
            }
            // Fallthrough elision, reachable blocks are emitted in ascending bid order,
            // so an edge to the next reachable block needs no jump.
            static const bool fallthrough_opt = true;
            std::vector<ir::BlockId> fallthrough_bid(irFuncs.blocks.size(), -1);
            {
                ir::BlockId prev = -1;
                for (size_t j = 0; j < irFuncs.blocks.size(); j++) {
                    if (!reach[j]) {
                        continue;
                    }
                    if (prev >= 0) {
                        fallthrough_bid[prev] = (ir::BlockId)j;
                    }
                    prev = (ir::BlockId)j;
                }
            }
            struct RE {
                arch::Gp reg;
                ir::Ty ty;
                bool is_const = false; // value is a compile-time int constant in `cval`
                int64_t cval = 0;
                // cached XMM form of a Float RE, a float op holds its result in
                // `vec` so a subsequent float consumer reuses it directly.
                arch::Vec vec;
                bool has_vec = false;
                // false when the value lives only in `vec` (a float-op result)
                // and the GP form hasn't been materialized yet; reg_of/box_re
                // materialize on demand. Elides the eager vec_to_gp after each float op.
                bool has_reg = true;
            };
            // Materialize an operand into a register (lazily for constants and vec-only float results).
            // Caches into `e.reg`
            auto reg_of = [&](RE &e) -> arch::Gp {
                if (e.is_const) {
                    arch::Gp g = cc.new_gp64();
                    cc.mov(g, Imm(e.cval));
                    e.reg = g;
                    e.is_const = false;
                    e.has_reg = true;
                } else if (!e.has_reg) {
                    // vec-only float result: emit the deferred vec_to_gp now.
                    arch::Gp g = cc.new_gp64("r_lazygp");
                    arch::vec_to_gp(cc, g, e.vec);
                    e.reg = g;
                    e.has_reg = true;
                }
                return e.reg;
            };
            // get the XMM form of a Float RE, reusing a cached vec from a producing float op when present
            auto vec_of = [&](RE &e) -> arch::Vec {
                if (e.has_vec) {
                    return e.vec;
                }
                arch::Vec v = arch::new_vec_f64(cc, "rfv");
                arch::gp_to_vec(cc, v, reg_of(e));
                e.vec = v;
                e.has_vec = true;
                return v;
            };
            // Compile-time operand stack.
            std::vector<RE> st;
            // produce a vreg holding the full boxed Value bits of an RE
            // (consts are encoded as immediates, raw-rep REs are returned as-is)
            auto box_re = [&](RE &e) -> arch::Gp {
                if (raw_rep(e.ty)) {
                    return e.reg; // already boxed bits (never const)
                }
                arch::Gp g = cc.new_gp64("r_box");
                if (e.ty == ir::Ty::Float) {
                    // a Float RE holds raw f64 bits, which are the boxed Value (make_float has no tag)
                    if (e.is_const) {
                        cc.mov(g, Imm(e.cval));
                    } else if (!e.has_reg) {
                        // vec-only float result. box_re must not mutate (slow-path safe),
                        // so materialize into a fresh reg from the cached vec WITHOUT caching back into e.
                        arch::vec_to_gp(cc, g, e.vec);
                    } else {
                        return e.reg;
                    }
                } else if (e.ty == ir::Ty::None) {
                    cc.mov(g, Imm(static_cast<int64_t>(nb.none_tag)));
                } else if (e.ty == ir::Ty::Bool) {
                    if (e.is_const) {
                        cc.mov(g, Imm(static_cast<int64_t>(nb.bool_tag | (e.cval ? 1u : 0u))));
                    } else {
                        arch::or_imm(cc, g, e.reg, static_cast<int64_t>(nb.bool_tag));
                    }
                } else { // Int48 raw payload
                    if (e.is_const) {
                        cc.mov(g, Imm(static_cast<int64_t>(nb.int_tag | (static_cast<uint64_t>(e.cval) & nb.ptr_mask))));
                    } else {
                        arch::Gp tmp = cc.new_gp64("r_boxt");
                        arch::mov_reg(cc, g, e.reg);
                        arch::nanbox_encode_int(cc, g, tmp);
                    }
                }
                return g;
            };
            // hand one op to a general-path (memory-stack ABI) helper.
            // Boxes + spills the entire operand stack to vm->stack.
            // The helper reads operands there, and a GC inside must see this frame's heap values
            auto escape_ex = [&](const void *helper, int kind, uint32_t immA, uint32_t immB, std::vector<RE *> extra, size_t consumed,
                                 bool produces, ir::Ty result_ty, int kind_hint = -1) -> RE {
                const size_t n = st.size() + extra.size();
                arch::Gp fin = cc.new_gp64("r_esc_fin");
                arch::load(cc, fin, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
                for (size_t i = 0; i < st.size(); i++) {
                    arch::Gp b = box_re(st[i]);
                    arch::store(cc, arch::ptr(fin, static_cast<int>((i * static_cast<size_t>(ValSize)))), b);
                }
                for (size_t i = 0; i < extra.size(); i++) {
                    arch::Gp b = box_re(*extra[i]);
                    arch::store(cc, arch::ptr(fin, static_cast<int>(((st.size() + i) * static_cast<size_t>(ValSize)))), b);
                }
                if (n) {
                    arch::add_imm(cc, fin, static_cast<int64_t>(n) * ValSize);
                    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), fin);
                }
                if (kind == 0) {
                    call0(ctx, helper);
                } else if (kind == 1) {
                    call_u32(ctx, helper, immA);
                } else if (kind == 2) {
                    call_u32_u32(ctx, helper, immA, immB);
                } else if (kind == 3) {
                    // the inline call fast path
                    emit_ir_call_value(ctx, immA, immB, kind_hint);
                } else {
                    emit_call_typeof(ctx);
                }
                RE out;
                out.ty = result_ty;
                arch::Gp fin2 = cc.new_gp64("r_esc_fin2");
                arch::load(cc, fin2, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
                if (produces) {
                    arch::Gp res = cc.new_gp64("r_esc_res");
                    arch::load(cc, res, arch::ptr(fin2, static_cast<int>((-ValSize))));
                    if (result_ty == ir::Ty::Int48) {
                        arch::sign_extend_48(cc, res);
                    } else if (result_ty == ir::Ty::Bool) {
                        arch::and_imm(cc, res, res, 1);
                    }
                    out.reg = res;
                } else {
                    out.reg = cc.new_gp64("r_esc_void");
                    cc.mov(out.reg, Imm(0));
                }
                const int64_t drop = static_cast<int64_t>((n - consumed + (produces ? 1 : 0))) * ValSize;
                if (drop) {
                    arch::sub_imm(cc, fin2, drop);
                    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), fin2);
                }
                return out;
            };
            // is this RE a raw untagged int/bool payload (inline register arith is valid) vs. boxed Value bits / none?
            auto re_raw = [](const RE &e) { return e.ty == ir::Ty::Int48 || e.ty == ir::Ty::Bool; };
            // Result-type clamp for escaped Dyn ops
            auto esc_result_ty = [&](ir::Ty t) {
                if (t == ir::Ty::Int48 || t == ir::Ty::Bool || raw_rep(t)) {
                    return t;
                }
                // a Float result's raw bits ARE the value (no tag),
                // so escape_ex keeps them unchanged and box_re round-trips them
                if (t == ir::Ty::Float) {
                    return t;
                }
                return ir::Ty::Unknown;
            };
            auto cmp_cond = [](ir::Op o) {
                switch (o) {
                    case ir::Op::DynCmpLt:
                    case ir::Op::ICmpLt:
                        return arch::CC::kLT;
                    case ir::Op::DynCmpLe:
                    case ir::Op::ICmpLe:
                        return arch::CC::kLE;
                    case ir::Op::DynCmpGt:
                    case ir::Op::ICmpGt:
                        return arch::CC::kGT;
                    case ir::Op::DynCmpGe:
                    case ir::Op::ICmpGe:
                        return arch::CC::kGE;
                    case ir::Op::DynCmpEq:
                    case ir::Op::DynStrictCmpEq:
                    case ir::Op::ICmpEq:
                        return arch::CC::kEQ;
                    default:
                        return arch::CC::kNE;
                }
            };
            for (size_t bid = 0; bid < irFuncs.blocks.size(); bid++) {
                if (!reach[bid]) {
                    continue;
                }
                const ir::Block &b = irFuncs.blocks[bid];
                cc.bind(rlabels[bid]);
                st.clear();
                for (ir::ValueId pv : b.phis) {
                    const ir::Inst &pin = irFuncs.inst(pv);
                    RE e;
                    auto phi_it = phi_reg.find(pv);
                    if (phi_it == phi_reg.end()) {
                        report(chunk, chunk_idx, "BAIL missing phi reg");
                        return { RegTierOutcome::Failed, nullptr };
                    }
                    e.reg = phi_it->second;
                    e.ty = pin.type;
                    st.push_back(e);
                }
                // if this block is a chosen preheader, materialize the hoisted array header(s) here.
                {
                    auto mit = hoist_plan.materialize.find((ir::BlockId)bid);
                    if (mit != hoist_plan.materialize.end()) {
                        for (uint32_t s : mit->second) {
                            arch::Gp ptr = cc.new_gp64("hdr_ptr");
                            arch::mov_reg(cc, ptr, slot_reg[s]);
                            arch::zero_extend_48(cc, ptr);
                            arch::load(cc, hdr_base[s], arch::ptr(ptr, static_cast<int>(ArrayVecStartOff)));
                            arch::load(cc, hdr_size[s], arch::ptr(ptr, static_cast<int>(ArrayVecFinishOff)));
                            arch::sub2(cc, hdr_size[s], hdr_base[s]);
                            arch::shr(cc, hdr_size[s], 3);
                        }
                    }
                }
                // Fuse a trailing comparison into a Branch: emit cmp+jcc directly,
                // leaving no materialized bool (avoids a per-iteration spill).
                bool fuse_cmp = false;
                // fused float-cmp -> branch (reg tier)
                bool fuse_fcmp = false;
                ir::Op fused_op = ir::Op::DynCmpLt;
                if (b.terminator != ir::InvalidValue && !b.insts.empty()) {
                    const ir::Inst &term = irFuncs.inst(b.terminator);
                    const ir::Inst &last = irFuncs.inst(b.insts.back());
                    bool is_cmp = last.op == ir::Op::DynCmpLt || last.op == ir::Op::DynCmpLe || last.op == ir::Op::DynCmpGt ||
                                  last.op == ir::Op::DynCmpGe || last.op == ir::Op::DynCmpEq || last.op == ir::Op::DynCmpNe ||
                                  last.op == ir::Op::DynStrictCmpEq || last.op == ir::Op::DynStrictCmpNe || last.op == ir::Op::ICmpLt ||
                                  last.op == ir::Op::ICmpLe || last.op == ir::Op::ICmpGt || last.op == ir::Op::ICmpGe ||
                                  last.op == ir::Op::ICmpEq || last.op == ir::Op::ICmpNe;
                    // Ordered float relops only (Eq/Ne need the ZF+PF combine, not a single jcc).
                    // Both operands must be Float.
                    bool is_fcmp = (last.op == ir::Op::FCmpLt || last.op == ir::Op::FCmpLe || last.op == ir::Op::FCmpGt ||
                                    last.op == ir::Op::FCmpGe) &&
                                   last.operands.size() == 2 && irFuncs.inst(last.operands[0]).type == ir::Ty::Float &&
                                   irFuncs.inst(last.operands[1]).type == ir::Ty::Float;
                    // fusion emits a raw register cmp, so both
                    // operands must be raw-rep
                    bool cmp_operands_raw = true;
                    if (is_cmp) {
                        for (ir::ValueId ov : last.operands) {
                            ir::Ty t = irFuncs.inst(ov).type;
                            if (t != ir::Ty::Int48 && t != ir::Ty::Bool) {
                                cmp_operands_raw = false;
                            }
                        }
                    }
                    if (term.op == ir::Op::Branch && ((is_cmp && cmp_operands_raw) || is_fcmp) && !term.operands.empty() &&
                        term.operands[0] == b.insts.back()) {
                        // don't fuse if either branch successor has phis
                        bool succ_has_phi = false;
                        if (term.target0 >= 0 && static_cast<size_t>(term.target0) < irFuncs.blocks.size() &&
                            !irFuncs.blocks[term.target0].phis.empty()) {
                            succ_has_phi = true;
                        }
                        if (term.target1 >= 0 && static_cast<size_t>(term.target1) < irFuncs.blocks.size() &&
                            !irFuncs.blocks[term.target1].phis.empty()) {
                            succ_has_phi = true;
                        }
                        if (!succ_has_phi) {
                            fused_op = last.op;
                            if (is_fcmp) {
                                fuse_fcmp = true;
                            } else {
                                fuse_cmp = true;
                            }
                        }
                    }
                }
                // write phi sources at the pred side. After the body
                // insts execute, the residual `st` carries `succ.phis.size()` items.
                auto emit_phi_writes_to = [&](ir::BlockId succ, size_t st_top_offset) {
                    if (succ < 0 || static_cast<size_t>(succ) >= irFuncs.blocks.size()) {
                        return;
                    }
                    const ir::Block &sb = irFuncs.blocks[succ];
                    if (sb.phis.empty()) {
                        return;
                    }
                    // The pred-side stack residual (excluding the terminator's
                    // consumed values) must have exactly sb.phis.size() items;
                    // these map positionally to phi[0..N-1].
                    size_t avail = (st.size() >= st_top_offset) ? (st.size() - st_top_offset) : 0;
                    size_t n = sb.phis.size();
                    if (avail < n) {
                        return; // mismatch; caller will fall back via finalize fail
                    }
                    size_t base = avail - n; // stack[base..base+n) -> phi[0..n)
                    for (size_t i = 0; i < n; i++) {
                        auto phi_it = phi_reg.find(sb.phis[i]);
                        if (phi_it == phi_reg.end()) {
                            return; // caller will fall back via finalize fail
                        }
                        arch::mov_reg(cc, phi_it->second, reg_of(st[base + i]));
                    }
                };
                size_t n_body = (fuse_cmp || fuse_fcmp) ? b.insts.size() - 1 : b.insts.size();
                for (size_t ii = 0; ii < n_body; ii++) {
                    const ir::Inst &in = irFuncs.inst(b.insts[ii]);
                    switch (in.op) {
                        case ir::Op::IConst: {
                            RE e;
                            e.ty = ir::Ty::Int48;
                            e.is_const = true;
                            e.cval = in.imm_int;
                            st.push_back(e);
                            break;
                        }
                        case ir::Op::FConst: {
                            // push the raw f64 bit pattern as a const RE
                            RE e;
                            e.ty = ir::Ty::Float;
                            e.is_const = true;
                            int64_t bits;
                            memcpy(&bits, &in.imm_float, sizeof(bits));
                            e.cval = bits;
                            st.push_back(e);
                            break;
                        }
                        case ir::Op::BConst: {
                            RE e;
                            e.ty = ir::Ty::Bool;
                            e.is_const = true;
                            e.cval = in.imm_int;
                            st.push_back(e);
                            break;
                        }
                        case ir::Op::NConst: {
                            RE e;
                            e.ty = ir::Ty::None;
                            e.is_const = true;
                            e.cval = 0;
                            st.push_back(e);
                            break;
                        }
                        case ir::Op::LoadSlot: {
                            const uint32_t s = in.imm_u32;
                            if (is_xmm_slot(s)) {
                                // Float slot lives in slot_vec[s],
                                // snapshot into a fresh vec
                                arch::Vec v = arch::new_vec_f64(cc, "ld_slot");
                                arch::vec_copy(cc, v, slot_vec[s]);
                                RE e;
                                e.ty = ir::Ty::Float;
                                e.vec = v;
                                e.has_vec = true;
                                e.has_reg = false;
                                st.push_back(e);
                                break;
                            }
                            arch::Gp g = cc.new_gp64();
                            arch::mov_reg(cc, g, slot_reg[s]); // snapshot
                            st.push_back({ g, slot_types[s] });
                            break;
                        }
                        case ir::Op::StoreSlot: {
                            const uint32_t s = in.imm_u32;
                            if (is_xmm_slot(s)) {
                                // write the value's XMM form into the slot's fixed vreg.
                                arch::vec_copy(cc, slot_vec[s], vec_of(st.back()));
                            } else if (raw_rep(slot_types[s])) {
                                // raw-rep slot, store boxed bits and write through to frame slot memory
                                arch::mov_reg(cc, slot_reg[s], box_re(st.back()));
                                slot_write_through(s);
                            } else {
                                arch::mov_reg(cc, slot_reg[s], reg_of(st.back())); // peek
                            }
                            break;
                        }
                        case ir::Op::Pop:
                            st.pop_back();
                            break;
                        case ir::Op::Dup:
                            st.push_back(st.back());
                            break;
                        // fused slot-store ops. No SSA result, no stack change
                        case ir::Op::StoreImmSlot:
                            if (raw_rep(slot_types[in.imm_u32])) {
                                cc.mov(
                                    slot_reg[in.imm_u32],
                                    Imm(static_cast<int64_t>(nb.int_tag | (static_cast<uint64_t>(in.imm_int) & nb.ptr_mask)))
                                );
                                slot_write_through(in.imm_u32);
                            } else {
                                cc.mov(slot_reg[in.imm_u32], Imm(in.imm_int));
                            }
                            break;
                        case ir::Op::StoreBImmSlot:
                            if (raw_rep(slot_types[in.imm_u32])) {
                                cc.mov(
                                    slot_reg[in.imm_u32],
                                    Imm(static_cast<int64_t>(nb.int_tag | (static_cast<uint64_t>(in.imm_int) & nb.ptr_mask)))
                                );
                                slot_write_through(in.imm_u32);
                            } else {
                                cc.mov(slot_reg[in.imm_u32], Imm(in.imm_int ? 1 : 0));
                            }
                            break;
                        case ir::Op::StoreNSlot:
                            if (raw_rep(slot_types[in.imm_u32])) {
                                cc.mov(slot_reg[in.imm_u32], Imm(static_cast<int64_t>(nb.none_tag)));
                                slot_write_through(in.imm_u32);
                            } else {
                                cc.mov(slot_reg[in.imm_u32], Imm(0));
                            }
                            break;
                        case ir::Op::CopySlot: {
                            const uint32_t src = static_cast<uint32_t>(in.imm_int);
                            const uint32_t dst = in.imm_u32;
                            if (is_xmm_slot(dst)) {
                                // Float->Float slot copy stays in XMM.
                                arch::vec_copy(cc, slot_vec[dst], slot_vec[src]);
                                break;
                            }
                            const bool src_raw = raw_rep(slot_types[src]);
                            const bool dst_raw = raw_rep(slot_types[dst]);
                            if (dst_raw && !src_raw) {
                                // box untagged payload into the raw slot
                                arch::Gp tmp = cc.new_gp64("r_cs_box");
                                if (slot_types[src] == ir::Ty::Bool) {
                                    arch::or_imm(cc, tmp, slot_reg[src], static_cast<int64_t>(nb.bool_tag));
                                } else {
                                    arch::Gp t2 = cc.new_gp64("r_cs_boxt");
                                    arch::mov_reg(cc, tmp, slot_reg[src]);
                                    arch::nanbox_encode_int(cc, tmp, t2);
                                }
                                arch::mov_reg(cc, slot_reg[dst], tmp);
                            } else {
                                // same representation (raw->untagged is
                                // rejected by eligibility)
                                arch::mov_reg(cc, slot_reg[dst], slot_reg[src]);
                            }
                            if (dst_raw) {
                                slot_write_through(dst);
                            }
                            break;
                        }
                        case ir::Op::IMod: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            arch::Gp lhs = cc.new_gp64("r_imod_l");
                            arch::mov_reg(cc, lhs, reg_of(ra));
                            arch::Gp res = cc.new_gp64("r_imod_r");
                            if (rb.is_const && rb.cval != 0 && rb.cval != -1) {
                                arch::Gp rhs = cc.new_gp64("r_imod_c");
                                cc.mov(rhs, Imm(rb.cval));
                                arch::smod_only(cc, res, lhs, rhs);
                            } else {
                                arch::Gp rhs = cc.new_gp64("r_imod_d");
                                arch::mov_reg(cc, rhs, reg_of(rb));
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::test_zero(cc, rhs);
                                arch::jcc(cc, arch::CC::kEQ, slow);
                                arch::cmp_imm(cc, rhs, Imm(-1));
                                arch::jcc(cc, arch::CC::kEQ, slow);
                                arch::Gp rem = cc.new_gp64("r_imod_m");
                                arch::smod_only(cc, rem, lhs, rhs);
                                arch::mov_reg(cc, res, rem);
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                // mod-by-zero must raise the proper runtime
                                // error; -1 kept off the idiv path to match
                                // the general tier.
                                RE r = escape_ex(reinterpret_cast<const void *>(jit_mod), 0, 0, 0, { &ra, &rb }, 2, true, ir::Ty::Int48);
                                arch::mov_reg(cc, res, r.reg);
                                cc.bind(done);
                            }
                            st.push_back({ res, ir::Ty::Int48 });
                            break;
                        }
                        case ir::Op::LoadIndex: {
                            RE ri = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            bool inline_ok = in.type == ir::Ty::Int48 && in.operands.size() >= 2;
                            if (inline_ok) {
                                const ir::Inst &obj = irFuncs.inst(in.operands[0]);
                                const ir::Inst &key = irFuncs.inst(in.operands[1]);
                                inline_ok =
                                    obj.op == ir::Op::LoadSlot && int_arr_slots.count(obj.imm_u32) != 0 && key.type == ir::Ty::Int48;
                            }
                            if (!inline_ok) {
                                // guarded inline array fast path for UNPROVEN receivers
                                const ir::Ty rty = esc_result_ty(in.type);
                                const bool key_raw = ri.ty == ir::Ty::Int48;
                                if (!raw_rep(ra.ty) || !(key_raw || raw_rep(ri.ty)) || !raw_rep(rty)) {
                                    RE r = escape_ex(reinterpret_cast<const void *>(jit_get_index), 0, 0, 0, { &ra, &ri }, 2, true, rty);
                                    st.push_back(r);
                                    break;
                                }
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::Gp g = cc.new_gp64("r_ggi_val");
                                arch::Gp t = cc.new_gp64("r_ggi_tag");
                                arch::mov_reg(cc, t, ra.reg);
                                arch::shr(cc, t, 48);
                                arch::cmp_imm(cc, t.r32(), Imm(static_cast<int>(tag._heap)));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp ptr = cc.new_gp64("r_ggi_ptr");
                                arch::mov_reg(cc, ptr, ra.reg);
                                arch::zero_extend_48(cc, ptr);
                                arch::cmp_mem8_imm(
                                    cc, arch::ptr8(ptr, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))),
                                    Imm(static_cast<int>(tag._array))
                                );
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp idx = cc.new_gp64("r_ggi_idx");
                                if (key_raw) {
                                    arch::mov_reg(cc, idx, reg_of(ri));
                                } else {
                                    arch::Gp kt = cc.new_gp64("r_ggi_ktag");
                                    arch::mov_reg(cc, kt, ri.reg);
                                    arch::shr(cc, kt, 48);
                                    arch::cmp_imm(cc, kt.r32(), Imm(static_cast<int>(tag._int)));
                                    arch::jcc(cc, arch::CC::kNE, slow);
                                    arch::mov_reg(cc, idx, ri.reg);
                                    arch::sign_extend_48(cc, idx);
                                }
                                arch::Gp start = cc.new_gp64("r_ggi_start");
                                arch::Gp size = cc.new_gp64("r_ggi_size");
                                arch::load(cc, start, arch::ptr(ptr, static_cast<int>(ArrayVecStartOff)));
                                arch::load(cc, size, arch::ptr(ptr, static_cast<int>(ArrayVecFinishOff)));
                                arch::sub2(cc, size, start);
                                arch::shr(cc, size, 3);
                                Label oob = cc.new_label();
                                // Unsigned comparison rejects both negative indices and
                                // indices past the end, matching jit_get_index.
                                cc.cmp(idx, size);
                                arch::jcc(cc, arch::CC::kUGE, oob);
                                arch::shl(cc, g, idx, 3);
                                arch::add2(cc, g, start);
                                arch::load(cc, g, arch::ptr(g, 0));
                                arch::jmp(cc, done);
                                cc.bind(oob);
                                cc.mov(g, Imm(static_cast<int64_t>(nb.none_tag)));
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                RE r = escape_ex(reinterpret_cast<const void *>(jit_get_index), 0, 0, 0, { &ra, &ri }, 2, true, rty);
                                arch::mov_reg(cc, g, r.reg);
                                cc.bind(done);
                                st.push_back({ g, rty });
                                break;
                            }
                            // Proven int-array + Int48 index.
                            const uint32_t li_slot = irFuncs.inst(in.operands[0]).imm_u32;
                            bool use_hoist = false;
                            {
                                auto vit = hoist_plan.valid.find((ir::BlockId)bid);
                                use_hoist = vit != hoist_plan.valid.end() && vit->second.count(li_slot) != 0;
                            }
                            arch::Gp start = cc.new_gp64("r_gi_start");
                            arch::Gp size = cc.new_gp64("r_gi_size");
                            if (use_hoist) {
                                start = hdr_base[li_slot];
                                size = hdr_size[li_slot];
                            } else {
                                arch::Gp ptr = cc.new_gp64("r_gi_ptr");
                                arch::mov_reg(cc, ptr, ra.reg);
                                arch::zero_extend_48(cc, ptr);
                                arch::load(cc, start, arch::ptr(ptr, static_cast<int>(ArrayVecStartOff)));
                                arch::load(cc, size, arch::ptr(ptr, static_cast<int>(ArrayVecFinishOff)));
                                arch::sub2(cc, size, start);
                                arch::shr(cc, size, 3);
                            }
                            arch::Gp idx = cc.new_gp64("r_gi_idx");
                            arch::mov_reg(cc, idx, reg_of(ri));
                            arch::Gp g = cc.new_gp64("r_gi_val");
                            Label oob = cc.new_label();
                            Label done = cc.new_label();
                            // A typed read is only valid when its index is proven
                            // in bounds. Keep the runtime check as a safeguard.
                            cc.cmp(idx, size);
                            arch::jcc(cc, arch::CC::kUGE, oob);
                            arch::shl(cc, g, idx, 3);
                            arch::add2(cc, g, start);
                            arch::load(cc, g, arch::ptr(g, 0));
                            arch::sign_extend_48(cc, g);
                            arch::jmp(cc, done);
                            cc.bind(oob);
                            cc.mov(g, Imm(0));
                            cc.bind(done);
                            st.push_back({ g, ir::Ty::Int48 });
                            break;
                        }
                        case ir::Op::StoreIndex: {
                            RE rv = st.back();
                            st.pop_back();
                            RE ri = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            bool inline_ok = in.operands.size() >= 3;
                            if (inline_ok) {
                                const ir::Inst &obj = irFuncs.inst(in.operands[0]);
                                const ir::Inst &key = irFuncs.inst(in.operands[1]);
                                const ir::Inst &val = irFuncs.inst(in.operands[2]);
                                inline_ok = obj.op == ir::Op::LoadSlot && int_arr_slots.count(obj.imm_u32) != 0 &&
                                            key.type == ir::Ty::Int48 && val.type == ir::Ty::Int48;
                            }
                            if (!inline_ok) {
                                // guarded inline in-bounds store for UNPROVEN receivers
                                const bool key_raw = ri.ty == ir::Ty::Int48;
                                if (!raw_rep(ra.ty) || !(key_raw || raw_rep(ri.ty))) {
                                    RE r = escape_ex(
                                        reinterpret_cast<const void *>(jit_set_index), 0, 0, 0, { &ra, &ri, &rv }, 3, true, in.type
                                    );
                                    st.push_back(r);
                                    break;
                                }
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::Gp t = cc.new_gp64("r_gsi_tag");
                                arch::mov_reg(cc, t, ra.reg);
                                arch::shr(cc, t, 48);
                                arch::cmp_imm(cc, t.r32(), Imm(static_cast<int>(tag._heap)));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp ptr = cc.new_gp64("r_gsi_ptr");
                                arch::mov_reg(cc, ptr, ra.reg);
                                arch::zero_extend_48(cc, ptr);
                                arch::cmp_mem8_imm(
                                    cc, arch::ptr8(ptr, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))),
                                    Imm(static_cast<int>(tag._array))
                                );
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp idx = cc.new_gp64("r_gsi_idx");
                                if (key_raw) {
                                    arch::mov_reg(cc, idx, reg_of(ri));
                                } else {
                                    arch::Gp kt = cc.new_gp64("r_gsi_ktag");
                                    arch::mov_reg(cc, kt, ri.reg);
                                    arch::shr(cc, kt, 48);
                                    arch::cmp_imm(cc, kt.r32(), Imm(static_cast<int>(tag._int)));
                                    arch::jcc(cc, arch::CC::kNE, slow);
                                    arch::mov_reg(cc, idx, ri.reg);
                                    arch::sign_extend_48(cc, idx);
                                }
                                arch::Gp start = cc.new_gp64("r_gsi_start");
                                arch::Gp size = cc.new_gp64("r_gsi_size");
                                arch::load(cc, start, arch::ptr(ptr, static_cast<int>(ArrayVecStartOff)));
                                arch::load(cc, size, arch::ptr(ptr, static_cast<int>(ArrayVecFinishOff)));
                                arch::sub2(cc, size, start);
                                arch::shr(cc, size, 3);
                                Label positive = cc.new_label();
                                // Bounds check, unsigned-compare form. Nari allows negative indices (from the
                                // end), but the overwhelmingly common case is 0 <= idx < size, and an *unsigned*
                                // compare decides that in one branch: a negative index looks huge unsigned, so it
                                // fails the same test as idx >= size. The wrap is handled off the fast path, and
                                // needs no second range test -- after idx += size with idx < 0, idx < size holds.
                                cc.cmp(idx, size);
                                arch::jcc(cc, arch::CC::kULT, positive);
                                arch::jns(cc, idx, slow);
                                arch::add2(cc, idx, size);
                                arch::js(cc, idx, slow);
                                cc.bind(positive);
                                {
                                    arch::Gp addr = cc.new_gp64("r_gsi_addr");
                                    arch::shl(cc, addr, idx, 3);
                                    arch::add2(cc, addr, start);
                                    arch::Gp boxed = box_re(rv);
                                    arch::store(cc, arch::ptr(addr, 0), boxed);
                                }
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                escape_ex(reinterpret_cast<const void *>(jit_set_index), 0, 0, 0, { &ra, &ri, &rv }, 3, true, in.type);
                                cc.bind(done);
                                st.push_back(rv);
                                break;
                            }
                            arch::Gp ptr = cc.new_gp64("r_si_ptr");
                            Label slow = cc.new_label();
                            Label done = cc.new_label();
                            if (!in.proven_array_receiver) {
                                arch::Gp receiver_tag = cc.new_gp64("r_si_tag");
                                arch::mov_reg(cc, receiver_tag, ra.reg);
                                arch::shr(cc, receiver_tag, 48);
                                arch::cmp_imm(cc, receiver_tag.r32(), Imm(static_cast<int>(tag._heap)));
                                arch::jcc(cc, arch::CC::kNE, slow);
                            }
                            arch::mov_reg(cc, ptr, ra.reg);
                            arch::zero_extend_48(cc, ptr);
                            if (!in.proven_array_receiver) {
                                arch::cmp_mem8_imm(
                                    cc, arch::ptr8(ptr, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))),
                                    Imm(static_cast<int>(tag._array))
                                );
                                arch::jcc(cc, arch::CC::kNE, slow);
                            }
                            arch::Gp start = cc.new_gp64("r_si_start");
                            arch::Gp size = cc.new_gp64("r_si_size");
                            arch::load(cc, start, arch::ptr(ptr, static_cast<int>(ArrayVecStartOff)));
                            arch::load(cc, size, arch::ptr(ptr, static_cast<int>(ArrayVecFinishOff)));
                            arch::sub2(cc, size, start);
                            arch::shr(cc, size, 3);
                            arch::Gp idx = cc.new_gp64("r_si_idx");
                            arch::mov_reg(cc, idx, reg_of(ri));
                            Label positive = cc.new_label();
                            // Bounds check, unsigned-compare form. Nari allows negative indices (from the
                            // end), but the overwhelmingly common case is 0 <= idx < size, and an *unsigned*
                            // compare decides that in one branch: a negative index looks huge unsigned, so it
                            // fails the same test as idx >= size. The wrap is handled off the fast path, and
                            // needs no second range test -- after idx += size with idx < 0, idx < size holds.
                            cc.cmp(idx, size);
                            arch::jcc(cc, arch::CC::kULT, positive);
                            arch::jns(cc, idx, slow);
                            arch::add2(cc, idx, size);
                            arch::js(cc, idx, slow);
                            cc.bind(positive);
                            {
                                arch::Gp addr = cc.new_gp64("r_si_addr");
                                arch::shl(cc, addr, idx, 3);
                                arch::add2(cc, addr, start);
                                arch::Gp boxed = box_re(rv);
                                arch::store(cc, arch::ptr(addr, 0), boxed);
                            }
                            arch::jmp(cc, done);
                            cc.bind(slow);
                            // OOB writes resize the array -- keep the general
                            // helper's exact semantics via escape. The
                            // helper's result equals val; the register RE
                            // already holds it, so discard the escape's copy.
                            escape_ex(reinterpret_cast<const void *>(jit_set_index), 0, 0, 0, { &ra, &ri, &rv }, 3, true, in.type);
                            cc.bind(done);
                            // StoreIndex's IR result is the stored value.
                            st.push_back(rv);
                            break;
                        }
                        case ir::Op::MakeArray: {
                            const uint32_t nelem = in.imm_u32;
                            std::vector<RE> elems(nelem);
                            std::vector<RE *> eptrs(nelem);
                            for (uint32_t k = 0; k < nelem; k++) {
                                elems[nelem - 1 - k] = st.back();
                                st.pop_back();
                            }
                            for (uint32_t k = 0; k < nelem; k++) {
                                eptrs[k] = &elems[k];
                            }
                            RE r = escape_ex(reinterpret_cast<const void *>(jit_make_array), 1, nelem, 0, eptrs, nelem, true, in.type);
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::ArraySpread: {
                            RE iterable = st.back();
                            st.pop_back();
                            RE target = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &target, &iterable };
                            RE result = escape_ex(reinterpret_cast<const void *>(jit_array_spread), 0, 0, 0, operands, 2, true, in.type);
                            st.push_back(result);
                            break;
                        }
                        case ir::Op::ArrayPush: {
                            RE value = st.back();
                            st.pop_back();
                            RE target = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &target, &value };
                            RE result = escape_ex(reinterpret_cast<const void *>(jit_array_push), 0, 0, 0, operands, 2, true, in.type);
                            st.push_back(result);
                            break;
                        }
                        case ir::Op::StrAppendSlot: {
                            RE rhs = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &rhs };
                            RE result = escape_ex(
                                reinterpret_cast<const void *>(jit_str_append_slot), 1, in.imm_u32, 0, operands, 1, true, in.type
                            );
                            st.push_back(result);
                            break;
                        }
                        case ir::Op::Call: {
                            const uint32_t argc = in.imm_u32;
                            std::vector<RE> args(argc + 1);
                            std::vector<RE *> aptrs(argc + 1);
                            for (uint32_t k = 0; k < argc + 1; k++) {
                                args[argc - k] = st.back();
                                st.pop_back();
                            }
                            for (uint32_t k = 0; k < argc + 1; k++) {
                                aptrs[k] = &args[k];
                            }
                            const ir::Inst &callee = irFuncs.inst(in.operands[0]);
                            const bool direct_typeof = argc == 1 && callee.op == ir::Op::LoadGlobal && callee.imm_u32 == typeof_name_idx_;
                            const bool direct_to_number =
                                argc == 1 && callee.op == ir::Op::LoadGlobal && callee.imm_u32 == js_to_number_name_idx_;
                            const void *helper = direct_to_number ? reinterpret_cast<const void *>(jit_call_js_to_number) : nullptr;
                            const int kind = direct_typeof ? 4 : helper ? 0 : 3;
                            RE r = escape_ex(
                                helper, kind, argc, static_cast<uint32_t>(in.imm_int), aptrs, argc + 1, true, in.type,
                                callee_kind_hint(ctx, in)
                            );
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::CallSpread: {
                            RE args = st.back();
                            st.pop_back();
                            RE callee = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &callee, &args };
                            RE result =
                                escape_ex(reinterpret_cast<const void *>(jit_call_spread), 1, in.imm_u32, 0, operands, 2, true, in.type);
                            st.push_back(result);
                            break;
                        }
                        case ir::Op::CallMethod: {
                            const uint32_t method_idx = in.imm_u32;
                            const uint32_t argc = static_cast<uint32_t>(in.imm_int);
                            // Inline arr.push(<raw int/bool>)
                            bool inline_push = method_idx < chunk.strings.size() && chunk.strings[method_idx] == "push" && argc == 1 &&
                                               in.operands.size() >= 2;
                            if (inline_push) {
                                const ir::Inst &recv = irFuncs.inst(in.operands[0]);
                                const ir::Inst &arg = irFuncs.inst(in.operands[1]);
                                inline_push = raw_rep(recv.type) && raw_rep(in.type) && // result RE holds raw none bits
                                              (arg.type == ir::Ty::Int48 || arg.type == ir::Ty::Bool);
                            }
                            if (inline_push) {
                                RE rarg = st.back();
                                st.pop_back();
                                RE rrecv = st.back();
                                st.pop_back();
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::Gp g = cc.new_gp64("r_push_res");
                                arch::Gp ptr = cc.new_gp64("r_push_ptr");
                                arch::Gp push_tag = cc.new_gp64("r_push_tag");
                                arch::mov_reg(cc, push_tag, rrecv.reg);
                                arch::shr(cc, push_tag, 48);
                                arch::cmp_imm(cc, push_tag.r32(), Imm(static_cast<int>(tag._heap)));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::mov_reg(cc, ptr, rrecv.reg);
                                arch::zero_extend_48(cc, ptr);
                                arch::cmp_mem8_imm(
                                    cc, arch::ptr8(ptr, static_cast<int>(NARI_JIT_OFF(HeapHeader, type_tag))),
                                    Imm(static_cast<int>(tag._array))
                                );
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp afin = cc.new_gp64("r_push_fin");
                                arch::Gp acap = cc.new_gp64("r_push_cap");
                                arch::load(cc, afin, arch::ptr(ptr, static_cast<int>(ArrayVecFinishOff)));
                                arch::load(cc, acap, arch::ptr(ptr, static_cast<int>(ArrayVecCapacityOff)));
                                cc.cmp(afin, acap);
                                arch::jcc(cc, arch::CC::kEQ, slow);
                                {
                                    arch::Gp boxed = box_re(rarg);
                                    arch::store(cc, arch::ptr(afin, 0), boxed);
                                    arch::add_imm(cc, afin, static_cast<int>(ValSize));
                                    arch::store(cc, arch::ptr(ptr, static_cast<int>(ArrayVecFinishOff)), afin);
                                }
                                cc.mov(g, Imm(static_cast<int64_t>(nb.none_tag)));
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                {
                                    RE r = escape_ex(
                                        reinterpret_cast<const void *>(jit_call_method), 2, method_idx, argc, { &rrecv, &rarg }, argc + 1,
                                        true, in.type
                                    );
                                    arch::mov_reg(cc, g, r.reg);
                                }
                                cc.bind(done);
                                st.push_back({ g, in.type });
                                break;
                            }
                            std::vector<RE> args(argc + 1);
                            std::vector<RE *> aptrs(argc + 1);
                            for (uint32_t k = 0; k < argc + 1; k++) {
                                args[argc - k] = st.back();
                                st.pop_back();
                            }
                            for (uint32_t k = 0; k < argc + 1; k++) {
                                aptrs[k] = &args[k];
                            }
                            RE r = escape_ex(
                                reinterpret_cast<const void *>(jit_call_method), 2, method_idx, argc, aptrs, argc + 1, true, in.type
                            );
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::LoadGlobal: {
                            // Inline indexed global-cache read (mirrors the
                            // general path's emit_ir_load_global); cache miss
                            // escapes to jit_load_global.
                            const uint32_t name_idx = in.imm_u32;
                            Label slow = cc.new_label();
                            Label done = cc.new_label();
                            arch::Gp g = cc.new_gp64("r_lg_val");
                            arch::Gp vaddr = cc.new_gp64("r_lg_vaddr");
                            arch::Gp vfin = cc.new_gp64("r_lg_vfin");
                            arch::load(cc, vaddr, arch::ptr(vm_reg, static_cast<int>(VMGlobalCacheValidStartOff)));
                            arch::load(cc, vfin, arch::ptr(vm_reg, static_cast<int>(VMGlobalCacheValidFinishOff)));
                            arch::add_imm(cc, vaddr, static_cast<int64_t>(name_idx));
                            cc.cmp(vaddr, vfin);
                            arch::jcc(cc, arch::CC::kFGE, slow);
                            arch::Gp valid = cc.new_gp64("r_lg_valid");
                            arch::load8_zx(cc, valid, arch::ptr8(vaddr, 0));
                            arch::test_zero(cc, valid);
                            arch::jcc(cc, arch::CC::kEQ, slow);
                            {
                                arch::Gp cstart = cc.new_gp64("r_lg_cstart");
                                arch::load(cc, cstart, arch::ptr(vm_reg, static_cast<int>(VMGlobalCacheStartOff)));
                                const int64_t cache_off = static_cast<int64_t>(name_idx) * static_cast<int64_t>(ValSize);
                                if (cache_off >= INT32_MIN && cache_off <= INT32_MAX) {
                                    arch::load(cc, g, arch::ptr(cstart, static_cast<int>(cache_off)));
                                } else {
                                    arch::add_imm(cc, cstart, cache_off);
                                    arch::load(cc, g, arch::ptr(cstart, 0));
                                }
                                if (in.type == ir::Ty::Int48) {
                                    arch::sign_extend_48(cc, g);
                                } else if (in.type == ir::Ty::Bool) {
                                    arch::and_imm(cc, g, g, 1);
                                }
                            }
                            arch::jmp(cc, done);
                            cc.bind(slow);
                            {
                                RE r = escape_ex(reinterpret_cast<const void *>(jit_load_global), 1, name_idx, 0, {}, 0, true, in.type);
                                arch::mov_reg(cc, g, r.reg);
                            }
                            cc.bind(done);
                            st.push_back({ g, in.type });
                            break;
                        }
                        // Not on Bool (0 <-> 1)
                        case ir::Op::Not: {
                            RE ra = st.back();
                            st.pop_back();
                            arch::Gp g = cc.new_gp64();
                            arch::mov_reg(cc, g, reg_of(ra));
                            arch::xor_imm(cc, g, 1);
                            st.push_back({ g, ir::Ty::Bool });
                            break;
                        }
                        case ir::Op::JsTruthy: {
                            RE ra = st.back();
                            st.pop_back();
                            if (ra.ty == ir::Ty::Bool) {
                                st.push_back({ reg_of(ra), ir::Ty::Bool });
                                break;
                            }
                            if (ra.ty == ir::Ty::None) {
                                arch::Gp result = cc.new_gp64("r_truthy_none");
                                cc.mov(result, Imm(0));
                                st.push_back({ result, ir::Ty::Bool });
                                break;
                            }
                            if (ra.ty == ir::Ty::Int48) {
                                arch::Gp result = cc.new_gp64("r_truthy_int");
                                cc.cmp(reg_of(ra), Imm(0));
                                arch::cset(cc, result, arch::CC::kNE);
                                st.push_back({ result, ir::Ty::Bool });
                                break;
                            }
                            RE r = escape_ex(reinterpret_cast<const void *>(jit_js_truthy), 0, 0, 0, { &ra }, 1, true, ir::Ty::Bool);
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::IsNone: {
                            RE ra = st.back();
                            st.pop_back();
                            arch::Gp raw = box_re(ra);
                            arch::Gp none = cc.new_gp64("r_none");
                            arch::Gp result = cc.new_gp64("r_is_none");
                            cc.mov(none, Imm(static_cast<int64_t>(nb.none_tag)));
                            cc.cmp(raw, none);
                            arch::cset(cc, result, arch::CC::kEQ);
                            st.push_back({ result, ir::Ty::Bool });
                            break;
                        }
                        case ir::Op::DynAdd:
                        case ir::Op::IAdd:
                        case ir::Op::DynSub:
                        case ir::Op::ISub: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            // non-raw operand (boxed bits, e.g. a Call result).
                            // Speculative both-int inline path
                            if (!re_raw(ra) || !re_raw(rb)) {
                                const bool is_add = in.op == ir::Op::DynAdd || in.op == ir::Op::IAdd;
                                const void *helper =
                                    is_add ? reinterpret_cast<const void *>(jit_add) : reinterpret_cast<const void *>(jit_sub);
                                const ir::Ty rty = esc_result_ty(in.type);
                                // Bool operands have add semantics only the
                                // helper knows; don't speculate.
                                const bool can_inline =
                                    (re_raw(ra) ? ra.ty == ir::Ty::Int48 : true) && (re_raw(rb) ? rb.ty == ir::Ty::Int48 : true);
                                if (!can_inline) {
                                    RE r = escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, rty);
                                    st.push_back(r);
                                    break;
                                }
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::Gp g = cc.new_gp64("r_dynbx_res");
                                // Unbox each side into a temp; boxed sides
                                // get an int-tag guard first. Temps keep the
                                // original REs unmutated (the slow path
                                // respills them via escape_ex).
                                auto unbox_side = [&](RE &e, const char *nm) {
                                    arch::Gp v = cc.new_gp64(nm);
                                    if (e.is_const) {
                                        cc.mov(v, Imm(e.cval));
                                        return v;
                                    }
                                    // reg_of materializes a vec-only float's GP
                                    arch::Gp er = reg_of(e);
                                    arch::mov_reg(cc, v, er);
                                    if (!re_raw(e)) {
                                        arch::Gp t = cc.new_gp64("r_dynbx_tag");
                                        arch::mov_reg(cc, t, er);
                                        arch::shr(cc, t, 48);
                                        arch::cmp_imm(cc, t.r32(), Imm(static_cast<int>(tag._int)));
                                        arch::jcc(cc, arch::CC::kNE, slow);
                                        arch::sign_extend_48(cc, v);
                                    }
                                    return v;
                                };
                                arch::Gp va = unbox_side(ra, "r_dynbx_a");
                                arch::Gp vb = unbox_side(rb, "r_dynbx_b");
                                if (is_add) {
                                    arch::add2(cc, va, vb);
                                } else {
                                    arch::sub2(cc, va, vb);
                                }
                                // int48 overflow -> float promotion: helper.
                                arch::Gp chk = cc.new_gp64("r_dynbx_chk");
                                arch::mov_reg(cc, chk, va);
                                arch::sign_extend_48(cc, chk);
                                cc.cmp(chk, va);
                                arch::jcc(cc, arch::CC::kNE, slow);
                                if (rty == ir::Ty::Int48) {
                                    arch::mov_reg(cc, g, va);
                                } else {
                                    // result rep is boxed bits: rebox.
                                    arch::Gp tmp = cc.new_gp64("r_dynbx_tmp");
                                    arch::mov_reg(cc, g, va);
                                    arch::nanbox_encode_int(cc, g, tmp);
                                }
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                RE r = escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, rty);
                                arch::mov_reg(cc, g, r.reg);
                                cc.bind(done);
                                st.push_back({ g, rty });
                                break;
                            }
                            const bool is_add_op = in.op == ir::Op::DynAdd || in.op == ir::Op::IAdd;
                            const void *helper = is_add_op ? reinterpret_cast<const void *>(jit_add)
                                                           : reinterpret_cast<const void *>(jit_sub);
                            const ir::Ty rty = esc_result_ty(in.type);
                            if (ra.ty != ir::Ty::Int48 || rb.ty != ir::Ty::Int48) {
                                st.push_back(escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, rty));
                                break;
                            }
                            if (in.proven_int48) {
                                arch::Gp g = cc.new_gp64("r_bounded_int");
                                if (rb.is_const) {
                                    const int64_t d = is_add_op ? rb.cval : -rb.cval;
                                    arch::lea(cc, g, reg_of(ra), d);
                                } else if (is_add_op) {
                                    arch::add3(cc, g, reg_of(ra), rb.reg);
                                } else {
                                    arch::mov_reg(cc, g, reg_of(ra));
                                    arch::sub2(cc, g, rb.reg);
                                }
                                st.push_back({ g, ir::Ty::Int48 });
                                break;
                            }
                            arch::Gp g = cc.new_gp64("r_checked_int");
                            Label slow = cc.new_label();
                            Label done = cc.new_label();
                            // Prefer the 3-operand forms: `g = a + b` as one LEA
                            // rather than `mov g,a; add g,b`. The operands fit
                            // Int48, so their sum/difference fits i64; check the
                            // Int48 range before boxing the result.
                            if (rb.is_const) {
                                const int64_t d = is_add_op ? rb.cval : -rb.cval;
                                if (d >= INT32_MIN && d <= INT32_MAX) {
                                    arch::lea(cc, g, reg_of(ra), d);
                                } else {
                                    arch::mov_reg(cc, g, reg_of(ra));
                                    if (is_add_op) {
                                        arch::add_imm(cc, g, rb.cval);
                                    } else {
                                        arch::sub_imm(cc, g, rb.cval);
                                    }
                                }
                            } else if (is_add_op) {
                                arch::add3(cc, g, reg_of(ra), rb.reg);
                            } else {
                                arch::mov_reg(cc, g, reg_of(ra));
                                arch::sub2(cc, g, rb.reg);
                            }
                            arch::Gp check = cc.new_gp64("r_checked_range");
                            arch::mov_reg(cc, check, g);
                            arch::sign_extend_48(cc, check);
                            cc.cmp(check, g);
                            arch::jcc(cc, arch::CC::kNE, slow);
                            arch::Gp tag_reg = cc.new_gp64("r_checked_tag");
                            arch::nanbox_encode_int(cc, g, tag_reg);
                            arch::jmp(cc, done);
                            cc.bind(slow);
                            RE fallback = escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, rty);
                            arch::mov_reg(cc, g, fallback.reg);
                            cc.bind(done);
                            st.push_back({ g, rty });
                            break;
                        }
                        // inline xmm float arithmetic. Operands are proven Float
                        case ir::Op::FAdd:
                        case ir::Op::FSub:
                        case ir::Op::FMul:
                        case ir::Op::FDiv: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            // reuse cached XMM forms (vec_of) so chained float ops don't reload operands from GP
                            arch::Vec fa = vec_of(ra);
                            arch::Vec fb = vec_of(rb);
                            arch::Vec fr = arch::new_vec_f64(cc, "rfb_r");
                            switch (in.op) {
                                case ir::Op::FAdd:
                                    arch::fadd(cc, fr, fa, fb);
                                    break;
                                case ir::Op::FSub:
                                    arch::fsub(cc, fr, fa, fb);
                                    break;
                                case ir::Op::FMul:
                                    arch::fmul(cc, fr, fa, fb);
                                    break;
                                default:
                                    arch::fdiv(cc, fr, fa, fb);
                                    break;
                            }
                            RE re;
                            re.ty = ir::Ty::Float;
                            // Cache the result's XMM form: a downstream float op
                            // reuses `fr` instead of gp_to_vec-ing `g` back.
                            re.vec = fr;
                            re.has_vec = true;
                            // defer the vec_to_gp. The GP form is only emitted if a non-float consumer
                            // reads it via reg_of/box_re
                            re.has_reg = false;
                            st.push_back(re);
                            break;
                        }
                        // inline xmm float compare. Result is a raw 0/1 Bool, saves one ucomisd over the general path
                        case ir::Op::FCmpEq:
                        case ir::Op::FCmpNe:
                        case ir::Op::FCmpLt:
                        case ir::Op::FCmpLe:
                        case ir::Op::FCmpGt:
                        case ir::Op::FCmpGe: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            // float_cmp is ucomisd (read-only), so cached operand vecs are safe and unclobbered.
                            arch::Vec fa = vec_of(ra);
                            arch::Vec fb = vec_of(rb);
                            arch::Gp g = cc.new_gp64("rfc_out");
                            if (in.op == ir::Op::FCmpEq) {
                                arch::float_cmp(cc, fa, fb);
                                arch::float_to_bool_eq(cc, g, true);
                            } else if (in.op == ir::Op::FCmpNe) {
                                arch::float_cmp(cc, fa, fb);
                                arch::float_to_bool_eq(cc, g, false);
                            } else {
                                const bool swap = (in.op == ir::Op::FCmpLt || in.op == ir::Op::FCmpLe);
                                const arch::CC::Cond fcc =
                                    (in.op == ir::Op::FCmpLt || in.op == ir::Op::FCmpGt) ? arch::CC::kFGT : arch::CC::kFGE;
                                if (swap) {
                                    arch::float_cmp(cc, fb, fa);
                                } else {
                                    arch::float_cmp(cc, fa, fb);
                                }
                                arch::cset(cc, g, fcc);
                            }
                            st.push_back({ g, ir::Ty::Bool });
                            break;
                        }
                        // Guarded integer fast paths for boxed numeric values.
                        case ir::Op::DynMul:
                        case ir::Op::IMul:
                        case ir::Op::DynDiv:
                        case ir::Op::DynMod: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            const bool multiply = in.op == ir::Op::DynMul || in.op == ir::Op::IMul;
                            const void *helper = multiply ? reinterpret_cast<const void *>(jit_mul)
                                                 : in.op == ir::Op::DynDiv ? reinterpret_cast<const void *>(jit_div)
                                                                           : reinterpret_cast<const void *>(jit_mod);
                            const ir::Ty rty = esc_result_ty(in.type);
                            if (in.proven_int48 && multiply && ra.ty == ir::Ty::Int48 && rb.ty == ir::Ty::Int48) {
                                arch::Gp result = cc.new_gp64("r_bounded_mul");
                                arch::imul(cc, result, reg_of(ra), reg_of(rb));
                                st.push_back({ result, ir::Ty::Int48 });
                                break;
                            }
                            const bool can_guard = in.op != ir::Op::DynDiv &&
                                (ra.ty == ir::Ty::Int48 || raw_rep(ra.ty)) &&
                                (rb.ty == ir::Ty::Int48 || raw_rep(rb.ty));
                            if (!can_guard) {
                                st.push_back(escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, rty));
                                break;
                            }
                            Label slow = cc.new_label();
                            Label done = cc.new_label();
                            auto int_side = [&](RE &e) {
                                if (e.ty == ir::Ty::Int48) {
                                    return reg_of(e);
                                }
                                arch::Gp tag_bits = cc.new_gp64("r_mulmod_tag");
                                arch::mov_reg(cc, tag_bits, e.reg);
                                arch::shr(cc, tag_bits, 48);
                                arch::cmp_imm(cc, tag_bits.r32(), Imm(static_cast<int>(tag._int)));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp payload = cc.new_gp64("r_mulmod_payload");
                                arch::mov_reg(cc, payload, e.reg);
                                arch::sign_extend_48(cc, payload);
                                return payload;
                            };
                            arch::Gp lhs = int_side(ra);
                            arch::Gp rhs = int_side(rb);
                            arch::Gp result = cc.new_gp64("r_mulmod_result");
                            if (multiply) {
#if NARI_JIT_ARM64
                                arch::Gp high = cc.new_gp64("r_mul_high");
                                cc.smulh(high, lhs, rhs);
#endif
                                arch::imul(cc, result, lhs, rhs);
#if NARI_JIT_ARM64
                                arch::Gp sign = cc.new_gp64("r_mul_sign");
                                cc.asr(sign, result, Imm(63));
                                cc.cmp(high, sign);
                                arch::jcc(cc, arch::CC::kNE, slow);
#else
                                cc.jo(slow);
#endif
                                arch::Gp range = cc.new_gp64("r_mul_range");
                                arch::mov_reg(cc, range, result);
                                arch::sign_extend_48(cc, range);
                                cc.cmp(range, result);
                                arch::jcc(cc, arch::CC::kNE, slow);
                            } else {
                                arch::test_zero(cc, rhs);
                                arch::jcc(cc, arch::CC::kEQ, slow);
                                arch::cmp_imm(cc, rhs, Imm(-1));
                                arch::jcc(cc, arch::CC::kEQ, slow);
                                arch::smod_only(cc, result, lhs, rhs);
                            }
                            if (rty != ir::Ty::Int48) {
                                arch::Gp tmp = cc.new_gp64("r_mulmod_box");
                                arch::nanbox_encode_int(cc, result, tmp);
                            }
                            arch::jmp(cc, done);
                            cc.bind(slow);
                            RE fallback = escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, rty);
                            arch::mov_reg(cc, result, fallback.reg);
                            cc.bind(done);
                            st.push_back({ result, rty });
                            break;
                        }
                        default: { // comparisons
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            // boxed operand, escape to the general-path dynamic comparison helper.
                            if (!re_raw(ra) || !re_raw(rb)) {
                                const void *h;
                                switch (in.op) {
                                    case ir::Op::DynCmpLt:
                                        h = reinterpret_cast<const void *>(jit_lt);
                                        break;
                                    case ir::Op::DynCmpLe:
                                        h = reinterpret_cast<const void *>(jit_le);
                                        break;
                                    case ir::Op::DynCmpGt:
                                        h = reinterpret_cast<const void *>(jit_gt);
                                        break;
                                    case ir::Op::DynCmpGe:
                                        h = reinterpret_cast<const void *>(jit_ge);
                                        break;
                                    case ir::Op::DynCmpEq:
                                        h = reinterpret_cast<const void *>(jit_eq);
                                        break;
                                    case ir::Op::DynStrictCmpEq:
                                        h = reinterpret_cast<const void *>(jit_strict_eq);
                                        break;
                                    case ir::Op::DynStrictCmpNe:
                                        h = reinterpret_cast<const void *>(jit_strict_ne);
                                        break;
                                    default:
                                        h = reinterpret_cast<const void *>(jit_ne);
                                        break;
                                }
                                const bool can_guard =
                                    (ra.ty == ir::Ty::Int48 || raw_rep(ra.ty)) &&
                                    (rb.ty == ir::Ty::Int48 || raw_rep(rb.ty));
                                if (!can_guard) {
                                    st.push_back(escape_ex(h, 0, 0, 0, { &ra, &rb }, 2, true, ir::Ty::Bool));
                                    break;
                                }
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::Gp g = cc.new_gp64("r_checked_cmp");
                                auto int_side = [&](RE &e) {
                                    if (e.ty == ir::Ty::Int48) {
                                        return reg_of(e);
                                    }
                                    arch::Gp v = cc.new_gp64("r_cmp_payload");
                                    arch::Gp tag_bits = cc.new_gp64("r_cmp_tag");
                                    arch::mov_reg(cc, tag_bits, e.reg);
                                    arch::shr(cc, tag_bits, 48);
                                    arch::cmp_imm(cc, tag_bits.r32(), Imm(static_cast<int>(tag._int)));
                                    arch::jcc(cc, arch::CC::kNE, slow);
                                    arch::mov_reg(cc, v, e.reg);
                                    arch::sign_extend_48(cc, v);
                                    return v;
                                };
                                arch::Gp left = int_side(ra);
                                arch::Gp right = int_side(rb);
                                cc.cmp(left, right);
                                arch::cset(cc, g, cmp_cond(in.op));
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                RE r = escape_ex(h, 0, 0, 0, { &ra, &rb }, 2, true, ir::Ty::Bool);
                                arch::mov_reg(cc, g, r.reg);
                                cc.bind(done);
                                st.push_back({ g, ir::Ty::Bool });
                                break;
                            }
                            if (rb.is_const) {
                                arch::cmp_imm(cc, reg_of(ra), Imm(rb.cval));
                            } else {
                                cc.cmp(reg_of(ra), reg_of(rb));
                            }
                            arch::Gp g = cc.new_gp64();
                            arch::cset(cc, g, cmp_cond(in.op));
                            st.push_back({ g, ir::Ty::Bool });
                            break;
                        }
                    }
                }
                const ir::Inst &t = irFuncs.inst(b.terminator);
                if (t.op == ir::Op::Jump) {
                    // 0 stack items consumed by Jump, all residual items are phi sources for the successor.
                    emit_phi_writes_to(t.target0, /*st_top_offset=*/0);
                    if (irFuncs.blocks[t.target0].start_pc <= b.start_pc && !irFuncs.blocks[t.target0].short_bounded_loop) {
                        poll_shutdown(ctx);
                    }
                    // skip the jump if the target is the next block
                    if (!(fallthrough_opt && t.target0 == fallthrough_bid[bid])) {
                        arch::jmp(cc, rlabels[t.target0]);
                    }
                } else if (t.op == ir::Op::Branch) {
                    if ((irFuncs.blocks[t.target0].start_pc <= b.start_pc && !irFuncs.blocks[t.target0].short_bounded_loop) ||
                        (irFuncs.blocks[t.target1].start_pc <= b.start_pc && !irFuncs.blocks[t.target1].short_bounded_loop)) {
                        poll_shutdown(ctx);
                    }
                    if (fuse_fcmp) {
                        // fused ordered float-cmp -> branch.
                        RE rb = st.back();
                        st.pop_back();
                        RE ra = st.back();
                        st.pop_back();
                        const bool swap = (fused_op == ir::Op::FCmpLt || fused_op == ir::Op::FCmpLe);
                        arch::CC::Cond cond = (fused_op == ir::Op::FCmpLt || fused_op == ir::Op::FCmpGt) ? arch::CC::kFGT : arch::CC::kFGE;
                        // vec_of may materialize a const via reg_of; take both
                        // vecs before emitting the compare.
                        arch::Vec fa = vec_of(ra);
                        arch::Vec fb = vec_of(rb);
                        bool ft_truthy = fallthrough_opt && t.target0 == fallthrough_bid[bid];
                        bool ft_falsy = fallthrough_opt && t.target1 == fallthrough_bid[bid];
                        ir::BlockId jcc_target = t.target0; // truthy edge
                        if (ft_truthy && !ft_falsy) {
                            cond = arch::CC::invert(cond);
                            jcc_target = t.target1; // falsy edge takes the jump
                        }
                        if (swap) {
                            arch::float_cmp(cc, fb, fa);
                        } else {
                            arch::float_cmp(cc, fa, fb);
                        }
                        arch::jcc(cc, cond, rlabels[jcc_target]);
                        ir::BlockId other = (jcc_target == t.target0) ? t.target1 : t.target0;
                        if (!(fallthrough_opt && other == fallthrough_bid[bid])) {
                            arch::jmp(cc, rlabels[other]);
                        }
                    } else if (fuse_cmp) {
                        // guarantees neither successor has phis when fuse_cmp is true
                        RE rb = st.back();
                        st.pop_back();
                        RE ra = st.back();
                        st.pop_back();
                        // if the truthy target is the fallthrough, invert the condition
                        // so the jcc takes the falsy edge and the truthy edge falls through
                        arch::CC::Cond cond = cmp_cond(fused_op);
                        bool ft_truthy = fallthrough_opt && t.target0 == fallthrough_bid[bid];
                        bool ft_falsy = fallthrough_opt && t.target1 == fallthrough_bid[bid];
                        ir::BlockId jcc_target = t.target0; // truthy
                        if (ft_truthy && !ft_falsy) {
                            cond = arch::CC::invert(cond);
                            jcc_target = t.target1; // falsy edge takes the jump
                        }
                        if (rb.is_const) {
                            arch::cmp_imm_jcc(cc, reg_of(ra), rb.cval, cond, rlabels[jcc_target]);
                        } else {
                            cc.cmp(reg_of(ra), reg_of(rb));
                            arch::jcc(cc, cond, rlabels[jcc_target]);
                        }
                        // Emit the residual unconditional jump only if the
                        // other edge is not the fallthrough block.
                        ir::BlockId other = (jcc_target == t.target0) ? t.target1 : t.target0;
                        if (!(fallthrough_opt && other == fallthrough_bid[bid])) {
                            arch::jmp(cc, rlabels[other]);
                        }
                    } else {
                        // Branch consumes 1 (the cond at TOS), the values below it are phi sources for BOTH successors
                        emit_phi_writes_to(t.target0, /*st_top_offset=*/1);
                        emit_phi_writes_to(t.target1, /*st_top_offset=*/1);
                        RE c = st.back();
                        st.pop_back();
                        const bool inverted = !re_raw(c);
                        if (inverted) {
                            c = escape_ex(reinterpret_cast<const void *>(jit_not), 0, 0, 0, { &c }, 1, true, ir::Ty::Bool);
                        }
                        arch::test_zero(cc, reg_of(c));
                        // same fallthrough elision as the fused path
                        // Truthy edge is taken on kNE (non-zero), invert to kE
                        // when the truthy target is the fallthrough.
                        arch::CC::Cond cond = inverted ? arch::CC::kEQ : arch::CC::kNE;
                        bool ft_truthy = fallthrough_opt && t.target0 == fallthrough_bid[bid];
                        bool ft_falsy = fallthrough_opt && t.target1 == fallthrough_bid[bid];
                        ir::BlockId jcc_target = t.target0; // truthy
                        if (ft_truthy && !ft_falsy) {
                            cond = arch::CC::invert(cond);
                            jcc_target = t.target1; // falsy edge takes the jump
                        }
                        arch::jcc(cc, cond, rlabels[jcc_target]);
                        ir::BlockId other = (jcc_target == t.target0) ? t.target1 : t.target0;
                        if (!(fallthrough_opt && other == fallthrough_bid[bid])) {
                            arch::jmp(cc, rlabels[other]);
                        }
                    }
                } else {
                    // fused return: box the result in a register, pop the frame,
                    // and store the result directly at the popped frame's slot_base
                    arch::Gp rv;
                    if (t.operands.empty() || st.empty() || st.back().ty == ir::Ty::None) {
                        rv = cc.new_gp64("r_ret_none");
                        cc.mov(rv, Imm(static_cast<int64_t>(nb.none_tag)));
                    } else {
                        rv = box_re(st.back());
                    }
                    // Register-frugal (rv + 2 vregs): extra vregs here
                    // have been observed to spill hot-loop values (see
                    // emit_inline_return's comment).
                    Label ret_empty = cc.new_label();
                    arch::Gp ff = cc.new_gp64("r_ret_ff");
                    arch::Gp sc = cc.new_gp64("r_ret_sc");
                    arch::load(cc, ff, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)));
                    arch::sub_imm(cc, ff, static_cast<int>(FrameSize));
                    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(FramesFinishOff)), ff);
                    arch::load(cc, sc, arch::ptr(vm_reg, static_cast<int>(FramesStartOff)));
                    cc.cmp(ff, sc);
                    arch::load(cc, sc, arch::ptr(ff, static_cast<int>(SlotBaseOff))); // flags live
                    arch::jcc(cc, arch::CC::kEQ, ret_empty);
                    arch::shl(cc, sc, sc, 3);
                    arch::load(cc, ff, arch::ptr(vm_reg, static_cast<int>(VMStackStartOff)));
                    arch::add2(cc, ff, sc);
                    arch::store(cc, arch::ptr(ff, 0), rv);
                    arch::add_imm(cc, ff, static_cast<int>(ValSize));
                    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), ff);
                    cc.ret();
                    cc.bind(ret_empty);
                    // frames now empty (top-level): plain push.
                    arch::load(cc, ff, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
                    arch::store(cc, arch::ptr(ff, 0), rv);
                    arch::add_imm(cc, ff, static_cast<int>(ValSize));
                    arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), ff);
                    cc.ret();
                }
            }
            // guard-failure trampoline. Reached only from the entry tag checks,
            // before any state was modified. This avoids running the whole call.
            if (spec_fail_used) {
                cc.bind(spec_fail);
                InvokeNode *fb_inv;
                (void)vm_arg0(ctx);
                arch::invoke_imm(
                    cc, &fb_inv, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(spec_fallback)), FuncSignature::build<void, void *>()
                );
                fb_inv->set_arg(0, vm_arg_scratch);
                cc.ret();
            }
            cc.bind(shutdown_requested);
            {
                InvokeNode *invoke;
                (void)vm_arg0(ctx);
                arch::invoke_imm(
                    cc, &invoke, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(jit_poll_shutdown)), FuncSignature::build<void, void *>()
                );
                invoke->set_arg(0, vm_arg_scratch);
            }
            cc.ret();
            cc.end_func();
            if (cc.finalize() != kErrorOk) {
                return { RegTierOutcome::Failed, nullptr };
            }
            size_t sz = code_holder.code_size();
            CompiledFunc fn = nullptr;
            {
                std::lock_guard<std::mutex> lk(jit_runtime_mutex());
                if (this->rt.add(&fn, &code_holder) != kErrorOk || !fn) {
                    return { RegTierOutcome::Failed, nullptr };
                }
                std::string sym = chunk.functions[chunk_idx].name.empty()
                                      ? std::string(spec ? "anon_ir_reg_spec" : "anon_ir_reg")
                                      : chunk.functions[chunk_idx].name + (spec ? "_ir_reg_spec" : "_ir_reg");
                register_gdb_jit_function(this, sym, reinterpret_cast<const void *>(fn), sz);
                perf_jitdump_register(sym, reinterpret_cast<const void *>(fn), sz);
            }
            const char *tier_tag = spec ? " [ir-reg-spec]" : " [ir-reg]";
            jit_dump_asm(
                chunk.functions[chunk_idx].name.empty() ? std::string("<anon>") + tier_tag : chunk.functions[chunk_idx].name + tier_tag,
                asm_logger.data(), static_cast<uint64_t>(reinterpret_cast<uintptr_t>(spec_fallback)),
                spec_fallback ? "spec_fallback (general tier)" : nullptr
            );

            report(chunk, chunk_idx, spec ? "OK  [ir-reg spec-param]" : "OK  [ir-reg fast path]");
            return { RegTierOutcome::Ok, fn };
        }
    }
    return { RegTierOutcome::FallThrough, nullptr };
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
