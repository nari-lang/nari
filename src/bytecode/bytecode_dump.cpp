#include "bytecode.h"
#include <cstdio>

namespace nari {
namespace bytecode {

void dump_chunk(const Chunk &chunk) {

    printf("=== Nari Bytecode Dump ===\n");
    printf("Strings: %zu\n", chunk.strings.size());
    for (size_t i = 0; i < chunk.strings.size(); i++) {
        printf("  [%zu] \"%s\"\n", i, chunk.strings[i].c_str());
    }

    printf("\nFunctions: %zu (main = #%u)\n", chunk.functions.size(), chunk.main_func_idx);

    for (size_t fi = 0; fi < chunk.functions.size(); fi++) {
        const auto &func = chunk.functions[fi];
        printf("\n--- Function #%zu: %s ---\n", fi, func.name.c_str());
        printf("  params: %u, captures: %u, rest_param: %d, lambda: %s\n", 
                func.param_count,
                func.capture_count,
                func.rest_param_index,
                func.is_lambda ? "yes" : "no");
        printf("  locals: %zu [", func.var_names.size());
        for (size_t i = 0; i < func.var_names.size(); i++) {
            if (i > 0) {
                printf(", ");
            }
            printf("%s", func.var_names[i].c_str());
        }
        printf("]\n");

        printf("  constants: %zu\n", func.constants.size());
        for (size_t i = 0; i < func.constants.size(); i++) {
            const auto &constant = func.constants[i];
            printf("  [%zu] ", i);
            switch (constant.type) {
                typedef nari::bytecode::Constant::Type CType;
                case CType::NONE:
                    printf("none\n");
                    break;
                case CType::INT:
#ifdef _WIN32
                    printf("int(%lld)\n", constant.as_int);
#else
                    printf("int(%ld)\n", constant.as_int);
#endif

                    break;
                case CType::FLOAT:
                    printf("float(%g)\n", constant.as_float);
                    break;
                case CType::STRING:
                    printf("string(%u) \"%s\"\n", 
                            constant.string_idx,
                            constant.string_idx < chunk.strings.size() ? chunk.strings[constant.string_idx].c_str() : "???");
                    break;
                case CType::FUNCTION:
                    printf("func(%u)\n", constant.func_idx);
                    break;
            }
        }

        printf("  code: %zu bytes\n", func.code.size());
        size_t ip = 0;
        while (ip < func.code.size()) {
            printf("  %04zu  ", ip);
            OpCode op = (OpCode)func.code[ip++];
            int operand_size = opcode_operand_size(op);

            if (op == OpCode::OP_MAKE_CLOSURE) {
                // variable-length: 2 (func_idx) + 2 (capture_count) + N*3
                uint16_t fidx = (func.code[ip] << 8) | func.code[ip + 1];
                ip += 2;
                uint16_t ncap = (func.code[ip] << 8) | func.code[ip + 1];
                ip += 2;
                printf("%-16s func=#%u captures=%u\n", opcode_name(op), fidx, ncap);
                for (uint16_t ci = 0; ci < ncap; ci++) {
                    uint8_t src = func.code[ip++];
                    uint16_t idx = (func.code[ip] << 8) | func.code[ip + 1];
                    ip += 2;
                    printf("  capture[%u]: %s #%u\n", ci, src == 0 ? "local" : src == 1 ? "upvalue" : "global", idx);
                }
            } else if (operand_size == 0) {
                printf("%s\n", opcode_name(op));
            } else if (operand_size == 1) {
                uint8_t val = func.code[ip++];
                printf("%-16s %u\n", opcode_name(op), val);
            } else if (operand_size == 2) {
                uint16_t val = (func.code[ip] << 8) | func.code[ip + 1];
                ip += 2;
                // for jumps, show signed offset
                if (op == OpCode::OP_JUMP || op == OpCode::OP_JUMP_IF_FALSE || op == OpCode::OP_JUMP_IF_TRUE ||
                    op == OpCode::OP_JUMP_IF_NONE) {
                    int16_t sval = (int16_t)val;
                    printf("%-16s %+d (-> %04zu)\n", opcode_name(op), sval, ip + sval);
                } else if (op == OpCode::OP_LOAD_GLOBAL || op == OpCode::OP_STORE_GLOBAL) {
                    printf("%-16s #%u", opcode_name(op), val);
                    if (val < chunk.strings.size()) {
                        printf(" (\"%s\")", chunk.strings[val].c_str());
                    }
                    printf("\n");
                } else if (op == OpCode::OP_GET_PROPERTY || op == OpCode::OP_JS_GET_PROP_STATIC || op == OpCode::OP_SET_PROPERTY ||
                           op == OpCode::OP_JS_SET_PROP_STATIC || op == OpCode::OP_JS_POSTINC) {
                    printf("%-16s #%u", opcode_name(op), val);
                    if (val < chunk.strings.size()) {
                        printf(" (.%s)", chunk.strings[val].c_str());
                    }
                    printf("\n");
                } else if (op == OpCode::OP_CLOSE_UPVALUES) {
                    printf("%-16s first_slot=%u\n", opcode_name(op), val);
                } else if (op == OpCode::OP_CALL_SPREAD) {
                    printf("%-16s callee=#%u", opcode_name(op), val);
                    if (val < chunk.strings.size()) {
                        printf(" (\"%s\")", chunk.strings[val].c_str());
                    }
                    printf("\n");
                } else {
                    printf("%-16s %u\n", opcode_name(op), val);
                }
            } else if (operand_size == 3) {
                if (op == OpCode::OP_CALL) {
                    uint8_t argc = func.code[ip++];
                    uint16_t label = (func.code[ip] << 8) | func.code[ip + 1];
                    ip += 2;
                    printf("%-16s argc=%u callee=#%u", opcode_name(op), argc, label);
                    if (label < chunk.strings.size()) {
                        printf(" (\"%s\")", chunk.strings[label].c_str());
                    }
                    printf("\n");
                } else {
                    uint16_t val = (func.code[ip] << 8) | func.code[ip + 1];
                    ip += 2;
                    uint8_t argc = func.code[ip++];
                    if (op == OpCode::OP_CALL_METHOD) {
                        printf("%-16s #%u", opcode_name(op), val);
                        if (val < chunk.strings.size()) {
                            printf(" (.%s)", chunk.strings[val].c_str());
                        }
                        printf(" argc=%u\n", argc);
                    } else {
                        printf("%-16s #%u argc=%u\n", opcode_name(op), val, argc);
                    }
                }
            } else if (operand_size == 4) {
                uint16_t a = (func.code[ip] << 8) | func.code[ip + 1];
                ip += 2;
                uint16_t b = (func.code[ip] << 8) | func.code[ip + 1];
                ip += 2;
                int16_t sa = (int16_t)a;
                int16_t sb = (int16_t)b;
                printf("%-16s catch=%+d (-> %04zu) finally=%+d (-> %04zu)\n", opcode_name(op), sa, ip + sa, sb, ip + sb);
            }
        }
    }
}

} // namespace bytecode
} // namespace nari
