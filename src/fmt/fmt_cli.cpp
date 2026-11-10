/*
  fmt/fmt_cli.cpp
  `nari fmt` subcommand: code formatter for Nari source files.

  Usage:
    nari fmt [options] <files...>
      (default)  print formatted source to stdout
    -w, --write  format files in place
    --check      exit 1 if any file is not formatted (CI mode; prints paths)
    --stdin      read from stdin, write to stdout
    -h, --help   show this help
*/
#include "fmt_cli.h"
#include "file_io.h"

#include <cstdio>
#include <string>
#include <vector>

#include "formatter.h"

namespace nari::fmt {

namespace {

void print_usage(const char *prog) {
    fprintf(stderr, "nari fmt - Nari code formatter\n\n");
    fprintf(stderr, "Usage: %s fmt [options] <files...>\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  (default)    Print formatted source to stdout\n");
    fprintf(stderr, "  -w, --write  Format files in place\n");
    fprintf(stderr, "  --check      Exit 1 if any file is not formatted (prints paths)\n");
    fprintf(stderr, "  --stdin      Read from stdin, write to stdout\n");
    fprintf(stderr, "  -h, --help   Show this help\n");
}

} // namespace

int run_fmt(int argc, char **argv) {
    bool write_mode = false;
    bool check_mode = false;
    bool stdin_mode = false;
    std::vector<std::string> files;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-w" || arg == "--write") {
            write_mode = true;
        } else if (arg == "--check") {
            check_mode = true;
        } else if (arg == "--stdin") {
            stdin_mode = true;
        } else if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        } else {
            files.push_back(arg);
        }
    }

    if (write_mode && check_mode) {
        fprintf(stderr, "Error: --write and --check are mutually exclusive\n");
        return 1;
    }

    FmtOptions opts;
    std::error_code file_error;

    auto write_output = [&](const std::string &out) {
        if (!nari::write_stream(stdout, out.data(), out.size(), file_error)) {
            fprintf(stderr, "nari fmt: error: failed to write stdout: %s\n", file_error.message().c_str());
            return false;
        }
        return true;
    };

    if (stdin_mode) {
        std::string src;
        if (!nari::read_stream(stdin, src, file_error)) {
            fprintf(stderr, "nari fmt: error: failed to read stdin: %s\n", file_error.message().c_str());
            return 2;
        }
        std::string out, err;
        if (!format_source(src, "<stdin>", opts, out, err)) {
            fprintf(stderr, "nari fmt: %s\n", err.c_str());
            return 2;
        }
        return write_output(out) ? 0 : 2;
    }

    if (files.empty()) {
        fprintf(stderr, "Error: no input files specified\n");
        print_usage(argv[0]);
        return 1;
    }

    bool any_unformatted = false;
    for (const std::string &path : files) {
        std::string src;
        if (!nari::read_file(path, src, file_error)) {
            fprintf(stderr, "nari fmt: error: failed to read '%s': %s\n", path.c_str(), file_error.message().c_str());
            return 2;
        }
        std::string out, err;
        if (!format_source(src, path, opts, out, err)) {
            fprintf(stderr, "nari fmt: %s\n", err.c_str());
            return 2;
        }
        if (check_mode) {
            if (out != src) {
                printf("%s\n", path.c_str());
                any_unformatted = true;
            }
        } else if (write_mode) {
            if (out != src && !nari::write_file(path, out.data(), out.size(), file_error)) {
                fprintf(stderr, "nari fmt: error: failed to write '%s': %s\n", path.c_str(), file_error.message().c_str());
                return 2;
            }
        } else {
            if (!write_output(out)) {
                return 2;
            }
        }
    }

    return (check_mode && any_unformatted) ? 1 : 0;
}

} // namespace nari::fmt
