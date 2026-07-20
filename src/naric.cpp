#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "bytecode.h"
#include "bytecode_serializer.h"
#include "parser_api.h"

// embedded stdlib
extern std::string nari_std_prelude_source();

static std::string read_file_to_string(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return {};
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return {};
    }
    std::string buf(static_cast<size_t>(sz), '\0');
    size_t n = fread(&buf[0], 1, static_cast<size_t>(sz), f);
    fclose(f);
    buf.resize(n);
    return buf;
}

static bool write_file(const std::string &path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    size_t n = fwrite(data, 1, size, f);
    fclose(f);
    return n == size;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "naric - Nari bytecode compiler\n\n");
    fprintf(stderr, "Usage: %s [options] <input.nari>\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>  Output file (default: <input>.naric)\n");
    fprintf(stderr, "  --dump  Print bytecode disassembly instead of writing file\n");
    fprintf(stderr, "  --no-stdlib  Don't include the standard library\n");
    fprintf(stderr, "  -h, --help  Show this help\n");
}

static void dump_chunk(const nari::bytecode::Chunk &chunk) {
    using namespace nari::bytecode;

    printf("=== Nari Bytecode Dump ===\n");
    printf("Strings: %zu\n", chunk.strings.size());
    for (size_t i = 0; i < chunk.strings.size(); i++) {
        printf("  [%zu] \"%s\"\n", i, chunk.strings[i].c_str());
    }

    printf("\nFunctions: %zu (main = #%u)\n", chunk.functions.size(), chunk.main_func_idx);

    for (size_t fi = 0; fi < chunk.functions.size(); fi++) {
        const auto &func = chunk.functions[fi];
        printf("\n--- Function #%zu: %s ---\n", fi, func.name.c_str());
        printf(
            "  params: %u, captures: %u, rest_param: %d, lambda: %s\n",
            func.param_count, func.capture_count, func.rest_param_index,
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
                    printf(
                        "string(%u) \"%s\"\n",
                        constant.string_idx, constant.string_idx < chunk.strings.size() ? chunk.strings[constant.string_idx].c_str() : "???");
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
                // variable-length: 2 (func_idx) + 1 (capture_count) + N*3
                uint16_t fidx = (func.code[ip] << 8) | func.code[ip + 1];
                ip += 2;
                uint8_t ncap = func.code[ip++];
                printf("%-16s func=#%u captures=%u\n", opcode_name(op), fidx, ncap);
                for (uint8_t ci = 0; ci < ncap; ci++) {
                    uint8_t src = func.code[ip++];
                    uint16_t idx = (func.code[ip] << 8) | func.code[ip + 1];
                    ip += 2;
                    printf(
                        "  capture[%u]: %s #%u\n",
                        ci,
                        src == 0   ? "local"
                        : src == 1 ? "upvalue"
                                   : "global",
                        idx);
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
                if (op == OpCode::OP_JUMP || op == OpCode::OP_JUMP_IF_FALSE || op == OpCode::OP_JUMP_IF_TRUE || op == OpCode::OP_JUMP_IF_NONE) {
                    int16_t sval = static_cast<int16_t>(val);
                    printf("%-16s %+d (-> %04zu)\n", opcode_name(op), sval, ip + sval);
                } else if (op == OpCode::OP_LOAD_GLOBAL || op == OpCode::OP_STORE_GLOBAL) {
                    printf("%-16s #%u", opcode_name(op), val);
                    if (val < chunk.strings.size()) {
                        printf(" (\"%s\")", chunk.strings[val].c_str());
                    }
                    printf("\n");
                } else if (op == OpCode::OP_GET_PROPERTY || op == OpCode::OP_SET_PROPERTY) {
                    printf("%-16s #%u", opcode_name(op), val);
                    if (val < chunk.strings.size()) {
                        printf(" (.%s)", chunk.strings[val].c_str());
                    }
                    printf("\n");
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
                int16_t sa = static_cast<int16_t>(a);
                int16_t sb = static_cast<int16_t>(b);
                printf("%-16s catch=%+d (-> %04zu) finally=%+d (-> %04zu)\n", opcode_name(op), sa, ip + sa, sb, ip + sb);
            }
        }
    }
}

int main(int argc, char **argv) {
    std::string input_path;
    std::string output_path;
    bool dump_mode = false;
    bool include_stdlib = true;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--dump") {
            dump_mode = true;
        } else if (arg == "--no-stdlib") {
            include_stdlib = false;
        } else if (arg == "-o" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        } else {
            input_path = arg;
        }
    }

    if (input_path.empty()) {
        fprintf(stderr, "Error: no input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    if (output_path.empty() && !dump_mode) {
        output_path = input_path;
        size_t dot = output_path.rfind('.');
        if (dot != std::string::npos) {
            output_path = output_path.substr(0, dot);
        }
        output_path += ".naric";
    }

    std::string src = read_file_to_string(input_path);
    if (src.empty()) {
        fprintf(stderr, "Error: failed to read '%s' (or file is empty)\n", input_path.c_str());
        return 2;
    }

    Parser::set_source_filename(input_path);

    FuncList combined;

    if (include_stdlib) {
        Parser::set_source_filename("<embedded_stdlib>");
        std::string embedded = nari_std_prelude_source();
        FuncList stdlib_funcs = Parser::parse_program_from_source(embedded, false);
        for (auto &f : stdlib_funcs) {
            combined.push_back(std::move(f));
        }
        Parser::set_source_filename(input_path);
    }

    Parser::ParseResult user_parse = Parser::parse_program_recovering(src);
    if (!user_parse.ok()) {
        for (const auto &err : user_parse.errors) {
            fprintf(stderr, "Parse error at %s:%d:%d: %s\n", err.filename.c_str(), err.line, err.col, err.message.c_str());
        }
        return 3;
    }
    FuncList user_funcs = std::move(user_parse.functions);
    if (user_funcs.empty()) {
        fprintf(stderr, "Error: parser produced no functions from '%s'\n", input_path.c_str());
        return 3;
    }

    for (auto &f : user_funcs) {
        combined.push_back(std::move(f));
    }

    bytecode::Chunk *chunk = bytecode::compile_bytecode(combined);
    if (!chunk) {
        fprintf(stderr, "Error: bytecode compilation failed\n");
        return 4;
    }

    if (dump_mode) {
        dump_chunk(*chunk);
        delete chunk;
        return 0;
    }

    auto bytes = bytecode::BytecodeSerializer::serialize(*chunk);
    delete chunk;

    if (!write_file(output_path, bytes.data(), bytes.size())) {
        fprintf(stderr, "Error: failed to write '%s'\n", output_path.c_str());
        return 5;
    }

    printf("Compiled %s -> %s (%zu bytes)\n", input_path.c_str(), output_path.c_str(), bytes.size());
    return 0;
}
