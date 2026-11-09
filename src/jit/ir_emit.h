#pragma once
// Shared lowering state for AsmJITMethodCompiler::ir_compile.
#ifndef DISABLE_JIT

#include "asmjit_jit.h"
#include "jit_arch.h"
#include "abi.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nari {
namespace jit {

using namespace abi;

struct DirectCallee {
    bool ok = false;
    uint64_t expected_raw = 0;
    uint32_t fidx = 0;
    uint32_t locals = 0;
    const void *meta = nullptr;
    const void *captures_raw = nullptr;
    const void *cap0 = nullptr;
    const void *cap1 = nullptr;
    const void *cap2 = nullptr;
};

// NARI_DCS_CENSUS=1: per-emitted-site tally of why a Call could not become a
// guarded direct call. Static (site) counts, not dynamic frequency.
struct DcsCensus {
    uint64_t sites = 0, argc_gt4 = 0, callee_op[64] = {}, cap_not_ok = 0, cap_oob = 0, not_fn = 0, no_meta = 0, native = 0,
             rest_or_cap0 = 0, arity = 0, fill_gt8 = 0, ok = 0;
    ~DcsCensus() {
        if (sites == 0) {
            return;
        }
        fprintf(stderr, "\n=== direct-call SITE census (%llu Call sites) ===\n", static_cast<unsigned long long>(sites));
        auto p = [&](const char *n, uint64_t v) {
            if (v) {
                fprintf(
                    stderr, "%10llu  %5.1f%%  %s\n", static_cast<unsigned long long>(v),
                    100.0 * static_cast<double>(v) / static_cast<double>(sites), n
                );
            }
        };
        p("argc > 4", argc_gt4);
        p("callee not LoadGlobal/LoadCapture", callee_op[0]);
        p("captures not resolvable", cap_not_ok);
        p("capture idx out of range", cap_oob);
        p("callee value not a function", not_fn);
        p("no jit_meta", no_meta);
        p("native_kind", native);
        p("rest param / Capture0", rest_or_cap0);
        p("arity mismatch", arity);
        p("locals-argc > 8", fill_gt8);
        p("DIRECT OK", ok);
    }
};

// Moved verbatim from AsmJITMethodCompiler::ir_compile (asmjit_jit.cpp).
struct FinishCache {
    arch::Compiler &cc;
    arch::Gp vm_reg;
    arch::Gp reg;
    bool have_reg = false;
    // Block-scoped cache of this frame's slot-array base
    // (stack_start + slot_base_bytes). It is invariant within a block: the
    // entry reserve guarantees generated pushes never reallocate the value
    // stack, so only a helper call or a block boundary can move it, which
    // are exactly the events that already invalidate `reg`.
    arch::Gp base_reg;
    arch::Gp base_bytes;
    bool have_base = false;
    bool have_base_bytes = false;

    explicit FinishCache(arch::Compiler &cc_, arch::Gp vm_) : cc(cc_), vm_reg(vm_) {
        reg = cc.new_gp64("fc_endp");
        base_reg = cc.new_gp64("fc_slotbase");
    }
    // `off` must hold slot_base*8 for the current frame (hoisted at entry).
    void bind_slot_base_bytes(arch::Gp off) {
        base_bytes = off;
        have_base_bytes = true;
        have_base = false;
    }
    arch::Gp slot_base() {
        if (!have_base) {
            arch::load(cc, base_reg, arch::ptr(vm_reg, static_cast<int>(VMStackStartOff)));
            arch::add2(cc, base_reg, base_bytes);
            have_base = true;
        }
        return base_reg;
    }
    // Return the vreg holding the current in-memory finish value,
    // loading from memory only if the cache is cold.
    arch::Gp ensure_reg() {
        if (!have_reg) {
            arch::load(cc, reg, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)));
            have_reg = true;
        }
        return reg;
    }
    arch::Gp get() {
        materialize();
        return ensure_reg();
    }
    // Store `src` to memory AND update the cached reg so subsequent
    // gets are hot. Every set() is immediate -- no deferral.
    void set(arch::Gp src) {
        arch::store(cc, arch::ptr(vm_reg, static_cast<int>(VMStackFinishOff)), src);
        if (src.id() != reg.id()) {
            arch::mov_reg(cc, reg, src);
        }
        have_reg = true;
    }
    // Drop knowledge that `reg` reflects memory (helper call mutated
    // memory, or we crossed a block boundary).
    void invalidate() {
        // Pending pushes are real stack values: they must reach memory before
        // anything stops trusting the cached pointer (helper call, slow path,
        // block boundary). Dropping them here desynchronizes the value stack.
        materialize();
        have_reg = false;
        have_base = false;
    }
    void reload() {
        invalidate();
        (void)get();
    }
    void merge_slow_path() {
        invalidate();
    }

    // Depth of the deferred-push queue below.
    enum {
        kShadowDepth = 4
    }; // local struct: no static data members

    // -- deferred pushes --
    // A value whose consumer runs in the same block need never touch memory.
    // `pend` holds values that are logically on the value stack above the
    // materialized finish pointer but have not been stored; `reg`/memory still
    // describe the stack strictly below them. pend[0] is the lowest.
    //
    // Correct-by-default: get() materializes first, so every pre-existing user
    // of the endp register (helper calls, slow paths, block boundaries, and all
    // ops not taught about pend) observes a fully materialized stack.
    // A pending value is either in a vreg or, for a compile-time constant, just
    // its raw nan-boxed bit pattern -- in which case nothing at all is emitted
    // for the push, and a consumer that accepts an immediate never needs a
    // register for it either.
    struct Pend {
        arch::Gp reg;
        int64_t imm = 0;
        bool is_imm = false;
    };
    Pend pend[kShadowDepth];
    int npend = 0;

    void store_pend(arch::Gp e, int off, const Pend &p) {
        if (!p.is_imm) {
            arch::store(cc, arch::ptr(e, off), p.reg);
            return;
        }
        // A nan-boxed constant carries its tag in bits 48-63, so it never fits
        // the imm32 form of a memory store; go through a scratch, which is
        // exactly what a non-deferred push would have emitted anyway.
        arch::Gp t = cc.new_gp64("fc_pimm");
        cc.mov(t, asmjit::Imm(p.imm));
        arch::store(cc, arch::ptr(e, off), t);
    }
    void materialize() {
        if (npend == 0) {
            return;
        }
        const int n = npend;
        npend = 0; // before get(), so it does not recurse back into here
        arch::Gp e = get();
        for (int i = 0; i < n; ++i) {
            store_pend(e, i * static_cast<int>(ValSize), pend[i]);
        }
        arch::add_imm(cc, e, n * static_cast<int>(ValSize));
        set(e);
    }
    void push_pend(const Pend &p) {
        if (npend == kShadowDepth) {
            materialize();
        }
        pend[npend++] = p;
    }
    void push_deferred(arch::Gp v) {
        Pend p;
        p.reg = v;
        push_pend(p);
    }
    void push_deferred_imm(int64_t raw) {
        Pend p;
        p.imm = raw;
        p.is_imm = true;
        push_pend(p);
    }
    // depth 0 == top of stack; null when nothing is pending at that depth
    const Pend *peek(int d) const {
        if (d < 0 || d >= npend) {
            return nullptr;
        }
        return &pend[npend - 1 - d];
    }
    void drop_pend(int n) {
        npend -= n;
    }
    int pend_snapshot() const {
        return npend;
    }
    void pend_restore(int n) {
        npend = n;
    }
    // Materialize every pending push except the top `k`, and guarantee the
    // cached endp register is live. An op that consumes the top k values can
    // then read them from registers while both arms of its fast/slow branch
    // still agree about the memory below them.
    arch::Gp materialize_keep(int k) {
        if (npend > k) {
            const int n = npend - k;
            Pend keep[kShadowDepth];
            for (int i = 0; i < k; ++i) {
                keep[i] = pend[n + i];
            }
            npend = 0; // before get(), so it does not recurse into materialize()
            arch::Gp e = get();
            for (int i = 0; i < n; ++i) {
                store_pend(e, i * static_cast<int>(ValSize), pend[i]);
            }
            arch::add_imm(cc, e, n * static_cast<int>(ValSize));
            set(e);
            for (int i = 0; i < k; ++i) {
                pend[i] = keep[i];
            }
            npend = k;
        }
        return ensure_reg(); // get() would materialize the survivors
    }
};

// Moved verbatim from AsmJITMethodCompiler::ir_compile (asmjit_jit.cpp).
// SlotRegCache: IR functions can't alias their own slots (no upvalues), so
// a slot's value is only written by this function's own Store*Slot/CopySlot ops
struct SlotRegCache {
    arch::Compiler &cc;
    struct Entry {
        arch::Gp reg;
        bool valid = false;
    };
    std::vector<Entry> entries;
    explicit SlotRegCache(arch::Compiler &cc_, uint32_t num_slots) : cc(cc_), entries(num_slots) {
    }
    // If slot is cached, return true and set out_reg to a vreg holding the raw slot value, else return false.
    bool try_get(uint32_t s, arch::Gp &out_reg) {
        if (s >= entries.size() || !entries[s].valid) {
            return false;
        }
        out_reg = entries[s].reg;
        return true;
    }
    // Update the cache for slot s to now-hold-value `reg`.
    void set(uint32_t s, arch::Gp reg) {
        if (s >= entries.size()) {
            return;
        }
        entries[s].reg = reg;
        entries[s].valid = true;
    }
    void invalidate(uint32_t s) {
        if (s < entries.size()) {
            entries[s].valid = false;
        }
    }
    void invalidate_all() {
        for (auto &e : entries) {
            e.valid = false;
        }
    }
};

// Register-tier outcome: the tier either declined (fall through to the stack
// tier), failed (abort the whole compile) or produced an entry point.
enum class RegTierOutcome {
    FallThrough,
    Failed,
    Ok
};

struct RegTierResult {
    RegTierOutcome outcome;
    AsmJITMethodCompiler::CompiledFunc entry;
};

// Borrowed lowering state. All references bind to AsmJITMethodCompiler::ir_compile
// locals and stay valid for the whole compile; nothing here owns memory.
struct IrEmitContext {
    const bytecode::Chunk &chunk;
    uint32_t chunk_idx;
    bool spec;
    bool *spec_candidate;
    AsmJITMethodCompiler::CompiledFunc spec_fallback;
    ir::Func &irFuncs;
    std::vector<ir::Ty> &slot_types;
    ir::IntArraySlots &int_arr_slots;
    asmjit::CodeHolder &code_holder;
    asmjit::StringLogger &asm_logger;
    arch::Compiler &cc;
    asmjit::FuncNode *func_node;
    const bool &pin_vm_callee_saved;
    arch::Gp &vm_reg;
    arch::Gp &vm_arg_scratch;
    asmjit::Label &shutdown_requested;
    std::vector<asmjit::Label> &blabels;
    bool &ok;
    ir::Op &unhandled_op;
    ir::BlockId &lowering_block;
    FinishCache &fc;
    SlotRegCache &slot_cache;
    arch::Gp &const_scratch;
    arch::Gp &cmp_scratch;
};

// Asm dump helper (moved from asmjit_jit.cpp; shared by all lowering TUs).
void jit_dump_asm(const std::string &func_name, const char *asm_text, uint64_t extra_addr = 0, const char *extra_name = nullptr);

// Pure opcode predicate (no capture in its original lambda form).
bool is_cmp_op(ir::Op op);
// Register allocator helper: needs no compiler instance, only shared context.
arch::Gp vm_arg0(IrEmitContext &ctx);
arch::Gp emit_ir_slot_addr(IrEmitContext &ctx, uint32_t slot, const char *name);
arch::Mem emit_ir_slot_mem(IrEmitContext &ctx, uint32_t slot);

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
