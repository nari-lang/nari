#ifndef DISABLE_REPL
#include <replxx.hxx>
#endif

#include "repl.h"

#include "bytecode.h"
#include "parser_api.h"
#include "runtime.h"
#ifndef DISABLE_JIT
#include "asmjit_jit.h"
#include "trace_jit.h"
#endif

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <corecrt_io.h>
#include <versionhelpers.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

// Provided by the generated embedded_stdlib.cpp
extern std::string nari_std_prelude_source();

// count unmatched open-braces/parens so we know whether the user has finished their statement yet
static int count_unmatched(const std::string &s) {
    int braces = 0, parens = 0;
    bool in_str = false;
    bool in_tmpl = false; // inside backtick template literal
    int tmpl_depth = 0;   // brace depth inside template {...}
    char str_ch = 0;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];

        if (in_tmpl) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (tmpl_depth > 0) {
                if (c == '{') {
                    ++tmpl_depth;
                    continue;
                }
                if (c == '}') {
                    --tmpl_depth;
                    continue;
                }
                continue;
            }
            if (c == '{') {
                tmpl_depth = 1;
                continue;
            }
            if (c == '`') {
                in_tmpl = false;
            }
            continue;
        }

        if (in_str) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == str_ch) {
                in_str = false;
            }
            continue;
        }

        // Single-line comment - everything after is not code
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            break;
        }

        if (c == '`') {
            in_tmpl = true;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_str = true;
            str_ch = c;
            continue;
        }
        if (c == '{') {
            ++braces;
        } else if (c == '}') {
            if (--braces < 0) {
                braces = 0;
            }
        } else if (c == '(') {
            ++parens;
        } else if (c == ')') {
            if (--parens < 0) {
                parens = 0;
            }
        }
    }
    return braces + parens;
}

// returns true if the first non-whitespace token suggests a declaration or assignment that should be accumulated
static bool looks_like_assignment(const std::string &src) {
    // walk past any leading whitespace.
    size_t i = 0;
    while (i < src.size() && std::isspace((unsigned char)src[i])) {
        ++i;
    }
    if (i >= src.size()) {
        return false;
    }
    // LHS must start with an identifier or '_'
    if (!std::isalpha((unsigned char)src[i]) && src[i] != '_') {
        return false;
    }

    int bracket_depth = 0;
    while (i < src.size()) {
        char c = src[i];
        if (c == '[') {
            ++bracket_depth;
            ++i;
            continue;
        }
        if (c == ']') {
            if (bracket_depth > 0) {
                --bracket_depth;
            }
            ++i;
            continue;
        }
        if (bracket_depth > 0) {
            ++i;
            continue;
        }
        if (std::isalnum((unsigned char)c) || c == '_' || c == '.') {
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        // Compound assignment: +=  -=  *=  /=  %=  @=
        if ((c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '@') && i + 1 < src.size() &&
            src[i + 1] == '=') {
            return true;
        }
        // Plain assignment '=' but NOT ==, !=, <=, >=
        if (c == '=') {
            char prev = (i > 0) ? src[i - 1] : 0;
            if (prev == '!' || prev == '<' || prev == '>' || prev == '=') {
                return false;
            }
            if (i + 1 < src.size() && src[i + 1] == '=') {
                return false;
            }
            return true;
        }
        break;
    }
    return false;
}

static bool should_accumulate(const std::string &src) {
    size_t i = 0;
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n')) {
        ++i;
    }
    if (i >= src.size()) {
        return false;
    }

    // starts_with
    auto sw = [&](const char *pfx) -> bool {
        size_t pl = std::strlen(pfx);
        if (src.size() - i < pl) {
            return false;
        }
        if (src.compare(i, pl, pfx) != 0) {
            return false;
        }
        size_t after = i + pl;
        if (after >= src.size()) {
            return true;
        }
        char nc = src[after];
        // for keywords that end with space or '(' we've already included the delimiter,
        // so just do a literal prefix match.
        return true;
    };

    return sw("func ") || sw("func(") || sw("async func ") || sw("class ") || sw("type ") || sw("enum ") ||
           sw("let ") || sw("global ") || sw("import ") || looks_like_assignment(src);
}

// trim trailing whitespace (without touching \n in the middle of the string).
static void rtrim(std::string &s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
}

// returns true if the terminal is a real TTY (for prompt / colour decisions).
static bool is_tty() {
#ifndef _WIN32
    return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#else
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#endif
}

static bool env_equals(const char *name, const char *value) {
    const char *env = std::getenv(name);
    return env && std::strcmp(env, value) == 0;
}

#ifdef _WIN32
static bool running_under_wine() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}
#endif

static bool supports_ansi_output() {
#ifndef _WIN32
    return is_tty();
#else
    if (!is_tty()) {
        return false;
    }

    if (env_equals("NARI_COLOR", "0")) {
        return false;
    }
    if (env_equals("NARI_COLOR", "1")) {
        return true;
    }

    // ConEmu / Cmder gives working ANSI on older Windows versions
    if (env_equals("ConEmuANSI", "ON")) {
        return true;
    }

    // Windows Terminal and modern terminal emulators **usually** expose TERM.
    if (std::getenv("WT_SESSION") != nullptr) {
        return true;
    }
    if (const char *term = std::getenv("TERM")) {
        if (*term != '\0' && std::strcmp(term, "dumb") != 0) {
            return true;
        }
    }

    // Windows 10+ supports ANSI output natively.
    if (!IsWindows10OrGreater()) {
        return false;
    }

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) {
        return false;
    }

    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) {
        return false;
    }

    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        return true;
    }

    if (SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        return true;
    }

    return false;
#endif
}

/*
  compile stdlib + accumulated definitions + new_code, then run them in a
  fresh VM so unwanted state from previous iterations doesn't bleed through.

  TODO: this could definitely be optimized to avoid re-parsing stdlib,
  but this was simple to implement and works well enough for now.

  returns true on clean execution, false on parse / compile / runtime error.
*/
#ifndef DISABLE_PARSER
static bool compile_and_run(const std::string &stdlib_src, const std::string &accumulated, const std::string &new_code,
                            bool auto_print, bool use_color, int argc, char **argv) {
    // build source that will be compiled in
    std::string full;
    if (!accumulated.empty()) {
        full = accumulated;
        if (full.back() != '\n') {
            full += '\n';
        }
    }

    if (auto_print) {
        // wrap the expression so we can inspect its return value
        full += "let __repl_result__ = (" + new_code + ")\n";
        if (use_color) {
            full += "print(\"\x1b[2m\x1b[37m\" @ to_string(__repl_result__) @ \"\x1b[0m\")\n";
        } else {
            full += "print(__repl_result__)\n";
        }
    } else {
        full += new_code;
    }

    // reset all persistent parser state so that nothing stale interferes with our session
    Parser::reset_parse_session();

    // parse the stdlib (no top-level aggregator since it registers its own funcs)
    Parser::set_source_filename("<stdlib>");
    FuncList stdlib_funcs = Parser::parse_program_from_source(stdlib_src, false);

    // Parse user code with the error-recovering parser for nicer diagnostics.
    Parser::set_source_filename("<repl>");
    Parser::ParseResult result = Parser::parse_program_recovering(full);

    if (!result.ok()) {
        // If the failure was caused by the auto-print wrapper, retry clean.
        if (auto_print) {
            return compile_and_run(stdlib_src, accumulated, new_code, false, use_color, argc, argv);
        }
        for (const auto &err : result.errors) {
            if (err.line > 0) {
                if (use_color) {
                    fprintf(stderr, "\033[31merror\033[0m [line %d]: %s\n", err.line, err.message.c_str());
                } else {
                    fprintf(stderr, "error [line %d]: %s\n", err.line, err.message.c_str());
                }
            } else {
                if (use_color) {
                    fprintf(stderr, "\033[31merror\033[0m: %s\n", err.message.c_str());
                } else {
                    fprintf(stderr, "error: %s\n", err.message.c_str());
                }
            }
        }
        return false;
    }

    if (result.functions.empty()) {
        return true; // nothing to run
    }

    // Merge stdlib + user into one FuncList for the bytecode compiler.
    FuncList combined;
    combined.reserve(stdlib_funcs.size() + result.functions.size());
    for (auto &f : stdlib_funcs) {
        combined.push_back(std::move(f));
    }
    for (auto &f : result.functions) {
        combined.push_back(std::move(f));
    }

    // Compile to bytecode.
    nari::bytecode::Chunk *chunk = nari::bytecode::compile_bytecode(combined);
    if (!chunk) {
        if (auto_print) {
            return compile_and_run(stdlib_src, accumulated, new_code, false, use_color, argc, argv);
        }
        if (use_color) {
            fprintf(stderr, "\033[31merror\033[0m: bytecode compilation failed\n");
        } else {
            fprintf(stderr, "error: bytecode compilation failed\n");
        }
        return false;
    }

    // run in a fresh VM so each REPL iteration starts from a clean slate.
    Runtime::reset_runtime_error_flag();
#ifndef DISABLE_JIT
    // The JIT compiler is reset automatically on chunk change
#endif
    nari::bytecode::VM vm(argc, argv);
    bool ok = vm.run(chunk);
    delete chunk;
    return ok;
}
#endif // DISABLE_PARSER

// Syntax highlighter
#ifndef DISABLE_REPL
static void do_highlight(const std::string &ctx, replxx::Replxx::colors_t &colors) {
    using Color = replxx::Replxx::Color;

    static constexpr const char *KEYWORDS[] = { "func",     "async", "await", "let",   "global", "return",
                                                "if",       "else",  "for",   "while", "in",     "break",
                                                "continue", "class", "type",  "enum",  "new",    "import",
                                                "export",   "true",  "false", "not" };

    size_t i = 0;
    while (i < ctx.size()) {
        char c = ctx[i];

        // single-line comment
        if (c == '/' && i + 1 < ctx.size() && ctx[i + 1] == '/') {
            while (i < ctx.size()) {
                colors[i++] = Color::GRAY;
            }
            break;
        }

        // string literals (double-quote and single-quote)
        if (c == '"' || c == '\'') {
            char delim = c;
            colors[i++] = Color::YELLOW;
            while (i < ctx.size()) {
                colors[i] = Color::YELLOW;
                if (ctx[i] == '\\') {
                    ++i;
                    if (i < ctx.size()) {
                        colors[i++] = Color::YELLOW;
                    }
                    continue;
                }
                if (ctx[i++] == delim) {
                    break;
                }
            }
            continue;
        }

        // backtick template literals `text {expr} text`
        if (c == '`') {
            colors[i++] = Color::YELLOW;
            while (i < ctx.size()) {
                if (ctx[i] == '\\') {
                    // escape sequence, colour both chars as string
                    colors[i++] = Color::YELLOW;
                    if (i < ctx.size()) {
                        colors[i++] = Color::YELLOW;
                    }
                    continue;
                }
                if (ctx[i] == '`') {
                    colors[i++] = Color::YELLOW;
                    break;
                }
                if (ctx[i] == '{') {
                    colors[i++] = Color::BRIGHTBLUE;
                    int depth = 1;
                    while (i < ctx.size() && depth > 0) {
                        if (ctx[i] == '{') {
                            ++depth;
                        } else if (ctx[i] == '}') {
                            --depth;
                            if (depth == 0) {
                                colors[i++] = Color::BRIGHTBLUE;
                                break;
                            }
                        }
                        ++i; // leave inner chars at DEFAULT
                    }
                    continue;
                }
                colors[i++] = Color::YELLOW;
            }
            continue;
        }

        // numeric literals (integer / float / hex)
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < ctx.size() && std::isdigit((unsigned char)ctx[i + 1]))) {
            while (i < ctx.size()) {
                char d = ctx[i];
                if (!std::isdigit((unsigned char)d) && d != '.' && d != 'x' && d != 'X' &&
                    !((d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F'))) {
                    break;
                }
                colors[i++] = Color::BRIGHTGREEN;
            }
            continue;
        }

        // identifiers and keywords
        if (std::isalpha(c) || c == '_') {
            size_t start = i;
            while (i < ctx.size() && (std::isalnum((ctx[i])) || ctx[i] == '_')) {
                ++i;
            }

            std::string word = ctx.substr(start, i - start);
            bool is_kw = false;
            for (const char *keyword : KEYWORDS) {
                if (word == keyword) {
                    is_kw = true;
                    break;
                }
            }
            Color col = is_kw ? Color::BRIGHTBLUE : Color::DEFAULT;
            for (size_t j = start; j < i; ++j) {
                colors[j] = col;
            }
            continue;
        }

        ++i;
    }
}
#endif // DISABLE_REPL

#if defined(_WIN32) && !defined(DISABLE_PARSER)
// plain repl in cases where there's issues
static void run_repl_plain(const std::string &stdlib_src, bool tty, bool use_color, int argc, char **argv) {
    std::string accumulated;
    std::string pending;
    if (tty) {
        printf("Nari REPL | 'exit' / Ctrl+D to quit\n");
    }
    for (;;) {
        if (tty) {
            fputs(pending.empty() ? ">> " : "... ", stdout);
            fflush(stdout);
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            if (tty) {
                printf("\n");
            }
            break;
        }
        rtrim(line);

        if (line == "clear" || line == "clear()") {
            if (tty) {
                fputs("\x1b[2J\x1b[H", stdout);
                fflush(stdout);
            }
            pending.clear();
            continue;
        }
        if (pending.empty() && (line == "exit" || line == "exit()")) {
            break;
        }

        if (!pending.empty()) {
            pending += '\n';
        }
        pending += line;
        if (!line.empty() && count_unmatched(pending) > 0) {
            continue;
        }

        std::string input = pending;
        pending.clear();
        if (input.empty()) {
            continue;
        }

        bool aprint = !should_accumulate(input) && input.find('\n') == std::string::npos;
        bool ran_ok = compile_and_run(stdlib_src, accumulated, input, aprint, use_color, argc, argv);
        if (ran_ok && should_accumulate(input)) {
            if (!accumulated.empty() && accumulated.back() != '\n') {
                accumulated += '\n';
            }
            accumulated += input;
        }
    }
}
#endif // _WIN32 && !DISABLE_PARSER

void run_repl(int argc, char **argv) {
#ifdef DISABLE_PARSER
    // no parser, which means no repl :(
    fprintf(stderr, "Error: REPL is not available when the parser is disabled.\n");
#else
    const std::string stdlib_src = nari_std_prelude_source();
    // declarative inputs from previous iterations
    std::string accumulated;
    // partial multi-line input
    std::string pending;
    const bool tty = is_tty();
    const bool use_color = supports_ansi_output();

#ifdef _WIN32
    if (running_under_wine()) {
        run_repl_plain(stdlib_src, tty, use_color, argc, argv);
        return;
    }
#endif

#ifndef DISABLE_REPL
    replxx::Replxx replxx;
    replxx.set_max_history_size(300);
    replxx.set_word_break_characters(" \t\n\"'{}()[]=+<>!&|^~*%/:;,.");
    replxx.set_highlighter_callback(do_highlight);

    // persist history across sessions.
    std::string history_file;
    const char *home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) {
        home = std::getenv("USERPROFILE");
    }
#endif
    if (home) {
        history_file = std::string(home) +
#ifdef _WIN32
                       "\\.nari_history";
#else
                       "/.nari_history";
#endif
        replxx.history_load(history_file);
    }

    if (tty) {
        printf("Nari REPL | 'exit' / Ctrl+D to quit, Ctrl+C to cancel\n");
    }

    bool ctrl_c_pressed = false;
    replxx.bind_key(replxx::Replxx::KEY::control('C'), [&ctrl_c_pressed](char32_t) -> replxx::Replxx::ACTION_RESULT {
        ctrl_c_pressed = true;
        return replxx::Replxx::ACTION_RESULT::BAIL;
    });

    int ctrl_c_count = 0;

    for (;;) {
        const char *prompt = pending.empty() ? ">> " : "... ";
        ctrl_c_pressed = false;
        const char *raw = replxx.input(prompt);

        if (!raw) {
            if (ctrl_c_pressed) {
                // Ctrl+C
                if (!pending.empty()) {
                    // cancel multi-line input and return to the primary prompt.
                    pending.clear();
                    ctrl_c_count = 0;
                    if (tty) {
                        printf("\n");
                    }
                } else {
                    ++ctrl_c_count;
                    if (ctrl_c_count >= 2) {
                        if (tty) {
                            printf("\n");
                        }
                        break;
                    }
                    if (tty) {
                        printf("\n(Press Ctrl+C again to exit)\n");
                    }
                }
                continue;
            }
            // Ctrl+D / EOF
            if (tty) {
                printf("\n");
            }
            break;
        }

        // reset on any successful read
        ctrl_c_count = 0;
        std::string line = raw;
        rtrim(line);

        if (line == "clear" || line == "clear()") {
            replxx.clear_screen();
            pending.clear();
            continue;
        }

        if (pending.empty() && (line == "exit" || line == "exit()")) {
            break;
        }

        // accumulate for multi-line blocks
        if (!pending.empty()) {
            pending += '\n';
        }
        pending += line;

        // if it's unbalanced, keep going
        if (!line.empty() && count_unmatched(pending) > 0) {
            continue;
        }

        std::string input = pending;
        pending.clear();
        if (input.empty()) {
            continue;
        }

        replxx.history_add(input);
        if (!history_file.empty()) {
            replxx.history_save(history_file);
        }

        // auto-print only for single-line expressions (not declarations/assignments)
        bool aprint = !should_accumulate(input) && input.find('\n') == std::string::npos;

        bool ran_ok = compile_and_run(stdlib_src, accumulated, input, aprint, use_color, argc, argv);

        // accumulate declarations and assignments so subsequent lines can reference them
        if (ran_ok && should_accumulate(input)) {
            if (!accumulated.empty() && accumulated.back() != '\n') {
                accumulated += '\n';
            }
            accumulated += input;
        }
    }

    if (!history_file.empty()) {
        replxx.history_save(history_file);
    }
#endif
#endif // DISABLE_PARSER
}
