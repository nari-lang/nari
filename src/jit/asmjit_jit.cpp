#ifndef DISABLE_JIT
#include "asmjit_jit.h"
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
        auto add = [&](const void *p, const char *name) { t.emplace(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)), name); };

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
        NARI_JIT_SYM(jit_str_append_slot);
        NARI_JIT_SYM(jit_format_value);
        NARI_JIT_SYM(jit_bit_and);
        NARI_JIT_SYM(jit_bit_or);
        NARI_JIT_SYM(jit_bit_xor);
        NARI_JIT_SYM(jit_bit_not);
        NARI_JIT_SYM(jit_lshift);
        NARI_JIT_SYM(jit_rshift);
        NARI_JIT_SYM(jit_js_bit_binary);
        NARI_JIT_SYM(jit_js_bit_not);
        NARI_JIT_SYM(jit_not);
        NARI_JIT_SYM(jit_js_truthy);
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
        NARI_JIT_SYM(jit_string_char_code_at);
        NARI_JIT_SYM(jit_js_length);
        NARI_JIT_SYM(jit_call_spread);
        NARI_JIT_SYM(jit_return);
        NARI_JIT_SYM(jit_make_array);
        NARI_JIT_SYM(jit_array_push);
        NARI_JIT_SYM(jit_array_spread);
        NARI_JIT_SYM(jit_iter_array);
        NARI_JIT_SYM(jit_make_object);
        NARI_JIT_SYM(jit_make_object_site);
        NARI_JIT_SYM(jit_make_closure);
        NARI_JIT_SYM(jit_reserve);
        NARI_JIT_SYM(jit_get_index);
        NARI_JIT_SYM(jit_set_index);
        NARI_JIT_SYM(jit_get_property);
        NARI_JIT_SYM(jit_js_get_prop_static);
        NARI_JIT_SYM(jit_set_property);
        NARI_JIT_SYM(jit_load_capture);
        NARI_JIT_SYM(jit_store_capture);

        // NOTE: jit_spawn/jit_pow/jit_throw/jit_new_instance/jit_load_this
        //  are declared in jit_helpers.h but have no definition since the JIT never emits calls to them
        NARI_JIT_SYM(jit_call_method);
        NARI_JIT_SYM(jit_method_length);
        NARI_JIT_SYM(jit_method_char_code_at);
        NARI_JIT_SYM(jit_method_starts_with);
        NARI_JIT_SYM(jit_method_substr);
        NARI_JIT_SYM(jit_check_type);
        NARI_JIT_SYM(jit_poll_shutdown);
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
                const char *name = (it != syms.end()) ? it->second : (extra_name && addr == extra_addr) ? extra_name : nullptr;
                if (name) {
                    size_t semi = line.find(';');
                    std::string instr = line.substr(0, i) + "call " + name;
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

static void jit_dump_asm(const std::string &func_name, const char *asm_text, uint64_t extra_addr = 0, const char *extra_name = nullptr) {
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
    std::string symbolized = jit_symbolize_calls(asm_text, extra_addr, extra_name);
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
static constexpr int64_t tagString = (int64_t)ValueTag::String;     // heap type_tag byte for String
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
// JIT-visible containers expose an explicit {begin, end, capacity} layout.
static const int64_t ObjFieldsStartOff = ObjFieldsOff + offsetof(Array, storage_begin);
static const int64_t ObjFieldsFinishOff = ObjFieldsOff + offsetof(Array, storage_end);
static const int64_t ObjLazyFieldMaskOff = field_offset(&ObjectObj::lazy_field_mask);
static const int64_t ObjFrozenOff = field_offset(&ObjectObj::frozen);
static const int64_t ObjDictModeOff = field_offset(&ObjectObj::dict_mode);
static const int64_t FDJitFuncIdxOff = field_offset(&FunctionData::jit_func_idx);
static const int64_t FDLocalsCountOff = field_offset(&FunctionData::jit_locals_count);
static const int64_t FDJitMetaOff = field_offset(&FunctionData::jit_meta);
static const int64_t FDInlineKindOff = field_offset(&FunctionData::jit_inline_kind);
static const int64_t FDNativeKindOff = field_offset(&FunctionData::jit_native_kind);
static const int64_t FDInlineImmOff = field_offset(&FunctionData::jit_inline_imm);
static const int64_t FDCapturesRawOff = field_offset(&FunctionData::jit_captures_raw);
static const int64_t FDCapture0RawOff = field_offset(&FunctionData::jit_capture0_raw);
static const int64_t FDCapture1RawOff = field_offset(&FunctionData::jit_capture1_raw);
static const int64_t FDCapture2RawOff = field_offset(&FunctionData::jit_capture2_raw);
static const int64_t FMCodeDataOff = field_offset(&FunctionMeta::code) + offsetof(ByteArray, storage_begin);
static const int64_t FMRestParamIndexOff = field_offset(&FunctionMeta::rest_param_index);
static const int64_t FMParamCountOff = field_offset(&FunctionMeta::param_count);
static const int64_t FMJsUndefinedParamsOff = field_offset(&FunctionMeta::js_undefined_params);
static const int64_t VMCapturesRawOff = field_offset(&VM::jit_captures_raw);

// vm->jit_captures_raw points at a std::vector<CellRef>. Indexing it
// from generated code needs two layout facts that the standard does not guarantee:
// the vector's first two words are begin/end, and a shared_ptr's first word is the
// stored pointer. Everything else the JIT touches uses the project's own standard-
// layout `Array`, so rather than assume silently, verify both once at startup and
// fall back to calling jit_load_capture when the check fails.
static bool nari_verify_capture_vec_layout() {
    std::vector<CellRef> v;
    v.push_back(CellRef::make());
    v.push_back(CellRef::make());
    if (sizeof(CellRef) != 16 || sizeof(void *) != 8) {
        return false;
    }
    void *const *words = reinterpret_cast<void *const *>(&v);
    const void *begin = words[0];
    const void *end = words[1];
    if (begin != static_cast<const void *>(v.data()) || end != static_cast<const void *>(v.data() + v.size())) {
        return false;
    }
    for (size_t i = 0; i < v.size(); i++) {
        if (*reinterpret_cast<Value *const *>(v.data() + i) != v[i].get()) {
            return false;
        }
    }
    return true;
}
static const bool kCaptureVecLayoutOk = nari_verify_capture_vec_layout();
static const int64_t VMCapture0RawOff = field_offset(&VM::jit_capture0_raw);
static const int64_t VMCapture1RawOff = field_offset(&VM::jit_capture1_raw);
static const int64_t VMCapture2RawOff = field_offset(&VM::jit_capture2_raw);
static const int64_t VMRuntimeOff = field_offset(&VM::runtime);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static const int64_t RuntimeTypeofValuesOff = __builtin_offsetof(ScriptRuntime, typeof_values);
#pragma clang diagnostic pop

// VM::stack storage pointers.
static const int64_t VMStackStartOff = field_offset(&VM::stack) + offsetof(Array, storage_begin);
static const int64_t VMStackFinishOff = field_offset(&VM::stack) + offsetof(Array, storage_end);
static const int64_t VMStackCapacityOff = field_offset(&VM::stack) + offsetof(Array, storage_capacity);
static const int64_t VMGlobalCacheStartOff = field_offset(&VM::global_cache) + offsetof(Array, storage_begin);
static const int64_t VMGlobalCacheValidStartOff = field_offset(&VM::global_cache_valid) + offsetof(ByteArray, storage_begin);
static const int64_t VMGlobalCacheValidFinishOff = field_offset(&VM::global_cache_valid) + offsetof(ByteArray, storage_end);
static const int64_t VMJsGetPropStaticICStartOff =
    field_offset(&VM::js_get_prop_static_ic) + offsetof(JsGetPropStaticICArray, storage_begin);
static const int64_t VMJsGetPropStaticICFinishOff =
    field_offset(&VM::js_get_prop_static_ic) + offsetof(JsGetPropStaticICArray, storage_end);
static const int64_t JsGetPropStaticICShapeOff = offsetof(JsGetPropStaticIC, shape);
static const int64_t JsGetPropStaticICSlotOff = offsetof(JsGetPropStaticIC, slot);
static const int64_t JsGetPropStaticICLazyMaskOff = offsetof(JsGetPropStaticIC, lazy_mask);
static const int64_t JsGetPropStaticICShape2Off = offsetof(JsGetPropStaticIC, shape2);
static const int64_t JsGetPropStaticICSlot2Off = offsetof(JsGetPropStaticIC, slot2);
static const int64_t JsGetPropStaticICLazyMask2Off = offsetof(JsGetPropStaticIC, lazy_mask2);

static const int64_t ArrayVecStartOff = field_offset(&ArrayObj::v) + offsetof(Array, storage_begin);
static const int64_t ArrayVecFinishOff = field_offset(&ArrayObj::v) + offsetof(Array, storage_end);
static const int64_t ArrayVecCapacityOff = field_offset(&ArrayObj::v) + offsetof(Array, storage_capacity);

// VM::frames storage pointers
static const int64_t FramesStartOff = field_offset(&VM::frames) + offsetof(FrameArray, storage_begin);
static const int64_t FramesFinishOff = field_offset(&VM::frames) + offsetof(FrameArray, storage_end);
static const int64_t VMFramesCapacityOff = field_offset(&VM::frames) + offsetof(FrameArray, storage_capacity);

// CallFrame field offsets
static const int64_t FrameSize = static_cast<int64_t>(sizeof(CallFrame));
static const int64_t FrameFunctionOff = field_offset(&CallFrame::function);
static const int64_t FrameIpOff = field_offset(&CallFrame::ip);
static const int64_t SlotBaseOff = field_offset(&CallFrame::slot_base);
static const int64_t FrameCapturesOff = field_offset(&CallFrame::captures);
static const int64_t FrameClosureRootOff = field_offset(&CallFrame::closure_root);
static const int64_t FrameReceiverOff = field_offset(&CallFrame::receiver);
static const int64_t FrameInlineUpvalueIdxOff = field_offset(&CallFrame::inline_upvalue_idx);
static const int64_t FrameInlineUpvalueOff = field_offset(&CallFrame::inline_upvalue);
static const int64_t UpvalOff = field_offset(&CallFrame::open_upvalues);
static const int64_t UpvalSizeOff = UpvalOff;
static const int64_t FrameOpenUpvalOff = UpvalOff;
static const int64_t FrameSingleCaptureCacheOff = field_offset(&CallFrame::single_capture_cache);

static void jit_write_layout_legend(FILE *f) {
    static bool written = false;
    if (written) {
        return;
    }
    written = true;
    fprintf(
        f,
        "; ============================================================\n"
        "; NARI JIT asm dump. `call` targets are symbolized to helper names where known.\n"
        "; Memory operands `[base+off]` into the VM pointer (first arg) or a CallFrame use these offsets:\n"
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
        (long long)VMStackStartOff, (long long)VMStackFinishOff, (long long)VMStackCapacityOff, (long long)VMCapturesRawOff,
        (long long)VMGlobalCacheStartOff, (long long)VMGlobalCacheValidStartOff, (long long)FramesStartOff, (long long)FramesFinishOff,
        (long long)VMFramesCapacityOff, (long long)ValSize, (long long)FrameSize, (long long)FrameFunctionOff, (long long)FrameIpOff,
        (long long)SlotBaseOff, (long long)FrameCapturesOff, (long long)FrameOpenUpvalOff
    );
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

// ---------------------------------------------------------------------------
// Background register allocation + encoding.
//
// cc.finalize() is 8.5% of a tsc run (381ms of the 516ms spent compiling) and,
// unlike ir::build/ir::optimize/lowering, it reads no VM state at all: it works
// purely on the CodeHolder's node graph. So it runs on a worker while the main
// thread keeps interpreting the not-yet-compiled function. The VM already
// tolerates this: the tier-up trigger is `++call_counts[i] == JIT_THRESHOLD`,
// exact equality, so each function enqueues exactly once and simply stays
// interpreted until the pointer is published.
namespace {

struct CompileJob {
    std::unique_ptr<CodeHolder> holder;
    std::unique_ptr<arch::Compiler> cc;
    uint32_t chunk_idx = 0;
    std::string sym;
    AsmJITMethodCompiler *owner = nullptr;
};

std::deque<std::unique_ptr<CompileJob>> g_jobs;
std::mutex g_jobs_mu;
std::condition_variable g_jobs_cv; // signals: work available, or stop
std::condition_variable g_idle_cv; // signals: queue drained and nothing running
std::vector<std::thread> g_workers;
bool g_worker_stop = false;
bool g_worker_started = false;
unsigned g_jobs_active = 0; // queued + in-flight, guarded by g_jobs_mu

// Guards the JitRuntime allocator and the symbol registries, which the main
// thread also touches. Held only across rt.add(), never across finalize().
std::mutex g_rt_mu;

void compile_worker_main() {
    for (;;) {
        std::unique_ptr<CompileJob> job;
        {
            std::unique_lock<std::mutex> lk(g_jobs_mu);
            g_jobs_cv.wait(lk, [] { return g_worker_stop || !g_jobs.empty(); });
            if (g_jobs.empty()) {
                if (g_worker_stop) {
                    return;
                }
                continue;
            }
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        if (job->cc->finalize() == kErrorOk) {
            job->owner->publish_async(job->chunk_idx, job->holder.get(), job->sym);
        }
        // Release the asmjit objects before reporting idle, so a drain that
        // returns cannot race with this job still holding the CodeHolder.
        job.reset();
        {
            std::lock_guard<std::mutex> lk(g_jobs_mu);
            if (--g_jobs_active == 0) {
                g_idle_cv.notify_all();
            }
        }
    }
}

void stop_compile_worker();

// process.exit() calls libc exit() directly, so ~AsmJITMethodCompiler never runs and
// static teardown would hit a still-joinable std::thread (-> std::terminate). Registered
// once the worker starts, i.e. after every static ctor, so it runs before their dtors.
void drain_and_stop_at_exit() {
    {
        std::unique_lock<std::mutex> lk(g_jobs_mu);
        g_idle_cv.wait(lk, [] { return g_jobs_active == 0; });
    }
    stop_compile_worker();
}

// Enqueue; starts the worker on first use so a run that never tiers up pays nothing.
void enqueue_compile_job(std::unique_ptr<CompileJob> job) {
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    if (!g_worker_started) {
        g_worker_started = true;
        // RA+encode is independent per function, so extra workers shrink the window in
        // which a hot function is still being interpreted. Capped: only ~380ms of work.
        unsigned n = 2;
        if (const char *e = getenv("NARI_JIT_WORKERS")) {
            n = (unsigned)std::max(1, atoi(e));
        }
        for (unsigned i = 0; i < n; i++) {
            g_workers.emplace_back(compile_worker_main);
        }
        std::atexit(drain_and_stop_at_exit);
    }
    ++g_jobs_active;
    g_jobs.push_back(std::move(job));
    g_jobs_cv.notify_one();
}

void stop_compile_worker() {
    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        if (!g_worker_started) {
            return;
        }
        g_worker_stop = true;
    }
    g_jobs_cv.notify_all();
    for (auto &t : g_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::lock_guard<std::mutex> lk(g_jobs_mu);
    g_workers.clear();
    g_worker_started = false;
    g_worker_stop = false;
}

} // namespace

bool async_jit_enabled() {
    // asm dumping attaches a stack-local StringLogger to the CodeHolder, which would
    // dangle once the frame goes away, and validation wants deterministic reporting.
    static const bool on = getenv("NARI_DISABLE_ASYNC_JIT") == nullptr && !jit_dump_asm_enabled() &&
                           getenv("NARI_JIT_VALIDATE") == nullptr && getenv("NARI_JIT_DUMP_NODES") == nullptr;
    return on;
}

void AsmJITMethodCompiler::publish_async(uint32_t chunk_idx, asmjit::CodeHolder *holder, const std::string &sym) {
    CompiledFunc fn = nullptr;
    size_t code_size = holder->code_size();
    {
        std::lock_guard<std::mutex> lk(g_rt_mu);
        if (this->rt.add(&fn, holder) != kErrorOk || !fn) {
            return;
        }
        register_gdb_jit_function(sym, (const void *)fn, code_size);
        perf_jitdump_register(sym, (const void *)fn, code_size);
    }
    // Single aligned pointer store into a pointer-stable vector. Generated code reads
    // this slot through a baked &compiled_fn_vec[i] immediate; on x86 the store is
    // atomic and rt.add() has already made the code bytes visible.
    if (chunk_idx < compiled_fn_vec.size()) {
        compiled_fn_vec[chunk_idx] = fn;
    }
}

void AsmJITMethodCompiler::drain_async() {
    std::unique_lock<std::mutex> lk(g_jobs_mu);
    g_idle_cv.wait(lk, [] { return g_jobs_active == 0; });
}

AsmJITMethodCompiler::~AsmJITMethodCompiler() {
    drain_async();
    stop_compile_worker();
}

// reset all per-chunk state when the bound chunk changes.
// when a new chunk arrives, free all generated code via the JitRuntime
// drop all per-chunk tables, and resize fresh for the incoming chunk.
void AsmJITMethodCompiler::reset_for_chunk(const nari::bytecode::Chunk &chunk) {
    // In-flight jobs are about to have their JitRuntime destroyed and their target
    // slot in compiled_fn_vec cleared, so they must all land first.
    drain_async();
    destroy_at(&rt);
    construct_at(&rt);

    // GDB-JIT registrations refer to the just-freed code addresses
    unregister_all_gdb_jit_functions();

    compiled_fn_vec.clear();
    not_compilable.clear();

    // size both vectors to chunk.functions.size() exactly once per chunk.
    compiled_fn_vec.resize(chunk.functions.size(), nullptr);

    fn_vec_base = compiled_fn_vec.data();
    // Publish the (now pointer-stable) table for the non-virtual call-path lookup.
    set_fn_table(compiled_fn_vec.data(), compiled_fn_vec.size());

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
    typeof_name_idx_ = ir::analyze_frozen_builtin(chunk, "typeof");
    js_to_number_name_idx_ = ir::analyze_frozen_builtin(chunk, "__js_to_number");

    // Resolve the transpiler-runtime builtins that emit_ir_call_value can inline,
    // so each call site only emits the body for the builtin it actually names.
    // Same name -> jit_native_kind table as Chunk::mark_native_kinds.
    builtin_kind_by_name_idx_.clear();
    static const struct {
        const char *name;
        int kind;
    } kInlinableBuiltins[] = {
        { "__js_lt", 2 },
        { "__js_gt", 3 },
        { "__js_le", 4 },
        { "__js_ge", 5 },
        { "__js_get_prop", 6 },
        { "__js_length", 9 },
        { "__js_str_char_code_at", 10 },
        { "__js_loose_eq", 12 },
        { "__js_to_string", 14 },
        { "__js_str_code_point_at", 15 },
        { "__js_add", 16 },
    };
    for (const auto &b : kInlinableBuiltins) {
        for (uint32_t i = 0; i < chunk.strings.size(); i++) {
            if (chunk.strings[i] == b.name) {
                builtin_kind_by_name_idx_[i] = b.kind;
                break;
            }
        }
    }
}

// A Call whose callee resolves at compile time to one specific closure. One
// 8-byte closure-identity compare then subsumes the callee tag / heap-type /
// jit_func_idx / native_kind / inline_kind / jit_meta / rest-param /
// param-count checks that jit_call_value_impl performs dynamically, and turns
// the callee's locals count, meta pointer and four capture pointers into
// immediates. Unlike the NARI_ENABLE_DIRECT_CALL_JIT path this does not have to
// refuse closures that own captures (83% of calls): with the closure pinned,
// the capture pointers to install are compile-time constants.
struct DirectCallee {
    bool ok = false;
    // Pinned FunctionMeta identity. Every closure instance of the same lambda
    // shares its meta, so factory-produced callees (createScanner()-style code,
    // which is most of transpiled tsc) still take the fast path -- unlike a
    // pinned closure value, which only ever matched the one instance that
    // happened to be live when the caller was compiled.
    uint64_t expected_raw = 0;
    uint32_t fidx = 0;
    uint32_t locals = 0;
    const void *meta = nullptr;
    const void *captures_raw = nullptr;
    const void *cap0 = nullptr;
    const void *cap1 = nullptr;
    const void *cap2 = nullptr;
};

// verify the vectors whose storage addresses get baked as immediates into generated code have not reallocated
void AsmJITMethodCompiler::assert_tables_stable() const {
    assert(compiled_fn_vec.data() == fn_vec_base && "compiled_fn_vec reallocated: baked code pointers now dangle");
}

AsmJITMethodCompiler::CompiledFunc AsmJITMethodCompiler::compile_chunk(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx) {
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

    bool spec_candidate = true;
    async_pending_ = false;
    auto fn = ir_compile(chunk, chunk_idx, nullptr, &spec_candidate);
    if (async_pending_) {
        // The worker publishes into compiled_fn_vec when it lands. Recording this
        // chunk as not_compilable would disable it permanently.
        return nullptr;
    }
    // for parameterized functions, attempt a speculative register-tier
    // recompile with params typed Int48 (entry tag guards, a failed
    // guard dispatches to the general-path version just compiled)
    if (fn && spec_candidate && getenv("NARI_DISABLE_SPEC_JIT") == nullptr && chunk.functions[chunk_idx].param_count > 0 &&
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
AsmJITMethodCompiler::ir_compile(const nari::bytecode::Chunk &chunk, uint32_t chunk_idx, CompiledFunc spec_fallback, bool *spec_candidate) {
    const bool spec = spec_fallback != nullptr;
    static const bool kJitReport = getenv("NARI_JIT_REPORT") != nullptr;
    auto report = [&](const char *what) {
        if (kJitReport) {
            fprintf(
                stderr, "[JIT] %-30s %s\n", chunk.functions[chunk_idx].name.empty() ? "<anon>" : chunk.functions[chunk_idx].name.c_str(),
                what
            );
        }
    };
    ir::Func irFuncs;
    if (!ir::build(chunk, chunk_idx, irFuncs)) {
        report("BAIL build (non-P1 op or stack-spans-block)");
        return nullptr;
    }
    ir::optimize(irFuncs);

    // ---- inlining feasibility census (NARI_INLINE_CENSUS=1, diagnostic only) ----
    static const bool kInlineCensus = getenv("NARI_INLINE_CENSUS") != nullptr;
    if (kInlineCensus) {
        struct Census {
            uint64_t sites = 0, cap_callee = 0, resolved = 0, small12 = 0, small30 = 0, self_rec = 0;
            ~Census() {
                fprintf(
                    stderr,
                    "\n[INLINE CENSUS] Call sites in compiled fns: %llu\n"
                    "  callee is LoadCapture:            %llu (%.1f%%)\n"
                    "  resolved to a live FunctionData:  %llu (%.1f%%)\n"
                    "    of which callee code <= 64B:    %llu (%.1f%%)\n"
                    "    of which callee code <= 160B:   %llu (%.1f%%)\n"
                    "    self-recursive (refuse):        %llu\n",
                    (unsigned long long)sites, (unsigned long long)cap_callee, sites ? 100.0 * (double)cap_callee / (double)sites : 0.0,
                    (unsigned long long)resolved, sites ? 100.0 * (double)resolved / (double)sites : 0.0, (unsigned long long)small12,
                    sites ? 100.0 * (double)small12 / (double)sites : 0.0, (unsigned long long)small30,
                    sites ? 100.0 * (double)small30 / (double)sites : 0.0, (unsigned long long)self_rec
                );
            }
        };
        static Census census;
        for (const auto &in : irFuncs.insts) {
            if (in.op != ir::Op::Call || in.operands.empty()) {
                continue;
            }
            census.sites++;
            const ir::Inst &callee_in = irFuncs.insts[in.operands[0]];
            if (callee_in.op != ir::Op::LoadCapture) {
                continue;
            }
            census.cap_callee++;
            auto *caps = g_compile_vm ? g_compile_vm->jit_captures_raw : nullptr;
            if (!caps || callee_in.imm_u32 >= caps->size()) {
                continue;
            }
            const Value &cv = *(*caps)[callee_in.imm_u32];
            if (!cv.is_function()) {
                continue;
            }
            const int32_t fidx = cv.get_function().jit_func_idx;
            if (fidx < 0 || (size_t)fidx >= chunk.functions.size()) {
                continue;
            }
            census.resolved++;
            if ((uint32_t)fidx == chunk_idx) {
                census.self_rec++;
                continue;
            }
            const size_t bytes = chunk.functions[(uint32_t)fidx].code.size();
            if (bytes <= 64) {
                census.small12++;
            }
            if (bytes <= 160) {
                census.small30++;
            }
        }
    }
    // Resolve a Call site to a single pinned closure, or return {ok=false}.
    // Reads g_compile_vm->jit_captures_raw, i.e. the *compiling* caller's
    // captures, so this is a speculation -- the emitted guard is what makes it
    // sound, and a failed guard falls back to jit_call_value.
    static const bool kNoDirectCall = getenv("NARI_DISABLE_DIRECT_CALL") != nullptr;
    // TEMP: A/B the inline strict-eq tag-dispatch tail (20.1% of all emitted JIT code)
    // NARI_DCS_CENSUS=1: per-emitted-site tally of why a Call could not become a
    // guarded direct call. Static (site) counts, not dynamic frequency.
    struct DcsCensus {
        uint64_t sites = 0, argc_gt4 = 0, callee_op[64] = {}, cap_not_ok = 0, cap_oob = 0, not_fn = 0, no_meta = 0, native = 0,
                 rest_or_cap0 = 0, arity = 0, fill_gt8 = 0, ok = 0;
        ~DcsCensus() {
            if (sites == 0) {
                return;
            }
            fprintf(stderr, "\n=== direct-call SITE census (%llu Call sites) ===\n", (unsigned long long)sites);
            auto p = [&](const char *n, uint64_t v) {
                if (v) {
                    fprintf(stderr, "%10llu  %5.1f%%  %s\n", (unsigned long long)v, 100.0 * (double)v / (double)sites, n);
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
    static DcsCensus dcs;
    static const bool kDcsCensus = getenv("NARI_DCS_CENSUS") != nullptr;
    auto resolve_direct_callee = [&](const ir::Inst &in, uint32_t argc) -> DirectCallee {
        DirectCallee d;
        if (kDcsCensus) {
            dcs.sites++;
        }
        if (kNoDirectCall || in.operands.empty() || argc > 4) {
            if (kDcsCensus && !in.operands.empty()) {
                dcs.argc_gt4++;
            }
            return d;
        }
        if (!g_compile_vm) {
            return d;
        }
        const ir::Inst &callee_in = irFuncs.inst(in.operands[0]);
        if (kDcsCensus && callee_in.op != ir::Op::LoadCapture && callee_in.op != ir::Op::LoadGlobal) {
            dcs.callee_op[0]++;
        }
        // Either an upvalue read (module-level function referenced through the
        // caller's closure) or a global read. Both are compile-time observations
        // that the emitted identity guard re-validates at run time, so a global
        // being reassigned later simply fails the guard.
        const Value *cvp = nullptr;
        if (callee_in.op == ir::Op::LoadCapture) {
            // Captures are only known to belong to the function being compiled on the
            // note_jit_callee path; elsewhere jit_captures_raw may describe an unrelated
            // closure, which would make this a guaranteed guard miss.
            if (!g_compile_captures_ok) {
                if (kDcsCensus) {
                    dcs.cap_not_ok++;
                }
                return d;
            }
            auto *caps = g_compile_vm->jit_captures_raw;
            if (!caps || callee_in.imm_u32 >= caps->size()) {
                if (kDcsCensus) {
                    dcs.cap_oob++;
                }
                return d;
            }
            cvp = (*caps)[callee_in.imm_u32].get();
        } else if (callee_in.op == ir::Op::LoadGlobal) {
            const uint32_t ni = callee_in.imm_u32;
            if (ni >= g_compile_vm->global_cache_valid.size() || !g_compile_vm->global_cache_valid[ni] ||
                ni >= g_compile_vm->global_cache.size()) {
                if (kDcsCensus) {
                    dcs.cap_oob++;
                }
                return d;
            }
            cvp = &g_compile_vm->global_cache[ni];
        } else {
            return d;
        }
        if (cvp == nullptr || !cvp->is_function()) {
            if (kDcsCensus) {
                dcs.not_fn++;
            }
            return d;
        }
        const Value &cv = *cvp;
        const FunctionData &fd = cv.get_function();
        if (fd.jit_func_idx < 0 || (size_t)fd.jit_func_idx >= chunk.functions.size()) {
            if (kDcsCensus) {
                dcs.no_meta++;
            }
            return d;
        }
        if (fd.jit_native_kind != 0 || fd.jit_meta == nullptr) {
            if (kDcsCensus) {
                dcs.native++;
            }
            return d;
        }
        if (fd.jit_rest_param_index >= 0 || fd.jit_inline_kind == JitInlineKind::Capture0) {
            if (kDcsCensus) {
                dcs.rest_or_cap0++;
            }
            return d;
        }
        // Exact arity only: no missing-argument fill, so param_count needs no check.
        if ((uint32_t)fd.jit_param_count != argc || fd.jit_locals_count < argc) {
            if (kDcsCensus) {
                dcs.arity++;
            }
            return d;
        }
        // The none-fill of non-parameter locals is unrolled, so bound it.
        if (fd.jit_locals_count - argc > 8) {
            if (kDcsCensus) {
                dcs.fill_gt8++;
            }
            return d;
        }
        if (kDcsCensus) {
            dcs.ok++;
        }
        d.ok = true;
        d.expected_raw = (uint64_t)(uintptr_t)fd.jit_meta;
        d.fidx = (uint32_t)fd.jit_func_idx;
        d.locals = fd.jit_locals_count;
        d.meta = (const void *)fd.jit_meta;
        d.captures_raw = (const void *)fd.jit_captures_raw;
        d.cap0 = (const void *)fd.jit_capture0_raw;
        d.cap1 = (const void *)fd.jit_capture1_raw;
        d.cap2 = (const void *)fd.jit_capture2_raw;
        return d;
    };
    // Statically-known jit_native_kind of a Call's callee (-1 = unknown), used to
    // emit only the matching builtin inline body at the site.
    auto callee_kind_hint = [&](const ir::Inst &in) -> int {
        if (in.operands.empty()) {
            return -1;
        }
        const ir::Inst &callee = irFuncs.inst(in.operands[0]);
        if (callee.op != ir::Op::LoadGlobal) {
            return -1;
        }
        auto it = builtin_kind_by_name_idx_.find(callee.imm_u32);
        return it == builtin_kind_by_name_idx_.end() ? -1 : it->second;
    };
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
        ir::IntArraySlots next = ir::analyze_int_array_slots(irFuncs, push_method_name_idx_, length_method_name_idx_);
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
    if (spec_candidate) {
        // These operations are unaffected by parameter type specialization and
        // are not supported by the register tier. Avoid rebuilding and
        // optimizing the same IR only to reject it during lowering.
        for (const ir::Inst &in : irFuncs.insts) {
            switch (in.op) {
                case ir::Op::LoadConst:
                case ir::Op::StoreGlobal:
                case ir::Op::StoreCSlot:
                case ir::Op::MakeObject:
                case ir::Op::MakeClosure:
                case ir::Op::StrConcat:
                case ir::Op::FormatValue:
                case ir::Op::IterArray:
                case ir::Op::LoadProperty:
                case ir::Op::StoreProperty:
                case ir::Op::JsSetPropStatic:
                case ir::Op::SelfTailCall:
                // Closure upvalue access, static property reads, JS bitwise ops
                // and Pow have no register-tier lowering either, and the
                // transpiled compiler emits them almost everywhere.
                case ir::Op::LoadCapture:
                case ir::Op::StoreCapture:
                case ir::Op::JsGetPropStatic:
                case ir::Op::JsPostinc:
                case ir::Op::JsBitBinary:
                case ir::Op::Pow:
                    *spec_candidate = false;
                    break;
                default:
                    break;
            }
            if (!*spec_candidate) {
                break;
            }
        }
    }
    if (getenv("NARI_IR_DUMP")) {
        fprintf(stderr, "=== IR for '%s' (func %u) ===\n%s", chunk.functions[chunk_idx].name.c_str(), chunk_idx, ir::dump(irFuncs).c_str());
    }
    // quantify cross-block redundant expressions that a future global-CSE/GVN pass could eliminate.
    // pure analysis: does not touch irFuncs or codegen. Runs on the final typed IR.
    static const bool kGvnReport = getenv("NARI_IR_GVN_REPORT") != nullptr;
    if (kGvnReport) {
        ir::gvn_report(irFuncs, chunk.functions[chunk_idx].name.c_str(), chunk_idx);
    }

    // Heap-allocated so ownership can transfer to the background compile worker.
    // The references below keep every existing `code_holder.` / `cc.` use unchanged.
    auto code_holder_owned = std::make_unique<CodeHolder>();
    CodeHolder &code_holder = *code_holder_owned;
    code_holder.init(this->rt.environment(), this->rt.cpu_features());
    StringLogger asm_logger;
    if (jit_dump_asm_enabled()) {
        asm_logger.add_flags(FormatFlags::kMachineCode);
        code_holder.set_logger(&asm_logger);
    }
    auto cc_owned = std::make_unique<arch::Compiler>(&code_holder);
    arch::Compiler &cc = *cc_owned;
    if (getenv("NARI_JIT_VALIDATE") != nullptr) {
        cc.add_diagnostic_options(asmjit::DiagnosticOptions::kValidateAssembler | asmjit::DiagnosticOptions::kValidateIntermediate);
    }
    FuncNode *func_node = cc.add_func(FuncSignature::build<void, void *>());
    // Density of operations that lower to a helper invoke. Pinning `vm` to a
    // callee-saved register below pays off once per call but costs one register
    // from the allocation pool for the whole function, so it is only worth it
    // where calls are frequent relative to the amount of code competing for
    // registers. Measured: tsc's compiled functions sit at a median density of
    // 0.111, while the practical benchmark's `bench_life` hot loop is 3 calls in
    // 355 IR ops (0.008) and loses 20% throughput when it gives up a register.
    unsigned helper_call_ops = 0;
    for (const auto &in : irFuncs.insts) {
        switch (in.op) {
            case ir::Op::Call:
            case ir::Op::CallMethod:
            case ir::Op::CallSpread:
            case ir::Op::LoadProperty:
            case ir::Op::StoreProperty:
            case ir::Op::JsSetPropStatic:
            case ir::Op::JsGetPropStatic:
            case ir::Op::JsPostinc:
            case ir::Op::MakeClosure:
            case ir::Op::StrConcat:
            case ir::Op::MakeObject:
                helper_call_ops++;
                break;
            default:
                break;
        }
    }
    const bool pin_vm_callee_saved = helper_call_ops * 20 >= irFuncs.insts.size();
    // `vm` is live across every helper call in the function, so leaving it in the
    // caller-saved argument register makes the allocator reload it from a spill
    // slot after each call: the tsc run emits 59,046 `mov rdi, [rsp+N]` reloads
    // that way, 3% of all emitted instructions. Requesting a callee-saved home
    // register instead keeps it live across calls for the cost of the argument
    // move asmjit already has to emit at each call site.
    //
    // The hint has to go on a copy, not on the argument register itself:
    // BaseRAPass::_init_gobal_live_spans overwrites hint_reg_id with the incoming
    // argument's register for any vreg bound by set_arg(), so a hint placed
    // directly on the argument vreg is discarded. One extra prologue mov per
    // compiled function buys the hint back.
    arch::Gp vm_reg = cc.new_gp64("vm");
    // asmjit satisfies a call argument by *reassigning* the argument vreg to the
    // ABI register, so passing `vm_reg` straight to an InvokeNode moves it into
    // the argument register and forces a spill/reload around the call even when
    // its home is callee-saved. A short-lived copy keeps `vm_reg` in its home:
    // the copy dies at the call, so the pair collapses to the argument move
    // asmjit has to emit anyway.
    arch::Gp vm_arg_scratch = vm_reg;
    if (pin_vm_callee_saved) {
        arch::Gp vm_arg = cc.new_gp64("vm_arg");
        func_node->set_arg(0, vm_arg);
        arch::mov_reg(cc, vm_reg, vm_arg);
        if (asmjit::VirtReg *vm_vreg = cc.virt_reg_by_reg(vm_reg)) {
#if NARI_JIT_ARM64
            vm_vreg->set_home_id_hint(19); // x19: first callee-saved GPR in AAPCS64
#else
            vm_vreg->set_home_id_hint(asmjit::x86::Gp::kIdBx);
#endif
            vm_vreg->set_weight(255);
        }
        vm_arg_scratch = cc.new_gp64("vm_arg0");
    } else {
        // Not worth a register: bind the argument directly, exactly as before, so
        // functions below the density threshold get byte-identical code.
        func_node->set_arg(0, vm_reg);
    }
    auto vm_arg0 = [&]() -> arch::Gp {
        if (pin_vm_callee_saved) {
            arch::mov_reg(cc, vm_arg_scratch, vm_reg);
        }
        return vm_arg_scratch;
    };

    // block-scoped load-only cache for the VM stack's _M_finish field
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
                arch::load(cc, base_reg, arch::ptr(vm_reg, (int)VMStackStartOff));
                arch::add2(cc, base_reg, base_bytes);
                have_base = true;
            }
            return base_reg;
        }
        // Return the vreg holding the current in-memory finish value,
        // loading from memory only if the cache is cold.
        arch::Gp ensure_reg() {
            if (!have_reg) {
                arch::load(cc, reg, arch::ptr(vm_reg, (int)VMStackFinishOff));
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
            arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), src);
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

        // --- deferred pushes ----------------------------------------------------
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
            cc.mov(t, Imm(p.imm));
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
                store_pend(e, i * (int)ValSize, pend[i]);
            }
            arch::add_imm(cc, e, n * (int)ValSize);
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
                    store_pend(e, i * (int)ValSize, pend[i]);
                }
                arch::add_imm(cc, e, n * (int)ValSize);
                set(e);
                for (int i = 0; i < k; ++i) {
                    pend[i] = keep[i];
                }
                npend = k;
            }
            return ensure_reg(); // get() would materialize the survivors
        }
    };
    FinishCache fc(cc, vm_reg);

    // Two shared scratch registers for the emitters below. Every use is a define
    // followed immediately by a store/compare with no intervening helper call, so the
    // live ranges are short and disjoint; handing asmjit's register allocator two
    // vregs with many defs instead of a fresh vreg per emitted operation measurably
    // cuts allocation work (JIT compilation is the largest single symbol in the tsc
    // profile). Declared here so every emit_* lambda below can see them.
    arch::Gp const_scratch = cc.new_gp64("ir_const_scratch");
    arch::Gp cmp_scratch = cc.new_gp64("ir_cmp_scratch");

    auto call0 = [&](const void *helper) {
        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *>());
        inv->set_arg(0, vm_arg_scratch);
    };
    auto emit_call_typeof = [&] {
        fc.invalidate();
        Label is_none = cc.new_label();
        Label is_int = cc.new_label();
        Label is_bool = cc.new_label();
        Label is_float = cc.new_label();
        Label is_heap = cc.new_label();
        Label is_string = cc.new_label();
        Label is_array = cc.new_label();
        Label is_object = cc.new_label();
        Label is_function = cc.new_label();
        Label is_regex = cc.new_label();
        Label is_handle = cc.new_label();
        Label slow = cc.new_label();
        Label store = cc.new_label();
        Label done = cc.new_label();

        arch::Gp endp = cc.new_gp64("typeof_end");
        arch::Gp raw = cc.new_gp64("typeof_raw");
        arch::Gp tag = cc.new_gp64("typeof_tag");
        arch::Gp result = cc.new_gp64("typeof_result");
        arch::load(cc, endp, arch::ptr(vm_reg, (int)VMStackFinishOff));
        arch::load(cc, raw, arch::ptr(endp, (int)-ValSize));
        arch::mov_reg(cc, tag, raw);
        arch::shr(cc, tag, 48);
        arch::cmp_imm(cc, tag.r32(), Imm((int)Value::TAG_NONE));
        arch::jcc(cc, arch::CC::kEQ, is_none);
        arch::cmp_imm(cc, tag.r32(), Imm((int)Value::TAG_INT));
        arch::jcc(cc, arch::CC::kEQ, is_int);
        arch::cmp_imm(cc, tag.r32(), Imm((int)Value::TAG_BOOL));
        arch::jcc(cc, arch::CC::kEQ, is_bool);
        arch::cmp_imm(cc, tag.r32(), Imm((int)Value::TAG_HEAP));
        arch::jcc(cc, arch::CC::kEQ, is_heap);
        arch::jmp(cc, is_float);

        cc.bind(is_heap);
        arch::zero_extend_48(cc, raw);
        arch::load8_zx(cc, tag, arch::ptr8(raw, (int)HeapTypeTagOff));
        arch::cmp_imm(cc, tag.r32(), Imm((int)ValueTag::String));
        arch::jcc(cc, arch::CC::kEQ, is_string);
        arch::cmp_imm(cc, tag.r32(), Imm((int)ValueTag::Array));
        arch::jcc(cc, arch::CC::kEQ, is_array);
        arch::cmp_imm(cc, tag.r32(), Imm((int)ValueTag::Object));
        arch::jcc(cc, arch::CC::kEQ, is_object);
        arch::cmp_imm(cc, tag.r32(), Imm((int)ValueTag::Function));
        arch::jcc(cc, arch::CC::kEQ, is_function);
        arch::cmp_imm(cc, tag.r32(), Imm((int)ValueTag::Regex));
        arch::jcc(cc, arch::CC::kEQ, is_regex);
        arch::cmp_imm(cc, tag.r32(), Imm((int)ValueTag::Handle));
        arch::jcc(cc, arch::CC::kEQ, is_handle);
        arch::cmp_imm(cc, tag.r32(), Imm((int)ValueTag::ClassInstance));
        arch::jcc(cc, arch::CC::kEQ, slow);
        arch::jmp(cc, is_none);

        auto load_typeof_value = [&](Label label, int index) {
            cc.bind(label);
            arch::load(cc, result, arch::ptr(vm_reg, (int)VMRuntimeOff));
            arch::load(cc, result, arch::ptr(result, (int)(RuntimeTypeofValuesOff + index * ValSize)));
            arch::jmp(cc, store);
        };
        load_typeof_value(is_none, ScriptRuntime::TY_NULL);
        load_typeof_value(is_int, ScriptRuntime::TY_INT);
        load_typeof_value(is_float, ScriptRuntime::TY_FLOAT);
        load_typeof_value(is_string, ScriptRuntime::TY_STRING);
        load_typeof_value(is_bool, ScriptRuntime::TY_BOOL);
        load_typeof_value(is_array, ScriptRuntime::TY_ARRAY);
        load_typeof_value(is_object, ScriptRuntime::TY_OBJECT);
        load_typeof_value(is_function, ScriptRuntime::TY_FUNCTION);
        load_typeof_value(is_regex, ScriptRuntime::TY_REGEX);
        load_typeof_value(is_handle, ScriptRuntime::TY_HANDLE);

        cc.bind(store);
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), result);
        arch::sub_imm(cc, endp, (int)ValSize);
        arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), endp);
        arch::jmp(cc, done);
        cc.bind(slow);
        call0((const void *)jit_call_typeof);
        cc.bind(done);
    };
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

    typedef void (*VMFunc)(nari::bytecode::VM *);

    auto call_vm = [&](VMFunc helper, nari::bytecode::VM *vm) { helper(vm); };
    auto call_u32 = [&](const void *helper, uint32_t a) {
        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, uint32_t>());
        inv->set_arg(0, vm_arg_scratch);
        inv->set_arg(1, Imm(a));
    };
    auto call_u32_u32 = [&](const void *helper, uint32_t a, uint32_t b) {
        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, uint32_t, uint32_t>());
        inv->set_arg(0, vm_arg_scratch);
        inv->set_arg(1, Imm(a));
        inv->set_arg(2, Imm(b));
    };
    auto call_i64 = [&](const void *helper, int64_t a) {
        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, int64_t>());
        inv->set_arg(0, vm_arg_scratch);
        inv->set_arg(1, Imm(a));
    };
    // jit_slot_store_raw(vm, uint32_t idx, uint64_t raw).
    auto call_u32_u64 = [&](const void *helper, uint32_t a, uint64_t b) {
        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, uint32_t, uint64_t>());
        inv->set_arg(0, vm_arg_scratch);
        inv->set_arg(1, Imm(a));
        inv->set_arg(2, Imm(b));
    };
    auto call_u32_u32_u64 = [&](const void *helper, uint32_t a, uint32_t b, uint64_t c) {
        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, uint32_t, uint32_t, uint64_t>());
        inv->set_arg(0, vm_arg_scratch);
        inv->set_arg(1, Imm(a));
        inv->set_arg(2, Imm(b));
        inv->set_arg(3, Imm(c));
    };

    // kind_hint: statically-known jit_native_kind of the callee builtin, or -1.
    // Guarded direct call. Structure follows the NARI_ENABLE_DIRECT_CALL_JIT path
    // below, but replaces its ~19 dynamic guards with one closure-identity
    // compare and bakes the callee's locals/meta/captures as immediates. The
    // compiled entry is *loaded* rather than baked so a later speculative
    // recompile of the callee is picked up.
    auto emit_direct_call_guarded = [&](uint32_t argc, uint32_t callee_label_idx, const DirectCallee &d) {
        Label slow = cc.new_label();
        Label done = cc.new_label();
        // Every temporary below is written and consumed within two instructions,
        // so one virtual register per role is enough. Allocating a fresh vreg per
        // use instead cost ~10 extra vregs at each of the thousands of direct-call
        // sites, and asmjit's register allocation is superlinear in vreg count.
        arch::Gp dc_tmp = cc.new_gp64("dc_tmp");
        auto jump_if_owned = [&](const arch::Mem &field, const char *) {
            arch::load(cc, dc_tmp, field);
            arch::test_zero(cc, dc_tmp);
            arch::jcc(cc, arch::CC::kNE, slow);
        };
        auto store_ptr_imm = [&](int off, const void *pv) {
            cc.mov(dc_tmp, Imm((uint64_t)(uintptr_t)pv));
            arch::store(cc, arch::ptr(vm_reg, off), dc_tmp);
        };

        arch::Gp endp = fc.get();

        // (1) callee identity, pinned on FunctionMeta rather than on one closure
        // value. meta equality subsumes jit_func_idx, locals/param counts, the
        // rest-param index and the inline kind (all derived from the meta), so the
        // only extra work versus a raw 64-bit compare is the tag + heap-type test.
        // The payoff is that a lambda produced by a factory matches on every
        // instance instead of just the instance that was live at compile time.
        const int callee_off = (int)(-(int64_t)(argc + 1) * ValSize);
        arch::Gp closure = cc.new_gp64("dc_closure");
        arch::load(cc, closure, arch::ptr(endp, callee_off));
        arch::cmp_mem16_imm(cc, arch::ptr16(endp, callee_off + tagWordOff), Imm((int)tagHeap));
        arch::jcc(cc, arch::CC::kNE, slow);
        arch::Gp dc_fd = cc.new_gp64("dc_fd");
        cc.mov(dc_fd, closure);
        arch::zero_extend_48(cc, dc_fd);
        arch::cmp_mem8_imm(cc, arch::ptr8(dc_fd, (int)HeapTypeTagOff), Imm((int)tagFunction));
        arch::jcc(cc, arch::CC::kNE, slow);
        // jit_func_idx is a 32-bit chunk-unique function id, so this is a single
        // cmp-with-immediate: no vreg for the expected value (asmjit's register
        // allocation is superlinear in vreg count and this site is emitted 4.6k times).
        arch::cmp_mem32_imm(cc, arch::ptr32(dc_fd, (int)FDJitFuncIdxOff), Imm((int)d.fidx));
        arch::jcc(cc, arch::CC::kNE, slow);

        // (2) compiled entry, from a fixed slot in the stable fn table
        assert_tables_stable();
        arch::Gp cslot = cc.new_gp64("dc_cslot");
        cc.mov(cslot, Imm((uint64_t)(uintptr_t)(compiled_fn_vec.data() + d.fidx)));
        arch::Gp callee = cc.new_gp64("dc_callee");
        arch::load(cc, callee, arch::ptr(cslot));
        arch::test_zero(cc, callee);
        arch::jcc(cc, arch::CC::kEQ, slow);

        // (3) frame headroom and call depth
        arch::Gp ff = cc.new_gp64("dc_ff");
        arch::Gp fcap = cc.new_gp64("dc_fcap");
        arch::load(cc, ff, arch::ptr(vm_reg, (int)FramesFinishOff));
        arch::load(cc, fcap, arch::ptr(vm_reg, (int)VMFramesCapacityOff));
        arch::sub2(cc, fcap, ff);
        arch::cmp_imm(cc, fcap, Imm((int)FrameSize));
        arch::jcc(cc, arch::CC::kFLT, slow);
        arch::Gp frame_begin = cc.new_gp64("dc_fbegin");
        arch::load(cc, frame_begin, arch::ptr(vm_reg, (int)FramesStartOff));
        arch::add_imm(cc, frame_begin, (int64_t)MAX_CALL_DEPTH * FrameSize);
        cc.cmp(ff, frame_begin);
        arch::jcc(cc, arch::CC::kUGE, slow);

        // (4) the raw frame slot may be reused only if it owns no C++ objects
        jump_if_owned(arch::ptr(ff, (int)FrameCapturesOff), "dc_fc_ptr");
        jump_if_owned(arch::ptr(ff, (int)(FrameCapturesOff + 8)), "dc_fc_own");
        jump_if_owned(arch::ptr(ff, (int)FrameInlineUpvalueOff), "dc_fi_ptr");
        jump_if_owned(arch::ptr(ff, (int)(FrameInlineUpvalueOff + 8)), "dc_fi_own");
        jump_if_owned(arch::ptr(ff, (int)FrameOpenUpvalOff), "dc_fu");
        jump_if_owned(arch::ptr(ff, (int)FrameSingleCaptureCacheOff), "dc_fs_ptr");
        jump_if_owned(arch::ptr(ff, (int)(FrameSingleCaptureCacheOff + 8)), "dc_fs_own");

        // (5) value-stack headroom. locals is an immediate, so this is one lea.
        arch::Gp nf = cc.new_gp64("dc_nf");
        arch::lea(cc, nf, endp, ((int64_t)d.locals - (int64_t)argc - 1) * (int64_t)ValSize);
        arch::Gp stack_cap = cc.new_gp64("dc_scap");
        arch::load(cc, stack_cap, arch::ptr(vm_reg, (int)VMStackCapacityOff));
        cc.cmp(nf, stack_cap);
        arch::jcc(cc, arch::CC::kUGT, slow);

        // (6) shift the arguments down over the callee slot
        for (int i = 0; i < (int)argc; i++) {
            const int src = (i - (int)argc) * (int)ValSize;
            arch::Gp tmp = dc_tmp;
            arch::load(cc, tmp, arch::ptr(endp, src));
            arch::store(cc, arch::ptr(endp, src - (int)ValSize), tmp);
        }

        // (7) none-fill the non-parameter locals (count is a compile-time constant,
        // so the helper's runtime init loop becomes a short unrolled run)
        arch::Gp none = cc.new_gp64("dc_none");
        cc.mov(none, Imm((uint64_t)Value::none().raw_bits()));
        for (uint32_t i = argc; i < d.locals; i++) {
            arch::store(cc, arch::ptr(endp, ((int)i - (int)argc - 1) * (int)ValSize), none);
        }
        arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), nf);

        // (8) publish the frame
        arch::Gp sbi = cc.new_gp64("dc_sbi");
        arch::lea(cc, sbi, endp, (int64_t)(-(int64_t)(argc + 1) * ValSize));
        arch::sub_mem(cc, sbi, arch::ptr(vm_reg, (int)VMStackStartOff));
        arch::shr(cc, sbi, 3);
        arch::Gp meta = cc.new_gp64("dc_meta");
        cc.mov(meta, Imm((uint64_t)(uintptr_t)d.meta));
        arch::Gp ip = cc.new_gp64("dc_ip");
        arch::load(cc, ip, arch::ptr(meta, (int)FMCodeDataOff));
        arch::store(cc, arch::ptr(ff, (int)FrameFunctionOff), meta);
        arch::store(cc, arch::ptr(ff, (int)FrameIpOff), ip);
        arch::store(cc, arch::ptr(ff, (int)SlotBaseOff), sbi);
        arch::store_imm(cc, arch::ptr(ff, (int)FrameCapturesOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)(FrameCapturesOff + 8)), Imm(0));
        arch::store(cc, arch::ptr(ff, (int)FrameClosureRootOff), closure);
        arch::store(cc, arch::ptr(ff, (int)FrameReceiverOff), none);
        arch::store_imm(cc, arch::ptr(ff, (int)FrameInlineUpvalueIdxOff), Imm(UINT16_MAX));
        arch::store_imm(cc, arch::ptr(ff, (int)FrameInlineUpvalueOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)(FrameInlineUpvalueOff + 8)), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)FrameOpenUpvalOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)FrameSingleCaptureCacheOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)(FrameSingleCaptureCacheOff + 8)), Imm(0));
        arch::add_imm(cc, ff, (int)FrameSize);
        arch::store(cc, arch::ptr(vm_reg, (int)FramesFinishOff), ff);

        // (9) rebind the VM's borrowed capture pointers to the callee's, exactly as
        // jit_call_value_impl does. These are immediates because the guard pinned
        // the closure; the caller's are runtime values and must be saved.
        arch::Gp sav_c = cc.new_gp64("dc_sav_c");
        arch::Gp sav_0 = cc.new_gp64("dc_sav_0");
        arch::Gp sav_1 = cc.new_gp64("dc_sav_1");
        arch::Gp sav_2 = cc.new_gp64("dc_sav_2");
        arch::load(cc, sav_c, arch::ptr(vm_reg, (int)VMCapturesRawOff));
        arch::load(cc, sav_0, arch::ptr(vm_reg, (int)VMCapture0RawOff));
        arch::load(cc, sav_1, arch::ptr(vm_reg, (int)VMCapture1RawOff));
        arch::load(cc, sav_2, arch::ptr(vm_reg, (int)VMCapture2RawOff));
        // Runtime loads, not immediates: the guard pinned the function, not the
        // closure, so each instance brings its own capture cells.
        auto copy_cap = [&](int64_t src_off, int dst_off) {
            arch::load(cc, dc_tmp, arch::ptr(dc_fd, (int)src_off));
            arch::store(cc, arch::ptr(vm_reg, dst_off), dc_tmp);
        };
        copy_cap(FDCapturesRawOff, (int)VMCapturesRawOff);
        copy_cap(FDCapture0RawOff, (int)VMCapture0RawOff);
        copy_cap(FDCapture1RawOff, (int)VMCapture1RawOff);
        copy_cap(FDCapture2RawOff, (int)VMCapture2RawOff);

        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        cc.invoke(Out(inv), callee, FuncSignature::build<void, void *>());
        inv->set_arg(0, vm_arg_scratch);

        arch::store(cc, arch::ptr(vm_reg, (int)VMCapturesRawOff), sav_c);
        arch::store(cc, arch::ptr(vm_reg, (int)VMCapture0RawOff), sav_0);
        arch::store(cc, arch::ptr(vm_reg, (int)VMCapture1RawOff), sav_1);
        arch::store(cc, arch::ptr(vm_reg, (int)VMCapture2RawOff), sav_2);
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32_u32((const void *)jit_call_value, argc, callee_label_idx);
        cc.bind(done);
        fc.invalidate();
    };

    auto emit_ir_call_value = [&](uint32_t argc, uint32_t callee_label_idx, int kind_hint = -1, const DirectCallee *dc = nullptr) {
        if (dc != nullptr && dc->ok) {
            emit_direct_call_guarded(argc, callee_label_idx, *dc);
            return;
        }
        if (argc == 1 && getenv("NARI_ENABLE_DIRECT_CALL_JIT") == nullptr) {
            if (kind_hint >= 0 && kind_hint != 9) {
                // callee is a known builtin with no argc==1 inline body: skip the guards
                call_u32_u32((const void *)jit_call_value, argc, callee_label_idx);
                return;
            }
            Label slow = cc.new_label();
            Label length = cc.new_label();
            Label done = cc.new_label();
            arch::Gp endp = fc.get();
            arch::Gp tag = cc.new_gp64("ir_cv_length_tag");
            arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
            arch::jcc(cc, arch::CC::kNE, slow);

            arch::Gp fd = cc.new_gp64("ir_cv_length_fd");
            arch::load(cc, fd, arch::ptr(endp, (int)(-2 * ValSize)));
            arch::zero_extend_48(cc, fd);
            arch::cmp_mem8_imm(cc, arch::ptr8(fd, (int)HeapTypeTagOff), Imm((int)tagFunction));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp native_kind = cc.new_gp64("ir_cv_length_kind");
            arch::load32_zx(cc, native_kind, fd, (int)FDNativeKindOff);
            arch::cmp_imm(cc, native_kind, Imm(9));
            arch::jcc(cc, arch::CC::kNE, slow);

            arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
            arch::jcc(cc, arch::CC::kNE, slow);
            arch::Gp object = cc.new_gp64("ir_cv_length_object");
            arch::load(cc, object, arch::ptr(endp, (int)-ValSize));
            arch::zero_extend_48(cc, object);
            arch::cmp_mem8_imm(cc, arch::ptr8(object, (int)HeapTypeTagOff), Imm((int)tagArray));
            arch::jcc(cc, arch::CC::kEQ, length);
            arch::cmp_mem8_imm(cc, arch::ptr8(object, (int)HeapTypeTagOff), Imm((int)tagString));
            arch::jcc(cc, arch::CC::kNE, slow);

            cc.bind(length);
            arch::Gp result = cc.new_gp64("ir_cv_length_result");
            InvokeNode *inv;
            arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)jit_js_length, FuncSignature::build<uint64_t, void *>());
            inv->set_arg(0, object);
            inv->set_ret(0, result);
            arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), result);
            arch::sub_imm(cc, endp, (int)ValSize);
            fc.set(endp);
            arch::jmp(cc, done);

            cc.bind(slow);
            call_u32_u32((const void *)jit_call_value, argc, callee_label_idx);
            fc.merge_slow_path();
            cc.bind(done);
            return;
        }
        if (argc == 2 && getenv("NARI_ENABLE_DIRECT_CALL_JIT") == nullptr) {
            // Emit only the inline body for the builtin this site actually names.
            // An unidentified callee used to get ALL of these bodies emitted
            // speculatively (~127 instructions per site, 1643 sites on tsc). Measured
            // slower than just calling jit_call_value: the icache cost of the
            // speculative bodies outweighs the dispatch they save.
            //
            // Adding kinds 15 and 16 here removed 1,760,438 dynamic generic-dispatch
            // calls on tsc (16.21M -> 14.45M, -10.9%): __js_add 862k and
            // __js_str_code_point_at 949k both drop to zero. 24-pair interleaved A/B:
            // -24.3 ms (-0.64%), t=-3.00, faster in 19/24. Naming kinds 12 and 14
            // costs nothing and buys the argc==1 guard skip below for __js_to_string;
            // kind 12 (__js_loose_eq, 405k calls, ~0.16%) is left to the generic path
            // because its coercion semantics are the easiest of these to get wrong.
            const bool want_cmp = kind_hint >= 2 && kind_hint <= 5;
            const bool want_get_prop = kind_hint == 6;
            const bool want_char_code = kind_hint == 10;
            const bool want_add = kind_hint == 16;
            const bool want_code_point = kind_hint == 15;
            if (!want_cmp && !want_get_prop && !want_char_code && !want_add && !want_code_point) {
                call_u32_u32((const void *)jit_call_value, argc, callee_label_idx);
                return;
            }
            Label slow = cc.new_label();
            Label cmp_lt = cc.new_label();
            Label cmp_gt = cc.new_label();
            Label cmp_le = cc.new_label();
            Label get_prop = cc.new_label();
            Label char_code = cc.new_label();
            Label add_ints = cc.new_label();
            Label code_point = cc.new_label();
            Label store = cc.new_label();
            Label done = cc.new_label();

            arch::Gp endp = fc.get();
            arch::Gp tag = cc.new_gp64("ir_cv_native_tag");
            arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-3 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
            arch::jcc(cc, arch::CC::kNE, slow);

            arch::Gp fd = cc.new_gp64("ir_cv_native_fd");
            arch::load(cc, fd, arch::ptr(endp, (int)(-3 * ValSize)));
            arch::zero_extend_48(cc, fd);
            arch::cmp_mem8_imm(cc, arch::ptr8(fd, (int)HeapTypeTagOff), Imm((int)tagFunction));
            arch::jcc(cc, arch::CC::kNE, slow);

            arch::Gp native_kind = cc.new_gp64("ir_cv_native_kind");
            arch::load32_zx(cc, native_kind, fd, (int)FDNativeKindOff);
            if (want_char_code) {
                arch::cmp_imm(cc, native_kind, Imm(10));
                arch::jcc(cc, want_cmp || want_get_prop ? arch::CC::kEQ : arch::CC::kNE, want_cmp || want_get_prop ? char_code : slow);
            }
            if (want_get_prop) {
                arch::cmp_imm(cc, native_kind, Imm(6));
                arch::jcc(cc, want_cmp ? arch::CC::kEQ : arch::CC::kNE, want_cmp ? get_prop : slow);
            }
            if (want_add) {
                arch::cmp_imm(cc, native_kind, Imm(16));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::jmp(cc, add_ints);
            }
            if (want_code_point) {
                arch::cmp_imm(cc, native_kind, Imm(15));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::jmp(cc, code_point);
            }
            if (!want_cmp) {
                // fall straight into whichever single body remains
                if (want_get_prop) {
                    arch::jmp(cc, get_prop);
                } else if (want_char_code) {
                    arch::jmp(cc, char_code);
                }
            } else {

                arch::cmp_imm(cc, native_kind, Imm(2));
                arch::jcc(cc, arch::CC::kLT, slow);
                arch::cmp_imm(cc, native_kind, Imm(5));
                arch::jcc(cc, arch::CC::kGT, slow);

                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);

                arch::Gp lhs = cc.new_gp64("ir_cv_native_lhs");
                arch::Gp rhs = cc.new_gp64("ir_cv_native_rhs");
                arch::load(cc, lhs, arch::ptr(endp, (int)(-2 * ValSize)));
                arch::load(cc, rhs, arch::ptr(endp, (int)(-ValSize)));
                arch::sign_extend_48(cc, lhs);
                arch::sign_extend_48(cc, rhs);

                arch::cmp_imm(cc, native_kind, Imm(2));
                arch::jcc(cc, arch::CC::kEQ, cmp_lt);
                arch::cmp_imm(cc, native_kind, Imm(3));
                arch::jcc(cc, arch::CC::kEQ, cmp_gt);
                arch::cmp_imm(cc, native_kind, Imm(4));
                arch::jcc(cc, arch::CC::kEQ, cmp_le);

                arch::Gp result = cc.new_gp64("ir_cv_native_result");
                cc.cmp(lhs, rhs);
                arch::cset(cc, result, arch::CC::kGE);
                arch::jmp(cc, store);
                cc.bind(cmp_lt);
                cc.cmp(lhs, rhs);
                arch::cset(cc, result, arch::CC::kLT);
                arch::jmp(cc, store);
                cc.bind(cmp_gt);
                cc.cmp(lhs, rhs);
                arch::cset(cc, result, arch::CC::kGT);
                arch::jmp(cc, store);
                cc.bind(cmp_le);
                cc.cmp(lhs, rhs);
                arch::cset(cc, result, arch::CC::kLE);

                cc.bind(store);
                arch::or_imm(cc, result, result, (int64_t)nbBoolTag);
                arch::store(cc, arch::ptr(endp, (int)(-3 * ValSize)), result);
                arch::sub_imm(cc, endp, (int)(2 * ValSize));
                fc.set(endp);
                arch::jmp(cc, done);
            } // want_cmp

            if (want_get_prop) {
                cc.bind(get_prop);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::Gp array = cc.new_gp64("ir_cv_get_prop_array");
                arch::load(cc, array, arch::ptr(endp, (int)(-2 * ValSize)));
                arch::zero_extend_48(cc, array);
                arch::cmp_mem8_imm(cc, arch::ptr8(array, (int)HeapTypeTagOff), Imm((int)tagArray));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::Gp index = cc.new_gp64("ir_cv_get_prop_index");
                arch::load(cc, index, arch::ptr(endp, (int)-ValSize));
                arch::sign_extend_48(cc, index);
                arch::js(cc, index, slow);
                arch::Gp start = cc.new_gp64("ir_cv_get_prop_start");
                arch::Gp finish = cc.new_gp64("ir_cv_get_prop_finish");
                arch::load(cc, start, arch::ptr(array, (int)ArrayVecStartOff));
                arch::load(cc, finish, arch::ptr(array, (int)ArrayVecFinishOff));
                arch::sub2(cc, finish, start);
                arch::shr(cc, finish, 3);
                cc.cmp(index, finish);
                arch::jcc(cc, arch::CC::kGE, slow);
                arch::Gp element = cc.new_gp64("ir_cv_get_prop_element");
                arch::shl(cc, element, index, 3);
                arch::add2(cc, element, start);
                arch::Gp get_prop_result = cc.new_gp64("ir_cv_get_prop_result");
                arch::load(cc, get_prop_result, arch::ptr(element));
                arch::store(cc, arch::ptr(endp, (int)(-3 * ValSize)), get_prop_result);
                arch::sub_imm(cc, endp, (int)(2 * ValSize));
                fc.set(endp);
                arch::jmp(cc, done);
            } // want_get_prop

            if (want_char_code) {
                cc.bind(char_code);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::Gp string_obj = cc.new_gp64("ir_cv_char_string");
                arch::load(cc, string_obj, arch::ptr(endp, (int)(-2 * ValSize)));
                arch::zero_extend_48(cc, string_obj);
                arch::cmp_mem8_imm(cc, arch::ptr8(string_obj, (int)HeapTypeTagOff), Imm((int)tagString));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::Gp char_index = cc.new_gp64("ir_cv_char_index");
                arch::load(cc, char_index, arch::ptr(endp, (int)(-ValSize)));
                arch::sign_extend_48(cc, char_index);
                arch::cmp_imm(cc, char_index, Imm(std::numeric_limits<int>::min()));
                arch::jcc(cc, arch::CC::kLT, slow);
                arch::cmp_imm(cc, char_index, Imm(std::numeric_limits<int>::max()));
                arch::jcc(cc, arch::CC::kGT, slow);
                arch::Gp char_result = cc.new_gp64("ir_cv_char_result");
                InvokeNode *char_inv;
                arch::invoke_imm(
                    cc, &char_inv, (uint64_t)(uintptr_t)jit_string_char_code_at, FuncSignature::build<uint64_t, void *, int64_t>()
                );
                char_inv->set_arg(0, string_obj);
                char_inv->set_arg(1, char_index);
                char_inv->set_ret(0, char_result);
                arch::store(cc, arch::ptr(endp, (int)(-3 * ValSize)), char_result);
                arch::sub_imm(cc, endp, (int)(2 * ValSize));
                fc.set(endp);
                arch::jmp(cc, done);
            } // want_char_code

            if (want_add) {
                // __js_add(int, int). Both operands must be int-tagged and fit in
                // int32; that window is a strict subset of the +-2^46 range the C++
                // fast path in jit_try_native_call accepts, so the sum is exact and
                // the result is identical to the scripted __js_add. Anything else
                // (strings, doubles, wide ints) falls through to the generic call.
                cc.bind(add_ints);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::Gp add_lhs = cc.new_gp64("ir_cv_add_lhs");
                arch::Gp add_rhs = cc.new_gp64("ir_cv_add_rhs");
                arch::load(cc, add_lhs, arch::ptr(endp, (int)(-2 * ValSize)));
                arch::load(cc, add_rhs, arch::ptr(endp, (int)(-ValSize)));
                arch::sign_extend_48(cc, add_lhs);
                arch::sign_extend_48(cc, add_rhs);
                arch::cmp_imm(cc, add_lhs, Imm(std::numeric_limits<int>::min()));
                arch::jcc(cc, arch::CC::kLT, slow);
                arch::cmp_imm(cc, add_lhs, Imm(std::numeric_limits<int>::max()));
                arch::jcc(cc, arch::CC::kGT, slow);
                arch::cmp_imm(cc, add_rhs, Imm(std::numeric_limits<int>::min()));
                arch::jcc(cc, arch::CC::kLT, slow);
                arch::cmp_imm(cc, add_rhs, Imm(std::numeric_limits<int>::max()));
                arch::jcc(cc, arch::CC::kGT, slow);
                arch::add2(cc, add_lhs, add_rhs);
                arch::nanbox_encode_int(cc, add_lhs, add_rhs);
                arch::store(cc, arch::ptr(endp, (int)(-3 * ValSize)), add_lhs);
                arch::sub_imm(cc, endp, (int)(2 * ValSize));
                fc.set(endp);
                arch::jmp(cc, done);
            } // want_add

            if (want_code_point) {
                // __js_str_code_point_at(s, i): same guards as char_code_at (string
                // receiver, int index in int32 range); the helper differs only in
                // returning undefined rather than -1 when the index is out of range.
                cc.bind(code_point);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::Gp cp_string = cc.new_gp64("ir_cv_cp_string");
                arch::load(cc, cp_string, arch::ptr(endp, (int)(-2 * ValSize)));
                arch::zero_extend_48(cc, cp_string);
                arch::cmp_mem8_imm(cc, arch::ptr8(cp_string, (int)HeapTypeTagOff), Imm((int)tagString));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::load16_zx(cc, tag, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
                arch::Gp cp_index = cc.new_gp64("ir_cv_cp_index");
                arch::load(cc, cp_index, arch::ptr(endp, (int)(-ValSize)));
                arch::sign_extend_48(cc, cp_index);
                arch::cmp_imm(cc, cp_index, Imm(std::numeric_limits<int>::min()));
                arch::jcc(cc, arch::CC::kLT, slow);
                arch::cmp_imm(cc, cp_index, Imm(std::numeric_limits<int>::max()));
                arch::jcc(cc, arch::CC::kGT, slow);
                arch::Gp cp_result = cc.new_gp64("ir_cv_cp_result");
                InvokeNode *cp_inv;
                arch::invoke_imm(
                    cc, &cp_inv, (uint64_t)(uintptr_t)jit_string_code_point_at, FuncSignature::build<uint64_t, void *, void *, int64_t>()
                );
                cp_inv->set_arg(0, vm_arg0());
                cp_inv->set_arg(1, cp_string);
                cp_inv->set_arg(2, cp_index);
                cp_inv->set_ret(0, cp_result);
                arch::store(cc, arch::ptr(endp, (int)(-3 * ValSize)), cp_result);
                arch::sub_imm(cc, endp, (int)(2 * ValSize));
                fc.set(endp);
                arch::jmp(cc, done);
            } // want_code_point

            cc.bind(slow);
            call_u32_u32((const void *)jit_call_value, argc, callee_label_idx);
            fc.merge_slow_path();
            cc.bind(done);
            return;
        }

        if (argc > 4 || getenv("NARI_ENABLE_DIRECT_CALL_JIT") == nullptr) {
            call_u32_u32((const void *)jit_call_value, argc, callee_label_idx);
            return;
        }
        Label slow = cc.new_label();
        Label done = cc.new_label();
        auto jump_if_owned = [&](const arch::Mem &field, const char *name) {
            arch::Gp value = cc.new_gp64(name);
            arch::load(cc, value, field);
            arch::test_zero(cc, value);
            arch::jcc(cc, arch::CC::kNE, slow);
        };

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
        arch::Gp locals = cc.new_gp64("ir_cv_locals");
        arch::load32_zx(cc, locals, fd, (int)FDLocalsCountOff);

        arch::Gp inline_kind = cc.new_gp64("ir_cv_inline");
        arch::load32_zx(cc, inline_kind, fd, (int)FDInlineKindOff);
        arch::cmp_imm(cc, inline_kind, Imm(to_int(JitInlineKind::Capture0)));
        arch::jcc(cc, arch::CC::kEQ, slow);
        jump_if_owned(arch::ptr(fd, (int)FDCapturesRawOff), "ir_cv_captures");

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
        arch::Gp frame_begin = cc.new_gp64("ir_cv_frame_begin");
        arch::load(cc, frame_begin, arch::ptr(vm_reg, (int)FramesStartOff));
        arch::add_imm(cc, frame_begin, (int64_t)MAX_CALL_DEPTH * FrameSize);
        cc.cmp(ff, frame_begin);
        arch::jcc(cc, arch::CC::kUGE, slow);

        // Reusing a raw frame slot is safe only while it owns no C++ objects.
        // push_jit_frame() remains the slow path that releases dirty slots.
        jump_if_owned(arch::ptr(ff, (int)FrameCapturesOff), "ir_cv_frame_caps_ptr");
        jump_if_owned(arch::ptr(ff, (int)(FrameCapturesOff + 8)), "ir_cv_frame_caps_owner");
        jump_if_owned(arch::ptr(ff, (int)FrameInlineUpvalueOff), "ir_cv_frame_inline_ptr");
        jump_if_owned(arch::ptr(ff, (int)(FrameInlineUpvalueOff + 8)), "ir_cv_frame_inline_owner");
        jump_if_owned(arch::ptr(ff, (int)FrameOpenUpvalOff), "ir_cv_frame_upvals");
        jump_if_owned(arch::ptr(ff, (int)FrameSingleCaptureCacheOff), "ir_cv_frame_cache_ptr");
        jump_if_owned(arch::ptr(ff, (int)(FrameSingleCaptureCacheOff + 8)), "ir_cv_frame_cache_owner");

        arch::Gp meta = cc.new_gp64("ir_cv_meta");
        arch::load(cc, meta, arch::ptr(fd, (int)FDJitMetaOff));
        arch::test_zero(cc, meta);
        arch::jcc(cc, arch::CC::kEQ, slow);
        arch::cmp_mem8_imm(cc, arch::ptr8(meta, (int)FMRestParamIndexOff), Imm(-1));
        arch::jcc(cc, arch::CC::kNE, slow);
        arch::Gp param_count = cc.new_gp64("ir_cv_params");
        arch::load8_zx(cc, param_count, arch::ptr(meta, (int)FMParamCountOff));
        arch::cmp_imm(cc, param_count, Imm((int)argc));
        Label args_ready = cc.new_label();
        arch::jcc(cc, arch::CC::kULE, args_ready);
        arch::cmp_mem8_imm(cc, arch::ptr8(meta, (int)FMJsUndefinedParamsOff), Imm(0));
        arch::jcc(cc, arch::CC::kNE, slow);
        cc.bind(args_ready);

        arch::Gp closure = cc.new_gp64("ir_cv_closure");
        arch::load(cc, closure, arch::ptr(endp, (int)(-(int64_t)(argc + 1) * ValSize)));

        arch::Gp nf = cc.new_gp64("ir_cv_nf");
        cc.mov(nf, locals);
        arch::shl(cc, nf, nf, 3);
        arch::add2(cc, nf, endp);
        arch::sub_imm(cc, nf, (int64_t)(argc + 1) * (int64_t)ValSize);

        arch::Gp stack_cap = cc.new_gp64("ir_cv_stack_cap");
        arch::load(cc, stack_cap, arch::ptr(vm_reg, (int)VMStackCapacityOff));
        cc.cmp(nf, stack_cap);
        arch::jcc(cc, arch::CC::kUGT, slow);

        for (int i = 0; i < (int)argc; i++) {
            int src = (i - (int)argc) * (int)ValSize;
            int dst = src - (int)ValSize;
            arch::Gp tmp = cc.new_gp64("ir_cv_arg");
            arch::load(cc, tmp, arch::ptr(endp, src));
            arch::store(cc, arch::ptr(endp, dst), tmp);
        }

        arch::Gp initp = cc.new_gp64("ir_cv_initp");
        arch::lea(cc, initp, endp, (int64_t)(-(int32_t)ValSize));
        arch::Gp none = cc.new_gp64("ir_cv_none");
        cc.mov(none, Imm((uint64_t)Value::none().raw_bits()));
        Label init_loop = cc.new_label();
        Label init_done = cc.new_label();
        cc.bind(init_loop);
        cc.cmp(initp, nf);
        arch::jcc(cc, arch::CC::kUGE, init_done);
        arch::store(cc, arch::ptr(initp), none);
        arch::add_imm(cc, initp, (int)ValSize);
        arch::jmp(cc, init_loop);
        cc.bind(init_done);
        arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), nf);

        arch::Gp sbp = cc.new_gp64("ir_cv_sbp");
        arch::lea(cc, sbp, endp, (int64_t)(-(int32_t)((int64_t)(argc + 1) * ValSize)));
        arch::Gp sbi = cc.new_gp64("ir_cv_sbi");
        cc.mov(sbi, sbp);
        arch::sub_mem(cc, sbi, arch::ptr(vm_reg, (int)VMStackStartOff));
        arch::shr(cc, sbi, 3);

        arch::Gp ip = cc.new_gp64("ir_cv_ip");
        arch::load(cc, ip, arch::ptr(meta, (int)FMCodeDataOff));
        arch::store(cc, arch::ptr(ff, (int)FrameFunctionOff), meta);
        arch::store(cc, arch::ptr(ff, (int)FrameIpOff), ip);
        arch::store(cc, arch::ptr(ff, (int)SlotBaseOff), sbi);
        arch::store_imm(cc, arch::ptr(ff, (int)FrameCapturesOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)(FrameCapturesOff + 8)), Imm(0));
        arch::store(cc, arch::ptr(ff, (int)FrameClosureRootOff), closure);
        arch::store(cc, arch::ptr(ff, (int)FrameReceiverOff), none);
        arch::store_imm(cc, arch::ptr(ff, (int)FrameInlineUpvalueIdxOff), Imm(UINT16_MAX));
        arch::store_imm(cc, arch::ptr(ff, (int)FrameInlineUpvalueOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)(FrameInlineUpvalueOff + 8)), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)FrameOpenUpvalOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)FrameSingleCaptureCacheOff), Imm(0));
        arch::store_imm(cc, arch::ptr(ff, (int)(FrameSingleCaptureCacheOff + 8)), Imm(0));
        arch::add_imm(cc, ff, (int)FrameSize);
        arch::store(cc, arch::ptr(vm_reg, (int)FramesFinishOff), ff);

        // The trampoline invokes another JITted Nari function which reads and writes the VM stack.
        fc.invalidate();
        InvokeNode *inv;
        (void)vm_arg0();
        cc.invoke(Out(inv), callee, FuncSignature::build<void, void *>());
        inv->set_arg(0, vm_arg_scratch);
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32_u32((const void *)jit_call_value, argc, callee_label_idx);
        cc.bind(done);
    };
    // register lowering
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
        auto raw_rep = [](ir::Ty t) { return t == ir::Ty::Unknown || t == ir::Ty::Heap; };
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
                    if (operand < 0 || (size_t)operand >= value_block.size() ||
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
                        if (in.imm_int >= 0 && (size_t)in.imm_int < slot_types.size() && raw_rep(slot_types[(uint32_t)in.imm_int]) &&
                            !raw_rep(slot_types[in.imm_u32])) {
                            elig = false;
                            if (!reject) {
                                reject = "copyslot raw->untagged";
                            }
                        }
                        // a Float xmm dst reads slot_vec[src], so src
                        // must also be an xmm Float slot.
                        if (in.imm_u32 < slot_types.size() && slot_types[in.imm_u32] == ir::Ty::Float &&
                            !(in.imm_int >= 0 && (size_t)in.imm_int < slot_types.size() &&
                              slot_types[(uint32_t)in.imm_int] == ir::Ty::Float)) {
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
                            snprintf(buf, sizeof(buf), "op %d", (int)in.op);
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
                    if (operand < 0 || (size_t)operand >= value_block.size() ||
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
            auto call_push_reg = [&](const void *helper, arch::Gp val) {
                InvokeNode *inv;
                (void)vm_arg0();
                arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)helper, FuncSignature::build<void, void *, int64_t>());
                inv->set_arg(0, vm_arg_scratch);
                inv->set_arg(1, val);
            };
            std::vector<arch::Gp> slot_reg(irFuncs.num_slots);
            // parallel XMM home for Float slots. slot_reg[s] stays
            // default-constructed (unused) for xmm slots
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
                        arch::load16_zx(cc, ptag, arch::ptr16(pbase, (int)((int64_t)s * ValSize + tagWordOff)));
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
            auto escape_ex = [&](const void *helper, int kind, uint32_t immA, uint32_t immB, std::vector<RE *> extra, size_t consumed,
                                 bool produces, ir::Ty result_ty, int kind_hint = -1) -> RE {
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
                } else if (kind == 3) {
                    // the inline call fast path
                    emit_ir_call_value(immA, immB, kind_hint);
                } else {
                    emit_call_typeof();
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
                const int64_t drop = (int64_t)(n - consumed + (produces ? 1 : 0)) * ValSize;
                if (drop) {
                    arch::sub_imm(cc, fin2, drop);
                    arch::store(cc, arch::ptr(vm_reg, (int)VMStackFinishOff), fin2);
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
                            arch::load(cc, hdr_base[s], arch::ptr(ptr, (int)ArrayVecStartOff));
                            arch::load(cc, hdr_size[s], arch::ptr(ptr, (int)ArrayVecFinishOff));
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
                                RE r = escape_ex((const void *)jit_mod, 0, 0, 0, { &ra, &rb }, 2, true, ir::Ty::Int48);
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
                                    RE r = escape_ex((const void *)jit_get_index, 0, 0, 0, { &ra, &ri }, 2, true, rty);
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
                                arch::cmp_mem8_imm(cc, arch::ptr8(ptr, (int)HeapTypeTagOff), Imm((int)tagArray));
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
                                // Bounds check, unsigned-compare form. Nari allows negative indices (from the
                                // end), but the overwhelmingly common case is 0 <= idx < size, and an *unsigned*
                                // compare decides that in one branch: a negative index looks huge unsigned, so it
                                // fails the same test as idx >= size. The wrap is handled off the fast path, and
                                // needs no second range test -- after idx += size with idx < 0, idx < size holds.
                                cc.cmp(idx, size);
                                arch::jcc(cc, arch::CC::kULT, positive);
                                arch::jns(cc, idx, oob);
                                arch::add2(cc, idx, size);
                                arch::js(cc, idx, oob);
                                cc.bind(positive);
                                arch::shl(cc, g, idx, 3);
                                arch::add2(cc, g, start);
                                arch::load(cc, g, arch::ptr(g, 0));
                                arch::jmp(cc, done);
                                cc.bind(oob);
                                cc.mov(g, Imm(static_cast<int64_t>(nbNone)));
                                arch::jmp(cc, done);
                                cc.bind(slow);
                                RE r = escape_ex((const void *)jit_get_index, 0, 0, 0, { &ra, &ri }, 2, true, rty);
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
                            // Bounds check, unsigned-compare form. Nari allows negative indices (from the
                            // end), but the overwhelmingly common case is 0 <= idx < size, and an *unsigned*
                            // compare decides that in one branch: a negative index looks huge unsigned, so it
                            // fails the same test as idx >= size. The wrap is handled off the fast path, and
                            // needs no second range test -- after idx += size with idx < 0, idx < size holds.
                            cc.cmp(idx, size);
                            arch::jcc(cc, arch::CC::kULT, positive);
                            arch::jns(cc, idx, oob);
                            arch::add2(cc, idx, size);
                            arch::js(cc, idx, oob);
                            cc.bind(positive);
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
                                    RE r = escape_ex((const void *)jit_set_index, 0, 0, 0, { &ra, &ri, &rv }, 3, true, in.type);
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
                                arch::cmp_mem8_imm(cc, arch::ptr8(ptr, (int)HeapTypeTagOff), Imm((int)tagArray));
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
                                escape_ex((const void *)jit_set_index, 0, 0, 0, { &ra, &ri, &rv }, 3, true, in.type);
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
                            escape_ex((const void *)jit_set_index, 0, 0, 0, { &ra, &ri, &rv }, 3, true, in.type);
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
                            RE r = escape_ex((const void *)jit_make_array, 1, nelem, 0, eptrs, nelem, true, in.type);
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::ArraySpread: {
                            RE iterable = st.back();
                            st.pop_back();
                            RE target = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &target, &iterable };
                            RE result = escape_ex((const void *)jit_array_spread, 0, 0, 0, operands, 2, true, in.type);
                            st.push_back(result);
                            break;
                        }
                        case ir::Op::ArrayPush: {
                            RE value = st.back();
                            st.pop_back();
                            RE target = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &target, &value };
                            RE result = escape_ex((const void *)jit_array_push, 0, 0, 0, operands, 2, true, in.type);
                            st.push_back(result);
                            break;
                        }
                        case ir::Op::StrAppendSlot: {
                            RE rhs = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &rhs };
                            RE result = escape_ex((const void *)jit_str_append_slot, 1, in.imm_u32, 0, operands, 1, true, in.type);
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
                            const void *helper = direct_to_number ? (const void *)jit_call_js_to_number : nullptr;
                            const int kind = direct_typeof ? 4 : helper ? 0 : 3;
                            RE r =
                                escape_ex(helper, kind, argc, (uint32_t)in.imm_int, aptrs, argc + 1, true, in.type, callee_kind_hint(in));
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::CallSpread: {
                            RE args = st.back();
                            st.pop_back();
                            RE callee = st.back();
                            st.pop_back();
                            std::vector<RE *> operands = { &callee, &args };
                            RE result = escape_ex((const void *)jit_call_spread, 1, in.imm_u32, 0, operands, 2, true, in.type);
                            st.push_back(result);
                            break;
                        }
                        case ir::Op::CallMethod: {
                            const uint32_t method_idx = in.imm_u32;
                            const uint32_t argc = (uint32_t)in.imm_int;
                            // Inline arr.push(<raw int/bool>)
                            bool inline_push = method_idx < chunk.strings.size() && chunk.strings[method_idx] == "push" && argc == 1 &&
                                               in.operands.size() >= 2;
                            bool recv_proven = false;
                            if (inline_push) {
                                const ir::Inst &recv = irFuncs.inst(in.operands[0]);
                                const ir::Inst &arg = irFuncs.inst(in.operands[1]);
                                recv_proven = recv.op == ir::Op::LoadSlot && int_arr_slots.count(recv.imm_u32) != 0;
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
                                    arch::cmp_mem8_imm(cc, arch::ptr8(ptr, (int)HeapTypeTagOff), Imm((int)tagArray));
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
                                    RE r = escape_ex(
                                        (const void *)jit_call_method, 2, method_idx, argc, { &rrecv, &rarg }, argc + 1, true, in.type
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
                            RE r = escape_ex((const void *)jit_call_method, 2, method_idx, argc, aptrs, argc + 1, true, in.type);
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
                                RE r = escape_ex((const void *)jit_load_global, 1, name_idx, 0, {}, 0, true, in.type);
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
                            RE r = escape_ex((const void *)jit_js_truthy, 0, 0, 0, { &ra }, 1, true, ir::Ty::Bool);
                            st.push_back(r);
                            break;
                        }
                        case ir::Op::IsNone: {
                            RE ra = st.back();
                            st.pop_back();
                            arch::Gp raw = box_re(ra);
                            arch::Gp none = cc.new_gp64("r_none");
                            arch::Gp result = cc.new_gp64("r_is_none");
                            cc.mov(none, Imm(static_cast<int64_t>(nbNone)));
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
                                const void *helper = is_add ? (const void *)jit_add : (const void *)jit_sub;
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
                                RE r = escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, rty);
                                arch::mov_reg(cc, g, r.reg);
                                cc.bind(done);
                                st.push_back({ g, rty });
                                break;
                            }
                            arch::Gp g = cc.new_gp64();
                            const bool is_add_op = in.op == ir::Op::DynAdd || in.op == ir::Op::IAdd;
                            // Prefer the 3-operand forms: `g = a + b` as one LEA
                            // rather than `mov g,a; add g,b`. Neither this path nor
                            // its consumers read flags (int48 overflow is handled by
                            // range analysis before we get here, not by a flag test).
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
                        // dynamic mul/div, escape to the general-path helper
                        case ir::Op::DynMul:
                        case ir::Op::DynDiv:
                        case ir::Op::DynMod: {
                            RE rb = st.back();
                            st.pop_back();
                            RE ra = st.back();
                            st.pop_back();
                            const void *helper = (in.op == ir::Op::DynMul)   ? (const void *)jit_mul
                                                 : (in.op == ir::Op::DynDiv) ? (const void *)jit_div
                                                                             : (const void *)jit_mod;
                            RE r = escape_ex(helper, 0, 0, 0, { &ra, &rb }, 2, true, esc_result_ty(in.type));
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
                                    case ir::Op::DynStrictCmpEq:
                                        h = (const void *)jit_strict_eq;
                                        break;
                                    case ir::Op::DynStrictCmpNe:
                                        h = (const void *)jit_strict_ne;
                                        break;
                                    default:
                                        h = (const void *)jit_ne;
                                        break;
                                }
                                RE r = escape_ex(h, 0, 0, 0, { &ra, &rb }, 2, true, ir::Ty::Bool);
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
                    if (irFuncs.blocks[t.target0].start_pc <= b.start_pc) {
                        poll_shutdown();
                    }
                    // skip the jump if the target is the next block
                    if (!(fallthrough_opt && t.target0 == fallthrough_bid[bid])) {
                        arch::jmp(cc, rlabels[t.target0]);
                    }
                } else if (t.op == ir::Op::Branch) {
                    if (irFuncs.blocks[t.target0].start_pc <= b.start_pc || irFuncs.blocks[t.target1].start_pc <= b.start_pc) {
                        poll_shutdown();
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
                            c = escape_ex((const void *)jit_not, 0, 0, 0, { &c }, 1, true, ir::Ty::Bool);
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
                (void)vm_arg0();
                arch::invoke_imm(cc, &fb_inv, (uint64_t)(uintptr_t)spec_fallback, FuncSignature::build<void, void *>());
                fb_inv->set_arg(0, vm_arg_scratch);
                cc.ret();
            }
            cc.bind(shutdown_requested);
            {
                InvokeNode *invoke;
                (void)vm_arg0();
                arch::invoke_imm(cc, &invoke, (uint64_t)(uintptr_t)jit_poll_shutdown, FuncSignature::build<void, void *>());
                invoke->set_arg(0, vm_arg_scratch);
            }
            cc.ret();
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
                chunk.functions[chunk_idx].name.empty() ? std::string("<anon>") + tier_tag : chunk.functions[chunk_idx].name + tier_tag,
                asm_logger.data(), (uint64_t)(uintptr_t)spec_fallback, spec_fallback ? "spec_fallback (general tier)" : nullptr
            );

            {
                std::string sym = chunk.functions[chunk_idx].name.empty()
                                      ? std::string(spec ? "anon_ir_reg_spec" : "anon_ir_reg")
                                      : chunk.functions[chunk_idx].name + (spec ? "_ir_reg_spec" : "_ir_reg");
                register_gdb_jit_function(sym, reinterpret_cast<const void *>(fn), sz);
                perf_jitdump_register(sym, reinterpret_cast<const void *>(fn), sz);
            }
            report(spec ? "OK  [ir-reg spec-param]" : "OK  [ir-reg fast path]");
            return fn;
        }
    }
    // speculative mode compiles only the register tier, the general-path version already exists (it's `spec_fallback`).
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
        return irFuncs.inst(in.operands[0]).type == ir::Ty::Float && irFuncs.inst(in.operands[1]).type == ir::Ty::Float;
    };
    auto both_int = [&](const ir::Inst &in) {
        return irFuncs.inst(in.operands[0]).type == ir::Ty::Int48 && irFuncs.inst(in.operands[1]).type == ir::Ty::Int48;
    };
    // Per-operand: an Int48-typed operand carries tagInt by construction, so its
    // runtime tag guard is dead code even when the other operand is unknown.
    auto lhs_int = [&](const ir::Inst &in) { return in.operands.size() >= 2 && irFuncs.inst(in.operands[0]).type == ir::Ty::Int48; };
    auto rhs_int = [&](const ir::Inst &in) { return in.operands.size() >= 2 && irFuncs.inst(in.operands[1]).type == ir::Ty::Int48; };
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
        arch::load(cc, slot_base_bytes_vreg, arch::ptr(frame_init, (int)SlotBaseOff));
        arch::shl(cc, slot_base_bytes_vreg, slot_base_bytes_vreg, 3);
    }
    fc.bind_slot_base_bytes(slot_base_bytes_vreg);

    auto emit_ir_slot_addr = [&](uint32_t slot, const char *name) {
        (void)name;
        // stack_start + slot_base_bytes + slot*8, with the base cached per block
        // instead of reloaded and re-added for every slot access. IR functions
        // can't have upvalues, so slot_base is fixed and no upvalue guard is
        // needed. Callers only ever read the returned register.
        arch::Gp base = fc.slot_base();
        if (slot == 0) {
            return base;
        }
        arch::Gp addr = cc.new_gp64("ir_sa");
        arch::lea(cc, addr, base, (int64_t)slot * (int64_t)ValSize);
        return addr;
    };
    // Slot access as a memory operand: folds the slot displacement into the
    // load/store, so no address register (and no extra vreg for the register
    // allocator) is needed at all.
    auto emit_ir_slot_mem = [&](uint32_t slot) { return arch::ptr(fc.slot_base(), (int)((int64_t)slot * (int64_t)ValSize)); };
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
    SlotRegCache slot_cache(cc, irFuncs.num_slots);

    auto emit_ir_load_slot = [&](uint32_t slot) {
        // fast path via SlotRegCache. If this slot's raw value is
        // already in a vreg from an earlier LoadSlot/StoreSlot in this block, skip the memory reload.
        {
            arch::Gp cached;
            if (slot_cache.try_get(slot, cached)) {
                fc.push_deferred(cached);
                return;
            }
        }
        arch::Mem slotm = emit_ir_slot_mem(slot);
        arch::Gp raw = cc.new_gp64("ir_ls_raw");
        arch::load(cc, raw, slotm);
        fc.push_deferred(raw);
        // cache the just-loaded raw value for this slot.
        slot_cache.set(slot, raw);
    };
    auto emit_ir_store_slot = [&](uint32_t slot) {
        arch::Mem slotm = emit_ir_slot_mem(slot);
        // store_slot doesn't modify _M_finish, and it need not touch the value stack
        // at all while its input is still pending: reading the value out of the
        // register avoids both the load and -- more importantly -- the queue flush
        // that fc.get() would force at every assignment. The entry stays queued,
        // because store_slot does not pop.
        arch::Gp raw;
        if (const auto *top = fc.peek(0)) {
            if (top->is_imm) {
                raw = cc.new_gp64("ir_ss_imm");
                cc.mov(raw, Imm(top->imm));
            } else {
                raw = top->reg;
            }
        } else {
            arch::Gp endp = fc.get();
            raw = cc.new_gp64("ir_ss_raw");
            arch::load(cc, raw, arch::ptr(endp, (int)(-ValSize)));
        }
        arch::store(cc, slotm, raw);
        // cache the just-stored raw value for this slot.
        slot_cache.set(slot, raw);
    };
    // fused slot-store emitters. Write a raw 64-bit value directly into a stack slot
    auto emit_ir_store_raw_to_slot = [&](uint32_t slot, uint64_t raw) {
        arch::Mem slotm = emit_ir_slot_mem(slot);
        arch::Gp rawr = cc.new_gp64("ir_sifs_raw");
        cc.mov(rawr, Imm((int64_t)raw));
        arch::store(cc, slotm, rawr);
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
                    emit_ir_store_raw_to_slot(slot, Value::make_int_checked(cst.as_int).raw_bits());
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
                    auto &mut_chunk = const_cast<nari::bytecode::Chunk &>(chunk);
                    emit_ir_store_raw_to_slot(slot, mut_chunk.get_const_string(cst.string_idx).raw_bits());
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
        arch::Mem srcm = emit_ir_slot_mem(src);
        arch::Mem dstm = emit_ir_slot_mem(dst);
        arch::Gp raw = cc.new_gp64("ir_cp_raw");
        arch::Gp cached;
        if (slot_cache.try_get(src, cached)) {
            arch::mov_reg(cc, raw, cached);
        } else {
            arch::load(cc, raw, srcm);
            slot_cache.set(src, raw);
        }
        arch::store(cc, dstm, raw);
        // dst now holds `raw`; propagate to cache.
        slot_cache.set(dst, raw);
    };
    auto emit_ir_pop = [&]() {
        // Value is NaN-boxed, trivially copyable, with no destructor (heap lifetime is GC-managed)
        // Popping a push that never reached memory is free: dropping the queue entry
        // cancels the two of them. Doing this through fc.get() instead would
        // materialize the value and then immediately pop it again.
        if (fc.peek(0) != nullptr) {
            fc.drop_pend(1);
            return;
        }
        arch::Gp endp = fc.get();
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
    };
    auto emit_ir_dup = [&]() {
        // Plain 8-byte copy. Value has no dtor/refcount, so dup of a heap value is
        // the same raw store as a non-heap one.
        // Capacity check is covered by the function-entry reservation.
        // If the top of stack is still a pending push, duplicating it is just
        // recording the same vreg twice: zero instructions.
        if (const auto *top = fc.peek(0)) {
            fc.push_pend(*top);
            return;
        }
        arch::Gp endp = fc.get();
        arch::Gp raw = cc.new_gp64("ir_dup_raw");
        arch::load(cc, raw, arch::ptr(endp, (int)(-ValSize)));
        fc.push_deferred(raw);
    };
    auto emit_ir_load_cached_capture = [&](int64_t capture_offset, const char *name) {
        Label missing = cc.new_label();
        Label store = cc.new_label();
        // Both values are dead by the end of this sequence: `cell` after the cell
        // load, `raw` after the push. `raw` is defined on both arms and merged at
        // `store`, so using one fixed register on each arm keeps them consistent.
        // `raw` outlives this emitter as a deferred push, so it cannot be the
        // shared const_scratch; `cell` is dead at the merge and still can be.
        arch::Gp cell = cmp_scratch;
        arch::Gp raw = cc.new_gp64(name);

        arch::load(cc, cell, arch::ptr(vm_reg, (int)capture_offset));
        arch::test_zero(cc, cell);
        arch::jcc(cc, arch::CC::kEQ, missing);
        arch::load(cc, raw, arch::ptr(cell, 0));
        arch::jmp(cc, store);
        cc.bind(missing);
        cc.mov(raw, Imm((int64_t)nbNone));
        cc.bind(store);

        // No helper call and no fc use between the branches, so an incoming
        // pending queue stays valid across them.
        fc.push_deferred(raw);
    };
    auto emit_ir_store_cached_capture = [&](int64_t capture_offset, const char *name) {
        Label done = cc.new_label();
        arch::Gp cell = cc.new_gp64(name);
        arch::load(cc, cell, arch::ptr(vm_reg, (int)capture_offset));
        arch::test_zero(cc, cell);
        arch::jcc(cc, arch::CC::kEQ, done);
        arch::Gp endp = fc.get();
        arch::Gp raw = cc.new_gp64("ir_sc_raw");
        arch::load(cc, raw, arch::ptr(endp, (int)-ValSize));
        arch::store(cc, arch::ptr(cell, 0), raw);
        cc.bind(done);
    };
    auto emit_ir_load_global = [&](uint32_t name_idx) {
        // Inlines jit_load_global's fast path, name_idx is a compile-time constant
        // Any incoming pending pushes must reach memory before the branch below so
        // that both arms agree on the materialized stack; only the result differs,
        // and it is carried in one vreg (`raw`) that both arms define. get() (not
        // just materialize()) also guarantees the cached endp register is live on
        // entry, so both arms leave it in the same state and the join needs no
        // invalidate -- which would otherwise force every downstream op to reload it.
        (void)fc.get();
        Label slow = cc.new_label();
        Label done = cc.new_label();

        // name_idx < global_cache_valid.size() <=> (vstart + name_idx) < vfin
        arch::Gp vaddr = cc.new_gp64("ir_lg_vaddr");
        arch::Gp vfin = cmp_scratch; // dead immediately after the cmp below
        arch::load(cc, vaddr, arch::ptr(vm_reg, (int)VMGlobalCacheValidStartOff));
        arch::load(cc, vfin, arch::ptr(vm_reg, (int)VMGlobalCacheValidFinishOff));
        arch::add_imm(cc, vaddr, (int64_t)name_idx);
        cc.cmp(vaddr, vfin);
        arch::jcc(cc, arch::CC::kFGE, slow);

        // global_cache_valid[name_idx] != 0
        arch::Gp valid = cmp_scratch; // vfin is dead here; vaddr is a distinct reg
        arch::load8_zx(cc, valid, arch::ptr8(vaddr, 0));
        arch::test_zero(cc, valid);
        arch::jcc(cc, arch::CC::kEQ, slow);

        // stack capacity check covered by the function-entry reservation.
        // read global_cache[name_idx] (plain 8-byte copy; no write barrier / refcount)
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
        // The push is deferred: no value store, no stack bump, and no write-back of
        // the finish pointer to the VM on this hot arm.
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32((const void *)jit_load_global, name_idx);
        fc.merge_slow_path();
        // The helper pushed its result to memory. Bring it back into the same vreg
        // and undo the bump so this arm matches the fast arm's deferred state
        // (memory finish below the result, result pending in `raw`). This arm is
        // cold, and duplicating work into cold blocks is free (iteration 24).
        {
            arch::Gp e = fc.get();
            arch::load(cc, raw, arch::ptr(e, (int)-ValSize));
            arch::sub_imm(cc, e, (int)ValSize);
            fc.set(e);
        }
        cc.bind(done);
        fc.push_deferred(raw);
    };
    auto emit_ir_js_get_prop_static = [&](uint32_t name_idx) {
        static const bool disabled = std::getenv("NARI_DISABLE_JS_GET_PROP_STATIC") != nullptr;
        if (disabled) {
            call_u32((const void *)jit_js_get_prop_static, name_idx);
            return;
        }

        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp ic = cc.new_gp64("ir_jsgp_ic");
        arch::load(cc, ic, arch::ptr(vm_reg, (int)VMJsGetPropStaticICStartOff));
        arch::add_imm(cc, ic, (int64_t)name_idx * (int64_t)sizeof(JsGetPropStaticIC));

        arch::Gp endp = fc.get();
        arch::Gp raw = cc.new_gp64("ir_jsgp_raw");
        arch::Gp tag = cc.new_gp64("ir_jsgp_tag");
        arch::load(cc, raw, arch::ptr(endp, (int)-ValSize));
        arch::mov_reg(cc, tag, raw);
        arch::shr(cc, tag, 48);
        arch::cmp_imm(cc, tag.r32(), Imm((int)tagHeap));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::zero_extend_48(cc, raw);
        arch::cmp_mem8_imm(cc, arch::ptr8(raw, (int)HeapTypeTagOff), Imm((int)ValueTag::Object));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp shape = cc.new_gp64("ir_jsgp_shape");
        arch::load(cc, shape, arch::ptr(ic, (int)JsGetPropStaticICShapeOff));
        Label primary = cc.new_label();
        arch::cmp_mem(cc, shape, arch::ptr(raw, (int)ObjShapeOff));
        arch::jcc(cc, arch::CC::kEQ, primary);
        arch::load(cc, shape, arch::ptr(ic, (int)JsGetPropStaticICShape2Off));
        arch::cmp_mem(cc, shape, arch::ptr(raw, (int)ObjShapeOff));
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp lazy_mask = cc.new_gp64("ir_jsgp_lazy_mask");
        arch::load(cc, lazy_mask, arch::ptr(ic, (int)JsGetPropStaticICLazyMask2Off));
        arch::Gp slot = cc.new_gp64("ir_jsgp_slot");
        arch::load32_zx_mem(cc, slot, arch::ptr(ic, (int)JsGetPropStaticICSlot2Off));
        Label selected = cc.new_label();
        arch::jmp(cc, selected);

        cc.bind(primary);
        arch::load(cc, lazy_mask, arch::ptr(ic, (int)JsGetPropStaticICLazyMaskOff));
        arch::load32_zx_mem(cc, slot, arch::ptr(ic, (int)JsGetPropStaticICSlotOff));

        cc.bind(selected);
        arch::Gp object_lazy_mask = cc.new_gp64("ir_jsgp_object_lazy_mask");
        arch::load(cc, object_lazy_mask, arch::ptr(raw, (int)ObjLazyFieldMaskOff));
        arch::and_reg(cc, object_lazy_mask, lazy_mask);
        arch::test_zero(cc, object_lazy_mask);
        arch::jcc(cc, arch::CC::kNE, slow);

        arch::Gp field = cc.new_gp64("ir_jsgp_field");
        arch::load(cc, field, arch::ptr(raw, (int)ObjFieldsStartOff));
        arch::Gp fields_end = cc.new_gp64("ir_jsgp_fields_end");
        arch::load(cc, fields_end, arch::ptr(raw, (int)ObjFieldsFinishOff));
        arch::shl(cc, slot, slot, 3);
        arch::add_reg(cc, field, slot);
        arch::cmp_jcc(cc, field, fields_end, arch::CC::kUGE, slow);
        arch::load(cc, raw, arch::ptr(field, 0));
        arch::store(cc, arch::ptr(endp, (int)-ValSize), raw);
        arch::jmp(cc, done);

        cc.bind(slow);
        call_u32((const void *)jit_js_get_prop_static, name_idx);
        fc.merge_slow_path();
        cc.bind(done);
    };
    // One reusable scratch for constant materialization. Every use is
    // def-then-store with no call in between, so the live ranges are disjoint and
    // short; giving the allocator a single vreg with many defs instead of one fresh
    // vreg per emitted constant cuts register-allocation work, and unlike splitting
    // the store into two dwords it keeps a single qword store (a split store to a
    // slot later loaded as a qword costs a store-forwarding stall: measured -0.27%
    // instructions but +4.7% time).
    auto emit_ir_raw_const = [&](uint64_t raw, const char *name) {
        // Deferred as a bare immediate: the `mov reg, imm64` is emitted only if and
        // where the value actually has to become a register or reach memory.
        (void)name;
        fc.push_deferred_imm((int64_t)raw);
    };
    auto emit_ir_fconst = [&](double imm) {
        // FConst's operand is a compile-time constant double. make_float()
        // memcpys the bits straight into _raw with no tag, so the pushed
        // value is just the f64 bit pattern. Never a heap pointer, no write
        // barrier. Mirrors emit_ir_iconst exactly.
        uint64_t bits;
        memcpy(&bits, &imm, sizeof(bits));
        emit_ir_raw_const(bits, "ir_fc_raw");
    };
    auto emit_ir_iconst = [&](int64_t imm) { emit_ir_raw_const(Value::make_int_checked(imm).raw_bits(), "ir_ic_raw"); };
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
        // Deferred as a bare immediate, like every other constant: no store, no
        // stack bump, no finish write-back -- and, unlike the eager form, no
        // fc.get(), which used to flush the whole pending-push queue at every
        // string constant.
        fc.push_deferred_imm((int64_t)raw);
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
            if (recv.op == ir::Op::LoadSlot && int_arr_slots.count(recv.imm_u32)) {
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
    auto emit_int_binop = [&](ir::Op op, bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false) {
        const bool skip_tag_check = skip_lhs_tag_check && skip_rhs_tag_check;
        asmjit::Label slow = cc.new_label();
        asmjit::Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_rhs_tag_check) {
            arch::Gp tagB = cc.new_gp64("ib_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        if (!skip_lhs_tag_check) {
            arch::Gp tagA = cc.new_gp64("ib_ta");
            arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
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
    auto emit_int_mul = [&](bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false) {
        const bool skip_tag_check = skip_lhs_tag_check && skip_rhs_tag_check;
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_rhs_tag_check) {
            arch::Gp tagB = cc.new_gp64("im_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        if (!skip_lhs_tag_check) {
            arch::Gp tagA = cc.new_gp64("im_ta");
            arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
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
    auto emit_int_mod = [&](bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false) {
        const bool skip_tag_check = skip_lhs_tag_check && skip_rhs_tag_check;
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_rhs_tag_check) {
            arch::Gp tagB = cc.new_gp64("imod_tb");
            arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        if (!skip_lhs_tag_check) {
            arch::Gp tagA = cc.new_gp64("imod_ta");
            arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
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
    auto emit_int_cmp = [&](ir::Op op, bool skip_lhs_tag_check = false, bool skip_rhs_tag_check = false) {
        const bool skip_tag_check = skip_lhs_tag_check && skip_rhs_tag_check;
        Label slow = cc.new_label();
        Label done = cc.new_label();
        arch::Gp endp = fc.get();
        if (!skip_rhs_tag_check) {
            arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
            arch::cmp_imm(cc, cmp_scratch.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        }
        if (!skip_lhs_tag_check) {
            arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, (int)(-2 * ValSize + tagWordOff)));
            arch::cmp_imm(cc, cmp_scratch.r32(), Imm((int)tagInt));
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
            case ir::Op::DynStrictCmpEq:
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
        // Strict equality with differing tags, neither of them a float, is provably
        // unequal -- that is jit_values_strict_equal's own second rule: tags differ
        // and either side is HEAP/BOOL/NONE => false. Tags >= TAG_HEAP are exactly
        // {HEAP, INT, BOOL, NONE} (0xFFFD is unused), and if they differ they cannot
        // both be INT, so at least one is HEAP/BOOL/NONE. Floats (tag < TAG_HEAP) are
        // excluded and still call the helper, which is required: `1 === 1.0` is true.
        // Measured on tsc: 15,059,405 of 26,819,373 jit_strict_eq calls (56%) land here.
        if (op == ir::Op::DynStrictCmpEq || op == ir::Op::DynStrictCmpNe) {
            const bool ne = (op == ir::Op::DynStrictCmpNe);
            // Exactly one operand is a proven Int48 here (both-proven returned early
            // above). That operand's tag IS tagInt, so two facts hold on this path:
            // the tags cannot be equal (the both-int fast path already ran), and the
            // proven operand is never a float. The entire same-tag/heap/string subtree
            // and one of the two float checks are therefore dead code. Measured on tsc:
            // 2885 of 6505 strict-eq sites qualify (2711 are `x === <int literal>`).
            const bool one_int = skip_lhs_tag_check || skip_rhs_tag_check;
            Label streq_helper = cc.new_label();
            Label same_tag = cc.new_label();
            Label unequal = cc.new_label();
            Label equal = cc.new_label();
            fc.invalidate();
            arch::Gp e = fc.get();
            if (one_int) {
                // only the unproven operand's tag matters
                const int unk = skip_rhs_tag_check ? (int)(-2 * ValSize) : (int)(-ValSize);
                arch::load16_zx(cc, cmp_scratch, arch::ptr16(e, unk + (int)tagWordOff));
            } else {
                arch::load16_zx(cc, cmp_scratch, arch::ptr16(e, (int)(-2 * ValSize + tagWordOff)));
                arch::load16_zx(cc, const_scratch, arch::ptr16(e, (int)(-ValSize + tagWordOff)));
                cc.cmp(cmp_scratch.r32(), const_scratch.r32());
                arch::jcc(cc, arch::CC::kEQ, same_tag); // same tag: needs a real compare
            }
            arch::cmp_imm(cc, cmp_scratch.r32(), Imm((int)tagHeap));
            arch::jcc(cc, arch::CC::kULT, streq_helper); // unproven side is a float
            if (!one_int) {
                arch::cmp_imm(cc, const_scratch.r32(), Imm((int)tagHeap));
                arch::jcc(cc, arch::CC::kULT, streq_helper); // rhs is a float
            }
            cc.bind(unequal);
            cc.mov(cmp_scratch, Imm((int64_t)(nbBoolTag | (ne ? 1ULL : 0ULL))));
            arch::store(cc, arch::ptr(e, (int)(-2 * ValSize)), cmp_scratch);
            arch::sub_imm(cc, e, (int)ValSize);
            fc.set(e);
            arch::jmp(cc, done);

            // Both operands carry the same tag. When that tag is HEAP, two of the three
            // outcomes need no helper: jit_values_strict_equal compares contents only for
            // String/String, so an identical pointer is equal and any non-String pair is
            // unequal. Measured on tsc, 13,058,370 of 13,163,089 helper calls arrive here
            // with two heap values; 40% are the same pointer and 52% are non-String, so
            // only ~8% still need the string compare.
            if (!one_int) {
                cc.bind(same_tag);
                arch::cmp_imm(cc, cmp_scratch.r32(), Imm((int)tagHeap));
                arch::jcc(cc, arch::CC::kNE, streq_helper); // int/bool/none/float: helper decides
                arch::load(cc, cmp_scratch, arch::ptr(e, (int)(-2 * ValSize)));
                arch::cmp_mem(cc, cmp_scratch, arch::ptr(e, (int)(-ValSize)));
                arch::jcc(cc, arch::CC::kEQ, equal); // same heap pointer
                arch::zero_extend_48(cc, cmp_scratch);
                arch::cmp_mem8_imm(cc, arch::ptr8(cmp_scratch, (int)HeapTypeTagOff), Imm((int)tagString));
                arch::jcc(cc, arch::CC::kNE, unequal);
                // lhs is a String, so the rhs pointer is worth loading now
                arch::load(cc, const_scratch, arch::ptr(e, (int)(-ValSize)));
                arch::zero_extend_48(cc, const_scratch);
                arch::cmp_mem8_imm(cc, arch::ptr8(const_scratch, (int)HeapTypeTagOff), Imm((int)tagString));
                arch::jcc(cc, arch::CC::kNE, unequal);
                arch::jmp(cc, streq_helper); // String === String: compare contents

                cc.bind(equal);
                cc.mov(cmp_scratch, Imm((int64_t)(nbBoolTag | (ne ? 0ULL : 1ULL))));
                arch::store(cc, arch::ptr(e, (int)(-2 * ValSize)), cmp_scratch);
                arch::sub_imm(cc, e, (int)ValSize);
                fc.set(e);
                arch::jmp(cc, done);
            } // !one_int

            cc.bind(streq_helper);
            fc.invalidate();
        }
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
            case ir::Op::DynStrictCmpEq:
                call0((const void *)jit_strict_eq);
                break;
            case ir::Op::DynStrictCmpNe:
                call0((const void *)jit_strict_ne);
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
        // or_imm folds the tag in directly (as emit_int_cmp already does), so the
        // extra vreg that only carried nbBoolTag is unnecessary.
        arch::or_imm(cc, out, out, (int64_t)nbBoolTag);
        arch::store(cc, arch::ptr(endp, (int)(-2 * ValSize)), out);
        arch::sub_imm(cc, endp, (int)ValSize);
        fc.set(endp);
    };
    // lower a straight-line (non-terminator) instruction.
    auto lower_body = [&](const ir::Inst &in) {
        if (jit_dump_asm_enabled()) {
            char obuf[64];
            snprintf(obuf, sizeof(obuf), "OP:%s", ir::op_name(in.op));
            cc.comment(obuf);
        }
        switch (in.op) {
            case ir::Op::IConst:
                emit_ir_iconst(in.imm_int);
                break;
            case ir::Op::FConst:
                emit_ir_fconst(in.imm_float);
                break;
            case ir::Op::BConst:
                emit_ir_raw_const(Value::make_bool(in.imm_int != 0).raw_bits(), "ir_bc_raw");
                break;
            case ir::Op::NConst:
                emit_ir_raw_const(Value::none().raw_bits(), "ir_nc_raw");
                break;
            case ir::Op::LoadConst:
                if (!emit_ir_sconst_inline(in.imm_u32)) {
                    call_u32((const void *)jit_load_const, in.imm_u32);
                }
                break;
            case ir::Op::LoadSlot:
                if (in.imm_u32 < irFuncs.captured_local_slots.size() && irFuncs.captured_local_slots[in.imm_u32]) {
                    call_u32((const void *)jit_load_var, in.imm_u32);
                } else {
                    emit_ir_load_slot(in.imm_u32);
                }
                break;
            case ir::Op::StoreSlot:
                if (in.imm_u32 < irFuncs.captured_local_slots.size() && irFuncs.captured_local_slots[in.imm_u32]) {
                    call_u32((const void *)jit_store_var, in.imm_u32);
                } else {
                    emit_ir_store_slot(in.imm_u32);
                }
                break;
            case ir::Op::LoadGlobal:
                emit_ir_load_global(in.imm_u32);
                break;
            case ir::Op::CloseUpvalues:
                call_u32((const void *)jit_close_upvalues, in.imm_u32);
                break;
            case ir::Op::StoreGlobal:
                call_u32((const void *)jit_store_global, in.imm_u32);
                break;
            case ir::Op::LoadCapture:
                // idx 0/1/2 have dedicated cached raw pointers; higher indices used to
                // call jit_load_capture, which the tsc run hits 28,188,532 times --
                // ~48 instructions per call for ~8 instructions of work. Instrumenting
                // it showed the null and bounds checks NEVER fail (nullcaps=0, oob=0),
                // so they are perfectly predicted here. Both temporaries are the shared
                // scratches, so this adds no vregs at the 6,210 static sites involved.
                if (in.imm_u32 >= 3 && kCaptureVecLayoutOk) {
                    Label lc_slow = cc.new_label();
                    Label lc_done = cc.new_label();
                    arch::Gp endp = fc.get();
                    arch::load(cc, cmp_scratch, arch::ptr(vm_reg, (int)VMCapturesRawOff));
                    arch::test_zero(cc, cmp_scratch);
                    arch::jcc(cc, arch::CC::kEQ, lc_slow);
                    arch::load(cc, const_scratch, arch::ptr(cmp_scratch, 8)); // vector end
                    arch::load(cc, cmp_scratch, arch::ptr(cmp_scratch, 0));   // vector begin
                    arch::add_imm(cc, cmp_scratch, (int64_t)in.imm_u32 * 16); // &elem[idx]
                    cc.cmp(cmp_scratch, const_scratch);
                    arch::jcc(cc, arch::CC::kUGE, lc_slow);                 // out of range
                    arch::load(cc, cmp_scratch, arch::ptr(cmp_scratch, 0)); // shared_ptr -> cell
                    arch::load(cc, cmp_scratch, arch::ptr(cmp_scratch, 0)); // cell -> Value
                    arch::store(cc, arch::ptr(endp, 0), cmp_scratch);
                    arch::add_imm(cc, endp, (int)ValSize);
                    fc.set(endp);
                    arch::jmp(cc, lc_done);
                    cc.bind(lc_slow);
                    call_u32((const void *)jit_load_capture, in.imm_u32);
                    fc.merge_slow_path();
                    cc.bind(lc_done);
                    break;
                }
                if (in.imm_u32 == 0) {
                    emit_ir_load_cached_capture(VMCapture0RawOff, "ir_lc0_cell");
                } else if (in.imm_u32 == 1) {
                    emit_ir_load_cached_capture(VMCapture1RawOff, "ir_lc1_cell");
                } else if (in.imm_u32 == 2) {
                    emit_ir_load_cached_capture(VMCapture2RawOff, "ir_lc2_cell");
                } else {
                    call_u32((const void *)jit_load_capture, in.imm_u32);
                }
                break;
            case ir::Op::StoreCapture:
                if (in.imm_u32 == 0) {
                    emit_ir_store_cached_capture(VMCapture0RawOff, "ir_sc0_cell");
                } else if (in.imm_u32 == 1) {
                    emit_ir_store_cached_capture(VMCapture1RawOff, "ir_sc1_cell");
                } else if (in.imm_u32 == 2) {
                    emit_ir_store_cached_capture(VMCapture2RawOff, "ir_sc2_cell");
                } else {
                    call_u32((const void *)jit_store_capture, in.imm_u32);
                }
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
            case ir::Op::Not: {
                // Measured on tsc: jit_not is called 9,011,251 times and the operand
                // carries TAG_BOOL in 100% of them. Negating a boxed bool is just
                // flipping payload bit 0 (the tag is untouched), so probe the tag and
                // do it in place instead of calling out.
                Label not_call = cc.new_label();
                Label not_done = cc.new_label();
                arch::Gp endp = fc.get();
                arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, cmp_scratch.r32(), Imm((int)tagBool));
                arch::jcc(cc, arch::CC::kNE, not_call);
                arch::load(cc, const_scratch, arch::ptr(endp, (int)(-ValSize)));
                arch::xor_imm(cc, const_scratch, 1);
                arch::store(cc, arch::ptr(endp, (int)(-ValSize)), const_scratch);
                arch::jmp(cc, not_done);
                cc.bind(not_call);
                call0((const void *)jit_not); // Not does not change stack height
                cc.bind(not_done);
                break;
            }
            case ir::Op::JsTruthy: {
                // is_js_truthy(bool) returns the bool unchanged, so for a Bool operand
                // this whole operation is the identity and needs no code at all.
                // Measured: 11,182,828 of 19,216,275 calls (58%) carry TAG_BOOL.
                // int/heap keep the helper (heap needs string-emptiness rules).
                Label truthy_skip = cc.new_label();
                arch::Gp endp = fc.get();
                arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                arch::cmp_imm(cc, cmp_scratch.r32(), Imm((int)tagBool));
                arch::jcc(cc, arch::CC::kEQ, truthy_skip);
                call0((const void *)jit_js_truthy);
                cc.bind(truthy_skip);
                break;
            }
            case ir::Op::IsNone: {
                fc.invalidate();
                InvokeNode *check;
                (void)vm_arg0();
                arch::invoke_imm(cc, &check, (uint64_t)(uintptr_t)jit_check_none, FuncSignature::build<int64_t, void *>());
                check->set_arg(0, vm_arg_scratch);
                arch::Gp result = cc.new_gp64("ir_is_none");
                check->set_ret(0, result);
                InvokeNode *push;
                (void)vm_arg0();
                arch::invoke_imm(cc, &push, (uint64_t)(uintptr_t)jit_push_bool, FuncSignature::build<void, void *, int64_t>());
                push->set_arg(0, vm_arg_scratch);
                push->set_arg(1, result);
                break;
            }
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
            case ir::Op::JsBitBinary:
                call_u32((const void *)jit_js_bit_binary, in.imm_u32);
                break;
            case ir::Op::JsBitNot:
                call0((const void *)jit_js_bit_not);
                break;
            case ir::Op::LoadIndex: {
                // skip array guard when array operand is LoadSlot of an int_arr_slot, skip guard if index is Int48
                bool sa = false, si = false;
                if (in.operands.size() >= 1) {
                    const ir::Inst &obj = irFuncs.inst(in.operands[0]);
                    if (obj.op == ir::Op::LoadSlot && int_arr_slots.count(obj.imm_u32)) {
                        sa = true;
                    }
                }
                if (in.operands.size() >= 2 && irFuncs.inst(in.operands[1]).type == ir::Ty::Int48) {
                    si = true;
                }
                emit_ir_load_index(sa, si);
                break;
            }
            case ir::Op::StoreIndex: {
                bool sa = false, si = false;
                if (in.operands.size() >= 1) {
                    const ir::Inst &obj = irFuncs.inst(in.operands[0]);
                    if (obj.op == ir::Op::LoadSlot && int_arr_slots.count(obj.imm_u32)) {
                        sa = true;
                    }
                }
                if (in.operands.size() >= 2 && irFuncs.inst(in.operands[1]).type == ir::Ty::Int48) {
                    si = true;
                }
                emit_ir_store_index(sa, si);
                break;
            }
            case ir::Op::LoadProperty:
                call_u32((const void *)jit_get_property, in.imm_u32);
                break;
            case ir::Op::JsGetPropStatic:
                emit_ir_js_get_prop_static(in.imm_u32);
                break;
            case ir::Op::StoreProperty:
                call_u32((const void *)jit_set_property, in.imm_u32);
                break;
            case ir::Op::JsSetPropStatic:
                call_u32((const void *)jit_js_set_prop_static, in.imm_u32);
                break;
            case ir::Op::JsPostinc:
                call_u32((const void *)jit_js_postinc, in.imm_u32);
                break;
            case ir::Op::MakeArray:
                call_u32((const void *)jit_make_array, in.imm_u32);
                break;
            case ir::Op::ArrayPush:
                call0((const void *)jit_array_push);
                break;
            case ir::Op::ArraySpread:
                call0((const void *)jit_array_spread);
                break;
            case ir::Op::MakeObject: {
                const uint8_t *site = chunk.functions[chunk_idx].code.data() + in.bytecode_pc + 1;
                call_u32_u64((const void *)jit_make_object_site, in.imm_u32, (uint64_t)(uintptr_t)site);
                break;
            }
            case ir::Op::MakeClosure: {
                const uint8_t *site = chunk.functions[chunk_idx].code.data() + in.bytecode_pc;
                call_i64((const void *)jit_make_closure, (int64_t)(uintptr_t)site);
                break;
            }
            case ir::Op::StrConcat:
                // imm_u32==1: mark_inplace_concat proved the lhs is a single-use
                // StrConcat result (fresh, uniquely-owned mutable string), so we
                // append into its buffer instead of copy+alloc.
                call0((const void *)(in.imm_u32 ? jit_str_concat_inplace : jit_str_concat));
                break;
            case ir::Op::StrAppendSlot:
                call_u32((const void *)jit_str_append_slot, in.imm_u32);
                break;
            case ir::Op::FormatValue:
                call_u32((const void *)jit_format_value, in.imm_u32);
                break;
            case ir::Op::IterArray:
                call0((const void *)jit_iter_array);
                break;
            case ir::Op::Call: {
                if (in.imm_u32 == 1 && !in.operands.empty()) {
                    const ir::Inst &callee = irFuncs.inst(in.operands[0]);
                    if (callee.op == ir::Op::LoadGlobal && callee.imm_u32 == typeof_name_idx_) {
                        emit_call_typeof();
                        break;
                    }
                    if (callee.op == ir::Op::LoadGlobal && callee.imm_u32 == js_to_number_name_idx_) {
                        call0((const void *)jit_call_js_to_number);
                        break;
                    }
                }
                {
                    const DirectCallee dc = resolve_direct_callee(in, in.imm_u32);
                    emit_ir_call_value(in.imm_u32, (uint32_t)in.imm_int, callee_kind_hint(in), dc.ok ? &dc : nullptr);
                }
                break;
            }
            case ir::Op::CallSpread:
                call_u32((const void *)jit_call_spread, in.imm_u32);
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
                    emit_int_binop(in.op, lhs_int(in), rhs_int(in));
                }
                break;
            case ir::Op::DynSub:
            case ir::Op::ISub:
                if (both_float(in)) {
                    emit_float_binop(in.op);
                } else {
                    // Inline int fast-path with a runtime tag guard; slow path -> jit_sub.
                    emit_int_binop(in.op, lhs_int(in), rhs_int(in));
                }
                break;
            case ir::Op::DynMul:
            case ir::Op::IMul:
                if (both_float(in)) {
                    emit_float_binop(in.op);
                } else {
                    // Inline int fast-path with a runtime tag guard; slow path -> jit_mul.
                    emit_int_mul(lhs_int(in), rhs_int(in));
                }
                break;
            case ir::Op::DynMod:
            case ir::Op::IMod:
                // Inline int fast-path with a runtime tag guard; slow path -> jit_mod.
                emit_int_mod(lhs_int(in), rhs_int(in));
                break;
            case ir::Op::Pow:
                call0((const void *)jit_pow);
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
            case ir::Op::DynStrictCmpEq:
            case ir::Op::DynStrictCmpNe:
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
                emit_int_cmp(in.op, lhs_int(in), rhs_int(in));
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
    ir::BlockId lowering_block = ir::InvalidBlock;
    auto lower_term = [&](const ir::Inst &t) {
        if (jit_dump_asm_enabled()) {
            char obuf[64];
            snprintf(obuf, sizeof(obuf), "OP:%s", ir::op_name(t.op));
            cc.comment(obuf);
        }
        // any deferred finish-store must hit memory before control leaves the block
        fc.invalidate();
        switch (t.op) {
            case ir::Op::Jump:
                if (irFuncs.blocks[t.target0].start_pc <= irFuncs.blocks[lowering_block].start_pc) {
                    poll_shutdown();
                }
                arch::jmp(cc, blabels[t.target0]);
                break;
            case ir::Op::Branch: {
                if (irFuncs.blocks[t.target0].start_pc <= irFuncs.blocks[lowering_block].start_pc ||
                    irFuncs.blocks[t.target1].start_pc <= irFuncs.blocks[lowering_block].start_pc) {
                    poll_shutdown();
                }
                // fast path: condition is statically Bool
                bool cond_is_bool =
                    !t.operands.empty() && t.operands[0] != ir::InvalidValue && irFuncs.inst(t.operands[0]).type == ir::Ty::Bool;
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
                // The condition is not *statically* Bool, but at run time it almost always
                // is: instrumenting jit_check_truthy over the tsc run showed 11,604,958
                // calls of which 11,595,237 (99.9%) carried TAG_BOOL. Inference cannot
                // prove it because ~a third of these conditions are the result of a
                // scripted call. So probe the tag inline and only pay for the call when
                // it really is something else (none/float/heap, where emptiness rules
                // make truthiness non-trivial anyway).
                arch::Gp truth = cc.new_gp64("ir_truth");
                Label truthy_call = cc.new_label();
                Label truthy_done = cc.new_label();
                {
                    arch::Gp endp = fc.get();
                    arch::load16_zx(cc, cmp_scratch, arch::ptr16(endp, (int)(-ValSize + tagWordOff)));
                    arch::cmp_imm(cc, cmp_scratch.r32(), Imm((int)tagBool));
                    arch::jcc(cc, arch::CC::kNE, truthy_call);
                    // Bool: the payload's low byte is the value (same as the static path).
                    arch::load8_zx(cc, truth, arch::ptr8(endp, (int)(-ValSize)));
                    arch::sub_imm(cc, endp, (int)ValSize);
                    fc.set(endp);
                    arch::jmp(cc, truthy_done);
                }
                cc.bind(truthy_call);
                // jit_check_truthy pops the condition and returns 1 (truthy) / 0.
                fc.invalidate();
                InvokeNode *inv;
                (void)vm_arg0();
                arch::invoke_imm(cc, &inv, (uint64_t)(uintptr_t)jit_check_truthy, FuncSignature::build<int64_t, void *>());
                inv->set_arg(0, vm_arg_scratch);
                inv->set_ret(0, truth);
                cc.bind(truthy_done);
                arch::test_zero(cc, truth);                       // ZF=1 iff truth==0 (falsy)
                arch::jcc(cc, arch::CC::kNE, blabels[t.target0]); // truthy
                arch::jmp(cc, blabels[t.target1]);                // falsy
                break;
            }
            case ir::Op::SelfTailCall: {
                const uint32_t argc = t.imm_u32;
                arch::Gp endp = fc.get();
                arch::Gp slot_base = emit_ir_slot_addr(0, "ir_stc_base");
                for (uint32_t i = 0; i < argc && i < irFuncs.num_params; i++) {
                    arch::Gp arg = cc.new_gp64("ir_stc_arg");
                    const int64_t offset = -static_cast<int64_t>(argc - i) * ValSize;
                    arch::load(cc, arg, arch::ptr(endp, offset));
                    arch::store(cc, arch::ptr(slot_base, static_cast<int64_t>(i) * ValSize), arg);
                }
                const uint64_t none = Value::none().raw_bits();
                for (uint32_t i = argc; i < irFuncs.num_params; i++) {
                    arch::store_imm(cc, arch::ptr(slot_base, static_cast<int64_t>(i) * ValSize), Imm(static_cast<int64_t>(none)));
                }
                arch::lea(cc, endp, slot_base, static_cast<int64_t>(irFuncs.num_slots) * ValSize);
                fc.set(endp);
                poll_shutdown();
                arch::jmp(cc, blabels[t.target0]);
                break;
            }
            case ir::Op::Return:
                if (t.operands.empty()) {
                    call0((const void *)jit_load_none); // implicit return none
                }
                fc.invalidate();
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
        return op == ir::Op::DynCmpLt || op == ir::Op::DynCmpLe || op == ir::Op::DynCmpGt || op == ir::Op::DynCmpGe ||
               op == ir::Op::DynCmpEq || op == ir::Op::DynCmpNe || op == ir::Op::DynStrictCmpEq || op == ir::Op::DynStrictCmpNe ||
               op == ir::Op::ICmpLt || op == ir::Op::ICmpLe || op == ir::Op::ICmpGt || op == ir::Op::ICmpGe || op == ir::Op::ICmpEq ||
               op == ir::Op::ICmpNe;
    };
    // fusable only when both operands are statically Float and the op is an ordered relational (Lt/Le/Gt/Ge)
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
        return irFuncs.inst(cmp.operands[0]).type == ir::Ty::Float && irFuncs.inst(cmp.operands[1]).type == ir::Ty::Float;
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
        arch::CC::Cond fcc = (cmp.op == ir::Op::FCmpLt || cmp.op == ir::Op::FCmpGt) ? arch::CC::kFGT : arch::CC::kFGE;
        if (swap) {
            arch::float_cmp(cc, fb, fa);
        } else {
            arch::float_cmp(cc, fa, fb);
        }
        arch::jcc(cc, fcc, blabels[term.target0]);
        arch::jmp(cc, blabels[term.target1]);
    };
    auto lower_fused_int_cmp_branch = [&](const ir::Inst &cmp, const ir::Inst &term) {
        // Operands whose IR type is already Int48 carry tagInt by construction, so
        // their runtime tag check is dead code -- and if BOTH are proven the whole
        // generic fallback block (a duplicate of cmp+term) is unreachable too.
        const bool lhs_known_int = cmp.operands.size() >= 2 && irFuncs.inst(cmp.operands[0]).type == ir::Ty::Int48;
        const bool rhs_known_int = cmp.operands.size() >= 2 && irFuncs.inst(cmp.operands[1]).type == ir::Ty::Int48;
        const bool need_slow = !(lhs_known_int && rhs_known_int);
        Label slow = cc.new_label();

        // Take the operands straight out of the deferred-push queue when they are
        // there (94% of sites; both operands at 50%). A fused operand never reaches
        // memory at all, which removes its producer's value store, stack bump and
        // finish write-back as well as the load here -- and each fused operand also
        // shrinks the pop below.
        //
        // The pends are deliberately NOT dropped before the slow arm is emitted:
        // that arm re-lowers the compare through emit_int_cmp, whose fc.get()
        // materializes them into the cold block, so it sees exactly the stack shape
        // it expects. Only when there is no slow arm must the drop be explicit.
        // Anything below the two operands is materialized up front so both arms
        // agree about the memory under them.
        arch::Gp endp = fc.materialize_keep(2);
        const auto *prhs = fc.peek(0);
        const auto *plhs = fc.peek(1);
        const bool fuse_rhs = prhs != nullptr;
        const bool fuse_lhs = plhs != nullptr;
        const int n_fused = (int)fuse_rhs + (int)fuse_lhs;

        // A constant rhs whose tag is statically Int48 folds straight into
        // `cmp lhs, imm`: no register, no load, no store, and its tag check is
        // resolved at compile time. `sign_extend_48` of a known bit pattern is just
        // arithmetic on that pattern, so do it here.
        int64_t rhs_imm = 0;
        bool rhs_as_imm = false;
        if (fuse_rhs && prhs->is_imm && (uint64_t)((uint64_t)prhs->imm >> 48) == (uint64_t)tagInt) {
            const int64_t pay = (int64_t)((uint64_t)prhs->imm << 16) >> 16;
            if (pay >= INT32_MIN && pay <= INT32_MAX) {
                rhs_as_imm = true;
                rhs_imm = pay;
            }
        }
        // Any other pending constant still has to become a register here; the queue
        // keeps the immediate so the slow arm materializes it independently.
        auto pend_reg = [&](const FinishCache::Pend &pv, const char *name) {
            if (!pv.is_imm) {
                return pv.reg;
            }
            arch::Gp r = cc.new_gp64(name);
            cc.mov(r, Imm(pv.imm));
            return r;
        };
        arch::Gp rhs_src;
        arch::Gp lhs_src;
        if (fuse_rhs && !rhs_as_imm) {
            rhs_src = pend_reg(*prhs, "fcb_rsrc");
        }
        if (fuse_lhs) {
            lhs_src = pend_reg(*plhs, "fcb_lsrc");
        }
        // Memory slot offsets: a fused rhs is not on the stack, so an unfused lhs
        // sits at the top instead of one below it.
        const int rhs_off = -ValSize;
        const int lhs_off = fuse_rhs ? -ValSize : -2 * ValSize;

        // A pend register must survive into the slow arm, which stores it to
        // memory, so read the tag out of a copy rather than mutating it.
        auto fused_tag_check = [&](arch::Gp src, const char *name) {
            arch::Gp tag = cc.new_gp64(name);
            arch::mov_reg(cc, tag, src);
            arch::shr(cc, tag, 48);
            arch::cmp_imm(cc, tag.r32(), Imm((int)tagInt));
            arch::jcc(cc, arch::CC::kNE, slow);
        };
        if (!rhs_known_int && !rhs_as_imm) {
            if (fuse_rhs) {
                fused_tag_check(rhs_src, "fcb_tb");
            } else {
                arch::Gp tagB = cc.new_gp64("fcb_tb");
                arch::load16_zx(cc, tagB, arch::ptr16(endp, (int)(rhs_off + tagWordOff)));
                arch::cmp_imm(cc, tagB.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
            }
        }
        if (!lhs_known_int) {
            if (fuse_lhs) {
                fused_tag_check(lhs_src, "fcb_ta");
            } else {
                arch::Gp tagA = cc.new_gp64("fcb_ta");
                arch::load16_zx(cc, tagA, arch::ptr16(endp, (int)(lhs_off + tagWordOff)));
                arch::cmp_imm(cc, tagA.r32(), Imm((int)tagInt));
                arch::jcc(cc, arch::CC::kNE, slow);
            }
        }

        arch::Gp rhs = cc.new_gp64("fcb_rhs");
        arch::Gp lhs = cc.new_gp64("fcb_lhs");
        // sign_extend_48 mutates in place, so a fused operand is copied first: a
        // queued vreg must stay intact for the slow arm to store.
        if (!rhs_as_imm) {
            if (fuse_rhs) {
                arch::mov_reg(cc, rhs, rhs_src);
            } else {
                arch::load(cc, rhs, arch::ptr(endp, (int)rhs_off));
            }
            arch::sign_extend_48(cc, rhs);
        }
        if (fuse_lhs) {
            arch::mov_reg(cc, lhs, lhs_src);
        } else {
            arch::load(cc, lhs, arch::ptr(endp, (int)lhs_off));
        }
        arch::sign_extend_48(cc, lhs);
        // Pop the operands that really are in memory BEFORE the compare: the
        // stack-pointer sub sets EFLAGS, so it must not sit between `cmp` and the
        // `jcc` that reads its result.
        const int n_mem = 2 - n_fused;
        if (n_mem > 0) {
            arch::sub_imm(cc, endp, (int)(n_mem * ValSize));
            fc.set(endp);
        }
        if (rhs_as_imm) {
            arch::cmp_imm(cc, lhs, Imm(rhs_imm));
        } else {
            cc.cmp(lhs, rhs);
        }

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
            case ir::Op::DynStrictCmpEq:
            case ir::Op::ICmpEq:
                cond = arch::CC::kEQ;
                break;
            default:
                cond = arch::CC::kNE;
                break;
        }
        arch::jcc(cc, cond, blabels[term.target0]);
        arch::jmp(cc, blabels[term.target1]);

        if (need_slow) {
            cc.bind(slow);
            lower_body(cmp); // its fc.get() materializes the fused operands here
            lower_term(term);
        } else {
            fc.drop_pend(n_fused); // no slow arm materialized them
        }
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
        lowering_block = static_cast<ir::BlockId>(bid);
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
            bool branch_to_last = term.op == ir::Op::Branch && !term.operands.empty() && term.operands[0] == b.insts.back();
            // fuse compare-and-branch for both statically-typed int compares and dynamically-typed compares
            fuse_cmp_branch = branch_to_last && is_cmp_op(last.op);
            // static-float ordered cmp + branch -> ucomisd + jcc, skipping bool synthesis and check_truthy
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
            fprintf(
                stderr, "[JIT] %-30s BAIL general (unhandled op %s)\n",
                chunk.functions[chunk_idx].name.empty() ? "<anon>" : chunk.functions[chunk_idx].name.c_str(), ir::op_name(unhandled_op)
            );
        }
        return nullptr; // unhandled op; VM runs this function in the interpreter
    }

    cc.bind(shutdown_requested);
    {
        InvokeNode *invoke;
        (void)vm_arg0();
        arch::invoke_imm(cc, &invoke, (uint64_t)(uintptr_t)jit_poll_shutdown, FuncSignature::build<void, void *>());
        invoke->set_arg(0, vm_arg_scratch);
    }
    cc.ret();
    cc.end_func();
    if (getenv("NARI_JIT_DUMP_NODES") != nullptr) {
        asmjit::String nodes_sb;
        asmjit::FormatOptions fo;
        asmjit::Formatter::format_node_list(nodes_sb, fo, &cc);
        fprintf(stderr, "==== pre-RA nodes for '%s' ====\n%s\n", chunk.functions[chunk_idx].name.c_str(), nodes_sb.data());
    }
    // Hand RA+encode to the worker unless something downstream needs the pointer
    // immediately. Spec recompiles are excluded both ways: compile_chunk passes this
    // general-tier pointer in as the spec tier's guard-failure fallback target.
    const bool will_spec = spec_candidate != nullptr && *spec_candidate && getenv("NARI_DISABLE_SPEC_JIT") == nullptr &&
                           chunk.functions[chunk_idx].param_count > 0 && chunk.functions[chunk_idx].param_count <= 8;
    if (async_jit_enabled() && spec_fallback == nullptr && !will_spec) {
        auto job = std::make_unique<CompileJob>();
        job->chunk_idx = chunk_idx;
        job->sym = chunk.functions[chunk_idx].name.empty() ? std::string("anon_ir") : chunk.functions[chunk_idx].name + "_ir";
        job->owner = this;
        job->holder = std::move(code_holder_owned);
        job->cc = std::move(cc_owned);
        // `cc` and `code_holder` dangle past this point; nothing below touches them.
        enqueue_compile_job(std::move(job));
        async_pending_ = true;
        report("OK  [ir-stack general path, async RA]");
        return nullptr;
    }
    Error err = cc.finalize();
    if (err != kErrorOk) {
        if (kJitReport) {
            fprintf(
                stderr, "[JIT] %-30s BAIL asmjit finalize err=%u (%s)\n",
                chunk.functions[chunk_idx].name.empty() ? "<anon>" : chunk.functions[chunk_idx].name.c_str(), err,
                asmjit::DebugUtils::error_as_string(err)
            );
            if (jit_dump_asm_enabled()) {
                fprintf(stderr, "---- partial asm ----\n%s---------------------\n", asm_logger.data());
            }
        }
        return nullptr;
    }
    size_t generated_code_size = code_holder.code_size();
    CompiledFunc fn = nullptr;
    err = this->rt.add(&fn, &code_holder);
    jit_dump_asm(chunk.functions[chunk_idx].name.empty() ? "<anon> [ir]" : chunk.functions[chunk_idx].name + " [ir]", asm_logger.data());
    if (err != kErrorOk || !fn) {
        return nullptr;
    }

    {
        std::string sym = chunk.functions[chunk_idx].name.empty() ? std::string("anon_ir") : chunk.functions[chunk_idx].name + "_ir";
        register_gdb_jit_function(sym, (const void *)fn, generated_code_size);
        perf_jitdump_register(sym, (const void *)fn, generated_code_size);
    }
    report("OK  [ir-stack general path]");
    return fn;
}

} // namespace jit
} // namespace nari

#endif // !DISABLE_JIT
