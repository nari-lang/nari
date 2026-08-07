#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#ifndef _WIN32
#include <signal.h>
#endif
#include <atomic>
#include <stdexcept>

#include "ast.h"

#include "bytecode.h"
#include "bytecode_serializer.h"
#include "dap/dap_server.h"
#ifndef DISABLE_PARSER
#include "parser_api.h"

#include "fmt/fmt_cli.h"
#endif
#include "repl.h"
#include "runtime.h"
#ifndef DISABLE_JIT
#include "asmjit_jit.h"
#include "trace_jit.h"
#endif

enum ReturnCode {
    SUCCESS = 0,
    ERROR_GENERIC = 1,
    ERROR_READING_FILE = 2,
    ERROR_PARSING = 3,
    ERROR_RUNTIME = 6,
    ERROR_BYTECODE_COMPILATION = 7,
    ERROR_BYTECODE_EXECUTION = 8
};

#ifndef DISABLE_PARSER
extern std::string nari_std_prelude_source();
#endif

namespace Runtime {
#ifndef DISABLE_PARSER
#endif
extern std::atomic<bool> g_shutdown_requested;
extern std::atomic<bool> g_runtime_error_occurred;
void reset_shutdown_flag();
void reset_runtime_error_flag();
} // namespace Runtime

#ifndef _WIN32
static void signal_handler(int signum) {
    Runtime::g_shutdown_requested.store(true);
}
#endif

static std::string read_file_to_string(const std::string &path) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        return {};
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::string result;
    if (file_size > 0) {
        result.resize(file_size);
        size_t bytes_read = fread(&result[0], 1, file_size, fp);
        result.resize(bytes_read);
    }
    fclose(fp);
    return result;
}

// merge a pre-compiled naric module chunk into the destination chunk, requires the parser for obvious reasons :)
#ifndef DISABLE_PARSER
static bool merge_naric_into_chunk(nari::bytecode::Chunk *dest, nari::bytecode::Chunk *src, const std::string &module_main_name) {
    using namespace nari::bytecode;

    std::vector<uint32_t> str_remap(src->strings.size());
    for (uint32_t i = 0; i < (uint32_t)src->strings.size(); i++) {
        str_remap[i] = dest->add_string(src->strings[i]);
    }

    uint32_t dest_func_base = (uint32_t)dest->functions.size();
    std::vector<uint32_t> func_remap(src->functions.size());
    for (uint32_t i = 0; i < (uint32_t)src->functions.size(); i++) {
        func_remap[i] = dest_func_base + i;
    }

    auto remap_constants = [&](std::vector<Constant> &consts) -> bool {
        for (auto &c : consts) {
            if (c.type == Constant::Type::STRING) {
                if (c.string_idx >= str_remap.size()) {
                    fprintf(stderr, "merge_naric_into_chunk: constant string index out of range\n");
                    return false;
                }
                c.string_idx = str_remap[c.string_idx];
            } else if (c.type == Constant::Type::FUNCTION) {
                if (c.func_idx >= func_remap.size()) {
                    fprintf(stderr, "merge_naric_into_chunk: constant function index out of range\n");
                    return false;
                }
                c.func_idx = func_remap[c.func_idx];
            }
        }
        return true;
    };

    auto remap_bytecode = [&](ByteArray &code) -> bool {
        auto need = [&](size_t p, size_t n) -> bool { return p <= code.size() && n <= code.size() - p; };
        auto rd16 = [&](size_t p) -> uint16_t { return ((uint16_t)code[p] << 8) | code[p + 1]; };
        auto wr16 = [&](size_t p, uint16_t v) {
            code[p] = (v >> 8) & 0xFF;
            code[p + 1] = v & 0xFF;
        };
        auto remap_string_operand = [&](size_t p, const char *what) -> bool {
            if (!need(p, 2)) {
                fprintf(stderr, "merge_naric_into_chunk: truncated %s operand\n", what);
                return false;
            }
            uint16_t idx = rd16(p);
            if (idx >= str_remap.size()) {
                fprintf(stderr, "merge_naric_into_chunk: %s string index out of range\n", what);
                return false;
            }
            wr16(p, (uint16_t)str_remap[idx]);
            return true;
        };

        size_t pc = 0;
        while (pc < code.size()) {
            size_t op_pc = pc;
            OpCode op = (OpCode)code[pc++];
            switch (op) {
                // zero operands
                case OpCode::OP_POP:
                case OpCode::OP_DUP:
                case OpCode::OP_LOAD_NONE:
                case OpCode::OP_LOAD_TRUE:
                case OpCode::OP_LOAD_FALSE:
                case OpCode::OP_LOAD_ZERO:
                case OpCode::OP_LOAD_ONE:
                case OpCode::OP_ADD:
                case OpCode::OP_SUB:
                case OpCode::OP_MUL:
                case OpCode::OP_DIV:
                case OpCode::OP_MOD:
                case OpCode::OP_POW:
                case OpCode::OP_NEG:
                case OpCode::OP_STR_CONCAT:
                case OpCode::OP_BIT_AND:
                case OpCode::OP_BIT_OR:
                case OpCode::OP_BIT_XOR:
                case OpCode::OP_BIT_NOT:
                case OpCode::OP_LSHIFT:
                case OpCode::OP_RSHIFT:
                case OpCode::OP_NOT:
                case OpCode::OP_JS_TRUTHY:
                case OpCode::OP_JS_BIT_AND:
                case OpCode::OP_JS_BIT_OR:
                case OpCode::OP_JS_BIT_XOR:
                case OpCode::OP_JS_BIT_NOT:
                case OpCode::OP_JS_SHL:
                case OpCode::OP_JS_SHR:
                case OpCode::OP_JS_USHR:
                case OpCode::OP_EQ:
                case OpCode::OP_NE:
                case OpCode::OP_STRICT_EQ:
                case OpCode::OP_STRICT_NE:
                case OpCode::OP_LT:
                case OpCode::OP_LE:
                case OpCode::OP_GT:
                case OpCode::OP_GE:
                case OpCode::OP_RETURN:
                case OpCode::OP_SPAWN:
                case OpCode::OP_ARRAY_PUSH:
                case OpCode::OP_ARRAY_SPREAD:
                case OpCode::OP_OBJECT_SPREAD:
                case OpCode::OP_GET_INDEX:
                case OpCode::OP_SET_INDEX:
                case OpCode::OP_MAKE_ITERATOR:
                case OpCode::OP_ITER_NEXT:
                case OpCode::OP_MAKE_ITERATOR_KV:
                case OpCode::OP_ITER_NEXT_KV:
                case OpCode::OP_ITER_ARRAY:
                case OpCode::OP_THROW:
                case OpCode::OP_POP_TRY:
                case OpCode::OP_BEGIN_CATCH:
                case OpCode::OP_BEGIN_FINALLY:
                case OpCode::OP_LOAD_THIS:
                    break;

                // 2-byte operands that don't need remapping
                case OpCode::OP_LOAD_CONST:
                case OpCode::OP_LOAD_VAR:
                case OpCode::OP_STORE_VAR:
                case OpCode::OP_LOAD_CAPTURE:
                case OpCode::OP_STORE_CAPTURE:
                case OpCode::OP_FORMAT_VALUE:
                case OpCode::OP_MAKE_ARRAY:
                case OpCode::OP_MAKE_OBJECT:
                case OpCode::OP_JUMP:
                case OpCode::OP_JUMP_IF_FALSE:
                case OpCode::OP_JUMP_IF_TRUE:
                case OpCode::OP_JUMP_IF_NONE:
                case OpCode::OP_STR_APPEND_VAR:
                case OpCode::OP_CLOSE_UPVALUES:
                    if (!need(pc, 2)) {
                        fprintf(stderr, "merge_naric_into_chunk: truncated operand at pc=%zu\n", op_pc);
                        return false;
                    }
                    pc += 2;
                    break;

                // 1-byte argc only
                case OpCode::OP_SELF_TAIL_CALL:
                    if (!need(pc, 1)) {
                        fprintf(stderr, "merge_naric_into_chunk: truncated argc at pc=%zu\n", op_pc);
                        return false;
                    }
                    pc += 1;
                    break;

                // 2-byte string index (remap)
                case OpCode::OP_CALL_SPREAD:
                    if (!remap_string_operand(pc, "callee label")) {
                        return false;
                    }
                    pc += 2;
                    break;

                // 1-byte argc + 2-byte string index (remap)
                case OpCode::OP_CALL:
                    if (!need(pc, 3)) {
                        fprintf(stderr, "merge_naric_into_chunk: truncated call operands at pc=%zu\n", op_pc);
                        return false;
                    }
                    if (!remap_string_operand(pc + 1, "callee label")) {
                        return false;
                    }
                    pc += 3;
                    break;

                // 2-byte string index (remap)
                case OpCode::OP_LOAD_GLOBAL:
                case OpCode::OP_STORE_GLOBAL:
                case OpCode::OP_GET_PROPERTY:
                case OpCode::OP_JS_GET_PROP_STATIC:
                case OpCode::OP_SET_PROPERTY:
                case OpCode::OP_JS_SET_PROP_STATIC:
                case OpCode::OP_JS_POSTINC:
                case OpCode::OP_OBJECT_SET:
                case OpCode::OP_STR_APPEND_GLOBAL:
                    if (!remap_string_operand(pc, "name")) {
                        return false;
                    }
                    pc += 2;
                    break;

                // 2-byte func_idx, 2-byte count, then count * (1-byte source + 2-byte index)
                case OpCode::OP_MAKE_CLOSURE: {
                    if (!need(pc, 4)) {
                        fprintf(stderr, "merge_naric_into_chunk: truncated closure header at pc=%zu\n", op_pc);
                        return false;
                    }
                    uint16_t fidx = rd16(pc);
                    if (fidx >= func_remap.size()) {
                        fprintf(stderr, "merge_naric_into_chunk: closure function index out of range\n");
                        return false;
                    }
                    wr16(pc, (uint16_t)func_remap[fidx]);
                    pc += 2;
                    uint16_t cap_count = rd16(pc);
                    pc += 2;
                    if (!need(pc, (size_t)(cap_count) * 3)) {
                        fprintf(stderr, "merge_naric_into_chunk: truncated closure captures at pc=%zu\n", op_pc);
                        return false;
                    }
                    for (uint16_t c = 0; c < cap_count; c++) {
                        uint8_t source = code[pc++]; // 0=local, 1=capture, 2=global
                        if (source == 2) {
                            if (!remap_string_operand(pc, "closure global capture")) {
                                return false;
                            }
                        }
                        pc += 2;
                    }
                    break;
                }

                case OpCode::OP_NEW_INSTANCE:
                case OpCode::OP_CHECK_TYPE:
                case OpCode::OP_CALL_METHOD:
                    if (!remap_string_operand(pc, "3-byte string")) {
                        return false;
                    }
                    if (!need(pc + 2, 1)) {
                        fprintf(stderr, "merge_naric_into_chunk: truncated 3-byte operand at pc=%zu\n", op_pc);
                        return false;
                    }
                    pc += 3;
                    break;

                case OpCode::OP_MAKE_REGEX:
                    if (!remap_string_operand(pc, "regex pattern")) {
                        return false;
                    }
                    if (!remap_string_operand(pc + 2, "regex flags")) {
                        return false;
                    }
                    pc += 4;
                    break;

                case OpCode::OP_SETUP_TRY:
                    if (!need(pc, 4)) {
                        fprintf(stderr, "merge_naric_into_chunk: truncated try operand at pc=%zu\n", op_pc);
                        return false;
                    }
                    pc += 4;
                    break;

                default:
                    fprintf(stderr, "merge_naric_into_chunk: unknown opcode 0x%02X at pc=%zu\n", (unsigned)op, op_pc);
                    return false;
            }
        }
        return true;
    };

    std::vector<FunctionMeta> remapped_functions;
    remapped_functions.reserve(src->functions.size());

    for (uint32_t i = 0; i < (uint32_t)src->functions.size(); i++) {
        FunctionMeta meta = src->functions[i];
        if (i == src->main_func_idx) {
            meta.name = module_main_name;
        }
        if (!remap_constants(meta.constants)) {
            return false;
        }
        if (!remap_bytecode(meta.code)) {
            return false;
        }
        remapped_functions.push_back(std::move(meta));
    }

    for (auto &meta : remapped_functions) {
        dest->functions.push_back(std::move(meta));
    }
    return true;
}
#endif // DISABLE_PARSER

int main(int argc, char **argv) {
    // `nari fmt ...`: code formatter subcommand. Handled before everything else
    // so no runtime/JIT initialization happens for a formatting run.
    if (argc > 1 && std::string(argv[1]) == "fmt") {
        return nari::fmt::run_fmt(argc - 1, argv + 1);
    }

    const char *usage = { "Usage: nari [--repl] [--dap] "
                          "[--trace-level=<none|error|info|debug>] "
                          "<script.nari | compiled.naric> [script args...]\n"
                          "       nari fmt [options] <files...>  (format source code; see 'nari fmt --help')\n" };

    // --dap: start as a Debug Adapter Protocol server on stdin/stdout.
    {
        bool dap_mode = false;
        std::string dap_script;
        std::vector<std::string> dap_rest;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--dap") {
                dap_mode = true;
            } else if (dap_mode && dap_script.empty() && !a.empty() && a[0] != '-') {
                dap_script = a;
            } else if (dap_mode) {
                dap_rest.push_back(a);
            }
        }
        if (dap_mode) {
#ifndef DISABLE_JIT
            // JIT disabled in --dap mode since the per-instruction debug hook exists
            // only in the bytecode VM dispatch, not in compiled native code.
#endif
            return nari::dap::run_dap_server(dap_script, dap_rest);
        }
    }

    // drop straight into the REPL if no args
    if (argc < 2) {
#ifndef DISABLE_JIT
        nari::jit::init_jit();
        nari::jit::init_trace_jit();
#endif
        run_repl(argc, argv);
#ifndef DISABLE_JIT
        nari::jit::shutdown_trace_jit();
        nari::jit::shutdown_jit();
#endif
        return SUCCESS;
    }

    std::string path;
    std::vector<std::string> script_args;
    std::string trace_level_str;
#ifndef DISABLE_REPL
    bool force_repl = false;
#endif

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const std::string tl_opt = "--trace-level=";

        if (a.rfind("--help", 0) == 0 || a.rfind("-h", 0) == 0) {
            fprintf(stderr, "%s", usage);
            return SUCCESS;
        }
        // hooo janky hack
#ifndef DISABLE_REPL
        if (a == "--repl") {
            force_repl = true;
        } else
#endif
            if (a.rfind(tl_opt, 0) == 0) {
            trace_level_str = a.substr(tl_opt.size());
        } else
            if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n%s", a.c_str(), usage);
            return ERROR_GENERIC;
        } else {
            if (path.empty()) {
                path = a;
                // All subsequent arguments belong to the script, not the interpreter.
                for (int j = i + 1; j < argc; ++j) {
                    script_args.push_back(argv[j]);
                }
                break;
            } else {
                script_args.push_back(a);
            }
        }
    }

#ifndef DISABLE_REPL
    // --repl flag or no script file launches the interactive REPL.
    if (force_repl || path.empty()) {
#ifndef DISABLE_JIT
        nari::jit::init_jit();
        nari::jit::init_trace_jit();
#endif
        run_repl(argc, argv);
#ifndef DISABLE_JIT
        nari::jit::shutdown_trace_jit();
        nari::jit::shutdown_jit();
#endif
        return SUCCESS;
    }
#endif // !DISABLE_REPL

    if (!trace_level_str.empty()) {
        if (trace_level_str == "none") {
            Runtime::set_runtime_trace_level(Runtime::TraceLevel::None);
        } else if (trace_level_str == "error") {
            Runtime::set_runtime_trace_level(Runtime::TraceLevel::Error);
        } else if (trace_level_str == "info") {
            Runtime::set_runtime_trace_level(Runtime::TraceLevel::Info);
        } else if (trace_level_str == "debug") {
            Runtime::set_runtime_trace_level(Runtime::TraceLevel::Debug);
        } else {
            fprintf(stderr, "Unknown trace level: %s\n%s", trace_level_str.c_str(), usage);
            return ERROR_GENERIC;
        }
    }

    // install signal handlers if possible for graceful shutdown
    // TODO: eventually i'll figure out how to do this on Windows too, but for now just ignore it there
    Runtime::reset_shutdown_flag();
#ifndef _WIN32
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

#ifndef DISABLE_JIT
    nari::jit::init_jit();
    nari::jit::init_trace_jit();
#endif

    std::string src = read_file_to_string(path);
    if (src.empty()) {
        fprintf(stderr, "Failed to read script file or file is empty: %s\n", path.c_str());
        return ERROR_READING_FILE;
    }

    // check if input is a pre-compiled .naric file
    bool is_naric = (path.size() >= 6 && path.substr(path.size() - 6) == ".naric");

    std::vector<std::string> runtime_argv_storage;
    runtime_argv_storage.push_back(path);
    for (const auto &arg : script_args) {
        runtime_argv_storage.push_back(arg);
    }

    std::vector<char *> runtime_argv;
    runtime_argv.reserve(runtime_argv_storage.size());
    for (auto &arg : runtime_argv_storage) {
        runtime_argv.push_back(arg.data());
    }

    int runtime_argc = (int)runtime_argv.size();
    char **runtime_argv_ptr = runtime_argv.empty() ? nullptr : runtime_argv.data();

#ifdef DISABLE_PARSER
    // bytecode-only build: source files cannot be parsed at runtime.
    // Scripts must be pre-compiled to .naric on a host using the naric tool.
    if (!is_naric) {
        fprintf(stderr,
                "Error: This is a bytecode-only build.\n"
                "  Source file '%s' cannot be parsed at runtime!\n"
                "  Pre-compile it using the naric tool:\n"
                "  naric %s -o output.naric\n",
                path.c_str(), path.c_str());
        return ERROR_PARSING;
    }
#else
    FuncList funcs;

    if (!is_naric) {
        Parser::set_source_filename(path);

        Parser::ParseResult parse_result = Parser::parse_program_recovering(src);
        if (!parse_result.ok()) {
            for (const auto &err : parse_result.errors) {
                fprintf(stderr, "Parse error at %s:%d:%d: %s\n", err.filename.c_str(), err.line, err.col, err.message.c_str());
            }
            return ERROR_PARSING;
        }
        funcs = std::move(parse_result.functions);

        if (funcs.empty()) {
            fprintf(stderr, "Parser produced no functions! (or parsing failed..)\n");
            return ERROR_PARSING;
        }
    }

#endif // DISABLE_PARSER

    try {
        Runtime::reset_runtime_error_flag();

        {
            bytecode::Chunk *chunk = nullptr;

            if (is_naric) {
                auto bc_data = reinterpret_cast<const uint8_t *>(src.data());
                chunk = bytecode::BytecodeSerializer::deserialize(bc_data, src.size());
                if (!chunk) {
                    fprintf(stderr, "Failed to load bytecode file: %s\n", path.c_str());
                    return ERROR_BYTECODE_COMPILATION;
                }
                // restore type declarations from the bytecode file into the parser's
                // type registry so that __ffi_membersof() and other runtime type
                // introspection works without needing the original source
#ifndef DISABLE_PARSER
                for (const auto &ti : chunk->types) {
                    if (Parser::is_registered_type(ti.name)) {
                        continue;
                    }
                    auto type_decl =
                        std::make_unique<nari::TypeDecl>(ti.name, ti.is_union ? nari::TypeDeclKind::Union : nari::TypeDeclKind::Struct);
                    if (!ti.alias_target.empty()) {
                        type_decl->alias_target = std::make_unique<nari::TypeAnnotation>(ti.alias_target);
                    }
                    for (const auto &fi : ti.fields) {
                        auto field_type = std::make_unique<nari::TypeAnnotation>(fi.type_name, fi.is_array);
                        field_type->fixed_array_count = fi.fixed_array_count;
                        type_decl->fields.emplace_back(fi.name, std::move(field_type));
                    }
                    Parser::register_type(std::move(type_decl));
                }
#endif
            }
#ifndef DISABLE_PARSER
            else {
                // parse stdlib and combine with user functions
                Parser::set_source_filename("<embedded_stdlib>");
                std::string embedded = nari_std_prelude_source();
                FuncList stdlib_funcs = Parser::parse_program_from_source(embedded, false);

                FuncList combined;
                combined.reserve(stdlib_funcs.size() + funcs.size());
                for (auto &f : stdlib_funcs) {
                    combined.push_back(std::move(f));
                }
                for (auto &f : funcs) {
                    combined.push_back(std::move(f));
                }

                // restore original filename
                Parser::set_source_filename(path);

                chunk = bytecode::compile_bytecode(combined);
                if (!chunk) {
                    fprintf(stderr, "Bytecode compilation failed!\n");
                    return ERROR_BYTECODE_COMPILATION;
                }
                if (getenv("NARI_DUMP_CHUNK")) {
                    bytecode::dump_chunk(*chunk);
                }
            }

            // merge any pre-compiled .naric modules that were imported from source.
            // each module's top-level function is renamed to the unique init-function name the parser emitted a CallExpr for
            if (!is_naric) {
                const auto &naric_imports = Parser::get_pending_naric_imports();
                for (const auto &[init_name, module_path] : naric_imports) {
                    std::string module_src = read_file_to_string(module_path);
                    if (module_src.empty()) {
                        fprintf(stderr, "Failed to read naric module: %s\n", module_path.c_str());
                        continue;
                    }
                    auto bc_data = reinterpret_cast<const uint8_t *>(module_src.data());
                    bytecode::Chunk *mod_chunk = bytecode::BytecodeSerializer::deserialize(bc_data, module_src.size());
                    if (!mod_chunk) {
                        fprintf(stderr, "Failed to deserialize naric module: %s\n", module_path.c_str());
                        continue;
                    }
                    for (const auto &ti : mod_chunk->types) {
                        if (Parser::is_registered_type(ti.name)) {
                            continue;
                        }
                        auto type_decl =
                            std::make_unique<nari::TypeDecl>(ti.name, ti.is_union ? nari::TypeDeclKind::Union : nari::TypeDeclKind::Struct);
                        if (!ti.alias_target.empty()) {
                            type_decl->alias_target = std::make_unique<nari::TypeAnnotation>(ti.alias_target);
                        }
                        for (const auto &fi : ti.fields) {
                            auto field_type = std::make_unique<nari::TypeAnnotation>(fi.type_name, fi.is_array);
                            field_type->fixed_array_count = fi.fixed_array_count;
                            type_decl->fields.emplace_back(fi.name, std::move(field_type));
                        }
                        Parser::register_type(std::move(type_decl));
                    }
                    if (!merge_naric_into_chunk(chunk, mod_chunk, init_name)) {
                        fprintf(stderr, "Failed to merge naric module: %s\n", module_path.c_str());
                        delete mod_chunk;
                        delete chunk;
                        Parser::clear_pending_naric_imports();
                        return ERROR_BYTECODE_COMPILATION;
                    }
                    delete mod_chunk;
                }
                Parser::clear_pending_naric_imports();
            }
#endif // !DISABLE_PARSER

            bytecode::VM vm(runtime_argc, runtime_argv_ptr);
            if (!vm.run(chunk)) {
                fprintf(stderr, "\n=== Execution stopped due to runtime error ===\n");
                delete chunk;
                return ERROR_BYTECODE_EXECUTION;
            }

            delete chunk;
        }
    } catch (const std::runtime_error &e) {
        fprintf(stderr, "Runtime error: %s\n", e.what());
        fprintf(stderr, "\n=== Execution stopped due to runtime error ===\n");
#ifndef DISABLE_JIT
        nari::jit::shutdown_trace_jit();
        nari::jit::shutdown_jit();
#endif
        return ERROR_RUNTIME;
    }

#ifndef DISABLE_JIT
    nari::jit::shutdown_trace_jit();
    nari::jit::shutdown_jit();
#endif
    return SUCCESS;
}

