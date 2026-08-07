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
    std::string buf((size_t)sz, '\0');
    size_t n = fread(&buf[0], 1, (size_t)sz, f);
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
        nari::bytecode::dump_chunk(*chunk);
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
