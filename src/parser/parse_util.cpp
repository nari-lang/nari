// Include resolution + error context helpers (split out of parser.cpp).
#include "module_resolver.h"
#include "nari_fs.h"
#include "parser_api.h"
#include "parser_internal.h"
#include "token.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include "win_funcs.h"
#endif

// Defined in embedded_stdlib.cpp (global namespace).
const std::string &nari_std_module_source(const std::string &name);

namespace Parser {

// Stdlib std/* virtual paths: anything imported as "std/<name>" maps to the
// virtual path "<std>/<name>". The include_source_import lambda recognizes
// this prefix and pulls the source from the embedded module table instead of
// touching the filesystem.
static constexpr const char *kStdVirtualPrefix = "<std>/";

static bool is_std_module_import(const std::string &inc) {
    return inc.size() > 4 && inc.compare(0, 4, "std/") == 0;
}

static std::string std_module_name_from_spec(const std::string &inc) {
    if (!is_std_module_import(inc)) {
        return "";
    }
    return inc.substr(4);
}

static std::string make_std_virtual_path(const std::string &name) {
    return std::string(kStdVirtualPrefix) + name;
}

bool is_std_virtual_path(const std::string &path) {
    return path.size() > 6 && path.compare(0, 6, kStdVirtualPrefix) == 0;
}

std::string resolve_include_path(const std::string &inc, const std::string &basefile) {
    namespace fs = nari::fs;
    g_last_import_resolution_error.clear();
    if (inc.empty()) {
        return inc;
    }

    if (is_std_module_import(inc)) {
        std::string name = std_module_name_from_spec(inc);
        const std::string &src = ::nari_std_module_source(name);
        if (src.empty()) {
            g_last_import_resolution_error = "Unknown stdlib module: 'std/" + name + "'";
            return "";
        }
        return make_std_virtual_path(name);
    }

    fs::Path incp(inc);
    if (incp.is_absolute()) {
        return resolve_package_directory_path(incp.lexically_normal().string(), g_last_import_resolution_error);
    }

    if (is_package_import_spec(inc)) {
        std::string package_path = resolve_package_import_path(inc, basefile, g_last_import_resolution_error);
        if (!package_path.empty()) {
            return package_path;
        }
        return "";
    }

    // get base path to resolve relative includes against
    fs::Path basepath;
    if (basefile.empty()) {
        basepath = fs::current_path();
    } else {
        fs::Path bf(basefile);
        if (bf.has_parent_path()) {
            basepath = bf.parent_path();
        } else {
            basepath = fs::current_path();
        }
    }

    fs::Path combined = basepath / incp;
    return resolve_package_directory_path(combined.lexically_normal().string(), g_last_import_resolution_error);
}

/*
  NOTE: these functions do not imply that an error *has* occured
  it just adds the context to the error stack for if one does happen.
*/
void push_error_context(const std::string &ctx) {
    error_contexts.push_back(ctx);
}

void pop_error_context() {
    if (!error_contexts.empty()) {
        error_contexts.pop_back();
    }
}

// prints filename:line:col header + message, and then exits.
// (declared in parser/lexer.h so the lexer can report lexical errors.)
void fatal_error(const std::string &msg, const Token *tok) {
    std::string fname = current_filename.empty() ? std::string("<input>") : current_filename;
    int l = 0, c = 0;
    if (tok) {
        if (!tok->filename.empty()) {
            fname = tok->filename;
        }
        l = tok->line;
        c = tok->col;
    }

    fprintf(stderr, "Exception at %s:%d:%d\n", fname.c_str(), l, c);
    fprintf(stderr, "  %s\n", msg.c_str());

    auto print_source_context = [&](const std::string &file, int line, int col) {
#ifndef NARI_ESP_IDF
        if (file.empty() || file[0] == '<' || line <= 0)
            return;
        FILE *fp = fopen(file.c_str(), "r");
        if (!fp)
            return;

        char *line_buffer = nullptr;
        size_t buffer_size = 0;
        std::string line_text;

        for (int i = 1; i <= line; ++i) {
            ssize_t line_length = getline(&line_buffer, &buffer_size, fp);
            if (line_length == -1) {
                free(line_buffer);
                fclose(fp);
                return;
            }
            if (i == line) {
                line_text = line_buffer;
                if (!line_text.empty() && line_text.back() == '\n') {
                    line_text.pop_back();
                }
            }
        }
        free(line_buffer);
        fclose(fp);

        fprintf(stderr, "  %s\n", line_text.c_str());
        int caret_pos = col > 0 ? col - 1 : 0;
        std::string caret_line(caret_pos, ' ');
        caret_line.push_back('^');
        fprintf(stderr, "  %s\n", caret_line.c_str());
#else
        fprintf(stderr, "  (source context not available on this platform!)\n");
#endif
    };

    print_source_context(fname, l, c);

    // prints out a trace of included files up to this point
    if (!error_contexts.empty()) {
        for (int i = static_cast<int>(error_contexts.size()) - 1; i >= 0; --i) {
            fprintf(stderr, "  included from %s\n", error_contexts[i].c_str());
        }
    }

    exit(1);
}

} // namespace Parser
