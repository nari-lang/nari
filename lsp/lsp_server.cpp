// -- Nari Language Server --
//
// Supported requests / notifications:
//   initialize / initialized
//   shutdown / exit
//   textDocument/didOpen
//   textDocument/didChange   (full-document sync)
//   textDocument/didClose
//   textDocument/completion
//   textDocument/hover
//   textDocument/definition
//   textDocument/references
//   textDocument/typeDefinition
//   textDocument/codeAction
//   textDocument/inlayHint
//   textDocument/semanticTokens/full

#ifndef DISABLE_PARSER
#include "ast.h"
#include "core_types.h"
#include "parser_api.h"
#endif

#include "json.hpp"

using json = nlohmann::json;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <shlobj.h> // SHGetFolderPathW
#endif

static std::ofstream g_log_file;

static void lsp_log(const std::string &msg) {
    if (!g_log_file.is_open()) {
        return;
    }
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    g_log_file << '[' << ms << "] " << msg << '\n';
    g_log_file.flush();
}

static void init_logger() {
    const char *path = "/tmp/nari-lsp.log";
    if (path && *path) {
        g_log_file.open(path, std::ios::app);
        lsp_log("=== nari-lsp started ===");
    }
}

static std::string read_lsp_message() {
    size_t content_length = 0;
    std::string hdr;
    while (std::getline(std::cin, hdr)) {
        if (!hdr.empty() && hdr.back() == '\r') {
            hdr.pop_back();
        }
        if (hdr.empty()) {
            break; // blank line ends headers
        }
        if (hdr.rfind("Content-Length:", 0) == 0) {
            content_length = (size_t)std::stoull(hdr.substr(16));
        }
    }
    if (content_length == 0 || !std::cin.good()) {
        return {};
    }
    std::string body(content_length, '\0');
    std::cin.read(body.data(), (std::streamsize)content_length);
    return body;
}

// needed to make calling send_response and send_notification thread safe
static std::mutex g_stdout_mutex;

static void write_lsp_message(const std::string &body) {
    std::lock_guard<std::mutex> lk(g_stdout_mutex);
    std::printf("Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    std::fflush(stdout);
}

// LSP CompletionItemKind values used in this server
enum LspCompletionKind {
    CK_Text = 1,
    CK_Method = 2,
    CK_Function = 3,
    CK_Constructor = 4,
    CK_Field = 5,
    CK_Variable = 6,
    CK_Class = 7,
    CK_Interface = 8,
    CK_Module = 9,
    CK_Property = 10,
    CK_Keyword = 14,
    CK_Enum = 13,
    CK_EnumMember = 20,
    CK_Struct = 22,
    CK_TypeParam = 25
};

struct SymInfo {
    std::string name;
    std::string detail;
    int kind = CK_Variable;
    int line = 0;
    int col = 0;
    std::string source_file; // path where symbol is declared
    std::string doc_comment; // leading // comment text
    int scope_fn_line = 0;   // 1-based start line of owning function (0 = top-level/global)
    // "string", "number", "bool", "regex", "array", "object", "function", "null", "" = unknown.
    std::string inferred_type;
    std::string owner_class;
};

struct MemberInfo {
    std::string name;
    std::string detail;
    int line = 0;
    int col = 0;
    std::string source_file;
    bool is_method = false;
    bool is_public = true;
    bool is_static = false;
};

struct ClassInfo {
    std::string name;
    int line = 0, col = 0;
    std::string source_file;
    std::string parent_name;
    std::vector<MemberInfo> members;
};

struct ObjectLiteralInfo {
    std::string name;
    int decl_line = 0;
    int scope_fn_line = 0;
    std::vector<SymInfo> members;
};

//  AST walker: collects symbols from a parsed FuncList

#ifndef DISABLE_PARSER

// Unused-variable analysis

// Forward declarations
static void collect_expr_reads(const nari::Expr *, std::unordered_set<std::string> &);
static void collect_stmt_reads(const nari::Stmt *, std::unordered_set<std::string> &);
static void collect_stmts_reads(const std::vector<nari::StmtPtr> &, std::unordered_set<std::string> &);

static void collect_expr_reads(const nari::Expr *e, std::unordered_set<std::string> &reads) {
    if (!e) {
        return;
    }
    switch (e->kind) {
        case nari::ExprKind::Ident:
            reads.insert(static_cast<const nari::IdentExpr *>(e)->name);
            break;
        case nari::ExprKind::Unary:
            collect_expr_reads(static_cast<const nari::UnaryExpr *>(e)->operand.get(), reads);
            break;
        case nari::ExprKind::Binary: {
            const auto *b = static_cast<const nari::BinaryExpr *>(e);
            collect_expr_reads(b->left.get(), reads);
            collect_expr_reads(b->right.get(), reads);
            break;
        }
        case nari::ExprKind::Call: {
            const auto *c = static_cast<const nari::CallExpr *>(e);
            collect_expr_reads(c->callee.get(), reads);
            for (const auto &a : c->args) {
                collect_expr_reads(a.get(), reads);
            }
            break;
        }
        case nari::ExprKind::Index: {
            const auto *ie = static_cast<const nari::IndexExpr *>(e);
            collect_expr_reads(ie->object.get(), reads);
            collect_expr_reads(ie->index.get(), reads);
            break;
        }
        case nari::ExprKind::Member:
            collect_expr_reads(static_cast<const nari::MemberExpr *>(e)->object.get(), reads);
            break;
        case nari::ExprKind::Ternary: {
            const auto *te = static_cast<const nari::TernaryExpr *>(e);
            collect_expr_reads(te->condition.get(), reads);
            collect_expr_reads(te->true_expr.get(), reads);
            collect_expr_reads(te->false_expr.get(), reads);
            break;
        }
        case nari::ExprKind::ArrayLiteral:
            for (const auto &el : static_cast<const nari::ArrayLiteralExpr *>(e)->elements) {
                collect_expr_reads(el.get(), reads);
            }
            break;
        case nari::ExprKind::ObjectLiteral:
            for (const auto &[k, v] : static_cast<const nari::ObjectLiteralExpr *>(e)->entries) {
                collect_expr_reads(v.get(), reads);
            }
            break;
        case nari::ExprKind::Function: {
            const auto *fe = static_cast<const nari::FunctionExpr *>(e);
            if (fe->body) {
                collect_stmts_reads(fe->body->stmts, reads);
            }
            break;
        }
        case nari::ExprKind::Spawn: {
            const auto *se = static_cast<const nari::SpawnExpr *>(e);
            if (se->body) {
                collect_stmts_reads(se->body->stmts, reads);
            }
            break;
        }
        case nari::ExprKind::Match: {
            const auto *me = static_cast<const nari::MatchExpr *>(e);
            collect_expr_reads(me->scrutinee.get(), reads);
            for (const auto &arm : me->arms) {
                collect_expr_reads(arm.body.get(), reads);
            }
            break;
        }
        default:
            break;
    }
}

static void collect_stmt_reads(const nari::Stmt *s, std::unordered_set<std::string> &reads) {
    if (!s) {
        return;
    }
    switch (s->stmt_kind) {
        case nari::StmtKind::Expr:
            collect_expr_reads(static_cast<const nari::ExprStmt *>(s)->expr.get(), reads);
            break;
        case nari::StmtKind::VarDecl:
            collect_expr_reads(static_cast<const nari::VarDeclStmt *>(s)->initializerExpr.get(), reads);
            break;
        case nari::StmtKind::Assign:
            // target (string) is the write destination, only value is a read
            collect_expr_reads(static_cast<const nari::AssignStmt *>(s)->value.get(), reads);
            break;
        case nari::StmtKind::IndexAssign: {
            const auto *ia = static_cast<const nari::IndexAssignStmt *>(s);
            collect_expr_reads(ia->target.get(), reads); // obj/arr itself is read
            collect_expr_reads(ia->value.get(), reads);
            break;
        }
        case nari::StmtKind::Block:
            collect_stmts_reads(static_cast<const nari::BlockStmt *>(s)->stmts, reads);
            break;
        case nari::StmtKind::If: {
            const auto *is = static_cast<const nari::IfStmt *>(s);
            collect_expr_reads(is->cond.get(), reads);
            collect_stmt_reads(is->then_branch.get(), reads);
            collect_stmt_reads(is->else_branch.get(), reads);
            break;
        }
        case nari::StmtKind::While: {
            const auto *ws = static_cast<const nari::WhileStmt *>(s);
            collect_expr_reads(ws->cond.get(), reads);
            collect_stmt_reads(ws->body.get(), reads);
            break;
        }
        case nari::StmtKind::For: {
            const auto *fs = static_cast<const nari::ForStmt *>(s);
            collect_stmt_reads(fs->init.get(), reads);
            collect_expr_reads(fs->cond.get(), reads);
            collect_stmt_reads(fs->post.get(), reads);
            collect_stmt_reads(fs->body.get(), reads);
            break;
        }
        case nari::StmtKind::ForEach: {
            const auto *fe = static_cast<const nari::ForEachStmt *>(s);
            collect_expr_reads(fe->iterable.get(), reads);
            collect_stmt_reads(fe->body.get(), reads);
            break;
        }
        case nari::StmtKind::Return:
            collect_expr_reads(static_cast<const nari::ReturnStmt *>(s)->value.get(), reads);
            break;
        case nari::StmtKind::Throw:
            collect_expr_reads(static_cast<const nari::ThrowStmt *>(s)->value.get(), reads);
            break;
        case nari::StmtKind::Try: {
            const auto *ts = static_cast<const nari::TryStmt *>(s);
            if (ts->try_block) {
                collect_stmts_reads(ts->try_block->stmts, reads);
            }
            if (ts->catch_block) {
                collect_stmts_reads(ts->catch_block->stmts, reads);
            }
            if (ts->finally_block) {
                collect_stmts_reads(ts->finally_block->stmts, reads);
            }
            break;
        }
        case nari::StmtKind::Switch: {
            const auto *ss = static_cast<const nari::SwitchStmt *>(s);
            collect_expr_reads(ss->value.get(), reads);
            for (const auto &c : ss->cases) {
                collect_expr_reads(c.match.get(), reads);
                if (c.body) {
                    collect_stmts_reads(c.body->stmts, reads);
                }
            }
            if (ss->default_body) {
                collect_stmts_reads(ss->default_body->stmts, reads);
            }
            break;
        }
        default:
            break;
    }
}

static void collect_stmts_reads(const std::vector<nari::StmtPtr> &stmts,
                                std::unordered_set<std::string> &reads) {
    for (const auto &sp : stmts) {
        collect_stmt_reads(sp.get(), reads);
    }
}

struct LocalDecl {
    std::string name;
    int line = 0;
    int col = 0;
};

static void collect_local_decls_stmt(const nari::Stmt *, std::vector<LocalDecl> &);
static void collect_local_decls_stmts(const std::vector<nari::StmtPtr> &stmts,
                                      std::vector<LocalDecl> &out) {
    for (const auto &sp : stmts) {
        collect_local_decls_stmt(sp.get(), out);
    }
}
static void collect_local_decls_stmt(const nari::Stmt *s, std::vector<LocalDecl> &out) {
    if (!s) {
        return;
    }
    switch (s->stmt_kind) {
        case nari::StmtKind::VarDecl: {
            const auto *vd = static_cast<const nari::VarDeclStmt *>(s);
            if (vd->is_global != VarDeclCtrl::LOCAL) {
                break;
            }
            if (vd->destructure_kind == nari::DestructureKind::None) {
                if (!vd->name.empty() && vd->name[0] != '_') {
                    out.push_back({ vd->name, s->line, s->col });
                }
            } else if (vd->destructure_kind == nari::DestructureKind::Array) {
                for (const auto &nm : vd->array_names) {
                    if (!nm.empty() && nm[0] != '_') {
                        out.push_back({ nm, s->line, s->col });
                    }
                }
            } else {
                for (const auto &[k, loc] : vd->object_bindings) {
                    if (!loc.empty() && loc[0] != '_') {
                        out.push_back({ loc, s->line, s->col });
                    }
                }
            }
            break;
        }
        case nari::StmtKind::ForEach: {
            const auto *fe = static_cast<const nari::ForEachStmt *>(s);
            if (!fe->var.empty() && fe->var[0] != '_') {
                out.push_back({ fe->var, s->line, s->col });
            }
            collect_local_decls_stmt(fe->body.get(), out);
            break;
        }
        case nari::StmtKind::For: {
            const auto *fs = static_cast<const nari::ForStmt *>(s);
            collect_local_decls_stmt(fs->init.get(), out);
            collect_local_decls_stmt(fs->body.get(), out);
            break;
        }
        case nari::StmtKind::Block:
            collect_local_decls_stmts(static_cast<const nari::BlockStmt *>(s)->stmts, out);
            break;
        case nari::StmtKind::If: {
            const auto *is = static_cast<const nari::IfStmt *>(s);
            collect_local_decls_stmt(is->then_branch.get(), out);
            collect_local_decls_stmt(is->else_branch.get(), out);
            break;
        }
        case nari::StmtKind::While:
            collect_local_decls_stmt(static_cast<const nari::WhileStmt *>(s)->body.get(), out);
            break;
        case nari::StmtKind::Try: {
            const auto *ts = static_cast<const nari::TryStmt *>(s);
            if (ts->try_block) {
                collect_local_decls_stmts(ts->try_block->stmts, out);
            }
            if (ts->catch_block) {
                collect_local_decls_stmts(ts->catch_block->stmts, out);
            }
            if (ts->finally_block) {
                collect_local_decls_stmts(ts->finally_block->stmts, out);
            }
            break;
        }
        case nari::StmtKind::Switch: {
            const auto *ss = static_cast<const nari::SwitchStmt *>(s);
            for (const auto &c : ss->cases) {
                if (c.body) {
                    collect_local_decls_stmts(c.body->stmts, out);
                }
            }
            if (ss->default_body) {
                collect_local_decls_stmts(ss->default_body->stmts, out);
            }
            break;
        }
        default:
            break;
    }
}

// Strict-mode type diagnostics: under "use strict", warn (severity=Info) on
// untyped params and missing return-type annotations so the runtime check
// can be emitted.
static void emit_strict_type_warnings(
    const FuncList &funcs,
    const std::string &doc_filename,
    const std::vector<std::string> &source_lines,
    json &diagnostics) {
    auto find_word_col = [](const std::string &src_line, const std::string &word,
                            size_t search_from = 0) -> int {
        size_t pos = search_from;
        while ((pos = src_line.find(word, pos)) != std::string::npos) {
            bool left_ok = (pos == 0) || (!std::isalnum((unsigned char)src_line[pos - 1]) && src_line[pos - 1] != '_');
            size_t end = pos + word.size();
            bool right_ok = (end >= src_line.size()) || (!std::isalnum((unsigned char)src_line[end]) && src_line[end] != '_');
            if (left_ok && right_ok) {
                return (int)pos + 1; // 1-based
            }
            pos++;
        }
        return -1;
    };

    auto make_range = [](int line_1based, int col_1based, int length) -> json {
        int l = std::max(0, line_1based - 1);
        int c = std::max(0, col_1based - 1);
        json rs, re, rng;
        rs["line"] = int64_t(l);
        rs["character"] = int64_t(c);
        re["line"] = int64_t(l);
        re["character"] = int64_t(c + length);
        rng["start"] = std::move(rs);
        rng["end"] = std::move(re);
        return rng;
    };

    for (const auto &fn : funcs) {
        if (!fn || !fn->body) {
            continue;
        }
        if (!fn->strict_mode) {
            continue;
        }
        if (fn->filename != doc_filename) {
            continue;
        }
        if (fn->name.rfind("__top_level__", 0) == 0) {
            continue;
        }
        if (fn->name.rfind("__module_namespace_", 0) == 0) {
            continue;
        }
        if (fn->name.rfind("__module_export_", 0) == 0) {
            continue;
        }
        if (fn->function_expr != nullptr) {
            continue; // lambdas may omit annotations intentionally
        }

        for (const auto &p : fn->params) {
            if (p.name.empty() || p.name[0] == '_') {
                continue; // _ prefix = intentionally untyped
            }
            if (p.is_rest) {
                continue; // rest params are always arrays
            }
            if (p.type) {
                continue; // already annotated
            }

            // The parameter has no type annotation in strict mode.
            // Find the column of the param name on the function's line.
            int param_line = fn->line > 0 ? fn->line : 1;
            int param_col = fn->col > 0 ? fn->col : 1;
            if (param_line >= 1 && param_line <= (int)source_lines.size()) {
                const std::string &sig = source_lines[param_line - 1];
                size_t paren = sig.find('(');
                int found = find_word_col(sig, p.name, paren != std::string::npos ? paren : 0);
                if (found > 0) {
                    param_col = found;
                }
            }

            json diag;
            diag["range"] = make_range(param_line, param_col, (int)p.name.size());
            diag["severity"] = int64_t(1); // Error
            diag["message"] = "Strict mode: parameter '" + p.name +
                              "' has no type annotation. Add ': <type>' for runtime type checking.";
            diag["source"] = "nari";
            diagnostics.push_back(std::move(diag));
        }

        // Warn if the function has a body but no return type annotation.
        // Only warn for non-trivial functions (those that likely return something).
        // We approximate "returns something" by checking if the name is non-empty
        // (i.e., it's a named function, not a helper, the user presumably knows the return type).
        if (!fn->return_type && !fn->name.empty()) {
            int fn_line = fn->line > 0 ? fn->line : 1;
            int fn_col = fn->col > 0 ? fn->col : 1;
            const std::string &label = fn->name.empty() ? "<anonymous>" : fn->name;
            if (fn_line >= 1 && fn_line <= (int)source_lines.size()) {
                const std::string &sig = source_lines[fn_line - 1];
                int found = find_word_col(sig, fn->name, (size_t)(fn_col > 1 ? fn_col - 1 : 0));
                if (found > 0) {
                    fn_col = found;
                }
            }
            json diag;
            diag["range"] = make_range(fn_line, fn_col, (int)label.size());
            diag["severity"] = int64_t(1); // Error
            diag["message"] = "Strict mode: '" + label +
                              "' has no return type annotation. Add '-> <type>' for return-value checking.";
            diag["source"] = "nari";
            diagnostics.push_back(std::move(diag));
        }
    }
}

// Emit unused-local hints for every function defined in doc_filename.
static void emit_unused_warnings(
    const FuncList &funcs,
    const std::string &doc_filename,
    const std::vector<std::string> &source_lines,
    json &diagnostics) {
    for (const auto &fn : funcs) {
        if (!fn || !fn->body) {
            continue;
        }
        if (fn->filename != doc_filename) {
            continue;
        }
        if (fn->name.rfind("__top_level__", 0) == 0) {
            continue;
        }
        if (fn->name.rfind("__module_namespace_", 0) == 0) {
            continue;
        }
        if (fn->name.rfind("__module_export_", 0) == 0) {
            continue;
        }

        // Collect every identifier that is *read* anywhere in the body
        std::unordered_set<std::string> reads;
        collect_stmts_reads(fn->body->stmts, reads);

        // Collect local declarations
        std::vector<LocalDecl> locals;
        collect_local_decls_stmts(fn->body->stmts, locals);

        // Scan a source line to find the 1-based column of `word` as a whole identifier,
        // starting at `search_from` (0-based byte offset).  Returns -1 if not found.
        auto find_word_col = [](const std::string &src_line, const std::string &word, size_t search_from = 0) -> int {
            size_t pos = search_from;
            while ((pos = src_line.find(word, pos)) != std::string::npos) {
                bool left_ok = (pos == 0) || (!std::isalnum((unsigned char)src_line[pos - 1]) && src_line[pos - 1] != '_');
                size_t end = pos + word.size();
                bool right_ok = (end >= src_line.size()) || (!std::isalnum((unsigned char)src_line[end]) && src_line[end] != '_');
                if (left_ok && right_ok) {
                    return (int)pos + 1;
                }
                pos++;
            }
            return -1;
        };

        // resolve the real column of `name` on `ast_line`
        auto resolve_col = [&](const std::string &name, int ast_line, int ast_col) -> int {
            if (ast_line >= 1 && ast_line <= (int)source_lines.size()) {
                size_t from = (ast_col > 1) ? (size_t)(ast_col - 1) : 0;
                int found = find_word_col(source_lines[ast_line - 1], name, from);
                if (found > 0) {
                    return found;
                }
            }
            return ast_col;
        };

        auto appears_after = [&](const std::string &name, int line, int col) -> bool {
            if (name.empty()) {
                return false;
            }
            int start_line = std::max(1, line);
            int end_line = (int)source_lines.size();
            for (int cur_line = start_line; cur_line <= end_line; cur_line++) {
                size_t from = 0;
                if (cur_line == line) {
                    from = (size_t)std::max(0, col - 1 + (int)name.size());
                }
                if (find_word_col(source_lines[cur_line - 1], name, from) > 0) {
                    return true;
                }
            }
            return false;
        };

        auto emit_unused = [&](const std::string &name, int line, int col) {
            int lsp_line = std::max(0, line - 1);
            int lsp_col = std::max(0, col - 1);
            json rs, re, rng;
            rs["line"] = int64_t(lsp_line);
            rs["character"] = int64_t(lsp_col);
            re["line"] = int64_t(lsp_line);
            re["character"] = int64_t(lsp_col + (int)name.size());
            rng["start"] = std::move(rs);
            rng["end"] = std::move(re);

            json diag;
            diag["range"] = std::move(rng);
            diag["severity"] = int64_t(4); // 4 = hint
            diag["message"] = "'" + name + "' is declared but never used";
            diag["source"] = "nari";
            diag["tags"] = json::array({ int64_t(1) }); // DiagnosticTag.Unnecessary
            diagnostics.push_back(std::move(diag));
        };

        for (const auto &loc : locals) {
            if (reads.count(loc.name)) {
                continue;
            }
            int col = resolve_col(loc.name, loc.line, loc.col);
            if (appears_after(loc.name, loc.line, col)) {
                continue;
            }
            emit_unused(loc.name, loc.line, col);
        }

        // check parameters: scan signature line after '(' to find real column.
        for (const auto &p : fn->params) {
            if (p.name.empty() || p.name[0] == '_') {
                continue;
            }
            if (reads.count(p.name)) {
                continue;
            }
            int param_col = fn->col > 0 ? fn->col : 1;
            if (fn->line >= 1 && fn->line <= (int)source_lines.size()) {
                const std::string &sig_line = source_lines[fn->line - 1];
                size_t paren = sig_line.find('(');
                int found = find_word_col(sig_line, p.name, paren != std::string::npos ? paren : 0);
                if (found > 0) {
                    param_col = found;
                }
            }
            if (appears_after(p.name, fn->line, param_col)) {
                continue;
            }
            emit_unused(p.name, fn->line, param_col);
        }
    }
}

static std::string build_func_sig(const nari::Function &fn, const std::string &name_override = "") {
    const std::string &fname = name_override.empty() ? fn.name : name_override;
    std::string s = "func " + fname + "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) {
            s += ", ";
        }
        const auto &p = fn.params[i];
        if (p.is_rest) {
            s += "...";
        }
        s += p.name;
        if (p.type) {
            s += ": " + p.type->to_string();
        }
        // TODO: default value omitted, make it an option maybe?
    }
    s += ")";
    if (fn.return_type) {
        s += " -> " + fn.return_type->to_string();
    }
    return s;
}

static std::string build_method_sig(const nari::ClassMethod &method, const std::string &owner_class = "") {
    std::string s;
    if (method.is_static) {
        s += "static ";
    }
    if (!owner_class.empty()) {
        s += owner_class + ".";
    }
    s += method.name + "(";
    for (size_t i = 0; i < method.params.size(); ++i) {
        if (i) {
            s += ", ";
        }
        const auto &p = method.params[i];
        if (p.is_rest) {
            s += "...";
        }
        s += p.name;
        if (p.type) {
            s += ": " + p.type->to_string();
        }
    }
    s += ")";
    if (method.return_type) {
        s += " -> " + method.return_type->to_string();
    }
    return s;
}

static int find_identifier_col_in_line(const std::string &src_line, const std::string &word, size_t search_from = 0) {
    size_t pos = search_from;
    while ((pos = src_line.find(word, pos)) != std::string::npos) {
        bool left_ok = (pos == 0) || (!std::isalnum((unsigned char)src_line[pos - 1]) && src_line[pos - 1] != '_');
        size_t end = pos + word.size();
        bool right_ok = (end >= src_line.size()) || (!std::isalnum((unsigned char)src_line[end]) && src_line[end] != '_');
        if (left_ok && right_ok) {
            return (int)pos + 1; // 1-based
        }
        ++pos;
    }
    return -1;
}

static int resolve_identifier_col(
    const std::vector<std::string> &source_lines,
    const std::string &name,
    int ast_line,
    int ast_col,
    size_t search_from = 0) {
    if (ast_line >= 1 && ast_line <= (int)source_lines.size()) {
        int found = find_identifier_col_in_line(source_lines[ast_line - 1], name, search_from);
        if (found > 0) {
            return found;
        }
    }
    return ast_col;
}

static std::string infer_annotation_type(const nari::TypeAnnotation *t) {
    if (!t) {
        return "";
    }
    if (t->is_array) {
        return "array";
    }
    return t->name;
}

static std::string normalize_method_receiver_type(std::string type_name) {
    if (type_name == "arrays") {
        return "array";
    }
    if (type_name == "objects") {
        return "object";
    }
    if (type_name == "strings") {
        return "string";
    }
    return type_name;
}

// infer a simple type tag from a literal/well-known initializer expression.
// returns one of: "regex", "string", "number", "bool", "array", "object", "function", "null", or "" (unknown).
static std::string infer_expr_type(const nari::Expr *e) {
    if (!e) {
        return "";
    }
    switch (e->kind) {
        case nari::ExprKind::Regex:
            return "regex";
        case nari::ExprKind::String:
            return "string";
        case nari::ExprKind::Number:
            return "number";
        case nari::ExprKind::Bool:
            return "bool";
        case nari::ExprKind::Null:
            return "null";
        case nari::ExprKind::ArrayLiteral:
            return "array";
        case nari::ExprKind::ObjectLiteral:
            return "object";
        case nari::ExprKind::Function:
            return "function";
        // new ClassName(...) -> returns the class name as the inferred type
        case nari::ExprKind::New: {
            const auto *ne = static_cast<const nari::NewExpr *>(e);
            return ne->class_name; // user-defined class type
        }
        // Regex method results
        case nari::ExprKind::Call: {
            // /pattern/.test(...) -> bool;  /pattern/.exec(...) -> object|null
            const auto *ce = static_cast<const nari::CallExpr *>(e);
            if (ce->callee && ce->callee->kind == nari::ExprKind::Member) {
                const auto *me = static_cast<const nari::MemberExpr *>(ce->callee.get());
                if (me->object && me->object->kind == nari::ExprKind::Regex) {
                    if (me->member == "test") {
                        return "bool";
                    }
                    if (me->member == "exec") {
                        return "object";
                    }
                }
            }
            return "";
        }
        default:
            return "";
    }
}

static bool is_scope_owner_kind(int kind) {
    return kind == CK_Function || kind == CK_Method;
}

static int find_enclosing_scope_line(const std::vector<SymInfo> &symbols,
                                     const std::string &source_file,
                                     int cursor_line_1) {
    int enclosing_scope_line = 0;
    for (const auto &sym : symbols) {
        if (!is_scope_owner_kind(sym.kind)) {
            continue;
        }
        if (sym.source_file != source_file) {
            continue;
        }
        if (sym.line <= cursor_line_1 && sym.line > enclosing_scope_line) {
            enclosing_scope_line = sym.line;
        }
    }
    return enclosing_scope_line;
}

static const SymInfo *find_symbol_in_scope(const std::vector<SymInfo> &symbols,
                                           const std::string &name,
                                           const std::string &source_file,
                                           int cursor_line_1) {
    int enclosing_scope_line = find_enclosing_scope_line(symbols, source_file, cursor_line_1);
    const SymInfo *fallback = nullptr;
    for (const auto &sym : symbols) {
        if (sym.name != name) {
            continue;
        }
        if (sym.kind == CK_Variable && sym.scope_fn_line != 0) {
            if (sym.scope_fn_line == enclosing_scope_line) {
                return &sym;
            }
            continue;
        }
        if (!fallback) {
            fallback = &sym;
        }
    }
    return fallback;
}

static const ClassInfo *find_class_info(const std::vector<ClassInfo> &classes, const std::string &name) {
    for (const auto &ci : classes) {
        if (ci.name == name) {
            return &ci;
        }
    }
    return nullptr;
}

static const MemberInfo *find_class_member(const std::vector<ClassInfo> &classes,
                                           const std::string &class_name,
                                           const std::string &member_name,
                                           bool static_only,
                                           std::unordered_set<std::string> &visited) {
    if (!visited.insert(class_name).second) {
        return nullptr;
    }
    const ClassInfo *ci = find_class_info(classes, class_name);
    if (!ci) {
        return nullptr;
    }

    for (const auto &member : ci->members) {
        if (member.name == member_name && (!static_only || member.is_static)) {
            return &member;
        }
    }

    if (!ci->parent_name.empty()) {
        return find_class_member(classes, ci->parent_name, member_name, static_only, visited);
    }

    return nullptr;
}

static const MemberInfo *find_class_member(const std::vector<ClassInfo> &classes,
                                           const std::string &class_name,
                                           const std::string &member_name,
                                           bool static_only = false) {
    std::unordered_set<std::string> visited;
    return find_class_member(classes, class_name, member_name, static_only, visited);
}

static std::string resolve_receiver_class_name(const std::vector<SymInfo> &symbols,
                                               const std::vector<ClassInfo> &classes,
                                               const std::string &receiver,
                                               const std::string &source_file,
                                               int cursor_line_1) {
    if (receiver == "this") {
        int enclosing_scope_line = find_enclosing_scope_line(symbols, source_file, cursor_line_1);
        for (const auto &sym : symbols) {
            if (sym.kind == CK_Method && sym.source_file == source_file && sym.line == enclosing_scope_line && !sym.owner_class.empty()) {
                return sym.owner_class;
            }
        }
        return "";
    }

    if (find_class_info(classes, receiver)) {
        return receiver;
    }

    const SymInfo *receiver_sym = find_symbol_in_scope(symbols, receiver, source_file, cursor_line_1);
    if (!receiver_sym) {
        return "";
    }
    return receiver_sym->inferred_type;
}

static std::string build_function_expr_sig(const std::string &name, const nari::FunctionExpr *fn) {
    std::string s = "func " + name + "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) {
            s += ", ";
        }
        const auto &p = fn->params[i];
        if (p.is_rest) {
            s += "...";
        }
        s += p.name;
        if (p.type) {
            s += ": " + p.type->to_string();
        }
    }
    s += ")";
    if (fn->return_type) {
        s += " -> " + fn->return_type->to_string();
    }
    return s;
}

static std::vector<SymInfo> collect_object_literal_members(const nari::ObjectLiteralExpr *object_expr) {
    std::vector<SymInfo> members;
    if (!object_expr) {
        return members;
    }

    for (const auto &[key, value] : object_expr->entries) {
        if (key.empty()) {
            continue;
        }

        SymInfo member;
        member.name = key;
        member.line = value ? value->line : 0;
        member.col = value ? value->col : 0;

        if (value && value->kind == nari::ExprKind::Function) {
            member.kind = CK_Method;
            member.detail = build_function_expr_sig(key, static_cast<const nari::FunctionExpr *>(value.get()));
        } else {
            member.kind = CK_Property;
            std::string value_type = infer_expr_type(value.get());
            member.detail = value_type.empty()
                                ? "property " + key
                                : "property " + key + ": " + value_type;
        }

        members.push_back(std::move(member));
    }

    return members;
}

static const ObjectLiteralInfo *find_object_literal_info(const std::vector<ObjectLiteralInfo> &infos,
                                                         const std::vector<SymInfo> &symbols,
                                                         const std::string &receiver,
                                                         const std::string &source_file,
                                                         int cursor_line_1) {
    const SymInfo *receiver_sym = find_symbol_in_scope(symbols, receiver, source_file, cursor_line_1);
    if (receiver_sym) {
        for (const auto &info : infos) {
            if (info.name != receiver) {
                continue;
            }
            if (info.scope_fn_line != receiver_sym->scope_fn_line) {
                continue;
            }
            if (info.decl_line == receiver_sym->line) {
                return &info;
            }
        }
    }

    const int enclosing_scope_line = find_enclosing_scope_line(symbols, source_file, cursor_line_1);
    const ObjectLiteralInfo *fallback = nullptr;
    for (const auto &info : infos) {
        if (info.name != receiver) {
            continue;
        }
        if (info.scope_fn_line != enclosing_scope_line) {
            continue;
        }
        if (info.decl_line > cursor_line_1) {
            continue;
        }
        if (!fallback || info.decl_line > fallback->decl_line) {
            fallback = &info;
        }
    }
    return fallback;
}

// Recursively walk a statement list to collect variable/foreach declarations.
// ns_to_path maps "__module_namespace_N__" -> original file path (for import detection).
// scope_fn_line: 1-based start line of the enclosing function (0 = top-level).
static void walk_stmts(const std::vector<nari::StmtPtr> &stmts,
                       std::vector<SymInfo> &out,
                       const std::map<std::string, std::string> &ns_to_path,
                       const std::vector<std::string> &source_lines,
                       int scope_fn_line = 0,
                       std::vector<ObjectLiteralInfo> *object_literals = nullptr) {
    for (const auto &sptr : stmts) {
        if (!sptr) {
            continue;
        }
        const nari::Stmt *s = sptr.get();

        if (s->stmt_kind == nari::StmtKind::VarDecl) {
            const auto *vd = static_cast<const nari::VarDeclStmt *>(s);
            if (vd->destructure_kind == nari::DestructureKind::None) {
                if (!vd->name.empty()) {
                    // Detect import bindings: let X = __module_namespace_N__
                    bool is_import = false;
                    std::string import_detail;
                    std::string import_source_path;
                    if (vd->initializerExpr &&
                        vd->initializerExpr->kind == nari::ExprKind::Ident) {
                        const auto *ie = static_cast<const nari::IdentExpr *>(vd->initializerExpr.get());
                        if (ie->name.rfind("__module_namespace_", 0) == 0) {
                            auto it = ns_to_path.find(ie->name);
                            if (it != ns_to_path.end()) {
                                import_source_path = it->second; // full filesystem path
                                // Extract just the filename for the display label
                                std::string fname = it->second;
                                auto slash = fname.find_last_of("/\\");
                                if (slash != std::string::npos) {
                                    fname = fname.substr(slash + 1);
                                }
                                import_detail = "module \"" + fname + "\"";
                                is_import = true;
                            }
                        }
                    }
                    int decl_col = resolve_identifier_col(source_lines, vd->name, s->line, s->col,
                                                          (s->col > 1) ? (size_t)(s->col - 1) : 0u);
                    if (is_import) {
                        out.push_back({ vd->name, import_detail, CK_Module, s->line, decl_col, import_source_path });
                    } else {
                        std::string itype = infer_expr_type(vd->initializerExpr.get());
                        std::string det;
                        if (!itype.empty()) {
                            det = (vd->is_global ? "global " : "") + itype + " " + vd->name;
                        } else {
                            det = vd->is_global ? "global variable" : "variable";
                        }
                        SymInfo si{ vd->name, det, CK_Variable, s->line, decl_col };
                        si.scope_fn_line = scope_fn_line;
                        si.inferred_type = itype;
                        out.push_back(std::move(si));

                        if (object_literals && vd->initializerExpr && vd->initializerExpr->kind == nari::ExprKind::ObjectLiteral) {
                            ObjectLiteralInfo info;
                            info.name = vd->name;
                            info.decl_line = s->line;
                            info.scope_fn_line = scope_fn_line;
                            info.members = collect_object_literal_members(
                                static_cast<const nari::ObjectLiteralExpr *>(vd->initializerExpr.get()));
                            object_literals->push_back(std::move(info));
                        }
                    }
                }
            } else if (vd->destructure_kind == nari::DestructureKind::Array) {
                for (const auto &nm : vd->array_names) {
                    if (!nm.empty()) {
                        int decl_col = resolve_identifier_col(source_lines, nm, s->line, s->col,
                                                              (s->col > 1) ? (size_t)(s->col - 1) : 0u);
                        SymInfo si{ nm, "variable", CK_Variable, s->line, decl_col };
                        si.scope_fn_line = scope_fn_line;
                        out.push_back(std::move(si));
                    }
                }
            } else {
                for (const auto &[key, local] : vd->object_bindings) {
                    (void)key;
                    if (!local.empty()) {
                        int decl_col = resolve_identifier_col(source_lines, local, s->line, s->col,
                                                              (s->col > 1) ? (size_t)(s->col - 1) : 0u);
                        SymInfo si{ local, "variable", CK_Variable, s->line, decl_col };
                        si.scope_fn_line = scope_fn_line;
                        out.push_back(std::move(si));
                    }
                }
            }

        } else if (s->stmt_kind == nari::StmtKind::ForEach) {
            const auto *fe = static_cast<const nari::ForEachStmt *>(s);
            if (!fe->var.empty()) {
                int decl_col = resolve_identifier_col(source_lines, fe->var, s->line, s->col,
                                                      (s->col > 1) ? (size_t)(s->col - 1) : 0u);
                SymInfo si{ fe->var, "variable", CK_Variable, s->line, decl_col };
                si.scope_fn_line = scope_fn_line;
                out.push_back(std::move(si));
            }
            if (fe->body && fe->body->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(fe->body.get())->stmts, out, ns_to_path,
                           source_lines, scope_fn_line, object_literals);
            }

        } else if (s->stmt_kind == nari::StmtKind::Block) {
            walk_stmts(static_cast<const nari::BlockStmt *>(s)->stmts, out, ns_to_path,
                       source_lines, scope_fn_line, object_literals);

        } else if (s->stmt_kind == nari::StmtKind::If) {
            const auto *is = static_cast<const nari::IfStmt *>(s);
            if (is->then_branch && is->then_branch->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(is->then_branch.get())->stmts, out, ns_to_path,
                           source_lines, scope_fn_line, object_literals);
            }
            if (is->else_branch && is->else_branch->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(is->else_branch.get())->stmts, out, ns_to_path,
                           source_lines, scope_fn_line, object_literals);
            }

        } else if (s->stmt_kind == nari::StmtKind::While) {
            const auto *ws = static_cast<const nari::WhileStmt *>(s);
            if (ws->body && ws->body->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(ws->body.get())->stmts, out, ns_to_path,
                           source_lines, scope_fn_line, object_literals);
            }

        } else if (s->stmt_kind == nari::StmtKind::For) {
            const auto *fs = static_cast<const nari::ForStmt *>(s);
            if (fs->body && fs->body->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(fs->body.get())->stmts, out, ns_to_path,
                           source_lines, scope_fn_line, object_literals);
            }
        }
    }
}

static std::vector<SymInfo> collect_symbols(const FuncList &funcs,
                                            const std::string &doc_filename,
                                            const std::vector<std::string> &source_lines,
                                            std::vector<ObjectLiteralInfo> *object_literals = nullptr) {
    std::vector<SymInfo> out;

    // Build reverse map: namespace global name -> source file path
    // so walk_stmts can detect import bindings.
    std::map<std::string, std::string> ns_to_path;
    for (const auto &[path, ns_name] : Parser::get_module_namespace_registry()) {
        ns_to_path[ns_name] = path;
    }

    for (const auto &fn : funcs) {
        if (!fn) {
            continue;
        }
        // skip the auto-generated aggregator wrapper
        const bool is_top_level = fn->name.empty() || fn->name == "__top_level__" || fn->name.rfind("__top_level__@", 0) == 0;
        if (is_top_level) {
            // Walk body for top-level let/global declarations and tag them with
            // their source file so goto-def can navigate to them.
            if (fn->body) {
                size_t before = out.size();
                walk_stmts(fn->body->stmts, out, ns_to_path, source_lines, 0, object_literals);
                for (size_t j = before; j < out.size(); ++j) {
                    if (out[j].source_file.empty()) {
                        out[j].source_file = fn->filename;
                    }
                }
            }
            continue;
        }
        // Skip internal __module_namespace_N__ accessor functions
        if (fn->name.rfind("__module_namespace_", 0) == 0) {
            continue;
        }

        // De-mangle exported function names: __module_export_N__originalName -> originalName
        // The parser renames exported functions to these internal aliases; we add both so
        // that hovering the original name (e.g. `add`) resolves correctly.
        std::string display_name = fn->name;
        static const std::string export_prefix = "__module_export_";
        if (fn->name.rfind(export_prefix, 0) == 0) {
            // format: __module_export_N__name, find the second __ after the prefix+digits
            size_t num_end = export_prefix.size();
            while (num_end < fn->name.size() && std::isdigit((unsigned char)fn->name[num_end])) {
                ++num_end;
            }
            if (num_end + 2 < fn->name.size() && fn->name[num_end] == '_' && fn->name[num_end + 1] == '_') {
                display_name = fn->name.substr(num_end + 2);
            }
        }

        int fn_col = resolve_identifier_col(source_lines, display_name, fn->line, fn->col,
                                            (fn->col > 1) ? (size_t)(fn->col - 1) : 0u);
        out.push_back({ display_name, build_func_sig(*fn, display_name), CK_Function, fn->line, fn_col, fn->filename });
        // Walk body for locals, always for non-mangled functions, and also for de-mangled (exported) functions defined in the current document.
        const bool is_local_def = (display_name == fn->name) || (!doc_filename.empty() && fn->filename == doc_filename);
        if (is_local_def) {
            size_t fn_param_search_from = 0;
            if (fn->line >= 1 && fn->line <= (int)source_lines.size()) {
                const std::string &sig = source_lines[fn->line - 1];
                size_t paren = sig.find('(');
                fn_param_search_from = (paren == std::string::npos) ? 0u : paren;
            }
            // Emit parameters as scoped variables so hover and goto-def work on them
            for (const auto &param : fn->params) {
                if (param.name.empty()) {
                    continue;
                }
                std::string param_detail = "(parameter) ";
                if (param.is_rest) {
                    param_detail += "...";
                }
                param_detail += param.name;
                if (param.type) {
                    param_detail += ": " + param.type->to_string();
                }
                SymInfo paramInfo;
                paramInfo.name = param.name;
                paramInfo.detail = param_detail;
                paramInfo.kind = CK_Variable;
                paramInfo.line = fn->line;
                paramInfo.col = resolve_identifier_col(source_lines, param.name, fn->line, fn_col,
                                                       fn_param_search_from);
                paramInfo.source_file = fn->filename;
                paramInfo.scope_fn_line = fn->line;
                paramInfo.inferred_type = infer_annotation_type(param.type.get());
                out.push_back(std::move(paramInfo));
            }
            if (fn->body) {
                walk_stmts(fn->body->stmts, out, ns_to_path, source_lines, fn->line, object_literals);
            }
        }
    }

    for (const auto &[name, cd] : Parser::get_all_registered_classes()) {
        if (!cd) {
            continue;
        }

        for (const auto &method : cd->methods) {
            SymInfo method_info{
                method.name,
                build_method_sig(method, cd->name),
                CK_Method,
                method.line,
                resolve_identifier_col(source_lines, method.name, method.line, method.col,
                                       (method.col > 1) ? (size_t)(method.col - 1) : 0u),
                method.filename.empty() ? cd->filename : method.filename
            };
            method_info.owner_class = cd->name;
            out.push_back(std::move(method_info));

            for (const auto &param : method.params) {
                if (param.name.empty()) {
                    continue;
                }
                std::string param_detail = "(parameter) ";
                if (param.is_rest) {
                    param_detail += "...";
                }
                param_detail += param.name;
                if (param.type) {
                    param_detail += ": " + param.type->to_string();
                }

                SymInfo param_info{
                    param.name,
                    param_detail,
                    CK_Variable,
                    method.line,
                    resolve_identifier_col(source_lines, param.name, method.line, method.col,
                                           [&source_lines, &method]() -> size_t {
                                               if (method.line < 1 || method.line > (int)source_lines.size()) {
                                                   return 0u;
                                               }
                                               const std::string &sig = source_lines[method.line - 1];
                                               size_t paren = sig.find('(');
                                               return paren == std::string::npos ? 0u : paren;
                                           }()),
                    method.filename.empty() ? cd->filename : method.filename
                };
                param_info.scope_fn_line = method.line;
                param_info.inferred_type = infer_annotation_type(param.type.get());
                out.push_back(std::move(param_info));
            }

            if (method.body) {
                walk_stmts(method.body->stmts, out, ns_to_path, source_lines, method.line, object_literals);
            }
        }
    }

    // Types from the parser registry
    for (const auto &[name, tdecl] : Parser::get_all_registered_types()) {
        if (!tdecl || Parser::is_registered_class(name)) {
            continue; // classes handled below
        }
        if (Parser::is_registered_enum(name)) {
            out.push_back({ name, "enum " + name, CK_Enum, tdecl->line, tdecl->col });
        } else if (tdecl->is_alias()) {
            out.push_back({ name, "type " + name + " = " + tdecl->alias_target->to_string(),
                            CK_Struct, tdecl->line, tdecl->col });
        } else {
            std::string detail = "type " + name;
            if (!tdecl->generic_params.empty()) {
                detail += "<";
                for (size_t i = 0; i < tdecl->generic_params.size(); ++i) {
                    if (i) {
                        detail += ", ";
                    }
                    detail += tdecl->generic_params[i];
                }
                detail += ">";
            }
            out.push_back({ name, detail, CK_Struct, tdecl->line, tdecl->col });
        }
    }

    return out;
}

static std::vector<ClassInfo> collect_classes() {
    std::vector<ClassInfo> result;
    for (const auto &[name, cd] : Parser::get_all_registered_classes()) {
        if (!cd) {
            continue;
        }

        ClassInfo ci;
        ci.name = name;
        ci.line = cd->line;
        ci.col = cd->col;
        ci.source_file = cd->filename;
        ci.parent_name = cd->parent_name;

        for (const auto &f : cd->fields) {
            MemberInfo m;
            m.name = f.name;
            m.detail = f.type ? f.type->to_string() : "";
            m.line = f.line;
            m.col = f.col;
            m.source_file = f.filename.empty() ? cd->filename : f.filename;
            m.is_method = false;
            m.is_public = (f.visibility == nari::Visibility::Public);
            m.is_static = f.is_static;
            ci.members.push_back(std::move(m));
        }
        for (const auto &meth : cd->methods) {
            MemberInfo m;
            m.name = meth.name;
            m.detail = build_method_sig(meth, cd->name);
            m.line = meth.line;
            m.col = meth.col;
            m.source_file = meth.filename.empty() ? cd->filename : meth.filename;
            m.is_method = true;
            m.is_public = (meth.visibility == nari::Visibility::Public);
            m.is_static = meth.is_static;
            ci.members.push_back(std::move(m));
        }
        result.push_back(std::move(ci));
    }
    return result;
}

#endif // DISABLE_PARSER

// --------------------------------------------------------------------------
//  Shared text / semantic-token helpers
// --------------------------------------------------------------------------

// Split text into lines, stripping trailing \r from each line.
static std::vector<std::string> lsp_split_lines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r') {
                --end;
            }
            lines.push_back(text.substr(start, end - start));
            start = i + 1;
        }
    }
    return lines;
}

// Find 1-based column of `word` as a whole identifier in `src_line`,
// starting at 0-based byte offset `from`.  Returns -1 if not found.
static int lsp_find_word_col(const std::string &src_line, const std::string &word,
                             size_t from = 0) {
    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    size_t pos = from;
    while ((pos = src_line.find(word, pos)) != std::string::npos) {
        bool left_ok = (pos == 0) || !is_id(src_line[pos - 1]);
        size_t rend = pos + word.size();
        bool right_ok = (rend >= src_line.size()) || !is_id(src_line[rend]);
        if (left_ok && right_ok) {
            return (int)pos + 1; // 1-based
        }
        ++pos;
    }
    return -1;
}

// Semantic token type indices must match the legend emitted by handle_initialize.
static constexpr int ST_NAMESPACE = 0;
static constexpr int ST_TYPE = 1;
static constexpr int ST_CLASS = 2;
static constexpr int ST_ENUM = 3;
static constexpr int ST_PARAM = 7;
static constexpr int ST_VARIABLE = 8;
static constexpr int ST_FUNCTION = 12;
// Token modifier bits.
static constexpr int SM_DECLARATION = (1 << 0);

// --------------------------------------------------------------------------
//  Text utilities
// --------------------------------------------------------------------------

// Extracts the identifier token that overlaps with 0-based (line0, col0).
// Returns {token, start_col}.
static std::pair<std::string, int>
word_at(const std::string &text, int line0, int col0) {
    std::istringstream ss(text);
    std::string ln;
    int cur = 0;
    while (std::getline(ss, ln)) {
        if (cur == line0) {
            break;
        }
        ++cur;
    }
    if (cur != line0 || col0 < 0 || col0 > (int)ln.size()) {
        return { "", col0 };
    }

    // identifier chars: [A-Za-z0-9_]
    auto is_ident = [](char c) { return isalnum((unsigned char)c) || c == '_'; };

    int start = col0;
    while (start > 0 && is_ident(ln[start - 1])) {
        --start;
    }
    int end2 = col0;
    while (end2 < (int)ln.size() && is_ident(ln[end2])) {
        ++end2;
    }
    return { ln.substr(start, end2 - start), start };
}

// Scan backward from `sym_line_1based - 1` collecting consecutive /// or // lines.
// Returns the comment text (newline-joined), or empty string if none.
static std::string extract_doc_comment(const std::string &text, int sym_line_1based) {
    if (sym_line_1based <= 1) {
        return {};
    }

    // Collect line-start offsets up to sym_line_1based lines
    std::vector<size_t> starts;
    starts.push_back(0);
    for (size_t i = 0; i < text.size() && (int)starts.size() < sym_line_1based; ++i) {
        if (text[i] == '\n') {
            starts.push_back(i + 1);
        }
    }

    auto get_line = [&](int idx) -> std::string_view {
        if (idx < 0 || idx >= (int)starts.size()) {
            return {};
        }
        size_t s = starts[idx];
        size_t e = (idx + 1 < (int)starts.size()) ? starts[idx + 1] : text.size();
        if (e > s && text[e - 1] == '\n') {
            --e;
        }
        if (e > s && text[e - 1] == '\r') {
            --e;
        }
        return { text.data() + s, e - s };
    };

    std::vector<std::string> comment_lines;
    for (int i = sym_line_1based - 2; i >= 0; --i) {
        std::string_view sv = get_line(i);
        size_t p = 0;
        while (p < sv.size() && (sv[p] == ' ' || sv[p] == '\t')) {
            ++p;
        }
        sv = sv.substr(p);
        if (sv.empty()) {
            break;
        }
        std::string_view content;
        if (sv.size() >= 3 && sv[0] == '/' && sv[1] == '/' && sv[2] == '/') {
            content = sv.substr(3);
            if (!content.empty() && content[0] == ' ') {
                content = content.substr(1);
            }
        } else if (sv.size() >= 2 && sv[0] == '/' && sv[1] == '/') {
            content = sv.substr(2);
            if (!content.empty() && content[0] == ' ') {
                content = content.substr(1);
            }
        } else {
            break;
        }
        comment_lines.push_back(std::string(content));
    }
    if (comment_lines.empty()) {
        return {};
    }
    std::reverse(comment_lines.begin(), comment_lines.end());
    std::string result;
    for (size_t i = 0; i < comment_lines.size(); ++i) {
        if (i) {
            result += '\n';
        }
        result += comment_lines[i];
    }
    return result;
}

// If the word starting at `word_start` in line `line0` is preceded by "identifier.",
// returns that identifier (the receiver). Otherwise returns "".
static std::string receiver_at(const std::string &text, int line0, int word_start) {
    if (word_start <= 1) {
        return {};
    }
    std::istringstream ss(text);
    std::string ln;
    int cur = 0;
    while (std::getline(ss, ln)) {
        if (cur == line0) {
            break;
        }
        ++cur;
    }
    if (cur != line0 || word_start - 1 >= (int)ln.size()) {
        return {};
    }
    if (ln[word_start - 1] != '.') {
        return {};
    }
    auto is_ident = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    int recv_end = word_start - 1; // index of '.'
    if (recv_end <= 0 || !is_ident(ln[recv_end - 1])) {
        return {};
    }
    int recv_start = recv_end - 1;
    while (recv_start > 0 && is_ident(ln[recv_start - 1])) {
        --recv_start;
    }
    return ln.substr(recv_start, recv_end - recv_start);
}

// If the cursor is inside a string literal on an import line, returns the
// unquoted specifier and fills start_col/end_col (0-based, columns of the
// opening and closing quotes).  Returns "" if not on an import string.
static std::string import_string_at(const std::string &text, int line0, int col0, int &out_start, int &out_end) {
    std::istringstream ss(text);
    std::string ln;
    int cur = 0;
    while (std::getline(ss, ln)) {
        if (cur == line0) {
            break;
        }
        ++cur;
    }
    if (cur != line0) {
        return {};
    }

    // Must look like an import statement
    bool has_import = (ln.find("import") != std::string::npos);
    if (!has_import) {
        return {};
    }

    // Scan for all "..." string literals in the line
    for (int i = 0; i < (int)ln.size(); ++i) {
        if (ln[i] != '"') {
            continue;
        }
        int str_start = i; // position of opening quote
        int j = i + 1;
        while (j < (int)ln.size() && ln[j] != '"') {
            if (ln[j] == '\\') {
                ++j; // skip escape
            }
            ++j;
        }
        if (j >= (int)ln.size()) {
            break;
        }
        int str_end = j; // position of closing quote
        // Check if cursor falls within (or on the quotes)
        if (col0 >= str_start && col0 <= str_end) {
            out_start = str_start;
            out_end = str_end;
            return ln.substr(str_start + 1, str_end - str_start - 1);
        }
        i = j; // advance past the closing quote
    }
    return {};
}

// --------------------------------------------------------------------------
//  NariLspServer
// --------------------------------------------------------------------------

static const char *kNariKeywords[] = {
    "let", "global", "func", "return", "if", "else", "while", "for",
    "foreach", "in", "break", "continue", "class", "new", "this",
    "type", "enum", "match", "spawn", "await", "try", "catch", "finally",
    "throw", "import", "export", "public", "private", "null",
    "true", "false", nullptr
};

static const struct {
    const char *name;
    const char *detail;
} kNariBuiltins[] = {
    { "print", "func print(...)" },
    { "to_string", "func to_string(value) -> string" },
    { "to_number", "func to_number(value) -> number" },
    { "to_bool", "func to_bool(value) -> bool" },
    { "typeof", "func typeof(value) -> string" },
    { "is_number", "func is_number(value) -> bool" },
    { "is_string", "func is_string(value) -> bool" },
    { "is_bool", "func is_bool(value) -> bool" },
    { "is_array", "func is_array(value) -> bool" },
    { "is_object", "func is_object(value) -> bool" },
    { "is_function", "func is_function(value) -> bool" },
    { "parse_int", "func parse_int(str, radix?) -> number" },
    { "parse_float", "func parse_float(str) -> number" },
    { "random", "func random() -> number" },
    { "time", "func time() -> number" },
    { "range", "func range(start, end, step?) -> number[]" },
    { "set_timeout", "func set_timeout(fn, delay)" },
    { "set_interval", "func set_interval(fn, interval) -> number" },
    { "clear_interval", "func clear_interval(id)" },
    { "read_line", "func read_line() -> string" },
    { nullptr, nullptr }
};

struct DocState {
    std::string content;
    std::size_t content_hash = 0; // latest editor content
    std::size_t parsed_hash = 0;  // last content version with fresh symbols/diagnostics
    std::vector<SymInfo> symbols;
    std::vector<ClassInfo> classes;
    std::vector<ObjectLiteralInfo> object_literals;
};

// -------------------------------------------------------------
//  Embedded builtins declarations (generated by meson)
// -------------------------------------------------------------
#ifndef DISABLE_PARSER
extern std::string nari_embedded_builtins();
extern std::string nari_std_prelude_source();
#endif

// Returns the directory used for Nari user data (~/.nari or %APPDATA%/nari).
static std::filesystem::path nari_data_dir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        return std::filesystem::path(buf) / "nari";
    }
    return std::filesystem::temp_directory_path() / "nari";
#else
    const char *home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".nari";
    }
    return std::filesystem::temp_directory_path() / "nari";
#endif
}

// Converts a filesystem path to a file:// URI.
static std::string path_to_uri(const std::filesystem::path &p) {
    std::string s = p.string();
    // Replace backslashes on Windows
    for (char &c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (s.empty() || s[0] != '/') {
        s = '/' + s;
    }
    return "file://" + s;
}

class NariLspServer {
  public:
    NariLspServer() = default;
    ~NariLspServer() {
        // Signal and join the background parse thread cleanly.
        {
            std::lock_guard<std::mutex> lk(parse_queue_mutex_);
            parse_thread_stop_ = true;
        }
        parse_queue_cv_.notify_all();
        if (parse_thread.joinable()) {
            parse_thread.join();
        }
        if (builtins_thread.joinable()) {
            builtins_thread.join();
        }
    }

    void run() {
        lsp_log("run: starting init_builtins on background thread");
        builtins_thread = std::thread([this]() {
            init_builtins();
            lsp_log("run: init_builtins thread finished");
        });
        // Start the dedicated background parse thread.
        parse_thread = std::thread([this]() { parse_thread_func(); });
        lsp_log("run: entering main loop");
        while (!this->exit) {
            std::string body = read_lsp_message();
            if (body.empty()) {
                if (std::cin.eof()) {
                    break;
                }
                continue;
            }
            json msg = json::parse(body, nullptr, false);
            if (msg.is_object()) {
                dispatch(msg);
            }
        }
    }

  private:
    bool shutdown = false;
    bool exit = false;
    std::unordered_map<std::string, DocState> docs_;
    // Protects docs_: request handlers take shared_lock; parse thread takes
    // unique_lock only for the brief commit step after parsing completes.
    std::shared_timed_mutex docs_mutex_;
    std::mutex builtins_mutex_;
    std::atomic<bool> builtins_ready_{ false };
    std::thread builtins_thread;
    std::map<std::string, SymInfo> builtin_syms_;
    std::map<std::string, std::vector<SymInfo>> stdlib_member_syms_;
    std::map<std::string, SymInfo> method_syms_;
    // Type-specific method maps: type_name -> {method_name -> SymInfo}
    // Keys are "regex", "string", "array", "number", "bool", etc.
    std::map<std::string, std::map<std::string, SymInfo>> method_type_syms_;
    std::string builtin_decls_uri_;
    std::string stdlib_decls_uri_;
    // Background parse queue: latest document content wins per URI.
    struct ParseRequest {
        std::string uri;
        std::string content;
        std::size_t hash;
    };
    std::vector<ParseRequest> parse_queue_;
    std::mutex parse_queue_mutex_;
    std::condition_variable parse_queue_cv_;
    bool parse_thread_stop_{ false };
    std::thread parse_thread;

    // write builtins.d.nari, parse it, build symbol map
    void init_builtins() {
#ifndef DISABLE_PARSER
        lsp_log("init_builtins: enter");
        try {
            std::filesystem::path dir = nari_data_dir();
            std::filesystem::create_directories(dir);
            std::filesystem::path file = dir / "builtins.d.nari";
            lsp_log("init_builtins: data dir = " + dir.string());

            std::string content = nari_embedded_builtins();
            lsp_log("init_builtins: embedded content size = " + std::to_string(content.size()));

            // Only rewrite if changed (avoids pointless churn).
            bool needs_write = true;
            if (std::filesystem::exists(file)) {
                std::ifstream ifs(file);
                std::string existing((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                needs_write = (existing != content);
            }
            if (needs_write) {
                std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
                ofs << content;
                lsp_log("init_builtins: wrote builtins.d.nari");
            } else {
                lsp_log("init_builtins: builtins.d.nari unchanged, skipping write");
            }

            std::string decls_uri = path_to_uri(file);

            // Build symbol table by scanning the text directly.
            // We intentionally avoid calling the parser here, its recovery
            // mode has pathological behaviour on the builtins declarations
            // file, causing unbounded memory growth.  A simple line scan
            // is perfectly sufficient for "func name(...) -> type {}" patterns.
            lsp_log("init_builtins: scanning builtins text for symbols");
            std::map<std::string, SymInfo> new_syms;
            std::map<std::string, SymInfo> new_method_syms;
            // type-group name -> {method_name -> SymInfo}
            std::map<std::string, std::map<std::string, SymInfo>> new_method_type_syms;
            std::string cur_method_type; // e.g. "regex", "string", "array"
            {
                std::istringstream ss(content);
                std::string line;
                int lineno = 0;
                bool in_methods = false; // set true after the dot-methods section marker
                std::string pending_comment;
                while (std::getline(ss, line)) {
                    ++lineno;
                    // Trim leading whitespace
                    size_t p0 = line.find_first_not_of(" \t");
                    if (p0 == std::string::npos) {
                        pending_comment.clear();
                        continue;
                    }
                    std::string_view lv(line.data() + p0, line.size() - p0);

                    // Section marker: switch to method map
                    if (lv.find("dot-methods") != std::string_view::npos) {
                        in_methods = true;
                        pending_comment.clear();
                        continue;
                    }

                    // Type-group marker inside dot-methods: // -- <type> --
                    if (in_methods && lv.size() >= 6 &&
                        lv[0] == '/' && lv[1] == '/' &&
                        lv.find("-- ") != std::string_view::npos) {
                        // Extract the word between "-- " and " --" (or end)
                        size_t dash_start = lv.find("-- ") + 3;
                        size_t dash_end = lv.find(" --", dash_start);
                        std::string type_word = dash_end != std::string_view::npos
                                                    ? std::string(lv.substr(dash_start, dash_end - dash_start))
                                                    : std::string(lv.substr(dash_start));
                        // Trim
                        auto &tw = type_word;
                        tw.erase(0, tw.find_first_not_of(" \t"));
                        tw.erase(tw.find_last_not_of(" \t") + 1);
                        cur_method_type = normalize_method_receiver_type(tw);
                        pending_comment.clear();
                        continue;
                    }

                    // Accumulate doc comment lines (///)
                    if (lv.size() >= 3 && lv[0] == '/' && lv[1] == '/' && lv[2] == '/') {
                        std::string_view text = lv.substr(3);
                        if (!text.empty() && text[0] == ' ') {
                            text = text.substr(1);
                        }
                        if (!pending_comment.empty()) {
                            pending_comment += '\n';
                        }
                        pending_comment += std::string(text);
                        continue;
                    }

                    if (lv.substr(0, 5) == "func ") {
                        // Extract: func NAME(...) -> RET {}
                        size_t name_start = 5;
                        size_t paren = lv.find('(', name_start);
                        if (paren == std::string_view::npos) {
                            pending_comment.clear();
                            continue;
                        }
                        std::string name(lv.substr(name_start, paren - name_start));
                        // Skip internal __double_underscore__ builtins, they are used by
                        // stdlib.nari but should not appear in user-facing completions/hover.
                        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') {
                            pending_comment.clear();
                            continue;
                        }

                        // Build a readable signature from the whole line
                        // Strip trailing " {}" or " {  }" to keep it clean
                        std::string sig(lv);
                        auto brace = sig.rfind('{');
                        if (brace != std::string::npos) {
                            while (brace > 0 && sig[brace - 1] == ' ') {
                                --brace;
                            }
                            sig = sig.substr(0, brace);
                        }
                        SymInfo si{ name, sig, CK_Function, lineno, (int)p0 + 1, "", pending_comment };
                        if (in_methods) {
                            new_method_syms[name] = si;
                            if (!cur_method_type.empty()) {
                                new_method_type_syms[cur_method_type][name] = si;
                            }
                        } else {
                            new_syms[name] = std::move(si);
                        }
                        pending_comment.clear();

                    } else if (lv.substr(0, 5) == "enum ") {
                        size_t name_end = lv.find_first_of(" \t{", 5);
                        if (name_end == std::string_view::npos) {
                            name_end = lv.size();
                        }
                        std::string name(lv.substr(5, name_end - 5));
                        new_syms[name] = { name, "enum " + name, CK_Enum, lineno, (int)p0 + 1, "", pending_comment };
                        pending_comment.clear();

                    } else if (lv.substr(0, 5) == "type ") {
                        size_t name_end = lv.find_first_of(" \t=<{", 5);
                        if (name_end == std::string_view::npos) {
                            name_end = lv.size();
                        }
                        std::string name(lv.substr(5, name_end - 5));
                        new_syms[name] = { name, std::string(lv), CK_Struct, lineno, (int)p0 + 1, "", pending_comment };
                        pending_comment.clear();

                    } else if (lv.substr(0, 6) == "class ") {
                        size_t name_end = lv.find_first_of(" \t{<", 6);
                        if (name_end == std::string_view::npos) {
                            name_end = lv.size();
                        }
                        std::string name(lv.substr(6, name_end - 6));
                        new_syms[name] = { name, "class " + name, CK_Class, lineno, (int)p0 + 1, "", pending_comment };
                        pending_comment.clear();

                    } else {
                        pending_comment.clear(); // non-comment, non-declaration resets the buffer
                    }
                }

                auto add_shared_method = [&](const std::string &method, std::initializer_list<const char *> types) {
                    auto mit = new_method_syms.find(method);
                    if (mit == new_method_syms.end()) {
                        return;
                    }
                    for (const char *type : types) {
                        new_method_type_syms[type][method] = mit->second;
                    }
                };
                add_shared_method("length", { "array", "string", "object" });
                add_shared_method("index_of", { "array", "string" });
                add_shared_method("last_index_of", { "array", "string" });
                add_shared_method("includes", { "array", "string" });
            }
            lsp_log("init_builtins: built " + std::to_string(new_syms.size()) + " builtin symbols");

            // -- Parse nari_embedded_stdlib() for global object members --------------
            {
                std::map<std::string, std::vector<SymInfo>> new_member_syms;
                std::string stdlib_text = nari_std_prelude_source();

                // Write stdlib.nari to disk so goto-def can navigate into it.
                std::filesystem::path stdlib_file = dir / "stdlib.nari";
                {
                    bool needs = true;
                    if (std::filesystem::exists(stdlib_file)) {
                        std::ifstream ifs(stdlib_file);
                        std::string ex((std::istreambuf_iterator<char>(ifs)),
                                       std::istreambuf_iterator<char>());
                        needs = (ex != stdlib_text);
                    }
                    if (needs) {
                        std::ofstream ofs(stdlib_file, std::ios::binary | std::ios::trunc);
                        ofs << stdlib_text;
                    }
                }
                std::string stdlib_fs_path = stdlib_file.string();

                auto is_ident_c = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
                auto is_ident_s = [](char c) { return std::isalpha((unsigned char)c) || c == '_'; };

                std::istringstream stdlib_ss(stdlib_text);
                std::string stdlib_ln;
                int stdlib_no = 0;
                std::string cur_obj;
                int cur_depth = 0;

                while (std::getline(stdlib_ss, stdlib_ln)) {
                    ++stdlib_no;
                    // ltrim
                    std::string_view sv(stdlib_ln);
                    while (!sv.empty() && (sv[0] == ' ' || sv[0] == '\t')) {
                        sv.remove_prefix(1);
                    }

                    if (cur_depth == 0) {
                        // Look for: global NAME = {
                        if (sv.size() > 7 && sv.substr(0, 7) == "global ") {
                            size_t p = 7;
                            if (p < sv.size() && is_ident_s(sv[p])) {
                                size_t ne = p + 1;
                                while (ne < sv.size() && is_ident_c(sv[ne])) {
                                    ne++;
                                }
                                std::string obj_name(sv.substr(p, ne - p));
                                size_t q = ne;
                                while (q < sv.size() && sv[q] == ' ') {
                                    q++;
                                }
                                if (q < sv.size() && sv[q] == '=') {
                                    q++;
                                    while (q < sv.size() && sv[q] == ' ') {
                                        q++;
                                    }
                                    if (q < sv.size() && sv[q] == '{') {
                                        cur_obj = obj_name;
                                        cur_depth = 1;
                                        // Register the object as a module-level symbol
                                        new_syms[obj_name] = { obj_name, obj_name, CK_Module,
                                                               stdlib_no, 1 };
                                        continue; // opening { already accounted for by depth=1
                                    }
                                }
                            }
                        }
                    } else {
                        // Inside an object literal.
                        // At depth==1 look for member definitions: ident ":" ...
                        if (cur_depth == 1 && !sv.empty() && is_ident_s(sv[0])) {
                            size_t ne = 1;
                            while (ne < sv.size() && is_ident_c(sv[ne])) {
                                ne++;
                            }
                            std::string mname(sv.substr(0, ne));
                            size_t q = ne;
                            while (q < sv.size() && sv[q] == ' ') {
                                q++;
                            }
                            if (q < sv.size() && sv[q] == ':') {
                                q++;
                                while (q < sv.size() && sv[q] == ' ') {
                                    q++;
                                }
                                std::string_view rest = sv.substr(q);
                                std::string detail;
                                int member_kind;
                                if (rest.size() >= 5 && rest.substr(0, 5) == "func(") {
                                    // func(arg1, arg2, ...): extract arg list
                                    size_t po = rest.find('(');
                                    size_t pc = rest.find(')');
                                    std::string args;
                                    if (po != std::string_view::npos && pc != std::string_view::npos && pc > po) {
                                        args = std::string(rest.substr(po + 1, pc - po - 1));
                                    }
                                    detail = "func " + mname + "(" + args + ")";
                                    member_kind = CK_Method;
                                } else {
                                    detail = mname;
                                    member_kind = CK_Property;
                                }
                                if (!(mname.size() >= 2 && mname[0] == '_' && mname[1] == '_')) {
                                    new_member_syms[cur_obj].push_back(
                                        { mname, detail, (LspCompletionKind)member_kind, stdlib_no, 1, stdlib_fs_path });
                                }
                            }
                        }
                        // Count braces to track depth
                        for (char c : stdlib_ln) {
                            if (c == '{') {
                                cur_depth++;
                            } else if (c == '}') {
                                cur_depth--;
                            }
                        }
                        if (cur_depth <= 0) {
                            cur_depth = 0;
                            cur_obj.clear();
                        }
                    }
                }

                std::lock_guard<std::mutex> lk(builtins_mutex_);
                builtin_syms_ = std::move(new_syms);
                builtin_decls_uri_ = std::move(decls_uri);
                stdlib_member_syms_ = std::move(new_member_syms);
                stdlib_decls_uri_ = path_to_uri(stdlib_file);
                method_syms_ = std::move(new_method_syms);
                method_type_syms_ = std::move(new_method_type_syms);
            }
            builtins_ready_.store(true, std::memory_order_release);
            lsp_log("init_builtins: done, builtins_ready = true");
        } catch (const std::exception &ex) {
            lsp_log(std::string("init_builtins: exception: ") + ex.what());
        } catch (...) {
            lsp_log("init_builtins: unknown exception");
        }
#endif
    }

    // ----------------------------------------------------------------
    //  Dispatch
    // ----------------------------------------------------------------
    void dispatch(const json &msg) {
        const std::string method = msg.value("method", std::string{});
        const json id = msg.contains("id") ? msg["id"] : json(nullptr);
        const json params = msg.contains("params") ? msg["params"] : json::object();

        lsp_log("dispatch: " + method);

        if (method == "initialize") {
            handle_initialize(id);
        } else if (method == "initialized") { /* no-op */
        } else if (method == "shutdown") {
            handle_shutdown(id);
        } else if (method == "exit") {
            exit = true;
        } else if (method == "textDocument/didOpen") {
            handle_did_open(params);
        } else if (method == "textDocument/didChange") {
            handle_did_change(params);
        } else if (method == "textDocument/didClose") {
            handle_did_close(params);
        } else if (method == "textDocument/completion") {
            handle_completion(id, params);
        } else if (method == "textDocument/hover") {
            handle_hover(id, params);
        } else if (method == "textDocument/definition") {
            handle_definition(id, params);
        } else if (method == "textDocument/references") {
            handle_references(id, params);
        } else if (method == "textDocument/typeDefinition") {
            handle_type_definition(id, params);
        } else if (method == "textDocument/codeAction") {
            handle_code_action(id, params);
        } else if (method == "textDocument/inlayHint") {
            handle_inlay_hints(id, params);
        } else if (method == "textDocument/semanticTokens/full") {
            handle_semantic_tokens_full(id, params);
        } else if (!id.is_null()) {
            send_error(id, -32601, "Method not found: " + method);
        }
    }

    void send_response(const json &id, json result) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = std::move(result);
        write_lsp_message(resp.dump());
    }

    void send_error(const json &id, int code, const std::string &msg) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["error"]["code"] = code;
        resp["error"]["message"] = msg;
        write_lsp_message(resp.dump());
    }

    void send_notification(const std::string &method, json params) {
        json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"] = method;
        notif["params"] = std::move(params);
        write_lsp_message(notif.dump());
    }

    struct ResolvedSymbol {
        SymInfo sym;
        std::string def_uri;
    };

    static bool same_symbol_identity(const SymInfo &a, const SymInfo &b) {
        if (a.kind != b.kind || a.name != b.name) {
            return false;
        }

        if (a.kind == CK_Field || a.kind == CK_Method) {
            return !a.owner_class.empty() && a.owner_class == b.owner_class;
        }

        if (a.kind == CK_Variable) {
            const bool a_is_param = !a.detail.empty() && a.detail.rfind("(parameter)", 0) == 0;
            const bool b_is_param = !b.detail.empty() && b.detail.rfind("(parameter)", 0) == 0;
            if (a_is_param != b_is_param) {
                return false;
            }

            if (a.scope_fn_line != 0 || b.scope_fn_line != 0) {
                return a.source_file == b.source_file && a.scope_fn_line == b.scope_fn_line;
            }

            return a.source_file == b.source_file && a.line == b.line;
        }

        return a.source_file == b.source_file && a.line == b.line;
    }

    bool resolve_symbol_at(const std::string &uri, int line, int col, ResolvedSymbol &out) {
        if (!docs_.count(uri)) {
            return false;
        }
        const DocState &doc = docs_[uri];

        int str_start = 0, str_end = 0;
        std::string spec = import_string_at(doc.content, line, col, str_start, str_end);
        if (!spec.empty()) {
            return false;
        }

        auto wp = word_at(doc.content, line, col);
        const std::string &word = wp.first;
        int word_start = wp.second;
        if (word.empty()) {
            return false;
        }

        std::string receiver = receiver_at(doc.content, line, word_start);

        const SymInfo *found = nullptr;
        SymInfo resolved_sym;
        std::string doc_fs_path = uri;
        if (doc_fs_path.rfind("file://", 0) == 0) {
            doc_fs_path = doc_fs_path.substr(7);
        }
        int cursor_line_1 = line + 1;

        if (word == "this") {
            std::string this_class = resolve_receiver_class_name(doc.symbols, doc.classes, "this", doc_fs_path, cursor_line_1);
            if (!this_class.empty()) {
                for (const auto &ci : doc.classes) {
                    if (ci.name != this_class) {
                        continue;
                    }
                    resolved_sym = { ci.name, "class " + ci.name, CK_Class, ci.line, ci.col, ci.source_file };
                    found = &resolved_sym;
                    break;
                }
            }
        }

        if (!receiver.empty()) {
            std::string module_source;
            for (const auto &sym : doc.symbols) {
                if (sym.name == receiver && sym.kind == CK_Module) {
                    module_source = sym.source_file;
                    break;
                }
            }
            if (!module_source.empty()) {
                for (const auto &sym : doc.symbols) {
                    if (sym.name == word && sym.source_file == module_source && sym.kind != CK_Variable) {
                        found = &sym;
                        break;
                    }
                }
            }
            if (!found) {
                std::string receiver_class = resolve_receiver_class_name(doc.symbols, doc.classes, receiver, doc_fs_path, cursor_line_1);
                if (!receiver_class.empty()) {
                    bool static_only = (receiver == receiver_class);
                    if (const MemberInfo *member = find_class_member(doc.classes, receiver_class, word, static_only)) {
                        resolved_sym = {
                            member->name,
                            member->detail,
                            member->is_method ? CK_Method : CK_Field,
                            member->line,
                            member->col,
                            member->source_file
                        };
                        resolved_sym.owner_class = receiver_class;
                        found = &resolved_sym;
                    }
                }
            }
        }

        if (!found) {
            found = find_symbol_in_scope(doc.symbols, word, doc_fs_path, cursor_line_1);
        }

        SymInfo stdlib_mem_sym;
        if (!found && !receiver.empty() && builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(builtins_mutex_);
            auto it = stdlib_member_syms_.find(receiver);
            if (it != stdlib_member_syms_.end()) {
                for (const auto &mem : it->second) {
                    if (mem.name == word) {
                        stdlib_mem_sym = mem;
                        found = &stdlib_mem_sym;
                        break;
                    }
                }
            }
            if (!found) {
                auto mit = method_syms_.find(word);
                if (mit != method_syms_.end()) {
                    stdlib_mem_sym = mit->second;
                    stdlib_mem_sym.source_file = "";
                    found = &stdlib_mem_sym;
                }
            }
        }

        SymInfo class_sym;
        if (!found) {
            for (const auto &ci : doc.classes) {
                if (ci.name == word) {
                    class_sym = { ci.name, "class " + ci.name, CK_Class, ci.line, ci.col, ci.source_file };
                    found = &class_sym;
                    break;
                }
            }
        }

        SymInfo builtin_sym;
        if (!found && builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(builtins_mutex_);
            if (!builtin_decls_uri_.empty()) {
                auto it = builtin_syms_.find(word);
                if (it != builtin_syms_.end()) {
                    builtin_sym = it->second;
                    found = &builtin_sym;
                }
            }
        }

        if (!found) {
            return false;
        }

        out.sym = *found;
        if (&builtin_sym == found) {
            out.def_uri = builtin_decls_uri_;
        } else if (found == &stdlib_mem_sym && found->source_file.empty()) {
            out.def_uri = builtin_decls_uri_;
        } else if (!found->source_file.empty()) {
            out.def_uri = path_to_uri(std::filesystem::path(found->source_file));
        } else {
            out.def_uri = uri;
        }
        return true;
    }

    template <typename Fn>
    static void for_each_identifier_occurrence(const std::string &content, Fn &&fn) {
        const auto is_ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        std::istringstream ss(content);
        std::string line;
        int line_no = 0;
        while (std::getline(ss, line)) {
            bool in_double = false;
            bool in_single = false;
            for (int i = 0; i < (int)line.size();) {
                if (!in_single && !in_double && i + 1 < (int)line.size() && line[i] == '/' && line[i + 1] == '/') {
                    break;
                }
                if (!in_single && line[i] == '"') {
                    in_double = !in_double;
                    ++i;
                    continue;
                }
                if (!in_double && line[i] == '\'') {
                    in_single = !in_single;
                    ++i;
                    continue;
                }
                if (in_single || in_double) {
                    if (line[i] == '\\' && i + 1 < (int)line.size()) {
                        i += 2;
                    } else {
                        ++i;
                    }
                    continue;
                }
                if (!std::isalpha((unsigned char)line[i]) && line[i] != '_') {
                    ++i;
                    continue;
                }
                int start = i++;
                while (i < (int)line.size() && is_ident(line[i])) {
                    ++i;
                }
                fn(line_no, start, std::string_view(line.data() + start, i - start));
            }
            ++line_no;
        }
    }

    //  initialize
    void handle_initialize(const json &id) {
        json result;
        result["capabilities"]["textDocumentSync"] = 1;
        result["capabilities"]["completionProvider"]["triggerCharacters"] = json::array({ "." });
        result["capabilities"]["completionProvider"]["resolveProvider"] = false;
        result["capabilities"]["hoverProvider"] = true;
        result["capabilities"]["definitionProvider"] = true;
        result["capabilities"]["referencesProvider"] = true;
        result["capabilities"]["typeDefinitionProvider"] = true;
        // Code actions: quickfix (unused vars) + refactor (wrap in try/catch)
        result["capabilities"]["codeActionProvider"]["codeActionKinds"] = json::array({ "quickfix", "refactor" });
        result["capabilities"]["codeActionProvider"]["resolveProvider"] = false;
        // Inlay hints (variable type annotations)
        result["capabilities"]["inlayHintProvider"] = true;
        // Semantic tokens (full document)
        {
            json legend;
            legend["tokenTypes"] = json::array({ "namespace", "type", "class", "enum", "interface", "struct",
                                                 "typeParameter", "parameter", "variable", "property",
                                                 "enumMember", "event", "function", "method", "macro",
                                                 "keyword", "modifier", "comment", "string", "number",
                                                 "regexp", "operator" });
            legend["tokenModifiers"] = json::array({ "declaration", "definition", "readonly", "static",
                                                     "deprecated", "abstract", "async", "modification",
                                                     "documentation", "defaultLibrary" });
            result["capabilities"]["semanticTokensProvider"]["legend"] = std::move(legend);
            result["capabilities"]["semanticTokensProvider"]["full"] = true;
        }
        result["serverInfo"]["name"] = "nari-lsp";
        result["serverInfo"]["version"] = "0.1.0";
        send_response(id, std::move(result));
    }

    void handle_shutdown(const json &id) {
        shutdown = true;
        send_response(id, nullptr);
    }

    //  textDocument/didOpen
    void handle_did_open(const json &params) {
        const auto &_td0 = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = _td0.value("uri", std::string{});
        std::string content = _td0.value("text", std::string{});
        if (uri.empty()) {
            return;
        }

        const std::size_t h = std::hash<std::string>{}(content);
        {
            std::unique_lock<std::shared_timed_mutex> lk(docs_mutex_);
            DocState &doc = docs_[uri];
            doc.content = content;
            doc.content_hash = h;
            doc.parsed_hash = 0;
            doc.symbols.clear();
            doc.classes.clear();
            doc.object_literals.clear();
        }
        enqueue_parse(uri, content, h);
    }

    //  textDocument/didChange
    void handle_did_change(const json &params) {
        const auto &_td1 = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = _td1.value("uri", std::string{});
        if (uri.empty()) {
            return;
        }
        if (!params.contains("contentChanges") || !params["contentChanges"].is_array() || params["contentChanges"].empty()) {
            return;
        }
        std::string content = params["contentChanges"].back().value("text", std::string{});
        // Skip the expensive re-parse if content is identical to what we already have.
        std::size_t h = std::hash<std::string>{}(content);
        {
            std::unique_lock<std::shared_timed_mutex> lk(docs_mutex_);
            DocState &doc = docs_[uri];
            if (doc.content_hash == h) {
                lsp_log("didChange: unchanged " + uri);
                return;
            }
            doc.content = content;
            doc.content_hash = h;
        }
        enqueue_parse(uri, content, h);
    }

    //  textDocument/didClose
    void handle_did_close(const json &params) {
        const auto &_td2 = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = _td2.value("uri", std::string{});
        if (uri.empty()) {
            return;
        }
        {
            std::unique_lock<std::shared_timed_mutex> lk(docs_mutex_);
            docs_.erase(uri);
        }
        json p;
        p["uri"] = uri;
        p["diagnostics"] = json::array();
        send_notification("textDocument/publishDiagnostics", std::move(p));
    }

    //  textDocument/completion
    void handle_completion(const json &id, const json &params) {
        const auto &_td3 = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &_pos3 = params.contains("position") ? params["position"] : json{};
        std::string uri = _td3.value("uri", std::string{});
        int line = _pos3.value("line", 0);
        int col = _pos3.value("character", 0);
        // Hold a shared lock while reading docs_ (parse thread may be committing).
        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);

        lsp_log("completion request: uri=" + uri + " line=" + std::to_string(line) + " col=" + std::to_string(col) + " doc_known=" + (docs_.count(uri) ? "yes" : "no") + " symbols=" + (docs_.count(uri) ? std::to_string(docs_[uri].symbols.size()) : "n/a"));

        // Determine prefix at cursor position
        std::string prefix;
        int word_start_col = col;
        if (docs_.count(uri)) {
            auto wp = word_at(docs_[uri].content, line, col);
            prefix = wp.first;
            word_start_col = wp.second;
        }

        lsp_log("completion prefix: '" + prefix + "'");

        json items = json::array();

        // -- Dot-completion: detect "receiver.prefix" -----------------------------
        // Get the source line so we can inspect the character before the word.
        std::string src_line;
        if (docs_.count(uri)) {
            std::istringstream _ss(docs_[uri].content);
            std::string _ln;
            int _cur = 0;
            while (std::getline(_ss, _ln)) {
                if (_cur == line) {
                    src_line = _ln;
                    break;
                }
                _cur++;
            }
        }
        std::string receiver;
        {
            int dot_pos = word_start_col - 1;
            if (dot_pos >= 0 && dot_pos < (int)src_line.size() && src_line[dot_pos] == '.') {
                int re = dot_pos;
                int rs = re;
                while (rs > 0 && (std::isalnum((unsigned char)src_line[rs - 1]) || src_line[rs - 1] == '_')) {
                    rs--;
                }
                if (rs < re) {
                    receiver = src_line.substr(rs, re - rs);
                }
            }
        }
        if (!receiver.empty()) {
            if (docs_.count(uri)) {
                std::string doc_fs_path = uri;
                if (doc_fs_path.rfind("file://", 0) == 0) {
                    doc_fs_path = doc_fs_path.substr(7);
                }
                const int cursor_line_1 = line + 1;
                if (const ObjectLiteralInfo *info = find_object_literal_info(
                        docs_[uri].object_literals, docs_[uri].symbols, receiver, doc_fs_path, cursor_line_1)) {
                    for (const auto &mem : info->members) {
                        if (!prefix.empty() && (mem.name.size() < prefix.size() || mem.name.substr(0, prefix.size()) != prefix)) {
                            continue;
                        }
                        json item;
                        item["label"] = mem.name;
                        item["detail"] = mem.detail;
                        item["kind"] = mem.kind;
                        items.push_back(std::move(item));
                    }
                    json result;
                    result["isIncomplete"] = false;
                    result["items"] = std::move(items);
                    send_response(id, std::move(result));
                    return;
                }

                std::string receiver_class = resolve_receiver_class_name(
                    docs_[uri].symbols, docs_[uri].classes, receiver, doc_fs_path, cursor_line_1);
                if (!receiver_class.empty()) {
                    bool static_only = (receiver == receiver_class);
                    for (const auto &ci : docs_[uri].classes) {
                        if (ci.name != receiver_class) {
                            continue;
                        }
                        for (const auto &member : ci.members) {
                            if (static_only && !member.is_static) {
                                continue;
                            }
                            if (!static_only && member.is_static) {
                                continue;
                            }
                            if (!prefix.empty() && (member.name.size() < prefix.size() || member.name.substr(0, prefix.size()) != prefix)) {
                                continue;
                            }
                            json item;
                            item["label"] = member.name;
                            item["detail"] = member.detail;
                            item["kind"] = member.is_method ? (int)CK_Method : (int)CK_Field;
                            items.push_back(std::move(item));
                        }
                        json result;
                        result["isIncomplete"] = false;
                        result["items"] = std::move(items);
                        send_response(id, std::move(result));
                        return;
                    }
                }
            }

            if (!builtins_ready_.load(std::memory_order_acquire)) {
                json result;
                result["isIncomplete"] = false;
                result["items"] = std::move(items);
                send_response(id, std::move(result));
                return;
            }

            std::lock_guard<std::mutex> lk(builtins_mutex_);
            auto it = stdlib_member_syms_.find(receiver);
            if (it != stdlib_member_syms_.end()) {
                for (const auto &mem : it->second) {
                    if (!prefix.empty() && (mem.name.size() < prefix.size() || mem.name.substr(0, prefix.size()) != prefix)) {
                        continue;
                    }
                    json item;
                    item["label"] = mem.name;
                    item["detail"] = mem.detail;
                    item["kind"] = mem.kind;
                    items.push_back(std::move(item));
                }
            } else {
                // Try to resolve the receiver to a typed local variable so we
                // can offer type-specific methods instead of every dot-method.
                std::string resolved_type;
                if (docs_.count(uri)) {
                    for (const auto &sym : docs_[uri].symbols) {
                        if (sym.name == receiver && !sym.inferred_type.empty()) {
                            resolved_type = sym.inferred_type;
                            break;
                        }
                    }
                }

                // Regex literals typed inline (e.g. /foo/.| -> receiver based on
                // the literal text) won't match a symbol; detect the common case
                // where the source token before the dot is 'TK_REGEX'-shaped.
                // (Leave resolved_type empty; we handle it below via a special path.)

                // If we have a known type, serve only that type's methods from
                // the type-specific map.  Fall back to the full method_syms_ list
                // for unknown types so we never show nothing.
                bool served_from_type = false;
                if (!resolved_type.empty()) {
                    auto tit = method_type_syms_.find(normalize_method_receiver_type(resolved_type));
                    if (tit != method_type_syms_.end()) {
                        served_from_type = true;
                        for (const auto &[mname, msym] : tit->second) {
                            if (!prefix.empty() && (mname.size() < prefix.size() || mname.substr(0, prefix.size()) != prefix)) {
                                continue;
                            }
                            json item;
                            item["label"] = mname;
                            item["detail"] = msym.detail;
                            item["kind"] = (int)CK_Method;
                            items.push_back(std::move(item));
                        }
                    }
                }
                if (!served_from_type) {
                    for (const auto &[mname, msym] : method_syms_) {
                        if (!prefix.empty() && (mname.size() < prefix.size() || mname.substr(0, prefix.size()) != prefix)) {
                            continue;
                        }
                        json item;
                        item["label"] = mname;
                        item["detail"] = msym.detail;
                        item["kind"] = (int)CK_Method;
                        items.push_back(std::move(item));
                    }
                }
            }
            json result;
            result["isIncomplete"] = false;
            result["items"] = std::move(items);
            send_response(id, std::move(result));
            return;
        }

        auto prefix_matches = [&](const std::string &name) {
            if (prefix.empty()) {
                return true;
            }
            return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
        };

        // Keywords
        for (int i = 0; kNariKeywords[i]; ++i) {
            std::string kw = kNariKeywords[i];
            if (!prefix_matches(kw)) {
                continue;
            }
            json item;
            item["label"] = kw;
            item["kind"] = (int)CK_Keyword;
            items.push_back(std::move(item));
        }

        // built-ins: prefer parsed declarations; fall back to static table
        if (builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(builtins_mutex_);
            for (const auto &[name, sym] : builtin_syms_) {
                if (!prefix_matches(name)) {
                    continue;
                }
                json item;
                item["label"] = name;
                item["detail"] = sym.detail;
                item["kind"] = sym.kind;
                items.push_back(std::move(item));
            }
        } else {
            for (int i = 0; kNariBuiltins[i].name; ++i) {
                std::string name = kNariBuiltins[i].name;
                if (!prefix_matches(name)) {
                    continue;
                }
                json item;
                item["label"] = name;
                item["detail"] = kNariBuiltins[i].detail;
                item["kind"] = (int)CK_Function;
                items.push_back(std::move(item));
            }
        }

        // Document symbols
        if (docs_.count(uri)) {
            for (const auto &sym : docs_[uri].symbols) {
                if (!prefix_matches(sym.name)) {
                    continue;
                }
                json item;
                item["label"] = sym.name;
                item["detail"] = sym.detail;
                item["kind"] = sym.kind;
                items.push_back(std::move(item));
            }
            for (const auto &ci : docs_[uri].classes) {
                if (!prefix_matches(ci.name)) {
                    continue;
                }
                json item;
                item["label"] = ci.name;
                item["kind"] = int64_t(CK_Class);
                items.push_back(std::move(item));
            }
        }

        json result;
        result["isIncomplete"] = false;
        result["items"] = std::move(items);
        send_response(id, std::move(result));
    }

    //  textDocument/hover
    void handle_hover(const json &id, const json &params) {
        const auto &_td3 = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &_pos3 = params.contains("position") ? params["position"] : json{};
        std::string uri = _td3.value("uri", std::string{});
        int line = _pos3.value("line", 0);
        int col = _pos3.value("character", 0);
        // Hold a shared lock while reading docs_ (parse thread may be committing).
        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);

        if (!docs_.count(uri)) {
            send_response(id, nullptr);
            return;
        }

        // Check if cursor is on a string literal in an import statement
        {
            int str_start = 0, str_end = 0;
            std::string spec = import_string_at(docs_[uri].content, line, col, str_start, str_end);
            if (!spec.empty()) {
                // Derive filesystem basefile from URI
                std::string basefile = uri;
                if (basefile.rfind("file://", 0) == 0) {
                    basefile = basefile.substr(7);
                }
#ifndef DISABLE_PARSER
                std::string resolved = Parser::resolve_import_path(spec, basefile);
#else
                std::string resolved;
#endif
                std::string hover_text = resolved.empty()
                                             ? "Cannot resolve \"" + spec + "\""
                                             : resolved;

                json sp, ep;
                sp["line"] = int64_t(line);
                sp["character"] = int64_t(str_start);
                ep["line"] = int64_t(line);
                ep["character"] = int64_t(str_end + 1);
                json rng;
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                json contents;
                contents["kind"] = "markdown";
                contents["value"] = "`" + hover_text + "`";
                json result;
                result["contents"] = std::move(contents);
                result["range"] = std::move(rng);
                send_response(id, std::move(result));
                return;
            }
        }

        auto wp = word_at(docs_[uri].content, line, col);
        const std::string &word = wp.first;
        int word_start = wp.second;
        if (word.empty()) {
            send_response(id, nullptr);
            return;
        }

        std::string detail;
        std::string comment;

        // search in document symbols, keep a pointer so we can pull doc_comment
        const SymInfo *found_sym = nullptr;

        // Dot-access receiver: check for "receiver.word" pattern (e.g. math.add)
        {
            std::string receiver = receiver_at(docs_[uri].content, line, word_start);
            if (!receiver.empty()) {
                std::string doc_fs_path_h = uri;
                if (doc_fs_path_h.rfind("file://", 0) == 0) {
                    doc_fs_path_h = doc_fs_path_h.substr(7);
                }
                int cursor_line_1h = line + 1;
                std::string module_source;
                for (const auto &sym : docs_[uri].symbols) {
                    if (sym.name == receiver && sym.kind == CK_Module) {
                        module_source = sym.source_file;
                        break;
                    }
                }
                if (!module_source.empty()) {
                    for (const auto &sym : docs_[uri].symbols) {
                        if (sym.name == word && sym.source_file == module_source && sym.kind != CK_Variable) {
                            found_sym = &sym;
                            break;
                        }
                    }
                }
                if (!found_sym) {
                    std::string receiver_class = resolve_receiver_class_name(
                        docs_[uri].symbols, docs_[uri].classes, receiver, doc_fs_path_h, cursor_line_1h);
                    if (!receiver_class.empty()) {
                        bool static_only = (receiver == receiver_class);
                        if (const MemberInfo *member = find_class_member(docs_[uri].classes, receiver_class, word, static_only)) {
                            detail = member->detail;
                        }
                    }
                }
                // unknown receiver (local var, array, string), check stdlib object members then dot-methods
                if (!found_sym && detail.empty() && builtins_ready_.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lk(builtins_mutex_);
                    auto sit = stdlib_member_syms_.find(receiver);
                    if (sit != stdlib_member_syms_.end()) {
                        for (const auto &mem : sit->second) {
                            if (mem.name == word) {
                                detail = mem.detail;
                                comment = mem.doc_comment;
                                break;
                            }
                        }
                    }
                    if (detail.empty()) {
                        auto mit = method_syms_.find(word);
                        if (mit != method_syms_.end()) {
                            detail = mit->second.detail;
                            comment = mit->second.doc_comment;
                        }
                    }
                }
            }
        }

        // direct search in document symbols, scope-aware for locals/params
        if (!found_sym) {
            std::string doc_fs_path_h = uri;
            if (doc_fs_path_h.rfind("file://", 0) == 0) {
                doc_fs_path_h = doc_fs_path_h.substr(7);
            }
            int cursor_line_1h = line + 1;
            found_sym = find_symbol_in_scope(docs_[uri].symbols, word, doc_fs_path_h, cursor_line_1h);
        }
        if (found_sym) {
            detail = found_sym->detail;
            comment = found_sym->doc_comment;
            // If no pre-stored comment, scan backward in the document text
            if (comment.empty() && found_sym->line > 0) {
                comment = extract_doc_comment(docs_[uri].content, found_sym->line);
            }
        }

        // Search in class names / members
        if (detail.empty()) {
            for (const auto &ci : docs_[uri].classes) {
                if (ci.name == word) {
                    detail = "class " + ci.name;
                    break;
                }
                for (const auto &m : ci.members) {
                    if (m.name == word) {
                        detail = m.detail;
                        break;
                    }
                }
                if (!detail.empty()) {
                    break;
                }
            }
        }

        // Search built-ins (parsed declarations take precedence over the static table)
        if (detail.empty()) {
            if (builtins_ready_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(builtins_mutex_);
                auto it = builtin_syms_.find(word);
                if (it != builtin_syms_.end()) {
                    detail = it->second.detail;
                    comment = it->second.doc_comment;
                }
            }
            if (detail.empty()) {
                for (int i = 0; kNariBuiltins[i].name; ++i) {
                    if (word == kNariBuiltins[i].name) {
                        detail = kNariBuiltins[i].detail;
                        break;
                    }
                }
            }
        }

        if (detail.empty()) {
            send_response(id, nullptr);
            return;
        }

        // Build the range for the hovered word
        json start_pos, end_pos;
        start_pos["line"] = int64_t(line);
        start_pos["character"] = int64_t(word_start);
        end_pos["line"] = int64_t(line);
        end_pos["character"] = int64_t(word_start + (int)word.size());
        json range;
        range["start"] = std::move(start_pos);
        range["end"] = std::move(end_pos);

        // Markdown: doc comment (if any) above the code block
        std::string hover_md;
        if (!comment.empty()) {
            hover_md = comment + "\n\n";
        }
        hover_md += "```nari\n" + detail + "\n```";

        json contents;
        contents["kind"] = "markdown";
        contents["value"] = std::move(hover_md);

        json result;
        result["contents"] = std::move(contents);
        result["range"] = std::move(range);
        send_response(id, std::move(result));
    }

    //  textDocument/definition
    void handle_definition(const json &id, const json &params) {
        const auto &_td3 = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &_pos3 = params.contains("position") ? params["position"] : json{};
        std::string uri = _td3.value("uri", std::string{});
        int line = _pos3.value("line", 0);
        int col = _pos3.value("character", 0);
        // Hold a shared lock while reading docs_ (parse thread may be committing).
        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);

        if (!docs_.count(uri)) {
            send_response(id, nullptr);
            return;
        }

        // Ctrl+click on a string literal in an import -> open the file directly
        {
            int str_start = 0, str_end = 0;
            std::string spec = import_string_at(docs_[uri].content, line, col, str_start, str_end);
            if (!spec.empty()) {
                std::string basefile = uri;
                if (basefile.rfind("file://", 0) == 0) {
                    basefile = basefile.substr(7);
                }
#ifndef DISABLE_PARSER
                std::string resolved = Parser::resolve_import_path(spec, basefile);
#else
                std::string resolved;
#endif
                if (resolved.empty()) {
                    send_response(id, nullptr);
                    return;
                }

                json sp, ep;
                sp["line"] = int64_t(0);
                sp["character"] = int64_t(0);
                ep["line"] = int64_t(0);
                ep["character"] = int64_t(0);
                json rng;
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                json location;
                location["uri"] = path_to_uri(std::filesystem::path(resolved));
                location["range"] = std::move(rng);
                send_response(id, std::move(location));
                return;
            }
        }

        ResolvedSymbol resolved;
        if (!resolve_symbol_at(uri, line, col, resolved)) {
            send_response(id, nullptr);
            return;
        }

        // Convert 1-based parser positions to 0-based LSP positions
        int lsp_line = std::max(0, resolved.sym.line - 1);
        int lsp_col = std::max(0, resolved.sym.col - 1);

        std::string word = word_at(docs_[uri].content, line, col).first;
        if (word.empty()) {
            word = resolved.sym.name;
        }

        json start_pos, end_pos;
        start_pos["line"] = int64_t(lsp_line);
        start_pos["character"] = int64_t(lsp_col);
        end_pos["line"] = int64_t(lsp_line);
        end_pos["character"] = int64_t(lsp_col + (int)word.size());
        json def_range;
        def_range["start"] = std::move(start_pos);
        def_range["end"] = std::move(end_pos);

        json location;
        location["uri"] = resolved.def_uri;
        location["range"] = std::move(def_range);
        send_response(id, std::move(location));
    }

    //  textDocument/references
    //  Open-document references only for now; uses the same resolver as go-to-definition
    void handle_references(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &pos = params.contains("position") ? params["position"] : json{};
        const auto &ctx = params.contains("context") ? params["context"] : json{};
        std::string uri = td.value("uri", std::string{});
        int line = pos.value("line", 0);
        int col = pos.value("character", 0);
        const bool include_decl = ctx.value("includeDeclaration", true);

        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);
        if (!docs_.count(uri)) {
            send_response(id, json::array());
            return;
        }

        ResolvedSymbol target;
        if (!resolve_symbol_at(uri, line, col, target)) {
            send_response(id, json::array());
            return;
        }

        json locations = json::array();
        std::set<std::tuple<std::string, int, int>> seen;

        for (const auto &entry : docs_) {
            const std::string &doc_uri = entry.first;
            const auto &doc = entry.second;
            for_each_identifier_occurrence(doc.content, [&](int occ_line, int occ_col, std::string_view ident) {
                if (ident != target.sym.name) {
                    return;
                }

                ResolvedSymbol candidate;
                if (!resolve_symbol_at(doc_uri, occ_line, occ_col, candidate)) {
                    return;
                }
                if (!same_symbol_identity(candidate.sym, target.sym)) {
                    return;
                }

                if (!include_decl &&
                    candidate.def_uri == doc_uri &&
                    occ_line == std::max(0, candidate.sym.line - 1) &&
                    occ_col == std::max(0, candidate.sym.col - 1)) {
                    return;
                }

                if (!seen.insert({ doc_uri, occ_line, occ_col }).second) {
                    return;
                }

                json sp, ep, rng, loc;
                sp["line"] = int64_t(occ_line);
                sp["character"] = int64_t(occ_col);
                ep["line"] = int64_t(occ_line);
                ep["character"] = int64_t(occ_col + (int)ident.size());
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                loc["uri"] = doc_uri;
                loc["range"] = std::move(rng);
                locations.push_back(std::move(loc));
            });
        }

        send_response(id, std::move(locations));
    }

    //  textDocument/codeAction
    void handle_code_action(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &range = params.contains("range") ? params["range"] : json{};
        const auto &context = params.contains("context") ? params["context"] : json{};
        std::string uri = td.value("uri", std::string{});

        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);
        if (!docs_.count(uri)) {
            send_response(id, json::array());
            return;
        }
        const std::string content = docs_[uri].content; // copy for use below the lock (lock released at scope end)

        json actions = json::array();

        // quick-fix: remove unused variable
        if (context.contains("diagnostics") && context["diagnostics"].is_array()) {
            std::vector<std::string> lines = lsp_split_lines(content);
            for (const auto &diag : context["diagnostics"]) {
                const std::string msg = diag.value("message", std::string{});
                const std::string pfx = "' is declared but never used";
                const size_t quote1 = msg.find('\'');
                const size_t quote2 = msg.find('\'', quote1 + 1);
                if (quote1 == std::string::npos || quote2 == std::string::npos) {
                    continue;
                }
                if (msg.find(pfx) == std::string::npos) {
                    continue;
                }
                const std::string var_name = msg.substr(quote1 + 1, quote2 - quote1 - 1);
                if (var_name.empty()) {
                    continue;
                }

                const auto &d_rng = diag.contains("range") ? diag["range"] : json{};
                const auto &d_start = d_rng.contains("start") ? d_rng["start"] : json{};
                const int diag_line = d_start.value("line", -1);
                const int diag_col = d_start.value("character", 0);
                if (diag_line < 0 || diag_line >= (int)lines.size()) {
                    continue;
                }

                const std::string &src_line = lines[diag_line];
                std::string trimmed = src_line;
                const size_t trim_off = trimmed.find_first_not_of(" \t");
                if (trim_off != std::string::npos) {
                    trimmed = trimmed.substr(trim_off);
                } else {
                    trimmed.clear();
                }

                // Is this a standalone let/global declaration (safe to delete the whole line)?
                const bool is_let_line = trimmed.rfind("let ", 0) == 0 || trimmed.rfind("global ", 0) == 0;
                const bool has_no_comma = trimmed.find(',') == std::string::npos;
                const bool is_standalone = is_let_line && has_no_comma;

                if (is_standalone) {
                    // Delete the whole line (range from {line,0} to {line+1,0})
                    const int next_line = (diag_line + 1 < (int)lines.size()) ? diag_line + 1 : diag_line;
                    const int end_char = (next_line == diag_line) ? (int)src_line.size() : 0;
                    json del_s, del_e, del_rng, del_edit, changes;
                    del_s["line"] = int64_t(diag_line);
                    del_s["character"] = int64_t(0);
                    del_e["line"] = int64_t(next_line);
                    del_e["character"] = int64_t(end_char);
                    del_rng["start"] = std::move(del_s);
                    del_rng["end"] = std::move(del_e);
                    del_edit["range"] = std::move(del_rng);
                    del_edit["newText"] = "";
                    changes[uri] = json::array({ std::move(del_edit) });
                    json action;
                    action["title"] = "Remove unused variable '" + var_name + "'";
                    action["kind"] = "quickfix";
                    action["diagnostics"] = json::array({ diag });
                    action["edit"]["changes"] = std::move(changes);
                    action["isPreferred"] = true;
                    actions.push_back(std::move(action));
                }

                // always offer: rename to _name (suppress warning without removing code)
                {
                    json es, ee, rename_rng, rename_edit, changes;
                    es["line"] = int64_t(diag_line);
                    es["character"] = int64_t(diag_col);
                    ee["line"] = int64_t(diag_line);
                    ee["character"] = int64_t(diag_col + (int)var_name.size());
                    rename_rng["start"] = std::move(es);
                    rename_rng["end"] = std::move(ee);
                    rename_edit["range"] = std::move(rename_rng);
                    rename_edit["newText"] = "_" + var_name;
                    changes[uri] = json::array({ std::move(rename_edit) });
                    json action;
                    action["title"] = "Suppress: rename '" + var_name + "' to '_" + var_name + "'";
                    action["kind"] = "quickfix";
                    action["diagnostics"] = json::array({ diag });
                    action["edit"]["changes"] = std::move(changes);
                    actions.push_back(std::move(action));
                }
            }
        }

        // refactor: wrap selection in try / catch
        const auto &r_start = range.contains("start") ? range["start"] : json{};
        const auto &r_end = range.contains("end") ? range["end"] : json{};
        const int sel_sl = r_start.value("line", 0);
        const int sel_el = r_end.value("line", 0);
        const int sel_sc = r_start.value("character", 0);
        const int sel_ec = r_end.value("character", 0);
        if (sel_sl != sel_el || sel_sc != sel_ec) {
            std::vector<std::string> lines = lsp_split_lines(content);
            const int start_ln = std::max(0, std::min(sel_sl, (int)lines.size() - 1));
            const int end_ln = std::max(0, std::min(sel_el, (int)lines.size() - 1));

            // Detect leading indent of first selected line
            std::string indent;
            if (start_ln < (int)lines.size()) {
                for (char c : lines[start_ln]) {
                    if (c == ' ' || c == '\t') {
                        indent += c;
                    } else {
                        break;
                    }
                }
            }
            const std::string inner = indent + "    ";

            std::string try_text = indent + "try {\n";
            for (int ln = start_ln; ln <= end_ln; ++ln) {
                if (ln >= (int)lines.size()) {
                    break;
                }
                // Strip leading whitespace and re-indent with inner
                size_t p = 0;
                while (p < lines[ln].size() && (lines[ln][p] == ' ' || lines[ln][p] == '\t')) {
                    ++p;
                }
                try_text += inner + lines[ln].substr(p) + "\n";
            }
            try_text += indent + "} catch (err) {\n";
            try_text += indent + "    \n";
            try_text += indent + "}";

            const int wrap_end_ln = (end_ln + 1 < (int)lines.size()) ? end_ln + 1 : end_ln;
            const int wrap_end_ch = (wrap_end_ln == end_ln) ? (int)lines[end_ln].size() : 0;
            json ws, we, wrap_rng, wrap_edit, changes;
            ws["line"] = int64_t(start_ln);
            ws["character"] = int64_t(0);
            we["line"] = int64_t(wrap_end_ln);
            we["character"] = int64_t(wrap_end_ch);
            wrap_rng["start"] = std::move(ws);
            wrap_rng["end"] = std::move(we);
            wrap_edit["range"] = std::move(wrap_rng);
            wrap_edit["newText"] = std::move(try_text);
            changes[uri] = json::array({ std::move(wrap_edit) });
            json action;
            action["title"] = "Wrap in try / catch";
            action["kind"] = "refactor";
            action["edit"]["changes"] = std::move(changes);
            actions.push_back(std::move(action));
        }

        send_response(id, std::move(actions));
    }

    //  textDocument/inlayHint
    void handle_inlay_hints(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &rng = params.contains("range") ? params["range"] : json{};
        std::string uri = td.value("uri", std::string{});

        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);
        if (!docs_.count(uri)) {
            send_response(id, json::array());
            return;
        }
        const DocState &doc = docs_[uri];

        const int range_start = rng.contains("start") ? rng["start"].value("line", 0) : 0;
        const int range_end = rng.contains("end") ? rng["end"].value("line", 0x7fffffff) : 0x7fffffff;

        const std::vector<std::string> lines = lsp_split_lines(doc.content);
        json hints = json::array();

        for (const auto &sym : doc.symbols) {
            // Only annotate variable declarations that have a known inferred type
            if (sym.kind != CK_Variable) {
                continue;
            }
            if (sym.inferred_type.empty()) {
                continue;
            }
            // skip parameters, they already display types in hover/signature
            if (!sym.detail.empty() && sym.detail.rfind("(parameter)", 0) == 0) {
                continue;
            }

            const int lsp_line = sym.line - 1;
            if (lsp_line < range_start || lsp_line > range_end) {
                continue;
            }
            if (lsp_line < 0 || lsp_line >= (int)lines.size()) {
                continue;
            }

            // Find the variable identifier position on this line (sym.col points to `let`)
            const size_t search_from = (sym.col > 0) ? (size_t)(sym.col - 1) : 0u;
            const int found_col1 = lsp_find_word_col(lines[lsp_line], sym.name, search_from);
            if (found_col1 < 0) {
                continue;
            }

            // Hint position: right after the variable name
            const int hint_col = (found_col1 - 1) + (int)sym.name.size(); // 0-based
            json hint;
            hint["position"]["line"] = int64_t(lsp_line);
            hint["position"]["character"] = int64_t(hint_col);
            hint["label"] = ": " + sym.inferred_type;
            hint["kind"] = int64_t(1); // InlayHintKind.Type = 1
            hint["paddingLeft"] = true;
            hints.push_back(std::move(hint));
        }

        send_response(id, std::move(hints));
    }

    //  textDocument/semanticTokens/full
    void handle_semantic_tokens_full(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        std::string uri = td.value("uri", std::string{});

        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);
        if (!docs_.count(uri)) {
            json r;
            r["data"] = json::array();
            send_response(id, std::move(r));
            return;
        }
        const DocState &doc = docs_[uri];
        const std::vector<std::string> lines = lsp_split_lines(doc.content);

        // build name -> entry map
        struct NameEntry {
            int token_type;
            int priority;
            bool emit_usages;
            std::set<std::pair<int, int>> decl_pos;
        };
        std::unordered_map<std::string, NameEntry> name_map;
        name_map.reserve(doc.symbols.size() * 2);

        auto upsert = [&](const std::string &name, int tok_type, int prio, bool emit_usages, int sym_line, int sym_col) {
            auto ins = name_map.emplace(name, NameEntry{ tok_type, prio, emit_usages, {} });
            auto it = ins.first;
            bool inserted = ins.second;
            if (!inserted && prio > it->second.priority) {
                it->second.token_type = tok_type;
                it->second.priority = prio;
                it->second.emit_usages = emit_usages;
            }
            // Resolve the actual identifier column (sym_col points to `let`/`func` keyword)
            const int lsp_ln = sym_line - 1;
            if (lsp_ln >= 0 && lsp_ln < (int)lines.size()) {
                const size_t from = (sym_col > 0) ? (size_t)(sym_col - 1) : 0u;
                const int fc = lsp_find_word_col(lines[lsp_ln], name, from);
                if (fc > 0) {
                    it->second.decl_pos.insert({ lsp_ln, fc - 1 });
                }
            }
        };

        for (const auto &sym : doc.symbols) {
            switch (sym.kind) {
                case CK_Function:
                    upsert(sym.name, ST_FUNCTION, 6, true, sym.line, sym.col);
                    break;
                case CK_Class:
                    upsert(sym.name, ST_CLASS, 5, true, sym.line, sym.col);
                    break;
                case CK_Enum:
                    upsert(sym.name, ST_ENUM, 4, true, sym.line, sym.col);
                    break;
                case CK_Struct:
                    upsert(sym.name, ST_TYPE, 3, true, sym.line, sym.col);
                    break;
                case CK_Module:
                    upsert(sym.name, ST_NAMESPACE, 2, true, sym.line, sym.col);
                    break;
                case CK_Variable:
                    if (!sym.detail.empty() && sym.detail.rfind("(parameter)", 0) == 0) {
                        upsert(sym.name, ST_PARAM, 1, false, sym.line, sym.col);
                    } else {
                        upsert(sym.name, ST_VARIABLE, 0, false, sym.line, sym.col);
                    }
                    break;
                default:
                    break;
            }
        }

        // Also include known builtin function names (not already in name_map)
        if (builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(builtins_mutex_);
            for (const auto &[bname, bsym] : builtin_syms_) {
                if (bsym.kind == CK_Function) {
                    name_map.emplace(bname, NameEntry{ ST_FUNCTION, 6, true, {} });
                }
            }
        }

        // scan source lines and collect tokens
        struct Token {
            int line, col, len, type, mods;
        };
        std::vector<Token> tokens;
        tokens.reserve(512);

        const auto is_id_c = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };

        for (int ln = 0; ln < (int)lines.size(); ++ln) {
            const std::string &line_str = lines[ln];

            // Quick check: skip pure comment lines
            {
                size_t p = 0;
                while (p < line_str.size() && (line_str[p] == ' ' || line_str[p] == '\t')) {
                    ++p;
                }
                if (p + 1 < line_str.size() && line_str[p] == '/' && line_str[p + 1] == '/') {
                    continue;
                }
            }

            bool in_str = false; // basic single-line string-skip
            for (size_t i = 0; i < line_str.size();) {
                if (!in_str && line_str[i] == '"') {
                    in_str = true;
                    ++i;
                    continue;
                }
                if (in_str) {
                    if (line_str[i] == '\\') {
                        i += 2;
                        continue;
                    }
                    if (line_str[i] == '"') {
                        in_str = false;
                    }
                    ++i;
                    continue;
                }
                // Skip line comment mid-line
                if (i + 1 < line_str.size() && line_str[i] == '/' && line_str[i + 1] == '/') {
                    break;
                }

                if (!std::isalpha((unsigned char)line_str[i]) && line_str[i] != '_') {
                    ++i;
                    continue;
                }

                const size_t id_start = i;
                while (i < line_str.size() && is_id_c(line_str[i])) {
                    ++i;
                }
                const size_t id_end = i;

                const std::string ident(line_str.data() + id_start, id_end - id_start);
                auto it = name_map.find(ident);
                if (it == name_map.end()) {
                    continue;
                }

                const NameEntry &entry = it->second;
                const std::pair<int, int> pos = { ln, (int)id_start };
                const bool is_decl = entry.decl_pos.count(pos) > 0;

                if (!entry.emit_usages && !is_decl) {
                    continue; // only decl positions for variables/params
                }

                tokens.push_back({ ln, (int)id_start, (int)(id_end - id_start),
                                   entry.token_type,
                                   is_decl ? SM_DECLARATION : 0 });
            }
        }

        // Sort and deduplicate (keep first occurrence at each position)
        std::stable_sort(tokens.begin(), tokens.end(), [](const Token &a, const Token &b) {
            return a.line != b.line ? a.line < b.line : a.col < b.col;
        });
        tokens.erase(std::unique(tokens.begin(), tokens.end(), [](const Token &a, const Token &b) {
                         return a.line == b.line && a.col == b.col;
                     }),
                     tokens.end());

        // Encode as LSP delta-encoded semantic tokens array
        json data = json::array();
        auto &arr = *data.get_ptr<json::array_t *>();
        arr.reserve(tokens.size() * 5);
        int prev_ln = 0, prev_col = 0;
        for (const auto &tok : tokens) {
            const int dl = tok.line - prev_ln;
            const int dc = (dl == 0) ? (tok.col - prev_col) : tok.col;
            arr.push_back(int64_t(dl));
            arr.push_back(int64_t(dc));
            arr.push_back(int64_t(tok.len));
            arr.push_back(int64_t(tok.type));
            arr.push_back(int64_t(tok.mods));
            prev_ln = tok.line;
            prev_col = tok.col;
        }

        json result;
        result["data"] = std::move(data);
        send_response(id, std::move(result));
    }

    // textDocument/typeDefinition
    // Jumps to the class/type declaration for the type of a variable.
    // e.g. "let p = new Person()" -> jumps to class Person { ... }
    void handle_type_definition(const json &id, const json &params) {
        const auto &td = params.contains("textDocument") ? params["textDocument"] : json{};
        const auto &pos = params.contains("position") ? params["position"] : json{};
        std::string uri = td.value("uri", std::string{});
        const int line = pos.value("line", 0);
        const int col = pos.value("character", 0);

        std::shared_lock<std::shared_timed_mutex> docs_lk(docs_mutex_);
        if (!docs_.count(uri)) {
            send_response(id, nullptr);
            return;
        }
        const DocState &doc = docs_[uri];

        auto wp = word_at(doc.content, line, col);
        const std::string &word = wp.first;
        int word_start = wp.second;
        if (word.empty()) {
            send_response(id, nullptr);
            return;
        }

        // primitive types are not user-defined, so no type declaration to jump to
        static const std::unordered_set<std::string> primitives = {
            "string", "number", "bool", "null", "array", "object", "function", "regex"
        };

        // if the word is a variable, look at its inferred_type for a class name
        std::string class_name;
        for (const auto &sym : doc.symbols) {
            if (sym.name != word || sym.kind != CK_Variable) {
                continue;
            }
            if (!sym.inferred_type.empty() && !primitives.count(sym.inferred_type)) {
                class_name = sym.inferred_type;
                break;
            }
        }

        // if the word is itself a class or type name, jump to its declaration
        if (class_name.empty()) {
            for (const auto &ci : doc.classes) {
                if (ci.name == word) {
                    class_name = word;
                    break;
                }
            }
        }
        if (class_name.empty()) {
            for (const auto &sym : doc.symbols) {
                if (sym.name == word && (sym.kind == CK_Class || sym.kind == CK_Struct || sym.kind == CK_Enum)) {
                    class_name = word;
                    break;
                }
            }
        }

        if (class_name.empty()) {
            send_response(id, nullptr);
            return;
        }

        // locate the class definition

        // first try doc.classes (parsed class declarations)
        for (const auto &ci : doc.classes) {
            if (ci.name != class_name) {
                continue;
            }
            // find source file via symbols
            std::string src_file;
            for (const auto &sym : doc.symbols) {
                if (sym.name == class_name && sym.kind == CK_Class) {
                    src_file = sym.source_file;
                    break;
                }
            }

            const int lsp_ln = std::max(0, ci.line - 1);
            const int lsp_ch = std::max(0, ci.col - 1);
            json sp, ep, rng, loc;
            sp["line"] = int64_t(lsp_ln);
            sp["character"] = int64_t(lsp_ch);
            ep["line"] = int64_t(lsp_ln);
            ep["character"] = int64_t(lsp_ch + (int)class_name.size());
            rng["start"] = std::move(sp);
            rng["end"] = std::move(ep);
            loc["range"] = std::move(rng);
            loc["uri"] = src_file.empty() ? uri : path_to_uri(std::filesystem::path(src_file));
            send_response(id, std::move(loc));
            return;
        }

        // fall back to the symbol table (enum / type aliases defined in stdlib or builtins)
        for (const auto &sym : doc.symbols) {
            if (sym.name != class_name) {
                continue;
            }
            if (sym.kind != CK_Enum && sym.kind != CK_Struct && sym.kind != CK_Class) {
                continue;
            }
            const int lsp_ln = std::max(0, sym.line - 1);
            const int lsp_ch = std::max(0, sym.col - 1);
            json sp, ep, rng, loc;
            sp["line"] = int64_t(lsp_ln);
            sp["character"] = int64_t(lsp_ch);
            ep["line"] = int64_t(lsp_ln);
            ep["character"] = int64_t(lsp_ch + (int)class_name.size());
            rng["start"] = std::move(sp);
            rng["end"] = std::move(ep);
            loc["range"] = std::move(rng);
            loc["uri"] = sym.source_file.empty() ? uri : path_to_uri(std::filesystem::path(sym.source_file));
            send_response(id, std::move(loc));
            return;
        }

        // check builtins
        if (builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(builtins_mutex_);
            auto bit = builtin_syms_.find(class_name);
            if (bit != builtin_syms_.end() && !builtin_decls_uri_.empty()) {
                const int lsp_ln = std::max(0, bit->second.line - 1);
                const int lsp_ch = std::max(0, bit->second.col - 1);
                json sp, ep, rng, loc;
                sp["line"] = int64_t(lsp_ln);
                sp["character"] = int64_t(lsp_ch);
                ep["line"] = int64_t(lsp_ln);
                ep["character"] = int64_t(lsp_ch + (int)class_name.size());
                rng["start"] = std::move(sp);
                rng["end"] = std::move(ep);
                loc["range"] = std::move(rng);
                loc["uri"] = builtin_decls_uri_;
                send_response(id, std::move(loc));
                return;
            }
        }

        send_response(id, nullptr);
    }

    //  Parse a document, update symbol table, publish diagnostics
    //      push a parse request to the background thread.
    void enqueue_parse(const std::string &uri, const std::string &content, std::size_t hash = 0) {
        if (hash == 0) {
            hash = std::hash<std::string>{}(content);
        }
        std::lock_guard<std::mutex> lk(parse_queue_mutex_);
        for (auto &req : parse_queue_) {
            if (req.uri == uri) {
                req.content = content;
                req.hash = hash;
                return;
            }
        }
        parse_queue_.push_back({ uri, content, hash });
        parse_queue_cv_.notify_one();
    }

    // background thread: drains the parse queue.
    void parse_thread_func() {
        while (true) {
            ParseRequest req;
            {
                std::unique_lock<std::mutex> lk(parse_queue_mutex_);
                parse_queue_cv_.wait(lk, [this] {
                    return !parse_queue_.empty() || parse_thread_stop_;
                });
                if (parse_thread_stop_ && parse_queue_.empty()) {
                    break;
                }
                req = std::move(parse_queue_.front());
                parse_queue_.erase(parse_queue_.begin());
            }
            parse_document(req.uri, req.content, req.hash);
        }
        lsp_log("parse_thread_func: exiting");
    }

    // parse_document: runs on the background parse thread.
    // Parser globals are thread_local so concurrent parses are safe
    void parse_document(const std::string &uri, const std::string &content, std::size_t content_hash = 0) {
        if (content_hash == 0) {
            content_hash = std::hash<std::string>{}(content);
        }

        // Skip re-parse if this version is already analyzed, or if a newer
        // document version has already replaced it in the live doc store.
        {
            std::shared_lock<std::shared_timed_mutex> lk(docs_mutex_);
            auto it = docs_.find(uri);
            if (it != docs_.end()) {
                if (it->second.parsed_hash == content_hash) {
                    return;
                }
                if (it->second.content_hash != 0 && it->second.content_hash != content_hash) {
                    return;
                }
            }
        }

        // all results are accumulated into locals; docs_ is not touched yet
        std::vector<SymInfo> new_symbols;
        std::vector<ClassInfo> new_classes;
        std::vector<ObjectLiteralInfo> new_object_literals;
        json diagnostics = json::array();

#ifndef DISABLE_PARSER
        Parser::reset_parse_session();

        // strip file:// to get a filesystem path for error messages
        std::string filename = uri;
        if (filename.rfind("file://", 0) == 0) {
            filename = filename.substr(7);
        }

        Parser::set_source_filename(filename);
        lsp_log("parse_document: parsing " + filename);

        // create_aggregator=false: we only need declarations, not a runnable chunk
        auto result = Parser::parse_program_recovering(content, /*create_aggregator=*/false);

        lsp_log("parse_document: " + std::to_string(result.functions.size()) + " funcs, " + std::to_string(result.errors.size()) + " errors");

        // Errors -> LSP diagnostics
        for (const auto &err : result.errors) {
            int lsp_line = std::max(0, err.line - 1);
            int lsp_col = std::max(0, err.col - 1);

            json rng_start, rng_end;
            rng_start["line"] = int64_t(lsp_line);
            rng_start["character"] = int64_t(lsp_col);
            rng_end["line"] = int64_t(lsp_line);
            rng_end["character"] = int64_t(lsp_col + 1);

            json rng;
            rng["start"] = std::move(rng_start);
            rng["end"] = std::move(rng_end);

            json diag;
            diag["range"] = std::move(rng);
            diag["severity"] = int64_t(1); // Error
            diag["message"] = err.message;
            diag["source"] = "nari";
            diagnostics.push_back(std::move(diag));
        }

        // build symbol table, then immediately free the AST, since we don't need it anymore
        std::vector<std::string> source_lines;
        {
            std::istringstream iss(content);
            std::string ln;
            while (std::getline(iss, ln)) {
                source_lines.push_back(ln);
            }
        }
        emit_unused_warnings(result.functions, filename, source_lines, diagnostics);
        emit_strict_type_warnings(result.functions, filename, source_lines, diagnostics);
        new_symbols = collect_symbols(result.functions, filename, source_lines, &new_object_literals);
        new_classes = collect_classes();
        result.functions.clear(); // free all AST unique_ptrs now
        result.functions.shrink_to_fit();

        // Inferred-type method-mismatch warnings
        // Build a name -> inferred_type map from the collected symbols.
        // Then scan source lines for "name.method(" patterns to detect
        // calls where the method doesn't exist for the inferred type.
        if (builtins_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(builtins_mutex_);
            if (!method_type_syms_.empty()) {
                // Build a lookup: variable name -> inferred type (for this doc)
                std::unordered_map<std::string, std::string> var_types;
                for (const auto &sym : new_symbols) {
                    if (!sym.inferred_type.empty()) {
                        var_types[sym.name] = sym.inferred_type;
                    }
                }

                // for each line, scan for all `ident.ident(` patterns
                for (int ln = 0; ln < (int)source_lines.size(); ++ln) {
                    const std::string &sl = source_lines[ln];
                    for (size_t i = 0; i + 2 < sl.size(); ++i) {
                        if (!std::isalpha((unsigned char)sl[i]) && sl[i] != '_') {
                            continue;
                        }
                        size_t id_end = i + 1;
                        while (id_end < sl.size() && (std::isalnum((unsigned char)sl[id_end]) || sl[id_end] == '_')) {
                            id_end++;
                        }
                        if (id_end >= sl.size() || sl[id_end] != '.') {
                            i = id_end - 1;
                            continue;
                        }
                        std::string var_name = sl.substr(i, id_end - i);

                        if (var_name == "return" || var_name == "if" || var_name == "while" ||
                            var_name == "for" || var_name == "let" || var_name == "func") {
                            i = id_end - 1;
                            continue;
                        }

                        auto varTps = var_types.find(var_name);
                        if (varTps == var_types.end()) {
                            i = id_end - 1;
                            continue;
                        }
                        const std::string &var_type = varTps->second;

                        // read the method name after the dot
                        size_t dot_pos = id_end + 1; // character after '.'
                        size_t mend = dot_pos;
                        while (mend < sl.size() && (std::isalnum((unsigned char)sl[mend]) || sl[mend] == '_')) {
                            mend++;
                        }
                        if (mend == dot_pos) {
                            i = id_end - 1;
                            continue;
                        }
                        std::string method_name = sl.substr(dot_pos, mend - dot_pos);

                        // check if this method exists for var_type
                        // look in the type-specific map
                        auto tit = method_type_syms_.find(var_type);
                        if (tit == method_type_syms_.end()) {
                            i = id_end - 1;
                            continue;
                        }
                        // method not found in this type's map?
                        if (tit->second.count(method_name)) {
                            i = id_end - 1;
                            continue;
                        }

                        // check if the method exists for ANY other type
                        bool found_elsewhere = false;
                        for (const auto &[tname, tmap] : method_type_syms_) {
                            if (tname == var_type) {
                                continue;
                            }
                            if (tmap.count(method_name)) {
                                found_elsewhere = true;
                                break;
                            }
                        }
                        if (!found_elsewhere) {
                            i = id_end - 1;
                            continue;
                        }

                        // Emit a warning
                        int lsp_line = ln;
                        int lsp_col = (int)dot_pos;
                        json rs, re, rng;
                        rs["line"] = int64_t(lsp_line);
                        rs["character"] = int64_t(lsp_col);
                        re["line"] = int64_t(lsp_line);
                        re["character"] = int64_t(lsp_col + (int)method_name.size());
                        rng["start"] = std::move(rs);
                        rng["end"] = std::move(re);
                        json diag;
                        diag["range"] = std::move(rng);
                        diag["severity"] = int64_t(2); // Warning
                        diag["message"] = "Method '" + method_name + "' does not exist on type '" + var_type + "'";
                        diag["source"] = "nari";
                        diagnostics.push_back(std::move(diag));

                        i = mend - 1;
                    }
                }
            }
        }

        lsp_log("parse_document: " + std::to_string(new_symbols.size()) + " symbols, " + std::to_string(new_classes.size()) + " classes");

        // Add class names as symbols too (for hover / goto-def)
        for (const auto &ci : new_classes) {
            new_symbols.push_back({ ci.name, "class " + ci.name, CK_Class,
                                    ci.line, ci.col, ci.source_file });
        }

        {
            std::unique_lock<std::shared_timed_mutex> lk(docs_mutex_);
            DocState &doc = docs_[uri];
            if (doc.content_hash != content_hash) {
                lsp_log("parse_document: dropping stale results for " + uri);
                return;
            }
            doc.content = content;
            doc.symbols = std::move(new_symbols);
            doc.classes = std::move(new_classes);
            doc.object_literals = std::move(new_object_literals);
            doc.parsed_hash = content_hash;
        }
#else
        (void)content;
        (void)content_hash;
#endif

        json p;
        p["uri"] = uri;
        p["diagnostics"] = std::move(diagnostics);
        send_notification("textDocument/publishDiagnostics", std::move(p));
    }
};

int main(int, char **) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Disable stdio sync for maximum throughput on the LSP pipe
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    init_logger();

    NariLspServer server;
    server.run();
    return 0;
}
