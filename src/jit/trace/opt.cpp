// Trace code cache and pre-passes.

#ifndef DISABLE_JIT

#include "trace/compiler.h"

using namespace asmjit;

namespace nari {
namespace jit {

TraceJITCompilerAsmJit::TraceJITCompilerAsmJit() {
}
TraceJITCompilerAsmJit::~TraceJITCompilerAsmJit() {
}

void TraceJITCompilerAsmJit::reset() {
    unregister_gdb_jit_functions(this);
    cache.clear();
    rt.~JitRuntime();
    new (&rt) asmjit::JitRuntime();
}

const CompiledTrace *TraceJITCompilerAsmJit::find(uint32_t func_idx, size_t anchor_pc) const {
    auto it = this->cache.find(make_key(func_idx, anchor_pc));
    if (it == this->cache.end()) {
        return nullptr;
    }
    return &it->second.trace;
}

std::vector<AsmLiveVar> asm_collect_live_vars(const std::vector<TraceStep> &steps) {
    std::unordered_map<uint16_t, TraceType> seen;
    for (const auto &s : steps) {
        if ((s.kind == TraceStep::Kind::LoadIntVar || s.kind == TraceStep::Kind::StoreIntVar) && seen.find(s.slot) == seen.end()) {
            seen[s.slot] = TraceType::Int;
        } else if (
            (s.kind == TraceStep::Kind::LoadFloatVar || s.kind == TraceStep::Kind::StoreFloatVar) && seen.find(s.slot) == seen.end()
        ) {
            seen[s.slot] = TraceType::Float;
        } else if (
            (s.kind == TraceStep::Kind::LoadObjVar || s.kind == TraceStep::Kind::ObjAddConstInPlace ||
             s.kind == TraceStep::Kind::ObjAddPropInPlace) &&
            seen.find(s.slot) == seen.end()
        ) {
            seen[s.slot] = TraceType::Obj;
        } else if (s.kind == TraceStep::Kind::LoadArrayVar && seen.find(s.slot) == seen.end()) {
            seen[s.slot] = TraceType::Array;
        } else if (
            (s.kind == TraceStep::Kind::ClosureInc || s.kind == TraceStep::Kind::ClosureAddConst) && seen.find(s.closure_slot) == seen.end()
        ) {
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

std::vector<TraceStep> optimize_object_update_traces(const std::vector<TraceStep> &steps) {
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

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
