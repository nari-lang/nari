#ifndef DISABLE_JIT
#include "asmjit_jit.h"
#include "core_types.h"
#include "ir.h"
#include "ir_build.h"
#include "ir_opt.h"
#include "jit_arch.h"
#include "jit_helpers.h"
#include "jit_layout.h"

#include "util.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace asmjit;
using namespace nari::bytecode;

// Fast sin/cos defined in trace_jit_asmjit.cpp
extern double nari_fast_sin(double) noexcept;
extern double nari_fast_cos(double) noexcept;

static bool jit_dump_asm_enabled() {
    static const bool enabled = getenv("NARI_JIT_DUMP_ASM") != nullptr;
    return enabled;
}

static const std::unordered_map<uint64_t, const char *> &jit_helper_symbols() {
    static const std::unordered_map<uint64_t, const char *> table = [] {
        std::unordered_map<uint64_t, const char *> t;
        auto add = [&](const void *p, const char *name) {
            t.emplace(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)), name);
        };
        
        #define NARI_JIT_SYM(fn) add(reinterpret_cast<const void *>(&fn), #fn)

        NARI_JIT_SYM(jit_load_const);
        NARI_JIT_SYM(jit_push_int);
        NARI_JIT_SYM(jit_push_float);
        NARI_JIT_SYM(jit_push_bool);
        NARI_JIT_SYM(jit_load_var);
        NARI_JIT_SYM(jit_store_var);
        NARI_JIT_SYM(jit_load_global);
        NARI_JIT_SYM(jit_store_global);
        NARI_JIT_SYM(jit_pop);
        NARI_JIT_SYM(jit_dup);
        NARI_JIT_SYM(jit_load_none);
        NARI_JIT_SYM(jit_load_true);
        NARI_JIT_SYM(jit_load_false);
        NARI_JIT_SYM(jit_load_zero);
        NARI_JIT_SYM(jit_load_one);
        NARI_JIT_SYM(jit_add);
        NARI_JIT_SYM(jit_sub);
        NARI_JIT_SYM(jit_mul);
        NARI_JIT_SYM(jit_div);
        NARI_JIT_SYM(jit_mod);
        NARI_JIT_SYM(jit_neg);
        NARI_JIT_SYM(jit_str_concat);
        NARI_JIT_SYM(jit_str_concat_inplace);
        NARI_JIT_SYM(jit_format_value);
        NARI_JIT_SYM(jit_bit_and);
        NARI_JIT_SYM(jit_bit_or);
        NARI_JIT_SYM(jit_bit_xor);
        NARI_JIT_SYM(jit_bit_not);
        NARI_JIT_SYM(jit_lshift);
        NARI_JIT_SYM(jit_rshift);
        NARI_JIT_SYM(jit_not);
        NARI_JIT_SYM(jit_eq);
        NARI_JIT_SYM(jit_ne);
        NARI_JIT_SYM(jit_lt);
        NARI_JIT_SYM(jit_le);
        NARI_JIT_SYM(jit_gt);
        NARI_JIT_SYM(jit_ge);
        NARI_JIT_SYM(jit_check_truthy);
        NARI_JIT_SYM(jit_check_none);
        NARI_JIT_SYM(jit_call);
        NARI_JIT_SYM(jit_call_value);
        NARI_JIT_SYM(jit_return);
        NARI_JIT_SYM(jit_make_array);
        NARI_JIT_SYM(jit_iter_array);
        NARI_JIT_SYM(jit_make_object);
        NARI_JIT_SYM(jit_make_object_site);
        NARI_JIT_SYM(jit_reserve);
        NARI_JIT_SYM(jit_get_index);
        NARI_JIT_SYM(jit_set_index);
        NARI_JIT_SYM(jit_get_property);
        NARI_JIT_SYM(jit_set_property);
        NARI_JIT_SYM(jit_load_capture);
        NARI_JIT_SYM(jit_store_capture);
        
        // NOTE: jit_make_closure/jit_spawn/jit_pow/jit_throw/jit_new_instance/jit_load_this
        //  are declared in jit_helpers.h but have no definition since the JIT never emits calls to them
        NARI_JIT_SYM(jit_call_method);
        NARI_JIT_SYM(jit_method_length);
        NARI_JIT_SYM(jit_method_char_code_at);
        NARI_JIT_SYM(jit_method_starts_with);
        NARI_JIT_SYM(jit_method_substr);
        NARI_JIT_SYM(jit_check_type);
        NARI_JIT_SYM(jit_slot_store_raw);
        NARI_JIT_SYM(jit_slot_copy);
#undef NARI_JIT_SYM
        add(reinterpret_cast<const void *>(&nari_fast_sin), "nari_fast_sin");
        add(reinterpret_cast<const void *>(&nari_fast_cos), "nari_fast_cos");
        return t;
    }();
    return table;
}

// Rewrite `call <decimal-address>` lines to `call <helper-name>` when the address matches a known helper
static std::string jit_symbolize_calls(const char *asm_text, uint64_t extra_addr = 0, const char *extra_name = nullptr) {
    const auto &syms = jit_helper_symbols();
    std::string out;
    out.reserve(std::strlen(asm_text) + 128);
    const char *p = asm_text;
    while (*p) {
        const char *nl = std::strchr(p, '\n');
        size_t len = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
        std::string line(p, len);
        size_t i = line.find_first_not_of(" \t");
        if (i != std::string::npos && line.compare(i, 5, "call ") == 0) {
            size_t num = i + 5;
            size_t j = num;
            while (j < line.size() && line[j] >= '0' && line[j] <= '9') {
                j++;
            }
            if (j > num) {
                uint64_t addr = strtoull(line.c_str() + num, nullptr, 10);
                auto it = syms.find(addr);
                const char *name = (it != syms.end()) ? it->second
                                   : (extra_name && addr == extra_addr)
                                       ? extra_name
                                       : nullptr;
                if (name) {
                    size_t semi = line.find(';');
                    std::string instr =
                        line.substr(0, i) + "call " + name;
                    if (semi != std::string::npos) {
                        if (instr.size() + 1 < semi) {
                            instr.append(semi - instr.size(), ' ');
                        } else {
                            instr.push_back(' ');
                        }
                        instr += line.substr(semi);
                    }
                    line.swap(instr);
                }
            }
        }
        out += line;
        if (nl) {
            out.push_back('\n');
            p = nl + 1;
        } else {
            break;
        }
    }
    return out;
}

// writes a one-time legend mapping struct offsets to field names so a reader
// can decode memory operands like `[reg+160]` in the dump.
namespace nari {
namespace jit {
static void jit_write_layout_legend(FILE *f);
}
} // namespace nari

static void jit_dump_asm(const std::string &func_name, const char *asm_text,
                         uint64_t extra_addr = 0,
                         const char *extra_name = nullptr) {
    const char *env = getenv("NARI_JIT_DUMP_ASM");
    if (!env) {
        return;
    }
    const char *path = (env[0] != '\0') ? env : "output.asm";
    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    nari::jit::jit_write_layout_legend(f);
    fprintf(f, "\n; -- %s --\n", func_name.c_str());
    std::string symbolized =
        jit_symbolize_calls(asm_text, extra_addr, extra_name);
    fputs(symbolized.c_str(), f);
    fclose(f);
}

namespace nari {
namespace jit {

// Value / VM layout constants
static const int64_t ValSize = static_cast<int64_t>(sizeof(Value));
// NaN-boxing: tag is upper 2 bytes at offset 6 within the 8-byte Value.
// Raw NaN-box tag values (upper 16 bits of uint64_t _raw):
static constexpr int64_t tagWordOff = 6; // offset of NaN-box tag word
static constexpr int64_t tagNone = (int64_t)0xFFFF;
static constexpr int64_t tagInt = (int64_t)0xFFFC;
static constexpr int64_t tagFloat = (int64_t)0x0000; // floats: upper16 < tagHeap
static constexpr int64_t tagBool = (int64_t)0xFFFE;
// kTagTrivialMax: values with upper16 < 0xFFFB are NOT heap (trivial)
// detect heap by: upper16 == tagHeap (0xFFFB)
static constexpr int64_t tagHeap = (int64_t)0xFFFB;                 // any heap-allocated type
static constexpr int64_t tagArray = (int64_t)ValueTag::Array;       // heap type_tag byte for Array
static constexpr int64_t tagFunction = (int64_t)ValueTag::Function; // heap type_tag byte for Function
// HeapTypeTagOff is computed via field_offset below

// NaN-box encoding constants (for boxing results)
static constexpr uint64_t nbIntTag = 0xFFFC000000000000ULL;
static constexpr uint64_t nbBoolTag = 0xFFFE000000000000ULL;
static constexpr uint64_t nbNone = 0xFFFF000000000000ULL;
static constexpr uint64_t nbPtrMask = 0x0000FFFFFFFFFFFFULL;

// FunctionData and ObjectObj field offsets (for generic CALL inline path)
// `field_offset` is the safe alternative to `offsetof` for non-standard-layout types
static const int64_t HeapTypeTagOff = field_offset(&HeapHeader::type_tag);
static const int64_t ObjShapeVersionOff = field_offset(&ObjectObj::shape_version);
static const int64_t ObjShapeOff = field_offset(&ObjectObj::shape);
static const int64_t ObjFieldsOff = field_offset(&ObjectObj::fields);
// std::vector layout: { _M_start, _M_finish, _M_end_of_storage } back-to-back pointers
static const int64_t ObjFieldsStartOff = ObjFieldsOff + 0;
static const int64_t ObjFieldsFinishOff = ObjFieldsOff + 8;
static const int64_t ObjFrozenOff = field_offset(&ObjectObj::frozen);
static const int64_t ObjDictModeOff = field_offset(&ObjectObj::dict_mode);
static const int64_t FDCapturesOff = field_offset(&FunctionData::captures);
static const int64_t FDJitFuncIdxOff = field_offset(&FunctionData::jit_func_idx);
static const int64_t FDLocalsCountOff = field_offset(&FunctionData::jit_locals_count);
static const int64_t FDJitMetaOff = field_offset(&FunctionData::jit_meta);
static const int64_t FDInlineKindOff = field_offset(&FunctionData::jit_inline_kind);
static const int64_t FDNativeKindOff = field_offset(&FunctionData::jit_native_kind);
static const int64_t FDInlineImmOff = field_offset(&FunctionData::jit_inline_imm);
static const int64_t FDCapture0RawOff = field_offset(&FunctionData::jit_capture0_raw);
static const int64_t StringStdOff = field_offset(&StringObj::s);
// libc++ std::string layout in long-string mode: {cap_word, size, data}.
static const int64_t StringCapWordOff = StringStdOff;
static const int64_t StringSizeOff = StringStdOff + 8;
static const int64_t StringDataOff = StringStdOff + 16;
static const int64_t FMCodeDataOff = field_offset(&FunctionMeta::code);
static const int64_t VMCapturesRawOff = field_offset(&VM::jit_captures_raw);

// VM::stack vector internal pointers (stack is the first VM field)
static const int64_t VMStackStartOff = field_offset(&VM::stack);
static const int64_t VMStackFinishOff = VMStackStartOff + 8;
static const int64_t VMStackCapacityOff = VMStackStartOff + 16;
static const int64_t VMGlobalCacheStartOff = field_offset(&VM::global_cache);
// static const int64_t VMGlobalCacheFinishOff = VMGlobalCacheStartOff + 8;
static const int64_t VMGlobalCacheValidStartOff = field_offset(&VM::global_cache_valid);
static const int64_t VMGlobalCacheValidFinishOff = VMGlobalCacheValidStartOff + 8;

// std::vector<Value> layout inside ArrayObj.
static const int64_t ArrayVecStartOff = field_offset(&ArrayObj::v);
static const int64_t ArrayVecFinishOff = ArrayVecStartOff + 8;
static const int64_t ArrayVecCapacityOff = ArrayVecStartOff + 16;

// VM::frames vector internal pointers
static const int64_t FramesStartOff = field_offset(&VM::frames);
static const int64_t FramesFinishOff = field_offset(&VM::frames) + static_cast<int64_t>(sizeof(void *));
static const int64_t VMFramesCapacityOff = FramesFinishOff + 8;

// CallFrame field offsets
static const int64_t FrameSize = static_cast<int64_t>(sizeof(CallFrame));
static const int64_t FrameFunctionOff = field_offset(&CallFrame::function);
static const int64_t FrameIpOff = field_offset(&CallFrame::ip);
static const int64_t SlotBaseOff = field_offset(&CallFrame::slot_base);
static const int64_t FrameCapturesOff = field_offset(&CallFrame::captures);
static const int64_t UpvalOff = field_offset(&CallFrame::open_upvalues);
static const int64_t UpvalSizeOff = UpvalOff;
static const int64_t FrameOpenUpvalOff = UpvalOff;

static void jit_write_layout_legend(FILE *f) {
    static bool written = false;
    if (written) {
        return;
    }
    written = true;
    fprintf(f,
            "; ============================================================\n"
            "; NARI JIT asm dump. `call` targets are symbolized to helper\n"
            "; names where known. Memory operands `[base+off]` into the VM\n"
            "; pointer (first arg) or a CallFrame use these offsets:\n"
            ";\n"
            ";   VM.stack.begin      +%-4lld    VM.stack.end        +%-4lld\n"
            ";   VM.stack.cap        +%-4lld    VM.jit_captures_raw +%-4lld\n"
            ";   VM.global_cache     +%-4lld    VM.gcache_valid     +%-4lld\n"
            ";   VM.frames.begin     +%-4lld    VM.frames.end       +%-4lld\n"
            ";   VM.frames.cap       +%-4lld\n"
            ";   sizeof(Value)=%lld  sizeof(CallFrame)=%lld\n"
            ";   CallFrame.function  +%-4lld    CallFrame.ip        +%-4lld\n"
            ";   CallFrame.slot_base +%-4lld    CallFrame.captures  +%-4lld\n"
            ";   CallFrame.open_upvalues +%lld\n"
            "; ============================================================\n",
            (long long)VMStackStartOff, (long long)VMStackFinishOff,
            (long long)VMStackCapacityOff, (long long)VMCapturesRawOff,
            (long long)VMGlobalCacheStartOff,
            (long long)VMGlobalCacheValidStartOff, (long long)FramesStartOff,
            (long long)FramesFinishOff, (long long)VMFramesCapacityOff,
            (long long)ValSize, (long long)FrameSize, (long long)FrameFunctionOff,
            (long long)FrameIpOff, (long long)SlotBaseOff,
            (long long)FrameCapturesOff, (long long)FrameOpenUpvalOff);
}

// Emits the jit_return(vm) equivalent. Must be immediately followed by cc.ret().
// This function assumes the return value has already been pushed on the VM stack.
// Deliberately uses less registers to avoid register pressure at return sites.
static void emit_inline_return(arch::Compiler &cc, arch::Gp vm_reg) {
    Label done = cc.new_label();
    arch::Gp ff = cc.new_gp64("iret_ff");
    arch::Gp sc = cc.new_gp64("iret_sc");
    arch::Gp res = cc.new_gp64("iret_res");
    arch::load(cc, ff, arch::ptr(vm_reg, (int)FramesFinishOff));
    arch::sub_imm(cc, ff, (int)FrameSize);
    arch::store(cc, arch::ptr(vm_reg, (int)FramesFinishOff), ff);
    arch::load(cc, sc, arch::ptr(vm_reg, (int)FramesStartOff));
    cc.cmp(ff, sc);
    arch::load(cc, sc, arch::ptr(ff, (int)SlotBaseOff)); // flags preserved
    arch::jcc(cc, arch::CC::kEQ, done);                  // frames now empty: stack unchanged
    // result is at [finish - 8]; sc holds the popped frame's slot_base
    arch::load(cc, res, arch::ptr(vm_reg, (int)VMStackFinishOff));
    arch::load(cc, res, arch::ptr(res, (int)(-ValSize)));
    arch::shl(cc, sc, sc, 3);
    arch::Gp nf = ff; // frame ptr dead; reuse for the new finish
    arch::load(cc, nf, arch::ptr(vm_reg, (int)VMStackStartOff));
    arch::add2(cc, nf, sc);
    arch::store(cc, arch::ptr(nf, 0), res);
    arch::add_imm(cc, nf, (int)ValSize);
    arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), nf);
    cc.bind(done);
}

static const int64_t SharedPtrSize = static_cast<int64_t>(sizeof(std::shared_ptr<Value>));

// signed division by constant: magic multiply
// replaces idiv with imul+shift for x % d (d > 1).
// algorithm adapted from Hacker's Delight, 2nd ed., Chapter 10.
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

bool AsmJITMethodCompiler::is_compiled(uint32_t chunk_idx) const {
    return chunk_idx < compiled_fn_vec.size() && compiled_fn_vec[chunk_idx] != nullptr;
}

using CompiledFunc = AsmJITMethodCompiler::CompiledFunc;

CompiledFunc AsmJITMethodCompiler::get_compiled(uint32_t chunk_idx) const {
    return chunk_idx < compiled_fn_vec.size() ? compiled_fn_vec[chunk_idx] : nullptr;
}
CompiledFunc AsmJITMethodCompiler::get_compiled_fast(uint32_t idx) const {
    return idx < compiled_fn_vec.size() ? compiled_fn_vec[idx] : nullptr;
}

// reset all per-chunk state when the bound chunk changes.
// when a new chunk arrives, free all generated code via the JitRuntime
// drop all per-chunk tables, and resize fresh for the incoming chunk.
void AsmJITMethodCompiler::reset_for_chunk(const nari::bytecode::Chunk &chunk) {
    destroy_at(&rt);
    construct_at(&rt);

    // GDB-JIT registrations refer to the just-freed code addresses
    unregister_all_gdb_jit_functions();

    compiled_fn_vec.clear();
    not_compilable.clear();

    // size both vectors to chunk.functions.size() exactly once per chunk.
    compiled_fn_vec.resize(chunk.functions.size(), nullptr);

    fn_vec_base = compiled_fn_vec.data();

    bound_chunk_ = &chunk;

    // One-shot bytecode scan: find write-once globals initialized to a literal
    // and remember the value's static type. Cheap (linear in total bytecode
    // size, once per chunk) and unlocks cascade-typing in infer_types() for
    // expressions that depend on module-scope constants.
    global_const_types_ = ir::analyze_const_globals(chunk);

    // Cache the string-pool index of "push" and "length" once per chunk. Used
    // by analyze_int_array_slots() to whitelist `arr.push(<int>)` as a mutation
    // that keeps a slot int-array-pure, and `arr.length()` as a pure read that
    // does NOT escape the array (returns an int count, stores nothing).
    push_method_name_idx_ = UINT32_MAX;
    length_method_name_idx_ = UINT32_MAX;
    for (uint32_t i = 0; i < chunk.strings.size(); i++) {
        if (chunk.strings[i] == "push") {
            push_method_name_idx_ = i;
        } else if (chunk.strings[i] == "length") {
            length_method_name_idx_ = i;
        }
    }

    // One-shot: is the "to_string" builtin unshadowable in this chunk? If so,
    // fuse_tostring_concat() may rewrite `s @ to_string(x)` -> `s @ x`. UINT32_MAX
    // (the common no-op case) when to_string is absent, user-defined, or ever
    // rebound via OP_STORE_GLOBAL.
    tostring_fuse_name_idx_ = ir::analyze_frozen_builtin(chunk, "to_string");
}

// verify the vectors whose storage addresses get baked as immediates into generated code have not reallocated
void AsmJITMethodCompiler::assert_tables_stable() const {
    assert(compiled_fn_vec.data() == fn_vec_base && "compiled_fn_vec reallocated: baked code pointers now dangle");
}

AsmJITMethodCompiler::CompiledFunc
AsmJITMethodCompiler::compile_chunk(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx) {
    if (bound_chunk_ != &chunk) {
        reset_for_chunk(chunk);
    }
    // chunk_idx must be in range; reset_for_chunk sized the vector to chunk.functions.size().
    if (chunk_idx >= compiled_fn_vec.size()) {
        return nullptr; // out-of-range function index for the bound chunk
    }
    if (compiled_fn_vec[chunk_idx]) {
        return compiled_fn_vec[chunk_idx];
    }
    // already determined this function cannot be JIT-compiled.
    if (not_compilable.count(chunk_idx)) {
        return nullptr;
    }

    auto fn = ir_compile(chunk, chunk_idx);
    // for parameterized functions, attempt a speculative register-tier
    // recompile with params typed Int48 (entry tag guards, a failed
    // guard dispatches to the general-path version just compiled)
    if (fn && chunk.functions[chunk_idx].param_count > 0 &&
        chunk.functions[chunk_idx].param_count <= 8) {
        if (CompiledFunc spec = ir_compile(chunk, chunk_idx, fn)) {
            fn = spec;
        }
    }
    if (fn) {
        compiled_fn_vec[chunk_idx] = fn;
    } else {
        not_compilable.insert(chunk_idx);
    }
    return fn;
}

// Optimizing-IR tier
//
// Build SSA IR, then lower it to AsmJIT
AsmJITMethodCompiler::CompiledFunc
AsmJITMethodCompiler::ir_compile(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, CompiledFunc spec_fallback) {
    const bool spec = spec_fallback != nullptr;
    static const bool kJitReport = getenv("NARI_JIT_REPORT") != nullptr;
    auto report = [&](const char *what) {
        if (kJitReport) {
            fprintf(stderr, "[JIT] %-30s %s\n",
                    chunk.functions[chunk_idx].name.empty()
                        ? "<anon>"
                        : chunk.functions[chunk_idx].name.c_str(),
                    what);
        }
    };
    ir::Func irFuncs;
    if (!ir::build(chunk, chunk_idx, irFuncs)) {
        report("BAIL build (non-P1 op or stack-spans-block)");
        return nullptr;
    }
    ir::optimize(irFuncs);
    std::vector<ir::Ty> slot_types;
    // Initial type inference (no int-array info yet), so MakeArray
    // initializers and CallMethod push args are typed before
    // analyze_int_array_slots() inspects them.
    ir::infer_types(irFuncs, slot_types, &global_const_types_, nullptr, spec);
    // identify slots that statically hold an int-only array.
    // Requires value types from the previous infer_types() to check that init values
    // and push args are Int48.
    ir::IntArraySlots int_arr_slots = ir::int_array_candidates(irFuncs);
    ir::infer_types(irFuncs, slot_types, &global_const_types_, &int_arr_slots, spec);
    for (int it = 0; it < 8; it++) {
        ir::IntArraySlots next =
            ir::analyze_int_array_slots(irFuncs, push_method_name_idx_, length_method_name_idx_);
        if (next.size() == int_arr_slots.size()) {
            int_arr_slots = std::move(next);
            break;
        }
        int_arr_slots = std::move(next);
        ir::infer_types(irFuncs, slot_types, &global_const_types_, &int_arr_slots, spec);
    }
    ir::specialize_types(irFuncs);
    ir::fold_redundant_not(irFuncs);
    ir::fold_branch_not(irFuncs);
    // fuse `s @ to_string(x)` -> `s @ x`, eliding the throwaway StringObj that builtin_toString allocates
    ir::fuse_tostring_concat(irFuncs, tostring_fuse_name_idx_);
    // Mark single-use left-associative StrConcat chains for in-place append
    // lowering (avoids O(n^2) prefix copies + per-link StringObj allocs). Runs
    // last, on the final IR, so global use-counts are stable.
    ir::mark_inplace_concat(irFuncs);
    if (getenv("NARI_IR_DUMP")) {
        fprintf(
            stderr,
            "=== IR for '%s' (func %u) ===\n%s",
            chunk.functions[chunk_idx].name.c_str(),
            chunk_idx,
            ir::dump(irFuncs).c_str());
    }
    // Report only: quantify cross-block redundant expressions that a
    // future global-CSE/GVN pass could eliminate. Pure analysis; does not touch
    // irFuncs or codegen. Runs on the FINAL typed IR so op costs are accurate.
    static const bool kGvnReport = getenv("NARI_IR_GVN_REPORT") != nullptr;
    if (kGvnReport) {
        ir::gvn_report(irFuncs, chunk.functions[chunk_idx].name.c_str(), chunk_idx);
    }

    CodeHolder code_holder;
    code_holder.init(this->rt.environment(), this->rt.cpu_features());
    StringLogger asm_logger;
    if (jit_dump_asm_enabled()) {
        asm_logger.add_flags(FormatFlags::kMachineCode);
        code_holder.set_logger(&asm_logger);
    }
    arch::Compiler cc(&code_holder);
    if (getenv("NARI_JIT_VALIDATE") != nullptr) {
        cc.add_diagnostic_options(asmjit::DiagnosticOptions::kValidateAssembler |
                                  asmjit::DiagnosticOptions::kValidateIntermediate);
    }
    FuncNode *func_node = cc.add_func(FuncSignature::build<void, void *>());
    arch::Gp vm_reg = cc.new_gp64("vm");
    func_node->set_arg(0, vm_reg);

    // block-scoped load-only cache for the VM stack's _M_finish field
    struct FinishCache {
        arch::Compiler &cc;
        arch::Gp vm_reg;
        arch::Gp reg;
        bool have_reg = false;

        explicit FinishCache(arch::Compiler &cc_, arch::Gp vm_)
            : cc(cc_), vm_reg(vm_) {
            reg = cc.new_gp64("fc_endp");
        }
        // Return the vreg holding the current in-memory finish value,
        // loading from memory only if the cache is cold.
        arch::Gp get() {
            if (!have_reg) {
                arch::load(cc, reg, arch::ptr(vm_reg, (int)VMStackFinishOff));
                have_reg = true;
            }
            return reg;
        }
        // Store `src` to memory AND update the cached reg so subsequent
        // gets are hot. Every set() is immediate -- no deferral.
        void set(arch::Gp src) {
            arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), src);
            if (src.id() != reg.id()) {
                arch::mov_reg(cc, reg, src);
            }
            have_reg = true;
        }
        // Drop knowledge that `reg` reflects memory (helper call mutated
        // memory, or we crossed a block boundary).
        void invalidate() {
            have_reg = false;
        }
        // Compatibility shim for Phase 1 auto-flush call sites. Since there
        // is no deferred store, this is just invalidate(). Retained so the
        // helper wrappers and terminator/branch emitters compile unchanged.
        void flush_invalidate() {
            invalidate();
        }
        void reload() {
            invalidate();
            (void)get();
        }
        void merge_slow_path() {
            invalidate();
        }
    };
    FinishCache fc(cc, vm_reg);

    auto call0 = [&](const void *helper) {
        fc.flush_invalidate();
        InvokeNode *inv;
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *>());
        inv->set_arg(0, vm_reg);
    };

    typedef void (*VMFunc)(nari::bytecode::VM *);

    auto call_vm = [&](VMFunc helper, nari::bytecode::VM *vm) {
        helper(vm);
    };
    auto call_u32 = [&](const void *helper, uint32_t a) {
        fc.flush_invalidate();
        InvokeNode *inv;
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, uint32_t>());
        inv->set_arg(0, vm_reg);
        inv->set_arg(1, Imm(a));
    };
    auto call_u32_u32 = [&](const void *helper, uint32_t a, uint32_t b) {
        fc.flush_invalidate();
        InvokeNode *inv;
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, uint32_t, uint32_t>());
        inv->set_arg(0, vm_reg);
        inv->set_arg(1, Imm(a));
        inv->set_arg(2, Imm(b));
    };
    auto call_i64 = [&](const void *helper, int64_t a) {
        fc.flush_invalidate();
        InvokeNode *inv;
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, int64_t>());
        inv->set_arg(0, vm_reg);
        inv->set_arg(1, Imm(a));
    };
    // jit_slot_store_raw(vm, uint32_t idx, uint64_t raw).
    auto call_u32_u64 = [&](const void *helper, uint32_t a, uint64_t b) {
        fc.flush_invalidate();
        InvokeNode *inv;
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper,
                         FuncSignature::build<void, void *, uint32_t, uint64_t>());
        inv->set_arg(0, vm_reg);
        inv->set_arg(1, Imm(a));
        inv->set_arg(2, Imm(b));
    };

    auto emit_ir_call_value = [&](uint32_t argc) {
        if (argc > 4) {
            call_u32((const void *)jit_call_value, argc);
            return;
        }
        Label slow = cc.new_label();
        Label done = cc.new_label();

        arch::Gp endp = cc.new_gp64("ir_cv_end");
        arch::Gp tag = cc.new_gp64("ir_cv_tag");
        arch::load(cc, endp, arch::ptr(vm_reg, (int)VMStackFinishOff));

        arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-(int64_t)(argc + 1) * ValSize + tagWordOff)));
        arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp fd = cc.new_gp64("ir_cv_fd");
        arch::load(cc, fd, arch::ptr(endp, (int)(-(int64_t)(argc + 1) * ValSize)));
        arch::zero_extend_48(cc, fd);
        arch::cmp_mem8_imm(cc, arch::ptr8(fd, (int)HeapTypeTagOff), Imm((int)tagFunction));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp fidx = cc.new_gp64("ir_cv_fidx");
        arch::load32_sx(cc, fidx, arch::ptr32(fd, (int)FDJitFuncIdxOff));
        arch::js(cc, fidx, slow);
        arch::cmp_mem32_imm(cc, arch::ptr32(fd, (int)FDLocalsCountOff), Imm((int)argc));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp callee = cc.new_gp64("ir_cv_cal");
        {
            arch::Gp base = cc.new_gp64("ir_cv_base");
            assert_tables_stable();
            cc.mov(base, Imm((uint64_t)(uintptr_t)compiled_fn_vec.data()));
            arch::shl(cc, fidx, fidx, 3);
            arch::add2(cc, fidx, base);
            arch::load(cc, callee, arch::ptr(fidx));
        }
        arch::test_zero(cc, callee);
        arch::jcc(cc, arch::CC::kEQ, slow);

        arch::Gp ff = cc.new_gp64("ir_cv_ff");
        arch::Gp fcap = cc.new_gp64("ir_cv_fcap");
        arch::load(cc, ff, arch::ptr(vm_reg, (int)FramesFinishOff));
        arch::load(cc, fcap, arch::ptr(vm_reg, (int)VMFramesCapacityOff));
        arch::sub2(cc, fcap, ff);
        arch::cmp_imm(cc, fcap, Imm((int)FrameSize));
        arch::jcc(cc, arch::CC::kFLT, slow);

        arch::Gp meta = cc.new_gp64("ir_cv_meta");
        arch::load(cc, meta, arch::ptr(fd, (int)FDJitMetaOff));
        arch::test_zero(cc, meta);
        arch::jcc(cc, arch::CC::kEQ, slow);

        for (int i = 0; i < (int)argc; i++) {
            int src = (i - (int)argc) * (int)ValSize;
            int dst = src - (int)ValSize;
            arch::Gp tmp = cc.new_gp64("ir_cv_arg");
            arch::load(cc, tmp, arch::ptr(endp, src));
            arch::store(cc, arch::ptr(endp, dst), tmp);
        }

        arch::Gp nf = cc.new_gp64("ir_cv_nf");
        arch::lea(cc, nf, endp, (int64_t)(-(int32_t)ValSize));
        arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), nf);

        arch::Gp sbp = cc.new_gp64("ir_cv_sbp");
        if (argc == 0) {
            cc.mov(sbp, nf);
        } else {
            arch::lea(cc, sbp, nf, (int64_t)(-(int32_t)((int64_t)argc * ValSize)));
        }
        arch::Gp sbi = cc.new_gp64("ir_cv_sbi");
        cc.mov(sbi, sbp);
        arch::sub_mem(cc, sbi, arch::ptr(vm_reg, (int)VMStackStartOff));
        arch::shr(cc, sbi, 3);

        arch::Gp cap_raw = cc.new_gp64("ir_cv_cap");
        arch::load(cc, cap_raw, arch::ptr(fd, (int)FDCapturesOff));
        arch::Gp prev_cap = cc.new_gp64("ir_cv_prevcap");
        arch::load(cc, prev_cap, arch::ptr(vm_reg, (int)VMCapturesRawOff));
        arch::store(cc, arch::ptr(vm_reg, (int)VMCapturesRawOff), cap_raw);

        arch::Gp ip = cc.new_gp64("ir_cv_ip");
        arch::load(cc, ip, arch::ptr(meta, (int)FMCodeDataOff));
        arch::store(cc, arch::ptr(ff, (int)FrameFunctionOff), meta);
        arch::store(cc, arch::ptr(ff, (int)FrameIpOff), ip);
        arch::store(cc, arch::ptr(ff, (int)SlotBaseOff), sbi);
        arch::store_imm(cc, arch::ptr(ff, (int)FrameCapturesOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)(FrameCapturesOff + 8)), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)FrameOpenUpvalOff), Imm(0));
        arch::add_imm(cc, ff, (int)FrameSize);
        arch::store(cc, arch::ptr(vm_reg, (int)FramesFinishOff), ff);

        // the trampoline below invokes another JITted Nari function which will read/write the VM stack.
        fc.invalidate();
        InvokeNode *inv;
        cc.invoke(Out(inv), callee, FuncSignature::build<void, void *>());
        inv->set_arg(0, vm_reg);
        arch::store(cc, arch::ptr(vm_reg, (int)VMCapturesRawOff), prev_cap);
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32((const void *)jit_call_value, argc);
        cc.bind(done);
    };
    // register lowering
    if (irFuncs.num_params == 0 || spec) {
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
        auto raw_rep = [](ir::Ty t) {
            return t == ir::Ty::Unknown || t == ir::Ty::Heap;
        };
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
                    if (pin.type != ir::Ty::Int48 && pin.type != ir::Ty::Bool &&
                        pin.type != ir::Ty::None) {
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
                    case ir::Op::DynCmpLt:
                    case ir::Op::DynCmpLe:
                    case ir::Op::DynCmpGt:
                    case ir::Op::DynCmpGe:
                    case ir::Op::DynCmpEq:
                    case ir::Op::DynCmpNe:
                        // a Dyn op with a non-raw operand (e.g. the Unknown result of a Call)
                        // is lowered via ESCAPE to the general-path helper
                        if (!operands_raw_int(in)) {
                            any_result_ty = true;
                            needs_escape_infra = true;
                        }
                        break;
                    case ir::Op::IAdd:
                    case ir::Op::ISub:
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
                        if (in.imm_int >= 0 &&
                            (size_t)in.imm_int < slot_types.size() &&
                            raw_rep(slot_types[(uint32_t)in.imm_int]) &&
                            !raw_rep(slot_types[in.imm_u32])) {
                            elig = false;
                            if (!reject) {
                                reject = "copyslot raw->untagged";
                            }
                        }
                        // a Float xmm dst reads slot_vec[src], so src
                        // must also be an xmm Float slot.
                        if (in.imm_u32 < slot_types.size() &&
                            slot_types[in.imm_u32] == ir::Ty::Float &&
                            !(in.imm_int >= 0 &&
                              (size_t)in.imm_int < slot_types.size() &&
                              slot_types[(uint32_t)in.imm_int] == ir::Ty::Float)) {
                            elig = false;
                            if (!reject) {
                                reject = "copyslot float dst non-float src";
                            }
                        }
                        break;
                    case ir::Op::Not:
                        // Lowered as xor 1: valid ONLY for a Bool operand.
                        if (in.operands.empty() ||
                            irFuncs.inst(in.operands[0]).type != ir::Ty::Bool) {
                            elig = false;
                            if (!reject) {
                                reject = "Not on non-bool";
                            }
                        }
                        break;
                    // lowered inline when proven (array index ops / imul / imod / load.global)
                    case ir::Op::IMul:
                    case ir::Op::IMod:
                        if (!operands_raw_int(in)) {
                            elig = false;
                            if (!reject) {
                                reject = "imul/imod operand non-raw";
                            }
                        }
                        needs_escape_infra = true;
                        break;
                    case ir::Op::LoadIndex:
                    case ir::Op::StoreIndex:
                    case ir::Op::MakeArray:
                    case ir::Op::Call:
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
                        if (in.operands.size() != 2 ||
                            irFuncs.inst(in.operands[0]).type != ir::Ty::Float ||
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
                        if (in.operands.size() != 2 ||
                            irFuncs.inst(in.operands[0]).type != ir::Ty::Float ||
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
                            snprintf(buf, sizeof(buf), "op %d", (int)in.op);
                            reject = buf;
                        }
                        break;
                }
                if (!any_result_ty && in.result != ir::InvalidValue &&
                    in.type != ir::Ty::Int48 &&
                    in.type != ir::Ty::Bool && in.type != ir::Ty::None) {
                    elig = false;
                    if (!reject) {
                        reject = "result type non-int/bool/none";
                    }
                }
            }
            if (b.terminator != ir::InvalidValue) {
                const ir::Inst &tin = irFuncs.inst(b.terminator);
                ir::Op to = tin.op;
                if (to != ir::Op::Jump && to != ir::Op::Branch && to != ir::Op::Return) {
                    elig = false;
                    if (!reject) {
                        reject = "non-J/B/R terminator";
                    }
                }
                // Branch truthiness is lowered as test_zero on the raw reg, true for bool and int48, wrong for boxed
                if (to == ir::Op::Branch && !tin.operands.empty()) {
                    ir::Ty ct = irFuncs.inst(tin.operands[0]).type;
                    if (ct != ir::Ty::Bool && ct != ir::Ty::Int48) {
                        elig = false;
                        if (!reject) {
                            reject = "branch cond non-raw";
                        }
                    }
                }
                if (to == ir::Op::Return) {
                    needs_escape_infra = true;
                }
            }
        }
        if (dbg_elig && !elig) {
            fprintf(stderr, "[REG-TIER] %-30s reject: %s\n",
                    chunk.functions[chunk_idx].name.empty()
                        ? "<anon>"
                        : chunk.functions[chunk_idx].name.c_str(),
                    reject ? reject : "?");
        }

        if (elig) {
            auto call_push_reg = [&](const void *helper, arch::Gp val) {
                InvokeNode *inv;
                arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper,
                                 FuncSignature::build<void, void *, int64_t>());
                inv->set_arg(0, vm_reg);
                inv->set_arg(1, val);
            };
            std::vector<arch::Gp> slot_reg(irFuncs.num_slots);
            // parallel XMM home for Float slots. slot_reg[s] stays
            // default-constructed (unused) for xmm slots
            std::vector<arch::Vec> slot_vec(irFuncs.num_slots);
            auto is_xmm_slot = [&](uint32_t s) {
                return s < slot_types.size() &&
                       slot_types[s] == ir::Ty::Float;
            };
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
                    cc.mov(slot_reg[s], Imm(static_cast<int64_t>(nbNone)));
                } else {
                    cc.mov(slot_reg[s], Imm(0)); // defined for allocator
                }
            }
            if (needs_escape_infra) {
                const uint32_t stack_bound = (uint32_t)irFuncs.insts.size() + 8;
                Label rz_ok = cc.new_label();
                arch::Gp rz_need = cc.new_gp64("rrz_need");
                arch::Gp rz_cap = cc.new_gp64("rrz_cap");
                arch::load(cc, rz_need, arch::ptr(vm_reg, (int)VMStackFinishOff));
                arch::load(cc, rz_cap, arch::ptr(vm_reg, (int)VMStackCapacityOff));
                arch::add_imm(cc, rz_need, (int64_t)stack_bound * (int64_t)ValSize);
                cc.cmp(rz_need, rz_cap);
                arch::jcc(cc, arch::CC::kULT, rz_ok);
                call_u32((const void *)jit_reserve, stack_bound);
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
                arch::load(cc, fr, arch::ptr(vm_reg, (int)FramesFinishOff));
                arch::sub_imm(cc, fr, (int)FrameSize);
                arch::load(cc, r_sbb, arch::ptr(fr, (int)SlotBaseOff));
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
                arch::load(cc, pbase, arch::ptr(vm_reg, (int)VMStackStartOff));
                arch::add2(cc, pbase, r_sbb);
                for (uint32_t s = 0; s < irFuncs.num_params && s < irFuncs.num_slots; s++) {
                    const ir::Ty pt = s < slot_types.size() ? slot_types[s] : ir::Ty::Unknown;
                    arch::load(cc, slot_reg[s], arch::ptr(pbase, (int)((int64_t)s * ValSize)));
                    if (pt == ir::Ty::Int48) {
                        arch::Gp ptag = cc.new_gp64("r_param_tag");
                        arch::load16_zx(cc, ptag,
                                        arch::ptr16(pbase, (int)((int64_t)s * ValSize + tagWordOff)));
                        arch::cmp_imm(cc, ptag.r32(), Imm((int)tagInt));
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
                arch::load(cc, addr, arch::ptr(vm_reg, (int)VMStackStartOff));
                arch::add2(cc, addr, r_sbb);
                arch::store(cc, arch::ptr(addr, (int)s * (int)ValSize), slot_reg[s]);
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
            if (irFuncs.entry >= 0 && (size_t)irFuncs.entry < irFuncs.blocks.size()) {
                std::vector<ir::BlockId> stk{ irFuncs.entry };
                reach[irFuncs.entry] = true;
                while (!stk.empty()) {
                    ir::BlockId bb = stk.back();
                    stk.pop_back();
                    for (ir::BlockId s : irFuncs.blocks[bb].succs) {
                        if (s >= 0 && (size_t)s < reach.size() && !reach[s]) {
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
            ir::ArrayHeaderHoist hoist_plan =
                ir::plan_array_header_hoist(irFuncs, int_arr_slots);
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
                    cc.mov(g, Imm(static_cast<int64_t>(nbNone)));
                } else if (e.ty == ir::Ty::Bool) {
                    if (e.is_const) {
                        cc.mov(g, Imm(static_cast<int64_t>(nbBoolTag | (e.cval ? 1u : 0u))));
                    } else {
                        arch::or_imm(cc, g, e.reg, (int64_t)nbBoolTag);
                    }
                } else { // Int48 raw payload
                    if (e.is_const) {
                        cc.mov(g, Imm(static_cast<int64_t>(nbIntTag | ((uint64_t)e.cval & nbPtrMask))));
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
            auto escape_ex = [&](const void *helper, int kind, uint32_t immA,
                                 uint32_t immB, std::vector<RE *> extra,
                                 size_t consumed, bool produces,
                                 ir::Ty result_ty) -> RE {
                const size_t n = st.size() + extra.size();
                arch::Gp fin = cc.new_gp64("r_esc_fin");
                arch::load(cc, fin, arch::ptr(vm_reg, (int)VMStackFinishOff));
                for (size_t i = 0; i < st.size(); i++) {
                    arch::Gp b = box_re(st[i]);
                    arch::store(cc, arch::ptr(fin, (int)(i * (size_t)ValSize)), b);
                }
                for (size_t i = 0; i < extra.size(); i++) {
                    arch::Gp b = box_re(*extra[i]);
                    arch::store(cc, arch::ptr(fin, (int)((st.size() + i) * (size_t)ValSize)), b);
                }
                if (n) {
                    arch::add_imm(cc, fin, (int64_t)n * ValSize);
                    arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), fin);
                }
                if (kind == 0) {
                    call0(helper);
                } else if (kind == 1) {
                    call_u32(helper, immA);
                } else if (kind == 2) {
                    call_u32_u32(helper, immA, immB);
                } else {
                    // the inline call fast path
                    emit_ir_call_value(immA);
                }
                RE out;
                out.ty = result_ty;
                arch::Gp fin2 = cc.new_gp64("r_esc_fin2");
                arch::load(cc, fin2, arch::ptr(vm_reg, (int)VMStackFinishOff));
                if (produces) {
                    arch::Gp res = cc.new_gp64("r_esc_res");
                    arch::load(cc, res, arch::ptr(fin2, (int)(-ValSize)));
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
                const int64_t drop =
                    (int64_t)(n - consumed + (produces ? 1 : 0)) * ValSize;
                if (drop) {
                    arch::sub_imm(cc, fin2, drop);
                    arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), fin2);
                }
                return out;
            };
            // is this RE a raw untagged int/bool payload (inline register arith is valid) vs. boxed Value bits / none?
            auto re_raw = [](const RE &e) {
                return e.ty == ir::Ty::Int48 || e.ty == ir::Ty::Bool;
            };
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
                    case ir::Op::ICmpEq:
                        return arch::CC::kEQ;
                    default:
                        return arch::CC::kNE; // DynCmpNe
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
                    e.reg = phi_reg.at(pv);
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
                            arch::load(cc, hdr_base[s],
                                       arch::ptr(ptr, (int)ArrayVecStartOff));
                            arch::load(cc, hdr_size[s],
                                       arch::ptr(ptr, (int)ArrayVecFinishOff));
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
                    bool is_cmp = last.op == ir::Op::DynCmpLt || last.op == ir::Op::DynCmpLe ||
                                  last.op == ir::Op::DynCmpGt || last.op == ir::Op::DynCmpGe ||
                                  last.op == ir::Op::DynCmpEq || last.op == ir::Op::DynCmpNe ||
                                  last.op == ir::Op::ICmpLt || last.op == ir::Op::ICmpLe ||
                                  last.op == ir::Op::ICmpGt || last.op == ir::Op::ICmpGe ||
                                  last.op == ir::Op::ICmpEq || last.op == ir::Op::ICmpNe;
                    // Ordered float relops only (Eq/Ne need the ZF+PF combine, not a single jcc).
                    // Both operands must be Float.
                    bool is_fcmp = (last.op == ir::Op::FCmpLt || last.op == ir::Op::FCmpLe ||
                                    last.op == ir::Op::FCmpGt || last.op == ir::Op::FCmpGe) &&
                                   last.operands.size() == 2 &&
                                   irFuncs.inst(last.operands[0]).type == ir::Ty::Float &&
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
                    if (term.op == ir::Op::Branch && ((is_cmp && cmp_operands_raw) || is_fcmp) &&
                        !term.operands.empty() &&
                        term.operands[0] == b.insts.back()) {
                        // don't fuse if either branch successor has phis
                        bool succ_has_phi = false;
                        if (term.target0 >= 0 && (size_t)term.target0 < irFuncs.blocks.size() &&
                            !irFuncs.blocks[term.target0].phis.empty()) {
                            succ_has_phi = true;
                        }
                        if (term.target1 >= 0 && (size_t)term.target1 < irFuncs.blocks.size() &&
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
                    if (succ < 0 || (size_t)succ >= irFuncs.blocks.size()) {
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
                        arch::mov_reg(cc, phi_reg.at(sb.phis[i]), reg_of(st[base + i]));
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
                                cc.mov(slot_reg[in.imm_u32], Imm(static_cast<int64_t>(nbIntTag | ((uint64_t)in.imm_int & nbPtrMask))));
                                slot_write_through(in.imm_u32);
                            } else {
                                cc.mov(slot_reg[in.imm_u32], Imm(in.imm_int));
                            }
                            break;
                        case ir::Op::StoreBImmSlot:
                            if (raw_rep(slot_types[in.imm_u32])) {
                                cc.mov(slot_reg[in.imm_u32], Imm(static_cast<int64_t>(nbIntTag | ((uint64_t)in.imm_int & nbPtrMask))));
                                slot_write_through(in.imm_u32);
                            } else {
                                cc.mov(slot_reg[in.imm_u32], Imm(in.imm_int ? 1 : 0));
                            }
                            break;
                        case ir::Op::StoreNSlot:
                            if (raw_rep(slot_types[in.imm_u32])) {
                                cc.mov(slot_reg[in.imm_u32], Imm(static_cast<int64_t>(nbNone)));
                                slot_write_through(in.imm_u32);
                            } else {
                                cc.mov(slot_reg[in.imm_u32], Imm(0));
                            }
                            break;
                        case ir::Op::CopySlot: {
                            const uint32_t src = (uint32_t)in.imm_int;
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
                                    arch::or_imm(cc, tmp, slot_reg[src], (int64_t)nbBoolTag);
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
                        case ir::Op::IMul: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            arch::Gp g = cc.new_gp64("r_imul");
                            if (rb.is_const && rb.cval >= INT32_MIN && rb.cval <= INT32_MAX) {
                                arch::imul_imm(cc, g, reg_of(ra), (int32_t)rb.cval);
                            } else {
                                arch::imul(cc, g, reg_of(ra), reg_of(rb));
                            }
                            // NOTE: like the tier's IAdd/ISub, no int48
                            // overflow guard (documented divergence: wraps at
                            // 2^64 instead of promoting to float).
                            st.push_back({ g, ir::Ty::Int48 });
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
                                RE r = escape_ex((const void *)jit_mod, 0, 0, 0,
                                                 { &ra, &rb }, 2, true, ir::Ty::Int48);
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
                            bool inline_ok = in.type == ir::Ty::Int48 &&
                                             in.operands.size() >= 2;
                            if (inline_ok) {
                                const ir::Inst &obj = irFuncs.inst(in.operands[0]);
                                const ir::Inst &key = irFuncs.inst(in.operands[1]);
                                inline_ok = obj.op == ir::Op::LoadSlot &&
                                            int_arr_slots.count(obj.imm_u32) != 0 &&
                                            key.type == ir::Ty::Int48;
                            }
                            if (!inline_ok) {
                                // guarded inline array fast path for UNPROVEN receivers
                                const ir::Ty rty = esc_result_ty(in.type);
                                const bool key_raw = ri.ty == ir::Ty::Int48;
                                if (!raw_rep(ra.ty) || !(key_raw || raw_rep(ri.ty)) ||
                                    !raw_rep(rty)) {
                                    RE r = escape_ex((const void *)jit_get_index, 0, 0, 0,
                                                     { &ra, &ri }, 2, true, rty);
                                    st.push_back(r);
                                    break;
                                }
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::Gp g = cc.new_gp64("r_ggi_val");
                                arch::Gp t = cc.new_gp64("r_ggi_tag");
                                arch::mov_reg(cc, t, ra.reg);
                                arch::shr(cc, t, 48);
                                arch::cmp_imm(cc, t.r32(), Imm((int)tagHeap));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp ptr = cc.new_gp64("r_ggi_ptr");
                                arch::mov_reg(cc, ptr, ra.reg);
                                arch::zero_extend_48(cc, ptr);
                                arch::cmp_mem8_imm(cc, arch::ptr8(ptr, (int)HeapTypeTagOff),
                                                   Imm((int)tagArray));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp idx = cc.new_gp64("r_ggi_idx");
                                if (key_raw) {
                                    arch::mov_reg(cc, idx, reg_of(ri));
                                } else {
                                    arch::Gp kt = cc.new_gp64("r_ggi_ktag");
                                    arch::mov_reg(cc, kt, ri.reg);
                                    arch::shr(cc, kt, 48);
                                    arch::cmp_imm(cc, kt.r32(), Imm((int)tagInt));
                                    arch::jcc(cc, arch::CC::kNE, slow);
                                    arch::mov_reg(cc, idx, ri.reg);
                                    arch::sign_extend_48(cc, idx);
                                }
                                arch::Gp start = cc.new_gp64("r_ggi_start");
                                arch::Gp size = cc.new_gp64("r_ggi_size");
                                arch::load(cc, start, arch::ptr(ptr, (int)ArrayVecStartOff));
                                arch::load(cc, size, arch::ptr(ptr, (int)ArrayVecFinishOff));
                                arch::sub2(cc, size, start);
                                arch::shr(cc, size, 3);
                                Label positive = cc.new_label();
                                Label oob = cc.new_label();
                                arch::jns(cc, idx, positive);
                                arch::add2(cc, idx, size);
                                arch::js(cc, idx, oob);
                                cc.bind(positive);
                                cc.cmp(idx, size);
                                arch::jcc(cc, arch::CC::kGE, oob);
                                arch::shl(cc, g, idx, 3);
                                arch::add2(cc, g, start);
                                arch::load(cc, g, arch::ptr(g, 0));
                                arch::jmp(cc, done);
                                cc.bind(oob);
                                cc.mov(g, Imm(static_cast<int64_t>(nbNone)));
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                RE r = escape_ex((const void *)jit_get_index, 0, 0, 0,
                                                 { &ra, &ri }, 2, true, rty);
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
                                arch::load(cc, start, arch::ptr(ptr, (int)ArrayVecStartOff));
                                arch::load(cc, size, arch::ptr(ptr, (int)ArrayVecFinishOff));
                                arch::sub2(cc, size, start);
                                arch::shr(cc, size, 3);
                            }
                            arch::Gp idx = cc.new_gp64("r_gi_idx");
                            arch::mov_reg(cc, idx, reg_of(ri));
                            arch::Gp g = cc.new_gp64("r_gi_val");
                            Label positive = cc.new_label();
                            Label oob = cc.new_label();
                            Label done = cc.new_label();
                            arch::jns(cc, idx, positive);
                            arch::add2(cc, idx, size);
                            arch::js(cc, idx, oob);
                            cc.bind(positive);
                            cc.cmp(idx, size);
                            arch::jcc(cc, arch::CC::kGE, oob);
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
                                inline_ok = obj.op == ir::Op::LoadSlot &&
                                            int_arr_slots.count(obj.imm_u32) != 0 &&
                                            key.type == ir::Ty::Int48 &&
                                            val.type == ir::Ty::Int48;
                            }
                            if (!inline_ok) {
                                // guarded inline in-bounds store for UNPROVEN receivers
                                const bool key_raw = ri.ty == ir::Ty::Int48;
                                if (!raw_rep(ra.ty) || !(key_raw || raw_rep(ri.ty))) {
                                    RE r = escape_ex((const void *)jit_set_index, 0, 0, 0,
                                                     { &ra, &ri, &rv }, 3, true, in.type);
                                    st.push_back(r);
                                    break;
                                }
                                Label slow = cc.new_label();
                                Label done = cc.new_label();
                                arch::Gp t = cc.new_gp64("r_gsi_tag");
                                arch::mov_reg(cc, t, ra.reg);
                                arch::shr(cc, t, 48);
                                arch::cmp_imm(cc, t.r32(), Imm((int)tagHeap));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp ptr = cc.new_gp64("r_gsi_ptr");
                                arch::mov_reg(cc, ptr, ra.reg);
                                arch::zero_extend_48(cc, ptr);
                                arch::cmp_mem8_imm(cc, arch::ptr8(ptr, (int)HeapTypeTagOff),
                                                   Imm((int)tagArray));
                                arch::jcc(cc, arch::CC::kNE, slow);
                                arch::Gp idx = cc.new_gp64("r_gsi_idx");
                                if (key_raw) {
                                    arch::mov_reg(cc, idx, reg_of(ri));
                                } else {
                                    arch::Gp kt = cc.new_gp64("r_gsi_ktag");
                                    arch::mov_reg(cc, kt, ri.reg);
                                    arch::shr(cc, kt, 48);
                                    arch::cmp_imm(cc, kt.r32(), Imm((int)tagInt));
                                    arch::jcc(cc, arch::CC::kNE, slow);
                                    arch::mov_reg(cc, idx, ri.reg);
                                    arch::sign_extend_48(cc, idx);
                                }
                                arch::Gp start = cc.new_gp64("r_gsi_start");
                                arch::Gp size = cc.new_gp64("r_gsi_size");
                                arch::load(cc, start, arch::ptr(ptr, (int)ArrayVecStartOff));
                                arch::load(cc, size, arch::ptr(ptr, (int)ArrayVecFinishOff));
                                arch::sub2(cc, size, start);
                                arch::shr(cc, size, 3);
                                Label positive = cc.new_label();
                                arch::jns(cc, idx, positive);
                                arch::add2(cc, idx, size);
                                arch::js(cc, idx, slow);
                                cc.bind(positive);
                                cc.cmp(idx, size);
                                arch::jcc(cc, arch::CC::kGE, slow);
                                {
                                    arch::Gp addr = cc.new_gp64("r_gsi_addr");
                                    arch::shl(cc, addr, idx, 3);
                                    arch::add2(cc, addr, start);
                                    arch::Gp boxed = box_re(rv);
                                    arch::store(cc, arch::ptr(addr, 0), boxed);
                                }
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                escape_ex((const void *)jit_set_index, 0, 0, 0,
                                          { &ra, &ri, &rv }, 3, true, in.type);
                                cc.bind(done);
                                st.push_back(rv);
                                break;
                            }
                            arch::Gp ptr = cc.new_gp64("r_si_ptr");
                            arch::mov_reg(cc, ptr, ra.reg);
                            arch::zero_extend_48(cc, ptr);
                            arch::Gp start = cc.new_gp64("r_si_start");
                            arch::Gp size = cc.new_gp64("r_si_size");
                            arch::load(cc, start, arch::ptr(ptr, (int)ArrayVecStartOff));
                            arch::load(cc, size, arch::ptr(ptr, (int)ArrayVecFinishOff));
                            arch::sub2(cc, size, start);
                            arch::shr(cc, size, 3);
                            arch::Gp idx = cc.new_gp64("r_si_idx");
                            arch::mov_reg(cc, idx, reg_of(ri));
                            Label positive = cc.new_label();
                            Label slow = cc.new_label();
                            Label done = cc.new_label();
                            arch::jns(cc, idx, positive);
                            arch::add2(cc, idx, size);
                            arch::js(cc, idx, slow);
                            cc.bind(positive);
                            cc.cmp(idx, size);
                            arch::jcc(cc, arch::CC::kGE, slow);
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
                            escape_ex((const void *)jit_set_index, 0, 0, 0,
                                      { &ra, &ri, &rv }, 3, true, in.type);
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
                            RE r = escape_ex((const void *)jit_make_array, 1, nelem, 0,
                                             eptrs, nelem, true, in.type);
                            st.push_back(r);
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
                            RE r = escape_ex(nullptr, 3, argc, 0,
                                             aptrs, argc + 1, true, in.type);
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::CallMethod: {
                            const uint32_t method_idx = in.imm_u32;
                            const uint32_t argc = (uint32_t)in.imm_int;
                            // Inline arr.push(<raw int/bool>)
                            bool inline_push =
                                method_idx < chunk.strings.size() &&
                                chunk.strings[method_idx] == "push" &&
                                argc == 1 && in.operands.size() >= 2;
                            bool recv_proven = false;
                            if (inline_push) {
                                const ir::Inst &recv = irFuncs.inst(in.operands[0]);
                                const ir::Inst &arg = irFuncs.inst(in.operands[1]);
                                recv_proven = recv.op == ir::Op::LoadSlot &&
                                              int_arr_slots.count(recv.imm_u32) != 0;
                                inline_push = raw_rep(recv.type) &&
                                              raw_rep(in.type) && // result RE holds raw none bits
                                              (arg.type == ir::Ty::Int48 ||
                                               arg.type == ir::Ty::Bool);
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
                                if (!recv_proven) {
                                    arch::Gp tag = cc.new_gp64("r_push_tag");
                                    arch::mov_reg(cc, tag, rrecv.reg);
                                    arch::shr(cc, tag, 48);
                                    arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
                                    arch::jcc(cc, arch::CC::kNE, slow);
                                }
                                arch::mov_reg(cc, ptr, rrecv.reg);
                                arch::zero_extend_48(cc, ptr);
                                if (!recv_proven) {
                                    arch::cmp_mem8_imm(cc, arch::ptr8(ptr, (int)HeapTypeTagOff),
                                                       Imm((int)tagArray));
                                    arch::jcc(cc, arch::CC::kNE, slow);
                                }
                                arch::Gp afin = cc.new_gp64("r_push_fin");
                                arch::Gp acap = cc.new_gp64("r_push_cap");
                                arch::load(cc, afin, arch::ptr(ptr, (int)ArrayVecFinishOff));
                                arch::load(cc, acap, arch::ptr(ptr, (int)ArrayVecCapacityOff));
                                cc.cmp(afin, acap);
                                arch::jcc(cc, arch::CC::kEQ, slow);
                                {
                                    arch::Gp boxed = box_re(rarg);
                                    arch::store(cc, arch::ptr(afin, 0), boxed);
                                    arch::add_imm(cc, afin, (int)ValSize);
                                    arch::store(cc, arch::ptr(ptr, (int)ArrayVecFinishOff), afin);
                                }
                                cc.mov(g, Imm(static_cast<int64_t>(nbNone)));
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                {
                                    RE r = escape_ex((const void *)jit_call_method, 2,
                                                     method_idx, argc, { &rrecv, &rarg },
                                                     argc + 1, true, in.type);
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
                            RE r = escape_ex((const void *)jit_call_method, 2,
                                             method_idx, argc, aptrs, argc + 1,
                                             true, in.type);
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
                            arch::load(cc, vaddr, arch::ptr(vm_reg, (int)VMGlobalCacheValidStartOff));
                            arch::load(cc, vfin, arch::ptr(vm_reg, (int)VMGlobalCacheValidFinishOff));
                            arch::add_imm(cc, vaddr, (int64_t)name_idx);
                            cc.cmp(vaddr, vfin);
                            arch::jcc(cc, arch::CC::kFGE, slow);
                            arch::Gp valid = cc.new_gp64("r_lg_valid");
                            arch::load8_zx(cc, valid, arch::ptr8(vaddr, 0));
                            arch::test_zero(cc, valid);
                            arch::jcc(cc, arch::CC::kEQ, slow);
                            {
                                arch::Gp cstart = cc.new_gp64("r_lg_cstart");
                                arch::load(cc, cstart, arch::ptr(vm_reg, (int)VMGlobalCacheStartOff));
                                const int64_t cache_off = (int64_t)name_idx * (int64_t)ValSize;
                                if (cache_off >= INT32_MIN && cache_off <= INT32_MAX) {
                                    arch::load(cc, g, arch::ptr(cstart, (int)cache_off));
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
                                RE r = escape_ex((const void *)jit_load_global, 1,
                                                 name_idx, 0, {}, 0, true, in.type);
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
                                const void *helper = is_add ? (const void *)jit_add
                                                            : (const void *)jit_sub;
                                const ir::Ty rty = esc_result_ty(in.type);
                                // Bool operands have add semantics only the
                                // helper knows; don't speculate.
                                const bool can_inline =
                                    (re_raw(ra) ? ra.ty == ir::Ty::Int48 : true) &&
                                    (re_raw(rb) ? rb.ty == ir::Ty::Int48 : true);
                                if (!can_inline) {
                                    RE r = escape_ex(helper, 0, 0, 0, { &ra, &rb },
                                                     2, true, rty);
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
                                        arch::cmp_imm(cc, t.r32(), Imm((int)tagInt));
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
                                RE r = escape_ex(helper, 0, 0, 0, { &ra, &rb },
                                                 2, true, rty);
                                arch::mov_reg(cc, g, r.reg);
                                cc.bind(done);
                                st.push_back({ g, rty });
                                break;
                            }
                            arch::Gp g = cc.new_gp64();
                            arch::mov_reg(cc, g, reg_of(ra));
                            if (rb.is_const) {
                                if (in.op == ir::Op::DynAdd || in.op == ir::Op::IAdd) {
                                    arch::add_imm(cc, g, rb.cval);
                                } else {
                                    arch::sub_imm(cc, g, rb.cval);
                                }
                            } else if (in.op == ir::Op::DynAdd || in.op == ir::Op::IAdd) {
                                arch::add2(cc, g, rb.reg);
                            } else {
                                arch::sub2(cc, g, rb.reg);
                            }
                            st.push_back({ g, ir::Ty::Int48 });
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
                                const bool swap = (in.op == ir::Op::FCmpLt ||
                                                   in.op == ir::Op::FCmpLe);
                                const arch::CC::Cond fcc =
                                    (in.op == ir::Op::FCmpLt || in.op == ir::Op::FCmpGt)
                                        ? arch::CC::kFGT
                                        : arch::CC::kFGE;
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
                        // dynamic mul/div, escape to the general-path helper
                        case ir::Op::DynMul:
                        case ir::Op::DynDiv:
                        case ir::Op::DynMod: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            const void *helper = (in.op == ir::Op::DynMul)
                                                     ? (const void *)jit_mul
                                                 : (in.op == ir::Op::DynDiv)
                                                     ? (const void *)jit_div
                                                     : (const void *)jit_mod;
                            RE r = escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2,
                                             true, esc_result_ty(in.type));
                            st.push_back(r);
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
                                        h = (const void *)jit_lt;
                                        break;
                                    case ir::Op::DynCmpLe:
                                        h = (const void *)jit_le;
                                        break;
                                    case ir::Op::DynCmpGt:
                                        h = (const void *)jit_gt;
                                        break;
                                    case ir::Op::DynCmpGe:
                                        h = (const void *)jit_ge;
                                        break;
                                    case ir::Op::DynCmpEq:
                                        h = (const void *)jit_eq;
                                        break;
                                    default:
                                        h = (const void *)jit_ne;
                                        break;
                                }
                                RE r = escape_ex(h, 0, 0, 0, { &ra, &rb }, 2,
                                                 true, ir::Ty::Bool);
                                st.push_back(r);
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
                    // skip the jump if the target is the next block
                    if (!(fallthrough_opt && t.target0 == fallthrough_bid[bid])) {
                        arch::jmp(cc, rlabels[t.target0]);
                    }
                } else if (t.op == ir::Op::Branch) {
                    if (fuse_fcmp) {
                        // fused ordered float-cmp -> branch.
                        RE rb = st.back();
                        st.pop_back();
                        RE ra = st.back();
                        st.pop_back();
                        const bool swap = (fused_op == ir::Op::FCmpLt ||
                                           fused_op == ir::Op::FCmpLe);
                        arch::CC::Cond cond =
                            (fused_op == ir::Op::FCmpLt || fused_op == ir::Op::FCmpGt)
                                ? arch::CC::kFGT
                                : arch::CC::kFGE;
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
                        arch::test_zero(cc, reg_of(c));
                        // same fallthrough elision as the fused path
                        // Truthy edge is taken on kNE (non-zero), invert to kE
                        // when the truthy target is the fallthrough.
                        arch::CC::Cond cond = arch::CC::kNE;
                        bool ft_truthy = fallthrough_opt && t.target0 == fallthrough_bid[bid];
                        bool ft_falsy = fallthrough_opt && t.target1 == fallthrough_bid[bid];
                        ir::BlockId jcc_target = t.target0; // truthy
                        if (ft_truthy && !ft_falsy) {
                            cond = arch::CC::kEQ;
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
                        cc.mov(rv, Imm(static_cast<int64_t>(nbNone)));
                    } else {
                        rv = box_re(st.back());
                    }
                    // Register-frugal (rv + 2 vregs): extra vregs here
                    // have been observed to spill hot-loop values (see
                    // emit_inline_return's comment).
                    Label ret_empty = cc.new_label();
                    arch::Gp ff = cc.new_gp64("r_ret_ff");
                    arch::Gp sc = cc.new_gp64("r_ret_sc");
                    arch::load(cc, ff, arch::ptr(vm_reg, (int)FramesFinishOff));
                    arch::sub_imm(cc, ff, (int)FrameSize);
                    arch::store(cc, arch::ptr(vm_reg, (int)FramesFinishOff), ff);
                    arch::load(cc, sc, arch::ptr(vm_reg, (int)FramesStartOff));
                    cc.cmp(ff, sc);
                    arch::load(cc, sc, arch::ptr(ff, (int)SlotBaseOff)); // flags live
                    arch::jcc(cc, arch::CC::kEQ, ret_empty);
                    arch::shl(cc, sc, sc, 3);
                    arch::load(cc, ff, arch::ptr(vm_reg, (int)VMStackStartOff));
                    arch::add2(cc, ff, sc);
                    arch::store(cc, arch::ptr(ff, 0), rv);
                    arch::add_imm(cc, ff, (int)ValSize);
                    arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), ff);
                    cc.ret();
                    cc.bind(ret_empty);
                    // frames now empty (top-level): plain push.
                    arch::load(cc, ff, arch::ptr(vm_reg, (int)VMStackFinishOff));
                    arch::store(cc, arch::ptr(ff, 0), rv);
                    arch::add_imm(cc, ff, (int)ValSize);
                    arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), ff);
                    cc.ret();
                }
            }
            // guard-failure trampoline. Reached only from the entry tag checks,
            // before any state was modified. This avoids running the whole call.
            if (spec_fail_used) {
                cc.bind(spec_fail);
                InvokeNode *fb_inv;
                arch::invoke_imm(cc, &fb_inv, (uint64_t)(uintptr_t)spec_fallback, FuncSignature::build<void, void *>());
                fb_inv->set_arg(0, vm_reg);
                cc.ret();
            }
            cc.end_func();
            if (cc.finalize() != kErrorOk) {
                return nullptr;
            }
            size_t sz = code_holder.code_size();
            CompiledFunc fn = nullptr;
            if (this->rt.add(&fn, &code_holder) != kErrorOk || !fn) {
                return nullptr;
            }
            const char *tier_tag = spec ? " [ir-reg-spec]" : " [ir-reg]";
            jit_dump_asm(
                chunk.functions[chunk_idx].name.empty()
                    ? std::string("<anon>") + tier_tag
                    : chunk.functions[chunk_idx].name + tier_tag,
                asm_logger.data(),
                (uint64_t)(uintptr_t)spec_fallback,
                spec_fallback ? "spec_fallback (general tier)" : nullptr);
            {
                std::string sym =
                    chunk.functions[chunk_idx].name.empty()
                        ? std::string(spec ? "anon_ir_reg_spec" : "anon_ir_reg")
                        : chunk.functions[chunk_idx].name +
                              (spec ? "_ir_reg_spec" : "_ir_reg");
                register_gdb_jit_function(
                    sym, reinterpret_cast<const void *>(fn), sz);
                perf_jitdump_register(
                    sym, reinterpret_cast<const void *>(fn), sz);
            }
            report(spec ? "OK  [ir-reg spec-param]" : "OK  [ir-reg fast path]");
            return fn;
        }
    }
    // Speculative mode compiles only the register tier; the general-path
    // version already exists (it's `spec_fallback`).
    if (spec) {
        report("spec param tier ineligible");
        return nullptr;
    }

    // one AsmJIT label per IR block (so jumps/branches can target any block)
    std::vector<asmjit::Label> blabels(irFuncs.blocks.size());
    for (size_t i = 0; i < irFuncs.blocks.size(); i++) {
        blabels[i] = cc.new_label();
    }

    bool ok = true;
    ir::Op unhandled_op = ir::Op::Return; // captured for NARI_JIT_REPORT
    // typed fast path:
    // both operands proven Float -> inline native f64 op on the value stack (no helper call, no runtime type dispatch).
    auto emit_float_binop = [&](ir::Op op) {
        // fc.get()/set() so the finish load is reused by any following emitter in the same block.
        arch::Gp endp = fc.get();
        arch::Vec fa = arch::new_vec_f64(cc, "fb_a");
        arch::Vec fb = arch::new_vec_f64(cc, "fb_b");
        arch::load_f64(cc, fb, endp, (int)(-ValSize));     // top  = rhs
        arch::load_f64(cc, fa, endp, (int)(-2 * ValSize)); // 2nd  = lhs
        arch::Vec fr = arch::new_vec_f64(cc, "fb_r");
        switch (op) {
            case ir::Op::DynAdd:
            case ir::Op::IAdd:
            case ir::Op::FAdd:
                arch::fadd(cc, fr, fa, fb);
                break;
            case ir::Op::DynSub:
            case ir::Op::ISub:
            case ir::Op::FSub:
                arch::fsub(cc, fr, fa, fb);
                break;
            case ir::Op::DynMul:
            case ir::Op::IMul:
            case ir::Op::FMul:
                arch::fmul(cc, fr, fa, fb);
                break;
            default:
                arch::fdiv(cc, fr, fa, fb);
                break; // DynDiv / FDiv
        }
        arch::store_f64(cc, endp, (int)(-2 * ValSize), fr); // write result to 2nd
        arch::sub_imm(cc, endp, (int)ValSize);              // pop one
        fc.set(endp);
    };
    auto both_float = [&](const ir::Inst &in) {
        return irFuncs.inst(in.operands[0]).type == ir::Ty::Float &&
               irFuncs.inst(in.operands[1]).type == ir::Ty::Float;
    };
    auto both_int = [&](const ir::Inst &in) {
        return irFuncs.inst(in.operands[0]).type == ir::Ty::Int48 &&
               irFuncs.inst(in.operands[1]).type == ir::Ty::Int48;
    };
    {
        const uint32_t stack_bound = (uint32_t)irFuncs.insts.size() + 8;
        Label rz_ok = cc.new_label();
        arch::Gp rz_need = cc.new_gp64("rz_need");
        arch::Gp rz_cap = cc.new_gp64("rz_cap");
        arch::load(cc, rz_need, arch::ptr(vm_reg, (int)VMStackFinishOff));
        arch::load(cc, rz_cap, arch::ptr(vm_reg, (int)VMStackCapacityOff));
        arch::add_imm(cc, rz_need, (int64_t)stack_bound * (int64_t)ValSize);
        cc.cmp(rz_need, rz_cap);
        arch::jcc(cc, arch::CC::kULT, rz_ok); // finish + bound*8 < capacity -> enough headroom
        call_u32((const void *)jit_reserve, stack_bound);
        cc.bind(rz_ok);
    }
    // Slot-address CSE: IR functions can't have upvalues, so slot_base is
    // fixed at frame init; hoist `slot_base*8` to entry (see emit_ir_slot_addr).
    arch::Gp slot_base_bytes_vreg = cc.new_gp64("slot_base_bytes");
    {
        arch::Gp frame_init = cc.new_gp64("frame_init");
        arch::load(cc, frame_init, arch::ptr(vm_reg, (int)FramesFinishOff));
        arch::sub_imm(cc, frame_init, (int)FrameSize);
        arch::load(cc, slot_base_bytes_vreg,
                   arch::ptr(frame_init, (int)SlotBaseOff));
        arch::shl(cc, slot_base_bytes_vreg, slot_base_bytes_vreg, 3);
    }

    auto emit_ir_slot_addr = [&](uint32_t slot, const char *name) {
        (void)name;
        // stack_start + slot_base_bytes + slot*8. IR functions can't have upvalues,
        // so slot_base is fixed and no upvalue guard is needed.
        arch::Gp addr = cc.new_gp64("ir_sa");
        arch::load(cc, addr, arch::ptr(vm_reg, (int)VMStackStartOff));
        arch::add2(cc, addr, slot_base_bytes_vreg);
        if (slot != 0) {
            arch::add_imm(cc, addr, (int)slot * (int)ValSize);
        }
        return addr;
    };
    // SlotRegCache: IR functions can't alias their own slots (no upvalues), so
    // a slot's value is only written by this function's own Store*Slot/CopySlot ops
    struct SlotRegCache {
        arch::Compiler &cc;
        struct Entry {
            arch::Gp reg;
            bool valid = false;
        };
        std::vector<Entry> entries;
        explicit SlotRegCache(arch::Compiler &cc_, uint32_t num_slots)
            : cc(cc_), entries(num_slots) {
        }
        // If slot is cached, return true and set out_reg to a vreg holding
        // the raw slot value; else return false. The caller is responsible
        // for NOT mutating the returned reg (readers should copy into a
        // fresh vreg if they need to modify).
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
    SlotRegCache slot_cache(cc, irFuncs.num_slots);

    auto emit_ir_load_slot = [&](uint32_t slot) {
        // fast path via SlotRegCache. If this slot's raw value is
        // already in a vreg from an earlier LoadSlot/StoreSlot in this block, skip the memory reload.
        {
            arch::Gp cached;
            if (slot_cache.try_get(slot, cached)) {
                arch::Gp endp = fc.get();
                arch::store(cc, arch::ptr(endp, 0), cached);
                arch::add_imm(cc, endp, (int)ValSize);
                fc.set(endp);
                return;
            }
        }
        arch::Gp slotp = emit_ir_slot_addr(slot, "ir_ls");
        arch::Gp endp = fc.get();
        arch::Gp raw = cc.new_gp64("ir_ls_raw");
        arch::load(cc, raw, arch::ptr(slotp, 0));
        arch::store(cc, arch::ptr(endp, 0), raw);
        arch::add_imm(cc, endp, (int)ValSize);
        fc.set(endp);
        // cache the just-loaded raw value for this slot.
        slot_cache.set(slot, raw);
    };
    auto emit_ir_store_slot = [&](uint32_t slot) {
        arch::Gp slotp = emit_ir_slot_addr(slot, "ir_ss");
        // store_slot doesn't modify _M_finish
        arch::Gp endp = fc.get();
        arch::Gp raw = cc.new_gp64("ir_ss_raw");
        arch::load(cc, raw, arch::ptr(endp, (int)(-ValSize)));
        arch::store(cc, arch::ptr(slotp, 0), raw);
        // cache the just-stored raw value for this slot.
        slot_cache.set(slot, raw);
    };
    // fused slot-store emitters. Write a raw 64-bit value directly into a stack slot
    auto emit_ir_store_raw_to_slot = [&](uint32_t slot, uint64_t raw) {
        arch::Gp slotp = emit_ir_slot_addr(slot, "ir_sifs");
        arch::Gp rawr = cc.new_gp64("ir_sifs_raw");
        cc.mov(rawr, Imm((int64_t)raw));
        arch::store(cc, arch::ptr(slotp, 0), rawr);
        // this immediate is now the slot's live value, cache it.
        slot_cache.set(slot, rawr);
    };
    auto emit_ir_store_imm_slot = [&](int64_t imm, uint32_t slot) {
        // Int48 -> raw NaN-boxed uint64 (or float-fallback for out-of-range).
        uint64_t raw = Value::make_int_checked(imm).raw_bits();
        emit_ir_store_raw_to_slot(slot, raw);
    };
    auto emit_ir_store_bimm_slot = [&](int64_t imm, uint32_t slot) {
        uint64_t raw = Value::make_bool(imm != 0).raw_bits();
        emit_ir_store_raw_to_slot(slot, raw);
    };
    auto emit_ir_store_n_slot = [&](uint32_t slot) {
        uint64_t raw = Value::none().raw_bits();
        emit_ir_store_raw_to_slot(slot, raw);
    };
    auto emit_ir_store_c_slot = [&](uint32_t const_idx, uint32_t slot) {
        // Read from the constant pool at JIT time when the value is a primitive (INT/FLOAT/NONE),
        // strings/functions require the runtime helper.
        auto *fmeta = &chunk.functions[chunk_idx];
        if (const_idx < fmeta->constants.size()) {
            const auto &cst = fmeta->constants[const_idx];
            switch (cst.type) {
                case nari::bytecode::Constant::Type::NONE:
                    emit_ir_store_raw_to_slot(slot, Value::none().raw_bits());
                    return;
                case nari::bytecode::Constant::Type::INT:
                    emit_ir_store_raw_to_slot(
                        slot, Value::make_int_checked(cst.as_int).raw_bits());
                    return;
                case nari::bytecode::Constant::Type::FLOAT: {
                    uint64_t raw;
                    double d = cst.as_float;
                    std::memcpy(&raw, &d, sizeof(raw));
                    emit_ir_store_raw_to_slot(slot, raw);
                    return;
                }
                case nari::bytecode::Constant::Type::STRING: {
                    // Same safety as emit_ir_sconst_inline
                    auto &mut_chunk =
                        const_cast<nari::bytecode::Chunk &>(chunk);
                    emit_ir_store_raw_to_slot(
                        slot,
                        mut_chunk.get_const_string(cst.string_idx)
                            .raw_bits());
                    return;
                }
                case nari::bytecode::Constant::Type::FUNCTION:
                    break;
            }
        }
        // Non-primitive: fall back to raw runtime helpers (no fast path).
        // The old split-form fusion would push+store+pop; the runtime path
        // has the same semantics but avoids the forward-reference to
        // emit_ir_pop and keeps this rare branch simple.
        call_u32((const void *)jit_load_const, const_idx);
        call_u32((const void *)jit_store_var, slot);
        call0((const void *)jit_pop);
    };
    auto emit_ir_copy_slot = [&](uint32_t src, uint32_t dst) {
        // if src slot is already cached, reuse it as the value to store into dst
        arch::Gp srcp = emit_ir_slot_addr(src, "ir_cp_s");
        arch::Gp dstp = emit_ir_slot_addr(dst, "ir_cp_d");
        arch::Gp raw = cc.new_gp64("ir_cp_raw");
        arch::Gp cached;
        if (slot_cache.try_get(src, cached)) {
            arch::mov_reg(cc, raw, cached);
        } else {
            arch::load(cc, raw, arch::ptr(srcp, 0));
            slot_cache.set(src, raw);
        }
        arch::store(cc, arch::ptr(dstp, 0), raw);
        // dst now holds `raw`; propagate to cache.
        slot_cache.set(dst, raw);
    };
    auto emit_ir_pop = [&]() {
        // Value is NaN-boxed, trivially copyable, with no destructor (heap lifetime is GC-managed)
        arch::Gp endp = fc.get();
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
    };
    auto emit_ir_dup = [&]() {
        // Plain 8-byte copy. Value has no dtor/refcount, so dup of a heap value is
        // the same raw store as a non-heap one. 
        // Capacity check is covered by the function-entry reservation.
        arch::Gp endp = fc.get();
        arch::Gp raw = cc.new_gp64("ir_dup_raw");
        arch::load(cc, raw, arch::ptr(endp, (int)(-ValSize)));
        arch::store(cc, arch::ptr(endp, 0), raw);
        arch::add_imm(cc, endp, (int)ValSize);
        fc.set(endp);
    };
    auto emit_ir_load_global = [&](uint32_t name_idx) {
        // Inlines jit_load_global's fast path, name_idx is a compile-time constant
        Label slow = cc.new_label();
        Label done = cc.new_label();

        // name_idx < global_cache_valid.size() <=> (vstart + name_idx) < vfin
        arch::Gp vaddr = cc.new_gp64("ir_lg_vaddr");
        arch::Gp vfin = cc.new_gp64("ir_lg_vfin");
        arch::load(cc, vaddr, arch::ptr(vm_reg, (int)VMGlobalCacheValidStartOff));
        arch::load(cc, vfin, arch::ptr(vm_reg, (int)VMGlobalCacheValidFinishOff));
        arch::add_imm(cc, vaddr, (int64_t)name_idx);
        cc.cmp(vaddr, vfin);
        arch::jcc(cc, arch::CC::kFGE, slow);

        // global_cache_valid[name_idx] != 0
        arch::Gp valid = cc.new_gp64("ir_lg_valid");
        arch::load8_zx(cc, valid, arch::ptr8(vaddr, 0));
        arch::test_zero(cc, valid);
        arch::jcc(cc, arch::CC::kEQ, slow);

        // stack capacity check covered by the function-entry reservation.
        arch::Gp endp = fc.get();

        // push global_cache[name_idx] (plain 8-byte copy; no write barrier / refcount)
        arch::Gp cstart = cc.new_gp64("ir_lg_cstart");
        arch::load(cc, cstart, arch::ptr(vm_reg, (int)VMGlobalCacheStartOff));
        arch::Gp raw = cc.new_gp64("ir_lg_raw");
        const int64_t cache_off = (int64_t)name_idx * (int64_t)ValSize;
        if (cache_off >= INT32_MIN && cache_off <= INT32_MAX) {
            arch::load(cc, raw, arch::ptr(cstart, (int)cache_off));
        } else {
            arch::add_imm(cc, cstart, cache_off);
            arch::load(cc, raw, arch::ptr(cstart, 0));
        }
        arch::store(cc, arch::ptr(endp, 0), raw);
        arch::add_imm(cc, endp, (int)ValSize);
        fc.set(endp);
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32((const void *)jit_load_global, name_idx);
        fc.merge_slow_path();
        cc.bind(done);
    };
    auto emit_ir_fconst = [&](double imm) {
        // FConst's operand is a compile-time constant double. make_float()
        // memcpys the bits straight into _raw with no tag, so the pushed
        // value is just the f64 bit pattern. Never a heap pointer, no write
        // barrier. Mirrors emit_ir_iconst exactly.
        int64_t bits;
        memcpy(&bits, &imm, sizeof(bits));
        arch::Gp endp = fc.get();
        arch::Gp rawr = cc.new_gp64("ir_fc_raw");
        cc.mov(rawr, Imm(bits));
        arch::store(cc, arch::ptr(endp, 0), rawr);
        arch::add_imm(cc, endp, (int)ValSize);
        fc.set(endp);
    };
    auto emit_ir_iconst = [&](int64_t imm) {
        const uint64_t raw = Value::make_int_checked(imm).raw_bits();
        arch::Gp endp = fc.get();
        arch::Gp rawr = cc.new_gp64("ir_ic_raw");
        cc.mov(rawr, Imm((int64_t)raw));
        arch::store(cc, arch::ptr(endp, 0), rawr);
        arch::add_imm(cc, endp, (int)ValSize);
        fc.set(endp);
    };
    // Push a string constant inline. 
    // LoadConst only carries STRING/FUNCTION/NONE (ir_build siphons INT->IConst, FLOAT->FConst).
    auto emit_ir_sconst_inline = [&](uint32_t const_idx) -> bool {
        auto &fmeta = chunk.functions[chunk_idx];
        if (const_idx >= fmeta.constants.size()) {
            return false;
        }
        const auto &cst = fmeta.constants[const_idx];
        if (cst.type != nari::bytecode::Constant::Type::STRING) {
            return false;
        }
        // get_const_string mutates only the lazy cache (logically const); the
        // returned Value is rooted for the chunk's lifetime.
        auto &mut_chunk = const_cast<nari::bytecode::Chunk &>(chunk);
        const uint64_t raw = mut_chunk.get_const_string(cst.string_idx).raw_bits();
        arch::Gp endp = fc.get();
        arch::Gp rawr = cc.new_gp64("ir_sc_raw");
        cc.mov(rawr, Imm((int64_t)raw));
        arch::store(cc, arch::ptr(endp, 0), rawr);
        arch::add_imm(cc, endp, (int)ValSize);
        fc.set(endp);
        return true;
    };
    auto emit_ir_call_method = [&](const ir::Inst &in) {
        uint32_t method_idx = in.imm_u32;
        uint32_t argc = (uint32_t)in.imm_int;
        // receiver is a LoadSlot of a proven int-array slot and the push arg is Int48. 
        bool push_skip_arr_guard = false;
        bool push_skip_val_guard = false;
        if (!in.operands.empty()) {
            const ir::Inst &recv = irFuncs.inst(in.operands[0]);
            if (recv.op == ir::Op::LoadSlot &&
                int_arr_slots.count(recv.imm_u32)) {
                push_skip_arr_guard = true;
            }
        }
        if (in.operands.size() >= 2) {
            const ir::Inst &arg = irFuncs.inst(in.operands[1]);
            if (arg.type == ir::Ty::Int48) {
                push_skip_val_guard = true;
            }
        }
        if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "length" && argc == 0) {
            call0((const void *)jit_method_length);
        } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "char_code_at" && argc == 1) {
            call0((const void *)jit_method_char_code_at);
        } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "starts_with" && argc == 1) {
            call0((const void *)jit_method_starts_with);
        } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "substr" && argc <= 2) {
            call_u32((const void *)jit_method_substr, argc);
        } else if (method_idx < chunk.strings.size() && chunk.strings[method_idx] == "push" && argc == 1) {
            Label slow = cc.new_label();
            Label done = cc.new_label();

            arch::Gp endp = fc.get();
            arch::Gp tag = cc.new_gp64("ir_push_tag");

            // keep only the trivial-value case inline
            if (!push_skip_val_guard) {
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
                arch::jcc(cc, arch::CC::kEQ, slow);
            }

            if (!push_skip_arr_guard) {
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
                arch::jcc(cc, arch::CC::kNE, slow);
            }

            arch::Gp arr = cc.new_gp64("ir_push_arr");
            arch::load(cc, arr, arch::ptr(endp, (int)(-2 * ValSize)));
            arch::zero_extend_48(cc, arr);
            if (!push_skip_arr_guard) {
                arch::cmp_mem8_imm(cc, arch::ptr8(arr, (int)HeapTypeTagOff), Imm((int)tagArray));
                arch::jcc(cc, arch::CC::kNE, slow);
            }

            arch::Gp fin = cc.new_gp64("ir_push_fin");
            arch::Gp cap = cc.new_gp64("ir_push_cap");
            arch::load(cc, fin, arch::ptr(arr, (int)ArrayVecFinishOff));
            arch::load(cc, cap, arch::ptr(arr, (int)ArrayVecCapacityOff));
            cc.cmp(fin, cap);
            arch::jcc(cc, arch::CC::kEQ, slow);

            arch::Gp val = cc.new_gp64("ir_push_val");
            arch::load(cc, val, arch::ptr(endp, (int)(-ValSize)));
            arch::store(cc, arch::ptr(fin, 0), val);
            arch::add_imm(cc, fin, (int)ValSize);
            arch::store(cc, arch::ptr(arr, (int)ArrayVecFinishOff), fin);

            arch::Gp none = cc.new_gp64("ir_push_none");
            cc.mov(none, Imm(static_cast<int64_t>(nbNone)));
            arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), none);
            arch::sub_imm(cc, endp, (int)ValSize);
            fc.set(endp);
            arch::jmp(cc, done);

            cc.bind(slow);
            call_u32_u32((const void *)jit_call_method, method_idx, argc);
            fc.merge_slow_path();
            cc.bind(done);
        } else {
            call_u32_u32((const void *)jit_call_method, method_idx, argc);
        }
    };
    auto emit_ir_load_index = [&](bool skip_arr_guard, bool skip_idx_guard) {
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        arch::Gp tag = cc.new_gp64("ir_gi_tag");

        if (!skip_idx_guard) {
            arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        if (!skip_arr_guard) {
            arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
            arch::jcc(cc, arch::CC::kNE, slow);
        }

        arch::Gp idx = cc.new_gp64("ir_gi_idx");
        arch::load(cc, idx, arch::ptr(endp, (int)(-ValSize)));
        arch::sign_extend_48(cc, idx);

        arch::Gp arr = cc.new_gp64("ir_gi_arr");
        arch::load(cc, arr, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::zero_extend_48(cc, arr);
        if (!skip_arr_guard) {
            arch::cmp_mem8_imm(cc, arch::ptr8(arr, (int)HeapTypeTagOff), Imm((int)tagArray));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        arch::Gp start = cc.new_gp64("ir_gi_start");
        arch::Gp size = cc.new_gp64("ir_gi_size");
        arch::load(cc, start, arch::ptr(arr, (int)ArrayVecStartOff));
        arch::load(cc, size, arch::ptr(arr, (int)ArrayVecFinishOff));
        arch::sub2(cc, size, start);
        arch::shr(cc, size, 3);

        Label positive = cc.new_label();
        arch::jns(cc, idx, positive);
        arch::add2(cc, idx, size);
        arch::jns(cc, idx, positive);
        arch::jmp(cc, slow);
        cc.bind(positive);
        cc.cmp(idx, size);
        arch::jcc(cc, arch::CC::kGE, slow);

        arch::Gp elem = cc.new_gp64("ir_gi_elem");
        arch::shl(cc, elem, idx, 3);
        arch::add2(cc, elem, start);
        
        arch::Gp val = cc.new_gp64("ir_gi_val");
        arch::load(cc, val, arch::ptr(elem, 0));
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), val);
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
        arch::jmp(cc, done);

        cc.bind(slow);
        call0((const void *)jit_get_index);
        fc.merge_slow_path();
        cc.bind(done);
    };
    auto emit_ir_store_index = [&](bool skip_arr_guard, bool skip_idx_guard) {
        // Fast path for array[int] = val with in-bounds idx.
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        arch::Gp tag = cc.new_gp64("ir_si_tag");

        if (!skip_idx_guard) {
            // key (at endp - 2*ValSize) must be int
            arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        if (!skip_arr_guard) {
            // obj (at endp - 3*ValSize) must be heap
            arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-3 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
            arch::jcc(cc, arch::CC::kNE, slow);
        }

        arch::Gp idx = cc.new_gp64("ir_si_idx");
        arch::load(cc, idx, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::sign_extend_48(cc, idx);

        arch::Gp arr = cc.new_gp64("ir_si_arr");
        arch::load(cc, arr, arch::ptr(endp, (int)(-3 * ValSize)));
        arch::zero_extend_48(cc, arr);
        if (!skip_arr_guard) {
            arch::cmp_mem8_imm(cc, arch::ptr8(arr, (int)HeapTypeTagOff), Imm((int)tagArray));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        arch::Gp start = cc.new_gp64("ir_si_start");
        arch::Gp size = cc.new_gp64("ir_si_size");
        arch::load(cc, start, arch::ptr(arr, (int)ArrayVecStartOff));
        arch::load(cc, size, arch::ptr(arr, (int)ArrayVecFinishOff));
        arch::sub2(cc, size, start);
        arch::shr(cc, size, 3);

        // bounds: handle negative-as-from-end, then require 0 <= idx < size.
        Label positive = cc.new_label();
        arch::jns(cc, idx, positive);
        arch::add2(cc, idx, size);
        arch::jns(cc, idx, positive);
        arch::jmp(cc, slow);
        cc.bind(positive);
        cc.cmp(idx, size);
        arch::jcc(cc, arch::CC::kGE, slow);

        // elem_ptr = start + idx*8
        arch::Gp elem = cc.new_gp64("ir_si_elem");
        arch::shl(cc, elem, idx, 3);
        arch::add2(cc, elem, start);

        // val (at endp - ValSize) -> *elem
        arch::Gp val = cc.new_gp64("ir_si_val");
        arch::load(cc, val, arch::ptr(endp, (int)(-ValSize)));
        arch::store(cc, arch::ptr(elem, 0), val);

        // Stack: pop obj and key (2 slots), leaving val on top.
        // Move val from endp-ValSize down to endp-3*ValSize.
        arch::store(cc, arch::ptr(endp, (int)(-3 * ValSize)), val);
        arch::sub_imm(cc, endp, (int)(2 * ValSize));
        fc.set(endp);
        arch::jmp(cc, done);

        cc.bind(slow);
        call0((const void *)jit_set_index);
        fc.merge_slow_path();
        cc.bind(done);
    };

    // when `skip_tag_check` is true, the IR has proven both operands are statically Int48
    auto emit_int_binop = [&](ir::Op op, bool skip_tag_check = false) {
        asmjit::Label slow = cc.new_label();
        asmjit::Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_tag_check) {
            arch::Gp tagA = cc.new_gp64("ib_ta");
            arch::Gp tagB = cc.new_gp64("ib_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::cmp_imm(cc, tagA.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        arch::Gp vb = cc.new_gp64("ib_vb");
        arch::Gp va = cc.new_gp64("ib_va");
        arch::load(cc, vb, arch::ptr(endp, (int)(-ValSize)));
        arch::sign_extend_48(cc, vb);
        arch::load(cc, va, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::sign_extend_48(cc, va);
        if (op == ir::Op::DynAdd || op == ir::Op::IAdd) {
            arch::add2(cc, va, vb); // va += vb
        } else {
            arch::sub2(cc, va, vb); // va -= vb
        }
        // int48 overflow -> fall to the helper (which boxes as float)
        arch::Gp ovf = cc.new_gp64("ib_ovf");
        arch::mov_reg(cc, ovf, va);
        arch::sign_extend_48(cc, ovf);
        cc.cmp(ovf, va);
        arch::jcc(cc, arch::CC::kNE, slow);
        arch::Gp itag = cc.new_gp64("ib_itag");
        arch::nanbox_encode_int(cc, va, itag);
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), va);
        arch::sub_imm(cc, endp, (int)ValSize); // pop one
        fc.set(endp);
        arch::jmp(cc, done);
        cc.bind(slow);
        call0((op == ir::Op::DynAdd || op == ir::Op::IAdd) ? (const void *)jit_add : (const void *)jit_sub);
        fc.merge_slow_path();
        cc.bind(done);
    };
    auto emit_int_mul = [&](bool skip_tag_check = false) {
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_tag_check) {
            arch::Gp tagA = cc.new_gp64("im_ta");
            arch::Gp tagB = cc.new_gp64("im_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::cmp_imm(cc, tagA.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }

        arch::Gp vb = cc.new_gp64("im_vb");
        arch::Gp va = cc.new_gp64("im_va");
        arch::load(cc, vb, arch::ptr(endp, (int)(-ValSize)));
        arch::sign_extend_48(cc, vb);
        arch::load(cc, va, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::sign_extend_48(cc, va);
        arch::imul(cc, va, va, vb);

        arch::Gp ovf = cc.new_gp64("im_ovf");
        arch::mov_reg(cc, ovf, va);
        arch::sign_extend_48(cc, ovf);
        cc.cmp(ovf, va);
        arch::jcc(cc, arch::CC::kNE, slow);
        arch::Gp itag = cc.new_gp64("im_itag");
        arch::nanbox_encode_int(cc, va, itag);
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), va);
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
        arch::jmp(cc, done);

        cc.bind(slow);
        call0((const void *)jit_mul);
        fc.merge_slow_path();
        cc.bind(done);
    };
    auto emit_int_mod = [&](bool skip_tag_check = false) {
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_tag_check) {
            arch::Gp tagA = cc.new_gp64("imod_ta");
            arch::Gp tagB = cc.new_gp64("imod_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::cmp_imm(cc, tagA.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }

        arch::Gp rhs = cc.new_gp64("imod_rhs");
        arch::Gp lhs = cc.new_gp64("imod_lhs");
        arch::load(cc, rhs, arch::ptr(endp, (int)(-ValSize)));
        arch::sign_extend_48(cc, rhs);
        arch::load(cc, lhs, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::sign_extend_48(cc, lhs);
        arch::test_zero(cc, rhs);
        arch::jcc(cc, arch::CC::kEQ, slow);
        arch::cmp_imm(cc, rhs, Imm(-1));
        arch::jcc(cc, arch::CC::kEQ, slow);

        arch::Gp hi = cc.new_gp64("imod_hi");
        cc.mov(hi, lhs);
        arch::sar(cc, hi, 63);
        arch::smod_only(cc, hi, lhs, rhs);
        arch::Gp itag = cc.new_gp64("imod_itag");
        arch::nanbox_encode_int(cc, hi, itag);
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), hi);
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
        arch::jmp(cc, done);

        cc.bind(slow);
        call0((const void *)jit_mod);
        fc.merge_slow_path();
        cc.bind(done);
    };
    auto emit_int_cmp = [&](ir::Op op, bool skip_tag_check = false) {
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_tag_check) {
            arch::Gp tagA = cc.new_gp64("ic_ta");
            arch::Gp tagB = cc.new_gp64("ic_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::cmp_imm(cc, tagA.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }

        arch::Gp rhs = cc.new_gp64("ic_rhs");
        arch::Gp lhs = cc.new_gp64("ic_lhs");
        arch::load(cc, rhs, arch::ptr(endp, (int)(-ValSize)));
        arch::sign_extend_48(cc, rhs);
        arch::load(cc, lhs, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::sign_extend_48(cc, lhs);
        cc.cmp(lhs, rhs);
        arch::CC::Cond cond = arch::CC::kEQ;
        switch (op) {
            case ir::Op::DynCmpLt:
            case ir::Op::ICmpLt:
                cond = arch::CC::kLT;
                break;
            case ir::Op::DynCmpLe:
            case ir::Op::ICmpLe:
                cond = arch::CC::kLE;
                break;
            case ir::Op::DynCmpGt:
            case ir::Op::ICmpGt:
                cond = arch::CC::kGT;
                break;
            case ir::Op::DynCmpGe:
            case ir::Op::ICmpGe:
                cond = arch::CC::kGE;
                break;
            case ir::Op::DynCmpEq:
            case ir::Op::ICmpEq:
                cond = arch::CC::kEQ;
                break;
            default:
                cond = arch::CC::kNE;
                break;
        }
        arch::Gp out = cc.new_gp64("ic_out");
        arch::cset(cc, out, cond);
        arch::or_imm(cc, out, out, (int64_t)nbBoolTag);
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), out);
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
        // When the tag guards above were elided, no path references `slow`,
        // and emitting it as a dead block confuses asmjit's RA liveness
        // analysis (Compiler RA crashes when computing liveness for a block
        // with no predecessor and a vreg-using call). Skip the slow tail
        // entirely in that case; the fast path is straight-line.
        if (skip_tag_check) {
            cc.bind(done);
            return;
        }
        arch::jmp(cc, done);

        cc.bind(slow);
        switch (op) {
            case ir::Op::DynCmpLt:
                call0((const void *)jit_lt);
                break;
            case ir::Op::DynCmpLe:
                call0((const void *)jit_le);
                break;
            case ir::Op::DynCmpGt:
                call0((const void *)jit_gt);
                break;
            case ir::Op::DynCmpGe:
                call0((const void *)jit_ge);
                break;
            case ir::Op::DynCmpEq:
                call0((const void *)jit_eq);
                break;
            default:
                call0((const void *)jit_ne);
                break;
        }
        fc.merge_slow_path();
        cc.bind(done);
    };
    auto emit_float_cmp = [&](ir::Op op) {
        // no slow path, uses fc directly.
        arch::Gp endp = fc.get();
        arch::Vec fa = arch::new_vec_f64(cc, "fc_a");
        arch::Vec fb = arch::new_vec_f64(cc, "fc_b");
        arch::load_f64(cc, fb, arch::ptr(endp, (int)(-ValSize)));
        arch::load_f64(cc, fa, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::Gp out = cc.new_gp64("fc_out");
        arch::float_cmp(cc, fa, fb);
        if (op == ir::Op::FCmpEq) {
            arch::float_to_bool_eq(cc, out, true);
        } else if (op == ir::Op::FCmpNe) {
            arch::float_to_bool_eq(cc, out, false);
        } else {
            bool swap = (op == ir::Op::FCmpLt || op == ir::Op::FCmpLe);
            arch::CC::Cond fcc = (op == ir::Op::FCmpLt || op == ir::Op::FCmpGt) ? arch::CC::kFGT : arch::CC::kFGE;
            if (swap) {
                arch::float_cmp(cc, fb, fa);
            }
            arch::cset(cc, out, fcc);
        }
        arch::Gp bool_tag = cc.new_gp64("fc_btag");
        cc.mov(bool_tag, Imm((int64_t)nbBoolTag));
        arch::or_reg(cc, out, bool_tag);
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), out);
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
    };
    // lower a straight-line (non-terminator) instruction.
    auto lower_body = [&](const ir::Inst &in) {
        switch (in.op) {
            case ir::Op::IConst:
                emit_ir_iconst(in.imm_int);
                break;
            case ir::Op::FConst:
                emit_ir_fconst(in.imm_float);
                break;
            case ir::Op::BConst:
                call0(in.imm_int ? (const void *)jit_load_true : (const void *)jit_load_false);
                break;
            case ir::Op::NConst:
                call0((const void *)jit_load_none);
                break;
            case ir::Op::LoadConst:
                if (!emit_ir_sconst_inline(in.imm_u32)) {
                    call_u32((const void *)jit_load_const, in.imm_u32);
                }
                break;
            case ir::Op::LoadSlot:
                emit_ir_load_slot(in.imm_u32);
                break;
            case ir::Op::StoreSlot:
                emit_ir_store_slot(in.imm_u32);
                break;
            case ir::Op::LoadGlobal:
                emit_ir_load_global(in.imm_u32);
                break;
            case ir::Op::StoreGlobal:
                call_u32((const void *)jit_store_global, in.imm_u32);
                break;
            case ir::Op::LoadCapture:
                call_u32((const void *)jit_load_capture, in.imm_u32);
                break;
            case ir::Op::StoreCapture:
                call_u32((const void *)jit_store_capture, in.imm_u32);
                break;
            case ir::Op::Pop:
                emit_ir_pop();
                break;
            case ir::Op::Dup:
                emit_ir_dup();
                break;
            case ir::Op::StoreImmSlot:
                emit_ir_store_imm_slot(in.imm_int, in.imm_u32);
                break;
            case ir::Op::StoreBImmSlot:
                emit_ir_store_bimm_slot(in.imm_int, in.imm_u32);
                break;
            case ir::Op::StoreNSlot:
                emit_ir_store_n_slot(in.imm_u32);
                break;
            case ir::Op::StoreCSlot:
                emit_ir_store_c_slot((uint32_t)in.imm_int, in.imm_u32);
                break;
            case ir::Op::CopySlot:
                emit_ir_copy_slot((uint32_t)in.imm_int, in.imm_u32);
                break;
            case ir::Op::Not:
                call0((const void *)jit_not);
                break;
            case ir::Op::INeg:
                call0((const void *)jit_neg);
                break;
            case ir::Op::IAnd:
                call0((const void *)jit_bit_and);
                break;
            case ir::Op::IOr:
                call0((const void *)jit_bit_or);
                break;
            case ir::Op::IXor:
                call0((const void *)jit_bit_xor);
                break;
            case ir::Op::INot:
                call0((const void *)jit_bit_not);
                break;
            case ir::Op::IShl:
                call0((const void *)jit_lshift);
                break;
            case ir::Op::IShr:
                call0((const void *)jit_rshift);
                break;
            case ir::Op::LoadIndex: {
                // skip array guard when array operand is LoadSlot of an int_arr_slot, skip guard if index is Int48
                bool sa = false, si = false;
                if (in.operands.size() >= 1) {
                    const ir::Inst &obj = irFuncs.inst(in.operands[0]);
                    if (obj.op == ir::Op::LoadSlot &&
                        int_arr_slots.count(obj.imm_u32)) {
                        sa = true;
                    }
                }
                if (in.operands.size() >= 2 &&
                    irFuncs.inst(in.operands[1]).type == ir::Ty::Int48) {
                    si = true;
                }
                emit_ir_load_index(sa, si);
                break;
            }
            case ir::Op::StoreIndex: {
                bool sa = false, si = false;
                if (in.operands.size() >= 1) {
                    const ir::Inst &obj = irFuncs.inst(in.operands[0]);
                    if (obj.op == ir::Op::LoadSlot &&
                        int_arr_slots.count(obj.imm_u32)) {
                        sa = true;
                    }
                }
                if (in.operands.size() >= 2 &&
                    irFuncs.inst(in.operands[1]).type == ir::Ty::Int48) {
                    si = true;
                }
                emit_ir_store_index(sa, si);
                break;
            }
            case ir::Op::LoadProperty:
                call_u32((const void *)jit_get_property, in.imm_u32);
                break;
            case ir::Op::StoreProperty:
                call_u32((const void *)jit_set_property, in.imm_u32);
                break;
            case ir::Op::MakeArray:
                call_u32((const void *)jit_make_array, in.imm_u32);
                break;
            case ir::Op::MakeObject: {
                const uint8_t *site =
                    chunk.functions[chunk_idx].code.data() +
                    in.bytecode_pc + 1;
                call_u32_u64((const void *)jit_make_object_site, in.imm_u32, (uint64_t)(uintptr_t)site);
                break;
            }
            case ir::Op::StrConcat:
                // imm_u32==1: mark_inplace_concat proved the lhs is a single-use
                // StrConcat result (fresh, uniquely-owned mutable string), so we
                // append into its buffer instead of copy+alloc.
                call0((const void *)(in.imm_u32 ? jit_str_concat_inplace
                                                : jit_str_concat));
                break;
            case ir::Op::FormatValue:
                call_u32((const void *)jit_format_value, in.imm_u32);
                break;
            case ir::Op::IterArray:
                call0((const void *)jit_iter_array);
                break;
            case ir::Op::Call:
                emit_ir_call_value(in.imm_u32);
                break;
            case ir::Op::CallMethod:
                emit_ir_call_method(in);
                break;
            case ir::Op::DynAdd:
            case ir::Op::IAdd:
                if (both_float(in)) {
                    emit_float_binop(in.op);
                } else {
                    // when IR proves both operands Int48, skip the tag guard.
                    emit_int_binop(in.op, both_int(in));
                }
                break;
            case ir::Op::DynSub:
            case ir::Op::ISub:
                if (both_float(in)) {
                    emit_float_binop(in.op);
                } else {
                    // Inline int fast-path with a runtime tag guard; slow path -> jit_sub.
                    emit_int_binop(in.op, both_int(in));
                }
                break;
            case ir::Op::DynMul:
            case ir::Op::IMul:
                if (both_float(in)) {
                    emit_float_binop(in.op);
                } else {
                    // Inline int fast-path with a runtime tag guard; slow path -> jit_mul.
                    emit_int_mul(both_int(in));
                }
                break;
            case ir::Op::DynMod:
            case ir::Op::IMod:
                // Inline int fast-path with a runtime tag guard; slow path -> jit_mod.
                emit_int_mod(both_int(in));
                break;
            case ir::Op::DynDiv:
            case ir::Op::FAdd:
            case ir::Op::FSub:
            case ir::Op::FMul:
            case ir::Op::FDiv:
                if (both_float(in)) {
                    emit_float_binop(in.op);
                } else {
                    call0((const void *)jit_div);
                }
                break;
            case ir::Op::DynCmpLt:
            case ir::Op::DynCmpLe:
            case ir::Op::DynCmpGt:
            case ir::Op::DynCmpGe:
            case ir::Op::DynCmpEq:
            case ir::Op::DynCmpNe:
            case ir::Op::ICmpLt:
            case ir::Op::ICmpLe:
            case ir::Op::ICmpGt:
            case ir::Op::ICmpGe:
            case ir::Op::ICmpEq:
            case ir::Op::ICmpNe:
                // Inline int fast-path with a runtime tag guard; slow path falls back
                // to the matching jit_lt/le/gt/ge/eq/ne helper.
                // 
                // when IR proves both operands Int48, skip the tag guard.
                emit_int_cmp(in.op, both_int(in));
                break;
            case ir::Op::FCmpLt:
            case ir::Op::FCmpLe:
            case ir::Op::FCmpGt:
            case ir::Op::FCmpGe:
            case ir::Op::FCmpEq:
            case ir::Op::FCmpNe:
                emit_float_cmp(in.op);
                break;
            default:
                ok = false; // op not handled by IR lowering; VM interprets it
                unhandled_op = in.op;
                break;
        }
    };
    // lower a block terminator (Jump/Branch/Return).
    auto lower_term = [&](const ir::Inst &t) {
        // any deferred finish-store must hit memory before control leaves the block
        fc.flush_invalidate();
        switch (t.op) {
            case ir::Op::Jump:
                arch::jmp(cc, blabels[t.target0]);
                break;
            case ir::Op::Branch: {
                // Fast path: condition is statically Bool. The boxed bool value
                // is `NB_BOOL_TAG | (truth ? 1 : 0)`; the NaN-box tag has zero
                // in the low byte, so the low byte of the Value's raw bits IS
                // the truth bit (Value is 8 bytes, little-endian, _raw is the
                // only field). Inline a load-low-byte + pop + test instead of
                // calling jit_check_truthy.
                bool cond_is_bool = !t.operands.empty() &&
                                    t.operands[0] != ir::InvalidValue &&
                                    irFuncs.inst(t.operands[0]).type == ir::Ty::Bool;
                if (cond_is_bool) {
                    arch::Gp endp = fc.get();
                    arch::Gp truth = cc.new_gp64("ir_br_truth");
                    // load the low byte of the top-of-stack Value (Value is at
                    // [endp - ValSize], low byte at the same offset on LE).
                    arch::load8_zx(cc, truth, arch::ptr8(endp, (int)(-ValSize)));
                    arch::sub_imm(cc, endp, (int)ValSize);
                    fc.set(endp);
                    arch::test_zero(cc, truth);                       // ZF=1 iff truth==0 (falsy)
                    arch::jcc(cc, arch::CC::kNE, blabels[t.target0]); // truthy
                    arch::jmp(cc, blabels[t.target1]);                // falsy
                    break;
                }
                // jit_check_truthy pops the condition and returns 1 (truthy) / 0.
                fc.flush_invalidate();
                InvokeNode *inv;
                arch::invoke_imm(
                    cc,
                    &inv,
                    (uint64_t)(uintptr_t)jit_check_truthy,
                    FuncSignature::build<int64_t, void *>());
                inv->set_arg(0, vm_reg);
                arch::Gp truth = cc.new_gp64("ir_truth");
                inv->set_ret(0, truth);
                arch::test_zero(cc, truth);                       // ZF=1 iff truth==0 (falsy)
                arch::jcc(cc, arch::CC::kNE, blabels[t.target0]); // truthy
                arch::jmp(cc, blabels[t.target1]);                // falsy
                break;
            }
            case ir::Op::Return:
                if (t.operands.empty()) {
                    call0((const void *)jit_load_none); // implicit return none
                }
                fc.flush_invalidate();
                emit_inline_return(cc, vm_reg);
                cc.ret();
                break;
            default:
                ok = false;
                unhandled_op = t.op;
                break;
        }
    };
    auto is_cmp_op = [](ir::Op op) {
        return op == ir::Op::DynCmpLt ||
               op == ir::Op::DynCmpLe ||
               op == ir::Op::DynCmpGt ||
               op == ir::Op::DynCmpGe ||
               op == ir::Op::DynCmpEq ||
               op == ir::Op::DynCmpNe ||
               op == ir::Op::ICmpLt ||
               op == ir::Op::ICmpLe ||
               op == ir::Op::ICmpGt ||
               op == ir::Op::ICmpGe ||
               op == ir::Op::ICmpEq ||
               op == ir::Op::ICmpNe;
    };
    // Fusable only when both operands are statically Float and the op is an
    // ordered relational (Lt/Le/Gt/Ge). FCmpEq/Ne need NaN-aware boolean
    // synthesis that doesn't trivially map to a single jcc, so we keep those
    // on the slow (synthesize bool, then test/jcc) path.
    auto is_fcmp_branch_fusable = [&](const ir::Inst &cmp) {
        switch (cmp.op) {
            case ir::Op::FCmpLt:
            case ir::Op::FCmpLe:
            case ir::Op::FCmpGt:
            case ir::Op::FCmpGe:
                break;
            default:
                return false;
        }
        return irFuncs.inst(cmp.operands[0]).type == ir::Ty::Float &&
               irFuncs.inst(cmp.operands[1]).type == ir::Ty::Float;
    };
    auto lower_fused_float_cmp_branch = [&](const ir::Inst &cmp, const ir::Inst &term) {
        // terminator branches out of this block. 
        // Both successors re-enter via `cc.bind(blabels[..])`
        arch::Gp endp = fc.get();
        arch::Vec fa = arch::new_vec_f64(cc, "ffcb_a");
        arch::Vec fb = arch::new_vec_f64(cc, "ffcb_b");
        arch::load_f64(cc, fb, arch::ptr(endp, (int)(-ValSize)));     // rhs
        arch::load_f64(cc, fa, arch::ptr(endp, (int)(-2 * ValSize))); // lhs
        arch::sub_imm(cc, endp, (int)(2 * ValSize));
        fc.set(endp);
        bool swap = (cmp.op == ir::Op::FCmpLt || cmp.op == ir::Op::FCmpLe);
        arch::CC::Cond fcc =
            (cmp.op == ir::Op::FCmpLt || cmp.op == ir::Op::FCmpGt) ? arch::CC::kFGT : arch::CC::kFGE;
        if (swap) {
            arch::float_cmp(cc, fb, fa);
        } else {
            arch::float_cmp(cc, fa, fb);
        }
        arch::jcc(cc, fcc, blabels[term.target0]);
        arch::jmp(cc, blabels[term.target1]);
    };
    auto lower_fused_int_cmp_branch = [&](const ir::Inst &cmp, const ir::Inst &term) {
        Label slow = cc.new_label();
        arch::Gp endp = fc.get();
        arch::Gp tagA = cc.new_gp64("fcb_ta");
        arch::Gp tagB = cc.new_gp64("fcb_tb");
        arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
        arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
        arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
        arch::jcc(cc, arch::CC::kNE, slow);
        arch::cmp_imm(cc, tagA.r32(), Imm((int)tagInt));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp rhs = cc.new_gp64("fcb_rhs");
        arch::Gp lhs = cc.new_gp64("fcb_lhs");
        arch::load(cc, rhs, arch::ptr(endp, (int)(-ValSize)));
        arch::sign_extend_48(cc, rhs);
        arch::load(cc, lhs, arch::ptr(endp, (int)(-2 * ValSize)));
        arch::sign_extend_48(cc, lhs);
        // Pop both operands BEFORE the compare: the stack-pointer sub sets EFLAGS,
        // so it must not sit between `cmp` and the `jcc` that reads its result.
        arch::sub_imm(cc, endp, (int)(2 * ValSize));
        fc.set(endp);
        cc.cmp(lhs, rhs);

        arch::CC::Cond cond = arch::CC::kEQ;
        switch (cmp.op) {
            case ir::Op::DynCmpLt:
            case ir::Op::ICmpLt:
                cond = arch::CC::kLT;
                break;
            case ir::Op::DynCmpLe:
            case ir::Op::ICmpLe:
                cond = arch::CC::kLE;
                break;
            case ir::Op::DynCmpGt:
            case ir::Op::ICmpGt:
                cond = arch::CC::kGT;
                break;
            case ir::Op::DynCmpGe:
            case ir::Op::ICmpGe:
                cond = arch::CC::kGE;
                break;
            case ir::Op::DynCmpEq:
            case ir::Op::ICmpEq:
                cond = arch::CC::kEQ;
                break;
            default:
                cond = arch::CC::kNE;
                break;
        }
        arch::jcc(cc, cond, blabels[term.target0]);
        arch::jmp(cc, blabels[term.target1]);

        cc.bind(slow);
        lower_body(cmp);
        lower_term(term);
    };

    // emit blocks in id order
    std::vector<bool> reachable(irFuncs.blocks.size(), false);
    if (irFuncs.entry >= 0 && (size_t)irFuncs.entry < irFuncs.blocks.size()) {
        std::vector<ir::BlockId> stk;
        stk.push_back(irFuncs.entry);
        reachable[irFuncs.entry] = true;
        while (!stk.empty()) {
            ir::BlockId bid = stk.back();
            stk.pop_back();
            for (ir::BlockId s : irFuncs.blocks[bid].succs) {
                if (s >= 0 && (size_t)s < reachable.size() && !reachable[s]) {
                    reachable[s] = true;
                    stk.push_back(s);
                }
            }
        }
    }
    for (size_t bid = 0; bid < irFuncs.blocks.size() && ok; bid++) {
        if (!reachable[bid]) {
            continue; // unreachable after optimization -> don't lower
        }
        const ir::Block &b = irFuncs.blocks[bid];
        cc.bind(blabels[bid]);
        // every predecessor flushed before its terminator
        fc.invalidate();
        // the cached vreg reflects whatever the pred wrote; on a back-edge or divergent
        // control-flow join, the cached vreg is stale.
        slot_cache.invalidate_all();
        bool fuse_cmp_branch = false;
        bool fuse_fcmp_branch = false;
        if (b.terminator != ir::InvalidValue && !b.insts.empty()) {
            const ir::Inst &term = irFuncs.inst(b.terminator);
            const ir::Inst &last = irFuncs.inst(b.insts.back());
            bool branch_to_last = term.op == ir::Op::Branch &&
                                  !term.operands.empty() &&
                                  term.operands[0] == b.insts.back();
            // Fuse compare-and-branch for both statically-typed int compares and
            // dynamically-typed compares; the fused emitter has a runtime tag guard
            // whose slow path falls back to the generic cmp + branch lowering.
            fuse_cmp_branch = branch_to_last && is_cmp_op(last.op);
            // Static-Float ordered cmp + branch -> ucomisd + jcc, skipping
            // bool synthesis and check_truthy. Disjoint with the int fuse above.
            if (!fuse_cmp_branch && branch_to_last && is_fcmp_branch_fusable(last)) {
                fuse_fcmp_branch = true;
            }
        }
        bool any_fuse = fuse_cmp_branch || fuse_fcmp_branch;
        size_t body_n = any_fuse ? b.insts.size() - 1 : b.insts.size();
        for (size_t ii = 0; ii < body_n; ii++) {
            ir::ValueId i = b.insts[ii];
            lower_body(irFuncs.inst(i));
            if (!ok) {
                break;
            }
        }
        if (ok && b.terminator != ir::InvalidValue) {
            if (fuse_cmp_branch) {
                lower_fused_int_cmp_branch(irFuncs.inst(b.insts.back()), irFuncs.inst(b.terminator));
            } else if (fuse_fcmp_branch) {
                lower_fused_float_cmp_branch(irFuncs.inst(b.insts.back()), irFuncs.inst(b.terminator));
            } else {
                lower_term(irFuncs.inst(b.terminator));
            }
        }
    }
    if (!ok) {
        if (kJitReport) {
            fprintf(stderr, "[JIT] %-30s BAIL general (unhandled op %s)\n",
                    chunk.functions[chunk_idx].name.empty()
                        ? "<anon>"
                        : chunk.functions[chunk_idx].name.c_str(),
                    ir::op_name(unhandled_op));
        }
        return nullptr; // unhandled op; VM runs this function in the interpreter
    }

    cc.end_func();
    if (getenv("NARI_JIT_DUMP_NODES") != nullptr) {
        asmjit::String nodes_sb;
        asmjit::FormatOptions fo;
        asmjit::Formatter::format_node_list(nodes_sb, fo, &cc);
        fprintf(stderr, "==== pre-RA nodes for '%s' ====\n%s\n",
                chunk.functions[chunk_idx].name.c_str(), nodes_sb.data());
    }
    Error err = cc.finalize();
    if (err != kErrorOk) {
        if (kJitReport) {
            fprintf(stderr, "[JIT] %-30s BAIL asmjit finalize err=%u (%s)\n",
                    chunk.functions[chunk_idx].name.empty()
                        ? "<anon>"
                        : chunk.functions[chunk_idx].name.c_str(),
                    err, asmjit::DebugUtils::error_as_string(err));
            if (jit_dump_asm_enabled()) {
                fprintf(stderr, "---- partial asm ----\n%s---------------------\n",
                        asm_logger.data());
            }
        }
        return nullptr;
    }
    size_t generated_code_size = code_holder.code_size();
    CompiledFunc fn = nullptr;
    err = this->rt.add(&fn, &code_holder);
    jit_dump_asm(
        chunk.functions[chunk_idx].name.empty()
            ? "<anon> [ir]"
            : chunk.functions[chunk_idx].name + " [ir]",
        asm_logger.data());
    if (err != kErrorOk || !fn) {
        return nullptr;
    }
    {
        std::string sym = chunk.functions[chunk_idx].name.empty()
                              ? std::string("anon_ir")
                              : chunk.functions[chunk_idx].name + "_ir";
        register_gdb_jit_function(
            sym, reinterpret_cast<const void *>(fn), generated_code_size);
        perf_jitdump_register(
            sym, reinterpret_cast<const void *>(fn), generated_code_size);
    }
    report("OK  [ir-stack general path]");
    return fn;
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
