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

#include <cstdio>
#include <string>
#include <vector>

#include "formatter.h"

namespace nari::fmt {

namespace {

std::string read_file_to_string(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return {};
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return {};
    }
    std::string buf((size_t)sz, '\0');
    size_t n = fread(&buf[0], 1, (size_t)sz, f);
    fclose(f);
    buf.resize(n);
    return buf;
}

std::string read_stdin_to_string() {
    std::string buf;
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
        buf.append(chunk, n);
    }
    return buf;
}

bool write_file(const std::string &path, const std::string &data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    size_t n = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return n == data.size();
}

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

    if (stdin_mode) {
        std::string src = read_stdin_to_string();
        std::string out, err;
        if (!format_source(src, "<stdin>", opts, out, err)) {
            fprintf(stderr, "nari fmt: %s\n", err.c_str());
            return 2;
        }
        fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }

    if (files.empty()) {
        fprintf(stderr, "Error: no input files specified\n");
        print_usage(argv[0]);
        return 1;
    }

    bool any_unformatted = false;
    for (const std::string &path : files) {
        std::string src = read_file_to_string(path);
        if (src.empty()) {
            fprintf(stderr, "nari fmt: error: failed to read '%s' (or file is empty)\n", path.c_str());
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
            if (out != src && !write_file(path, out)) {
                fprintf(stderr, "nari fmt: error: failed to write '%s'\n", path.c_str());
                return 2;
            }
        } else {
            fwrite(out.data(), 1, out.size(), stdout);
        }
    }

    return (check_mode && any_unformatted) ? 1 : 0;
}

} // namespace nari::fmt
