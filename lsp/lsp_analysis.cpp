/*
  lsp_analysis.cpp
  Analysis helpers split verbatim from lsp/lsp_server.cpp.
*/

#include "lsp_analysis.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifndef DISABLE_PARSER
extern const std::string &nari_std_module_source(const std::string &name);
#endif

//  AST walker: collects symbols from a parsed FuncList

#ifndef DISABLE_PARSER

// Unused-variable analysis

// Forward declarations
void collect_expr_reads(const nari::Expr *e, std::unordered_set<std::string> &reads) {
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

void collect_stmt_reads(const nari::Stmt *s, std::unordered_set<std::string> &reads) {
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

void collect_stmts_reads(const std::vector<nari::StmtPtr> &stmts, std::unordered_set<std::string> &reads) {
    for (const auto &sp : stmts) {
        collect_stmt_reads(sp.get(), reads);
    }
}

void collect_local_decls_stmts(const std::vector<nari::StmtPtr> &stmts, std::vector<LocalDecl> &out) {
    for (const auto &sp : stmts) {
        collect_local_decls_stmt(sp.get(), out);
    }
}

void collect_local_decls_stmt(const nari::Stmt *s, std::vector<LocalDecl> &out) {
    if (!s) {
        return;
    }
    switch (s->stmt_kind) {
        case nari::StmtKind::VarDecl: {
            const auto *vd = static_cast<const nari::VarDeclStmt *>(s);
            if (vd->is_global != nari::VarDeclCtrl::LOCAL) {
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

void emit_strict_type_warnings(const FuncList &funcs, const std::string &doc_filename, const std::vector<std::string> &source_lines,
                                      json &diagnostics) {
    auto find_word_col = [](const std::string &src_line, const std::string &word, size_t search_from = 0) -> int {
        size_t pos = search_from;
        while ((pos = src_line.find(word, pos)) != std::string::npos) {
            bool left_ok = (pos == 0) || (!std::isalnum(static_cast<unsigned char>(src_line[pos - 1])) && src_line[pos - 1] != '_');
            size_t end = pos + word.size();
            bool right_ok = (end >= src_line.size()) || (!std::isalnum(static_cast<unsigned char>(src_line[end])) && src_line[end] != '_');
            if (left_ok && right_ok) {
                return static_cast<int>(pos) + 1; // 1-based
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
            if (param_line >= 1 && param_line <= static_cast<int>(source_lines.size())) {
                const std::string &sig = source_lines[param_line - 1];
                size_t paren = sig.find('(');
                int found = find_word_col(sig, p.name, paren != std::string::npos ? paren : 0);
                if (found > 0) {
                    param_col = found;
                }
            }

            json diag;
            diag["range"] = make_range(param_line, param_col, static_cast<int>(p.name.size()));
            diag["severity"] = int64_t(1); // Error
            diag["message"] = "Strict mode: parameter '" + p.name + "' has no type annotation. Add ': <type>' for runtime type checking.";
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
            if (fn_line >= 1 && fn_line <= static_cast<int>(source_lines.size())) {
                const std::string &sig = source_lines[fn_line - 1];
                int found = find_word_col(sig, fn->name, static_cast<size_t>((fn_col > 1 ? fn_col - 1 : 0)));
                if (found > 0) {
                    fn_col = found;
                }
            }
            json diag;
            diag["range"] = make_range(fn_line, fn_col, static_cast<int>(label.size()));
            diag["severity"] = int64_t(1); // Error
            diag["message"] = "Strict mode: '" + label + "' has no return type annotation. Add '-> <type>' for return-value checking.";
            diag["source"] = "nari";
            diagnostics.push_back(std::move(diag));
        }
    }
}

void emit_unused_warnings(const FuncList &funcs, const std::string &doc_filename, const std::vector<std::string> &source_lines,
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
                bool left_ok = (pos == 0) || (!std::isalnum(static_cast<unsigned char>(src_line[pos - 1])) && src_line[pos - 1] != '_');
                size_t end = pos + word.size();
                bool right_ok = (end >= src_line.size()) || (!std::isalnum(static_cast<unsigned char>(src_line[end])) && src_line[end] != '_');
                if (left_ok && right_ok) {
                    return static_cast<int>(pos) + 1;
                }
                pos++;
            }
            return -1;
        };

        // resolve the real column of `name` on `ast_line`
        auto resolve_col = [&](const std::string &name, int ast_line, int ast_col) -> int {
            if (ast_line >= 1 && ast_line <= static_cast<int>(source_lines.size())) {
                size_t from = (ast_col > 1) ? static_cast<size_t>(ast_col - 1) : 0;
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
            int end_line = static_cast<int>(source_lines.size());
            for (int cur_line = start_line; cur_line <= end_line; cur_line++) {
                size_t from = 0;
                if (cur_line == line) {
                    from = static_cast<size_t>(std::max(0, col - 1 + static_cast<int>(name.size())));
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
            re["character"] = int64_t(lsp_col + static_cast<int>(name.size()));
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
            if (fn->line >= 1 && fn->line <= static_cast<int>(source_lines.size())) {
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

std::string build_func_sig(const nari::Function &fn, const std::string &name_override ) {
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
        if (p.default_value) {
            s += p.default_value->kind == nari::ExprKind::Null ? " = null" : " = ...";
        }
    }
    s += ")";
    if (fn.return_type) {
        s += " -> " + fn.return_type->to_string();
    }
    return s;
}

std::string build_method_sig(const nari::ClassMethod &method, const std::string &owner_class ) {
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
        if (p.default_value) {
            s += p.default_value->kind == nari::ExprKind::Null ? " = null" : " = ...";
        }
    }
    s += ")";
    if (method.return_type) {
        s += " -> " + method.return_type->to_string();
    }
    return s;
}

int find_identifier_col_in_line(const std::string &src_line, const std::string &word, size_t search_from ) {
    size_t pos = search_from;
    while ((pos = src_line.find(word, pos)) != std::string::npos) {
        bool left_ok = (pos == 0) || (!std::isalnum(static_cast<unsigned char>(src_line[pos - 1])) && src_line[pos - 1] != '_');
        size_t end = pos + word.size();
        bool right_ok = (end >= src_line.size()) || (!std::isalnum(static_cast<unsigned char>(src_line[end])) && src_line[end] != '_');
        if (left_ok && right_ok) {
            return static_cast<int>(pos) + 1; // 1-based
        }
        ++pos;
    }
    return -1;
}

int resolve_identifier_col(const std::vector<std::string> &source_lines, const std::string &name, int ast_line, int ast_col,
                                  size_t search_from ) {
    if (ast_line >= 1 && ast_line <= static_cast<int>(source_lines.size())) {
        int found = find_identifier_col_in_line(source_lines[ast_line - 1], name, search_from);
        if (found > 0) {
            return found;
        }
    }
    return ast_col;
}

std::string infer_annotation_type(const nari::TypeAnnotation *t) {
    if (!t) {
        return "";
    }
    if (t->is_array) {
        return "array";
    }
    return t->name;
}

std::string normalize_method_receiver_type(std::string type_name) {
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

std::string infer_expr_type(const nari::Expr *e) {
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

bool is_scope_owner_kind(int kind) {
    return kind == CK_Function || kind == CK_Method;
}

int find_enclosing_scope_line(const std::vector<SymInfo> &symbols, const std::string &source_file, int cursor_line_1) {
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

const SymInfo *find_symbol_in_scope(const std::vector<SymInfo> &symbols, const std::string &name, const std::string &source_file,
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

const ClassInfo *find_class_info(const std::vector<ClassInfo> &classes, const std::string &name) {
    for (const auto &ci : classes) {
        if (ci.name == name) {
            return &ci;
        }
    }
    return nullptr;
}

const MemberInfo *find_class_member(const std::vector<ClassInfo> &classes, const std::string &class_name,
                                           const std::string &member_name, bool static_only, std::unordered_set<std::string> &visited) {
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

const MemberInfo *find_class_member(const std::vector<ClassInfo> &classes, const std::string &class_name,
                                           const std::string &member_name, bool static_only) {
    std::unordered_set<std::string> visited;
    return find_class_member(classes, class_name, member_name, static_only, visited);
}

std::filesystem::path find_package_declarations(const std::string &specifier, const std::string &source_file) {
    std::filesystem::path source_dir = std::filesystem::path(source_file).parent_path();
    if (specifier.rfind("./", 0) == 0 || specifier.rfind("../", 0) == 0) {
        std::filesystem::path base = source_dir / specifier;
        std::vector<std::filesystem::path> candidates;
        if (base.extension() == ".nari") {
            candidates.push_back(base.parent_path() / (base.stem().string() + ".d.nari"));
        }
        candidates.push_back(std::filesystem::path(base.string() + ".d.nari"));
        for (const auto &candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
        return {};
    }

    for (std::filesystem::path dir = source_dir; !dir.empty();) {
        std::filesystem::path candidate = dir / "nari_modules" / std::filesystem::path(specifier + ".d.nari");
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = std::move(parent);
    }
    return {};
}

bool load_package_declarations(const std::filesystem::path &path, const std::string &binding_name, SymInfo &binding,
                                      std::vector<ClassInfo> &classes) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::string default_name;
    std::vector<std::string> source_lines;
    std::istringstream lines(content);
    for (std::string line; std::getline(lines, line);) {
        source_lines.push_back(line);
        size_t marker = default_name.empty() ? line.find("// @default ") : std::string::npos;
        if (marker != std::string::npos) {
            default_name = line.substr(marker + 12);
            size_t end = default_name.find_first_of(" \t\r");
            if (end != std::string::npos) {
                default_name.resize(end);
            }
        }
    }
    if (default_name.empty()) {
        return false;
    }

    Parser::set_source_filename(path.string());
    auto result = Parser::parse_program_recovering(content, /*create_aggregator=*/false);
    if (!result.ok()) {
        return false;
    }
    for (const auto &fn : result.functions) {
        if (!fn || fn->name != default_name) {
            continue;
        }
        binding.kind = CK_Function;
        binding.inferred_type = "function";
        binding.call_return_type = fn->return_type ? fn->return_type->to_string() : "";
        binding.detail = build_func_sig(*fn, binding_name);
        binding.doc_comment = extract_doc_comment(source_lines, fn->line);
        break;
    }

    for (const auto &[name, declaration] : Parser::get_all_registered_classes()) {
        if (!declaration || declaration->filename != path.string() || find_class_info(classes, name)) {
            continue;
        }
        ClassInfo cls;
        cls.name = name;
        cls.line = declaration->line;
        cls.col = declaration->col;
        cls.source_file = path.string();
        cls.parent_name = declaration->parent_name;
        for (const auto &field : declaration->fields) {
            MemberInfo member;
            member.name = field.name;
            member.detail = "property " + field.name;
            member.source_file = path.string();
            member.line = field.line;
            member.col = field.col;
            member.return_type = field.type ? field.type->to_string() : "";
            if (!member.return_type.empty()) {
                member.detail += ": " + member.return_type;
            }
            member.doc_comment = extract_doc_comment(source_lines, field.line);
            cls.members.push_back(std::move(member));
        }
        for (const auto &method : declaration->methods) {
            MemberInfo member;
            member.name = method.name;
            member.detail = build_method_sig(method, declaration->name);
            member.source_file = path.string();
            member.line = method.line;
            member.col = method.col;
            member.is_method = true;
            member.is_static = method.is_static;
            member.return_type = method.return_type ? method.return_type->to_string() : "";
            for (const auto &param : method.params) {
                member.parameter_types.push_back(param.type ? param.type->to_string() : "");
            }
            member.doc_comment = extract_doc_comment(source_lines, method.line);
            cls.members.push_back(std::move(member));
        }
        classes.push_back(std::move(cls));
    }
    return binding.kind == CK_Function;
}

std::string infer_expr_type(const nari::Expr *e, const std::vector<SymInfo> &symbols, const std::vector<ClassInfo> &classes,
                                   const std::string &source_file, int line) {
    std::string simple = infer_expr_type(e);
    if (!simple.empty() || !e) {
        return simple;
    }
    if (e->kind == nari::ExprKind::Member) {
        const auto *member = static_cast<const nari::MemberExpr *>(e);
        std::string receiver_type = infer_expr_type(member->object.get(), symbols, classes, source_file, line);
        if (const MemberInfo *info = find_class_member(classes, receiver_type, member->member)) {
            return info->return_type;
        }
    }
    if (e->kind != nari::ExprKind::Call) {
        if (e->kind == nari::ExprKind::Ident) {
            const auto *ident = static_cast<const nari::IdentExpr *>(e);
            if (const SymInfo *sym = find_symbol_in_scope(symbols, ident->name, source_file, line)) {
                return sym->inferred_type;
            }
        }
        return "";
    }
    const auto *call = static_cast<const nari::CallExpr *>(e);
    if (call->callee && call->callee->kind == nari::ExprKind::Ident) {
        const auto *ident = static_cast<const nari::IdentExpr *>(call->callee.get());
        if (const SymInfo *sym = find_symbol_in_scope(symbols, ident->name, source_file, line)) {
            return sym->call_return_type;
        }
    }
    if (call->callee && call->callee->kind == nari::ExprKind::Member) {
        const auto *member = static_cast<const nari::MemberExpr *>(call->callee.get());
        std::string receiver_type = infer_expr_type(member->object.get(), symbols, classes, source_file, line);
        if (const MemberInfo *info = find_class_member(classes, receiver_type, member->member)) {
            return info->return_type;
        }
    }
    return "";
}

std::string resolve_receiver_class_name(const std::vector<SymInfo> &symbols, const std::vector<ClassInfo> &classes,
                                               const std::string &receiver, const std::string &source_file, int cursor_line_1) {
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
    std::string type_name = receiver_sym->inferred_type;
    size_t generic = type_name.find('<');
    if (generic != std::string::npos) {
        type_name.resize(generic);
    }
    if (type_name.size() >= 2 && type_name.substr(type_name.size() - 2) == "[]") {
        type_name.resize(type_name.size() - 2);
    }
    return type_name;
}

std::string build_function_expr_sig(const std::string &name, const nari::FunctionExpr *fn) {
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

std::vector<SymInfo> collect_object_literal_members(const nari::ObjectLiteralExpr *object_expr) {
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
            member.detail = value_type.empty() ? "property " + key : "property " + key + ": " + value_type;
        }

        members.push_back(std::move(member));
    }

    return members;
}

const ObjectLiteralInfo *find_object_literal_info(const std::vector<ObjectLiteralInfo> &infos, const std::vector<SymInfo> &symbols,
                                                         const std::string &receiver, const std::string &source_file, int cursor_line_1) {
    const SymInfo *receiver_sym = find_symbol_in_scope(symbols, receiver, source_file, cursor_line_1);
    if (receiver_sym) {
        for (const auto &info : infos) {
            if (info.name != receiver || info.source_file != receiver_sym->source_file) {
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
        if (info.name != receiver || info.source_file != source_file) {
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

void walk_stmts(const std::vector<nari::StmtPtr> &stmts, std::vector<SymInfo> &out, const StringMap &ns_to_path,
                       const std::vector<std::string> &source_lines, int scope_fn_line ,
                       std::vector<ObjectLiteralInfo> *object_literals , std::vector<ClassInfo> *classes ,
                       const std::string &source_file ) {
    for (const auto &sptr : stmts) {
        if (!sptr) {
            continue;
        }
        const nari::Stmt *s = sptr.get();

        if (s->stmt_kind == nari::StmtKind::Expr && classes) {
            const auto *expr_stmt = static_cast<const nari::ExprStmt *>(s);
            if (expr_stmt->expr && expr_stmt->expr->kind == nari::ExprKind::Call) {
                const auto *call = static_cast<const nari::CallExpr *>(expr_stmt->expr.get());
                const MemberInfo *called_member = nullptr;
                if (call->callee && call->callee->kind == nari::ExprKind::Member) {
                    const auto *member = static_cast<const nari::MemberExpr *>(call->callee.get());
                    std::string receiver_type = infer_expr_type(member->object.get(), out, *classes, source_file, s->line);
                    called_member = find_class_member(*classes, receiver_type, member->member);
                }
                if (called_member && !called_member->parameter_types.empty()) {
                    for (size_t i = 0; i < call->args.size(); ++i) {
                        const auto *arg = call->args[i].get();
                        if (!arg || arg->kind != nari::ExprKind::Function) {
                            continue;
                        }
                        size_t parameter_index = std::min(i, called_member->parameter_types.size() - 1);
                        std::string callback_type = called_member->parameter_types[parameter_index];
                        if (callback_type.size() >= 2 && callback_type.substr(callback_type.size() - 2) == "[]") {
                            callback_type.resize(callback_type.size() - 2);
                        }
                        size_t generic = callback_type.find('<');
                        if (generic != std::string::npos) {
                            callback_type.resize(generic);
                        }
                        const MemberInfo *call_signature = find_class_member(*classes, callback_type, "call");
                        if (!call_signature) {
                            continue;
                        }

                        const auto *fn = static_cast<const nari::FunctionExpr *>(arg);
                        SymInfo scope_owner{ "", "callback", CK_Function, fn->line, fn->col, source_file };
                        out.push_back(std::move(scope_owner));
                        for (size_t p = 0; p < fn->params.size(); ++p) {
                            const auto &param = fn->params[p];
                            SymInfo param_info;
                            param_info.name = param.name;
                            param_info.detail = "(parameter) " + param.name;
                            param_info.kind = CK_Variable;
                            param_info.line = fn->line;
                            param_info.col = fn->col;
                            param_info.source_file = source_file;
                            param_info.scope_fn_line = fn->line;
                            if (p < call_signature->parameter_types.size()) {
                                param_info.inferred_type = call_signature->parameter_types[p];
                                param_info.detail += ": " + param_info.inferred_type;
                            }
                            out.push_back(std::move(param_info));
                        }
                        if (fn->body) {
                            walk_stmts(fn->body->stmts, out, ns_to_path, source_lines, fn->line, object_literals, classes, source_file);
                        }
                    }
                }
            }
        } else if (s->stmt_kind == nari::StmtKind::VarDecl) {
            const auto *vd = static_cast<const nari::VarDeclStmt *>(s);
            if (vd->destructure_kind == nari::DestructureKind::None) {
                if (!vd->name.empty()) {
                    // Detect import bindings: let X = __module_namespace_N__
                    bool is_import = false;
                    std::string import_detail;
                    std::string import_source_path;
                    if (vd->initializerExpr && vd->initializerExpr->kind == nari::ExprKind::Ident) {
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

                    int decl_col =
                        resolve_identifier_col(source_lines, vd->name, s->line, s->col, (s->col > 1) ? static_cast<size_t>(s->col - 1) : 0u);

                    if (is_import) {
                        out.push_back({ vd->name, import_detail, CK_Module, s->line, decl_col, import_source_path });
                    } else {
                        std::string itype = classes ? infer_expr_type(vd->initializerExpr.get(), out, *classes, source_file, s->line)
                                                    : infer_expr_type(vd->initializerExpr.get());
                        std::string det;
                        if (!itype.empty()) {
                            det = (vd->is_global ? "global " : "") + itype + " " + vd->name;
                        } else {
                            det = vd->is_global ? "global variable" : "variable";
                        }
                        SymInfo si{ vd->name, det, CK_Variable, s->line, decl_col };
                        si.scope_fn_line = scope_fn_line;
                        si.inferred_type = itype;
                        if (vd->initializerExpr && vd->initializerExpr->kind == nari::ExprKind::Member) {
                            const auto *member = static_cast<const nari::MemberExpr *>(vd->initializerExpr.get());
                            if (member->object && member->object->kind == nari::ExprKind::Ident) {
                                const auto *namespace_ident = static_cast<const nari::IdentExpr *>(member->object.get());
                                auto import = ns_to_path.find(namespace_ident->name);
                                if (import != ns_to_path.end()) {
                                    si.import_source_file = import->second;
                                    si.import_export_name = member->member;
                                }
                            }
                        }
                        if (classes && vd->initializerExpr && vd->initializerExpr->kind == nari::ExprKind::Call) {
                            const auto *call = static_cast<const nari::CallExpr *>(vd->initializerExpr.get());
                            if (call->callee && call->callee->kind == nari::ExprKind::Ident &&
                                static_cast<const nari::IdentExpr *>(call->callee.get())->name == "require" && !call->args.empty() &&
                                call->args[0] && call->args[0]->kind == nari::ExprKind::String) {
                                const auto *arg = static_cast<const nari::StringExpr *>(call->args[0].get());
                                std::filesystem::path declarations = find_package_declarations(arg->value, source_file);
                                if (!declarations.empty()) {
                                    load_package_declarations(declarations, vd->name, si, *classes);
                                }
                            }
                        }
                        out.push_back(std::move(si));

                        if (object_literals && vd->initializerExpr && vd->initializerExpr->kind == nari::ExprKind::ObjectLiteral) {
                            ObjectLiteralInfo info;
                            info.name = vd->name;
                            info.decl_line = s->line;
                            info.scope_fn_line = scope_fn_line;
                            info.source_file = source_file;
                            info.members =
                                collect_object_literal_members(static_cast<const nari::ObjectLiteralExpr *>(vd->initializerExpr.get()));
                            std::vector<std::string> std_module_lines;
                            if (source_file.rfind("<std>/", 0) == 0) {
                                std_module_lines = lsp_split_lines(nari_std_module_source(source_file.substr(6)));
                            }
                            for (auto &member : info.members) {
                                member.source_file = source_file;
                                if (member.line > 0 && member.line <= static_cast<int>(std_module_lines.size())) {
                                    const std::string &line = std_module_lines[member.line - 1];
                                    const size_t key_col = line.rfind(member.name + ":", static_cast<size_t>(std::max(0, member.col - 1)));
                                    if (key_col != std::string::npos) {
                                        member.col = static_cast<int>(key_col + 1);
                                    }
                                }
                            }
                            object_literals->push_back(std::move(info));
                        }
                    }
                }
            } else if (vd->destructure_kind == nari::DestructureKind::Array) {
                for (const auto &nm : vd->array_names) {
                    if (!nm.empty()) {
                        int decl_col = resolve_identifier_col(source_lines, nm, s->line, s->col, (s->col > 1) ? static_cast<size_t>(s->col - 1) : 0u);

                        SymInfo si{ nm, "variable", CK_Variable, s->line, decl_col };
                        si.scope_fn_line = scope_fn_line;
                        out.push_back(std::move(si));
                    }
                }
            } else {
                for (const auto &[key, local] : vd->object_bindings) {
                    (void)key;
                    if (!local.empty()) {
                        int decl_col =
                            resolve_identifier_col(source_lines, local, s->line, s->col, (s->col > 1) ? static_cast<size_t>(s->col - 1) : 0u);

                        SymInfo si{ local, "variable", CK_Variable, s->line, decl_col };
                        si.scope_fn_line = scope_fn_line;
                        out.push_back(std::move(si));
                    }
                }
            }

        } else if (s->stmt_kind == nari::StmtKind::ForEach) {
            const auto *fe = static_cast<const nari::ForEachStmt *>(s);
            if (!fe->var.empty()) {
                int decl_col = resolve_identifier_col(source_lines, fe->var, s->line, s->col, (s->col > 1) ? static_cast<size_t>(s->col - 1) : 0u);

                SymInfo si{ fe->var, "variable", CK_Variable, s->line, decl_col };
                si.scope_fn_line = scope_fn_line;
                out.push_back(std::move(si));
            }
            if (fe->body && fe->body->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(fe->body.get())->stmts, out, ns_to_path, source_lines, scope_fn_line,
                           object_literals, classes, source_file);
            }

        } else if (s->stmt_kind == nari::StmtKind::Block) {
            walk_stmts(static_cast<const nari::BlockStmt *>(s)->stmts, out, ns_to_path, source_lines, scope_fn_line, object_literals,
                       classes, source_file);

        } else if (s->stmt_kind == nari::StmtKind::If) {
            const auto *is = static_cast<const nari::IfStmt *>(s);
            if (is->then_branch && is->then_branch->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(is->then_branch.get())->stmts, out, ns_to_path, source_lines, scope_fn_line,
                           object_literals, classes, source_file);
            }
            if (is->else_branch && is->else_branch->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(is->else_branch.get())->stmts, out, ns_to_path, source_lines, scope_fn_line,
                           object_literals, classes, source_file);
            }

        } else if (s->stmt_kind == nari::StmtKind::While) {
            const auto *ws = static_cast<const nari::WhileStmt *>(s);
            if (ws->body && ws->body->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(ws->body.get())->stmts, out, ns_to_path, source_lines, scope_fn_line,
                           object_literals, classes, source_file);
            }

        } else if (s->stmt_kind == nari::StmtKind::For) {
            const auto *fs = static_cast<const nari::ForStmt *>(s);
            if (fs->body && fs->body->stmt_kind == nari::StmtKind::Block) {
                walk_stmts(static_cast<const nari::BlockStmt *>(fs->body.get())->stmts, out, ns_to_path, source_lines, scope_fn_line,
                           object_literals, classes, source_file);
            }
        }
    }
}

std::vector<SymInfo> collect_symbols(const FuncList &funcs, const std::string &doc_filename,
                                            const std::vector<std::string> &source_lines,
                                            std::vector<ObjectLiteralInfo> *object_literals ,
                                            std::vector<ClassInfo> *classes ) {
    std::vector<SymInfo> out;

    // Build reverse map: namespace global name -> source file path
    // so walk_stmts can detect import bindings.
    StringMap ns_to_path;
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
                walk_stmts(fn->body->stmts, out, ns_to_path, source_lines, 0, object_literals, classes, fn->filename);
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
            while (num_end < fn->name.size() && std::isdigit(static_cast<unsigned char>(fn->name[num_end]))) {
                ++num_end;
            }
            if (num_end + 2 < fn->name.size() && fn->name[num_end] == '_' && fn->name[num_end + 1] == '_') {
                display_name = fn->name.substr(num_end + 2);
            }
        }

        int fn_col = resolve_identifier_col(source_lines, display_name, fn->line, fn->col, (fn->col > 1) ? static_cast<size_t>(fn->col - 1) : 0u);
        out.push_back({ display_name, build_func_sig(*fn, display_name), CK_Function, fn->line, fn_col, fn->filename });
        // Walk body for locals, always for non-mangled functions, and also for de-mangled (exported) functions defined in the current
        // document.
        const bool is_local_def = (display_name == fn->name) || (!doc_filename.empty() && fn->filename == doc_filename);
        if (is_local_def) {
            size_t fn_param_search_from = 0;
            if (fn->line >= 1 && fn->line <= static_cast<int>(source_lines.size())) {
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
                paramInfo.col = resolve_identifier_col(source_lines, param.name, fn->line, fn_col, fn_param_search_from);
                paramInfo.source_file = fn->filename;
                paramInfo.scope_fn_line = fn->line;
                paramInfo.inferred_type = infer_annotation_type(param.type.get());
                out.push_back(std::move(paramInfo));
            }
            if (fn->body) {
                walk_stmts(fn->body->stmts, out, ns_to_path, source_lines, fn->line, object_literals, classes, fn->filename);
            }
        }
    }

    for (const auto &[name, cd] : Parser::get_all_registered_classes()) {
        if (!cd) {
            continue;
        }

        for (const auto &method : cd->methods) {
            SymInfo method_info{ method.name,
                                 build_method_sig(method, cd->name),
                                 CK_Method,
                                 method.line,
                                 resolve_identifier_col(source_lines, method.name, method.line, method.col,
                                                        (method.col > 1) ? static_cast<size_t>(method.col - 1) : 0u),
                                 method.filename.empty() ? cd->filename : method.filename };
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

                SymInfo param_info{ param.name,
                                    param_detail,
                                    CK_Variable,
                                    method.line,
                                    resolve_identifier_col(source_lines, param.name, method.line, method.col,
                                                           [&source_lines, &method]() -> size_t {
                                                               if (method.line < 1 || method.line > static_cast<int>(source_lines.size())) {
                                                                   return 0u;
                                                               }
                                                               const std::string &sig = source_lines[method.line - 1];
                                                               size_t paren = sig.find('(');
                                                               return paren == std::string::npos ? 0u : paren;
                                                           }()),
                                    method.filename.empty() ? cd->filename : method.filename };
                param_info.scope_fn_line = method.line;
                param_info.inferred_type = infer_annotation_type(param.type.get());
                out.push_back(std::move(param_info));
            }

            if (method.body) {
                walk_stmts(method.body->stmts, out, ns_to_path, source_lines, method.line, object_literals, classes,
                           method.filename.empty() ? cd->filename : method.filename);
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
            out.push_back({ name, "type " + name + " = " + tdecl->alias_target->to_string(), CK_Struct, tdecl->line, tdecl->col });
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

    if (object_literals) {
        const size_t original_count = object_literals->size();
        for (const auto &sym : out) {
            if (sym.import_source_file.empty()) {
                continue;
            }
            for (size_t i = 0; i < original_count; ++i) {
                const auto &object = (*object_literals)[i];
                if (object.name == sym.import_export_name && object.source_file == sym.import_source_file) {
                    ObjectLiteralInfo alias = object;
                    alias.name = sym.name;
                    alias.decl_line = sym.line;
                    alias.scope_fn_line = sym.scope_fn_line;
                    alias.source_file = sym.source_file;
                    object_literals->push_back(std::move(alias));
                    break;
                }
            }
        }
    }
    return out;
}

std::vector<ClassInfo> collect_classes() {
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

//  Shared text / semantic-token helpers
// Split text into lines, stripping trailing \r from each line.
std::vector<std::string> lsp_split_lines(const std::string &text) {
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
int lsp_find_word_col(const std::string &src_line, const std::string &word, size_t from ) {
    auto is_id = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    size_t pos = from;
    while ((pos = src_line.find(word, pos)) != std::string::npos) {
        bool left_ok = (pos == 0) || !is_id(src_line[pos - 1]);
        size_t rend = pos + word.size();
        bool right_ok = (rend >= src_line.size()) || !is_id(src_line[rend]);
        if (left_ok && right_ok) {
            return static_cast<int>(pos) + 1; // 1-based
        }
        ++pos;
    }
    return -1;
}


// Text utilities

// Extracts the identifier token that overlaps with 0-based (line0, col0).
// Returns {token, start_col}.
std::pair<std::string, int> word_at(const std::string &text, int line0, int col0) {
    std::istringstream ss(text);
    std::string ln;
    int cur = 0;
    while (std::getline(ss, ln)) {
        if (cur == line0) {
            break;
        }
        ++cur;
    }
    if (cur != line0 || col0 < 0 || col0 > static_cast<int>(ln.size())) {
        return { "", col0 };
    }

    // identifier chars: [A-Za-z0-9_]
    auto is_ident = [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    int start = col0;
    while (start > 0 && is_ident(ln[start - 1])) {
        --start;
    }
    int end2 = col0;
    while (end2 < static_cast<int>(ln.size()) && is_ident(ln[end2])) {
        ++end2;
    }
    return { ln.substr(start, end2 - start), start };
}

// Scan backward from `sym_line_1based - 1` collecting consecutive // lines.
// Returns the comment text (newline-joined), or empty string if none.
std::string extract_doc_comment(const std::vector<std::string> &lines, int sym_line_1based) {
    if (sym_line_1based <= 1 || sym_line_1based > static_cast<int>(lines.size())) {
        return {};
    }

    std::vector<std::string> comment_lines;
    for (int i = sym_line_1based - 2; i >= 0; --i) {
        std::string_view sv = lines[i];
        size_t p = 0;
        while (p < sv.size() && (sv[p] == ' ' || sv[p] == '\t')) {
            ++p;
        }
        sv = sv.substr(p);
        if (sv.empty()) {
            break;
        }

        std::string_view content;
        if (sv.size() >= 2 && sv.substr(0, 2) == "//") {
            content = sv.substr(2);
        } else {
            break;
        }
        if (!content.empty() && content[0] == ' ') {
            content.remove_prefix(1);
        }
        comment_lines.emplace_back(content);
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

std::string extract_doc_comment(const std::string &text, int sym_line_1based) {
    if (sym_line_1based <= 1) {
        return {};
    }

    // Collect line-start offsets up to sym_line_1based lines
    std::vector<size_t> starts;
    starts.push_back(0);
    for (size_t i = 0; i < text.size() && static_cast<int>(starts.size() < sym_line_1based); ++i) {
        if (text[i] == '\n') {
            starts.push_back(i + 1);
        }
    }

    auto get_line = [&](int idx) -> std::string_view {
        if (idx < 0 || idx >= static_cast<int>(starts.size())) {
            return {};
        }
        size_t s = starts[idx];
        size_t e = (idx + 1 < static_cast<int>(starts.size()) ? starts[idx + 1] : text.size());
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
std::string receiver_at(const std::string &text, int line0, int word_start) {
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
    if (cur != line0 || word_start - 1 >= static_cast<int>(ln.size())) {
        return {};
    }
    if (ln[word_start - 1] != '.') {
        return {};
    }
    auto is_ident = [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; };
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
std::string import_string_at(const std::string &text, int line0, int col0, int &out_start, int &out_end) {
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
    for (int i = 0; i < static_cast<int>(ln.size()); ++i) {
        if (ln[i] != '"') {
            continue;
        }
        int str_start = i; // position of opening quote
        int j = i + 1;
        while (j < static_cast<int>(ln.size()) && ln[j] != '"') {
            if (ln[j] == '\\') {
                ++j; // skip escape
            }
            ++j;
        }
        if (j >= static_cast<int>(ln.size())) {
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

// Converts a filesystem path to a file:// URI.
std::string path_to_uri(const std::filesystem::path &p) {
    std::string s = p.generic_string();
    if (s.rfind("<std>/", 0) == 0) {
        const std::filesystem::path file = lsp_std_module_file(s.substr(6));
        if (file.empty()) {
            return {};
        }
        s = file.string();
    }
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
