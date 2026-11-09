// JIT asm-dump helpers (split out of asmjit_jit.cpp).
#ifndef DISABLE_JIT
#include "asmjit_dump.h"
#include "jit_helpers.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace nari::bytecode;

// Fast sin/cos defined in trace/lower.cpp.
extern double nari_fast_sin(double) noexcept;
extern double nari_fast_cos(double) noexcept;

bool jit_dump_asm_enabled() {
    static const bool enabled = getenv("NARI_JIT_DUMP_ASM") != nullptr;
    return enabled;
}

const std::unordered_map<uint64_t, const char *> &jit_helper_symbols() {
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
std::string jit_symbolize_calls(const char *asm_text, uint64_t extra_addr, const char *extra_name) {
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

#endif // !DISABLE_JIT
