#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#endif
#include <atomic>
#include <stdexcept>

#include "ast.h"

#include "parser_api.h"
#include "runtime.h"

using FunctionList = std::vector<std::unique_ptr<nari::Function>>;

namespace Runtime {
    void run_program_with_runtime(std::vector<std::unique_ptr<nari::Function>> &funcs);
    extern std::atomic<bool> g_shutdown_requested;
    extern std::atomic<bool> g_runtime_error_occurred;
    void reset_runtime_error_flag();
}

// Signal handler for graceful shutdown
#ifndef _WIN32
static void signal_handler(int signum) {
    Runtime::g_shutdown_requested.store(true);
}
#endif

static std::string read_file_to_string(const std::string &path) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return {};

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

int main(int argc, char **argv) {
    const char *usage = "Usage: interpreter [--dump-ast=<file>] [--trace-level=<none|error|info|debug>] <script.nari>\n";
    if (argc < 2) {
        fprintf(stderr, "%s", usage);
        return 1;
    }

    // Optional flags:
    //   --dump-ast=<path>      : write AST dump to file
    //   --trace-level=<level>  : one of none,error,info,debug
    std::string dump_ast_path;
    std::string path;
    std::string trace_level_str;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const std::string dump_opt = "--dump-ast=";
        const std::string tl_opt = "--trace-level=";
        if (a.rfind(dump_opt, 0) == 0) {
            dump_ast_path = a.substr(dump_opt.size());
        } else if (a.rfind(tl_opt, 0) == 0) {
            trace_level_str = a.substr(tl_opt.size());
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n%s", a.c_str(), usage);
            return 1;
        } else {
            path = a;
        }
    }

    if (path.empty()) {
        fprintf(stderr, "%s", usage);
        return 1;
    }

    if (!trace_level_str.empty()) {
        if (trace_level_str == "none") Runtime::set_runtime_trace_level(Runtime::TraceLevel::None);
        else if (trace_level_str == "error") Runtime::set_runtime_trace_level(Runtime::TraceLevel::Error);
        else if (trace_level_str == "info") Runtime::set_runtime_trace_level(Runtime::TraceLevel::Info);
        else if (trace_level_str == "debug") Runtime::set_runtime_trace_level(Runtime::TraceLevel::Debug);
        else {
            fprintf(stderr, "Unknown trace level: %s\n%s", trace_level_str.c_str(), usage);
            return 1;
        }
    }

    // Install signal handlers for graceful shutdown
#ifndef _WIN32
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    std::string src = read_file_to_string(path);
    if (src.empty()) {
        fprintf(stderr, "Failed to read script file or file is empty: %s\n", path.c_str());
        return 2;
    }

    Parser::set_source_filename(path);
    
    FunctionList funcs = Parser::parse_program_from_source(src);

    if (funcs.empty()) {
        fprintf(stderr, "Parser produced no functions! (or parsing failed D:)\n");
        return 3;
    }

    if (funcs.empty()) {
        fprintf(stderr, "No functions parsed from script; nothing to run!\n");
        return 4;
    }

    if (!dump_ast_path.empty()) {
        FILE* fp = fopen(dump_ast_path.c_str(), "wb");
        if (!fp) {
            fprintf(stderr, "Failed to open AST dump file for writing: %s\n", dump_ast_path.c_str());
            return 5;
        }
#ifndef _WIN32
        int stdout_fd = dup(STDOUT_FILENO);
        int file_fd = fileno(fp);
        dup2(file_fd, STDOUT_FILENO);
#endif

        printf("AST Dump for script: %s\n", path.c_str());
        for (const auto &fptr : funcs) {
            if (fptr) fptr->pretty_print(0);
        }
        fflush(stdout);
        
#ifndef _WIN32
        dup2(stdout_fd, STDOUT_FILENO);
        close(stdout_fd);
#endif
        fclose(fp);
        printf("Wrote AST dump to: %s\n", dump_ast_path.c_str());
    }

    try {
        Runtime::reset_runtime_error_flag();
        Runtime::run_program_with_runtime(funcs);
    } catch (const std::runtime_error &e) {
        fprintf(stderr, "\n=== Execution stopped due to runtime error ===\n");
        return 6;
    }

    return 0;
}
