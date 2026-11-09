// Interp-block pre-resolution (split out of parser.cpp).
#include "ast.h"
#include "parser_api.h"
#include "parser_internal.h"
#include "token.h"

#include <string>
#include <utility>
#include <vector>

using nari::FunctionPtr;

namespace Parser {

// pre-parse expression fragments in StringInterpolationExpr nodes so the
// runtime and bytecode compiler don't need to re-invoke the parser.

void ri_stmt(nari::Stmt *s, std::vector<ParseError> *errors);
void ri_block(nari::BlockStmt *blk, std::vector<ParseError> *errors);

void ri_expr(nari::Expr *e, std::vector<ParseError> *errors) {
    if (!e) {
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::StringInterpolation) {
        auto *sie = static_cast<nari::StringInterpolationExpr *>(e);
        if (sie->exprs.empty() && !sie->expr_sources.empty()) {
            for (const auto &src : sie->expr_sources) {
                std::string previous_filename = current_filename;
                set_source_filename(sie->filename);
                FuncList funcs;
                // TODO: kinda ugly way of doing interp slots
                // but this doesn't technically break anything, it's never exposed to the user, and it's free speed wise
                const std::string wrapped = "let __nari_interp_slot__ = (" + src + ");";
                if (errors) {
                    auto result = parse_program_recovering(wrapped);
                    for (auto &error : result.errors) {
                        errors->push_back(std::move(error));
                    }
                    funcs = std::move(result.functions);
                } else {
                    funcs = parse_program_from_source(wrapped);
                }
                set_source_filename(previous_filename);
                nari::ExprPtr parsed;
                if (funcs.size() >= 2 && funcs[1] && funcs[1]->body && !funcs[1]->body->stmts.empty()) {
                    if ((funcs[1]->body->stmts[0].get()) != nullptr &&
                        (funcs[1]->body->stmts[0].get())->stmt_kind == nari::StmtKind::VarDecl) {
                        auto *var_decl = static_cast<nari::VarDeclStmt *>(funcs[1]->body->stmts[0].get());
                        parsed = std::move(var_decl->initializerExpr);
                    }
                }
                sie->exprs.push_back(std::move(parsed));
            }
        }
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Binary) {
        auto *be = static_cast<nari::BinaryExpr *>(e);
        ri_expr(be->left.get(), errors);
        ri_expr(be->right.get(), errors);
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Unary) {
        auto *ue = static_cast<nari::UnaryExpr *>(e);
        ri_expr(ue->operand.get(), errors);
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Call) {
        auto *ce = static_cast<nari::CallExpr *>(e);
        ri_expr(ce->callee.get(), errors);
        for (auto &a : ce->args) {
            ri_expr(a.get(), errors);
        }
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::ArrayLiteral) {
        auto *ae = static_cast<nari::ArrayLiteralExpr *>(e);
        for (auto &el : ae->elements) {
            ri_expr(el.get(), errors);
        }
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::ObjectLiteral) {
        auto *oe = static_cast<nari::ObjectLiteralExpr *>(e);
        for (auto &kv : oe->entries) {
            ri_expr(kv.second.get(), errors);
        }
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Index) {
        auto *ie = static_cast<nari::IndexExpr *>(e);
        ri_expr(ie->object.get(), errors);
        ri_expr(ie->index.get(), errors);
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Member) {
        auto *me = static_cast<nari::MemberExpr *>(e);
        ri_expr(me->object.get(), errors);
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::New) {
        auto *ne = static_cast<nari::NewExpr *>(e);
        for (auto &a : ne->args) {
            ri_expr(a.get(), errors);
        }
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Ternary) {
        auto *te = static_cast<nari::TernaryExpr *>(e);
        ri_expr(te->condition.get(), errors);
        ri_expr(te->true_expr.get(), errors);
        ri_expr(te->false_expr.get(), errors);
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Function) {
        auto *fe = static_cast<nari::FunctionExpr *>(e);
        for (auto &p : fe->params) {
            ri_expr(p.default_value.get(), errors);
        }
        ri_block(fe->body.get(), errors);
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Spawn) {
        auto *se = static_cast<nari::SpawnExpr *>(e);
        ri_block(se->body.get(), errors);
        return;
    }
    if ((e) != nullptr && (e)->kind == nari::ExprKind::Match) {
        auto *me2 = static_cast<nari::MatchExpr *>(e);
        ri_expr(me2->scrutinee.get(), errors);
        for (auto &arm : me2->arms) {
            ri_expr(arm.body.get(), errors);
        }
        return;
    }
}

void ri_stmt(nari::Stmt *s, std::vector<ParseError> *errors) {
    if (!s) {
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Expr) {
        auto *es = static_cast<nari::ExprStmt *>(s);
        ri_expr(es->expr.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::VarDecl) {
        auto *vd = static_cast<nari::VarDeclStmt *>(s);
        ri_expr(vd->initializerExpr.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Assign) {
        auto *as = static_cast<nari::AssignStmt *>(s);
        ri_expr(as->value.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::IndexAssign) {
        auto *ia = static_cast<nari::IndexAssignStmt *>(s);
        ri_expr(ia->target.get(), errors);
        ri_expr(ia->value.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Return) {
        auto *rs = static_cast<nari::ReturnStmt *>(s);
        ri_expr(rs->value.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Block) {
        auto *blk = static_cast<nari::BlockStmt *>(s);
        ri_block(blk, errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::If) {
        auto *is = static_cast<nari::IfStmt *>(s);
        ri_expr(is->cond.get(), errors);
        ri_stmt(is->then_branch.get(), errors);
        ri_stmt(is->else_branch.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::While) {
        auto *ws = static_cast<nari::WhileStmt *>(s);
        ri_expr(ws->cond.get(), errors);
        ri_stmt(ws->body.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::For) {
        auto *fs = static_cast<nari::ForStmt *>(s);
        ri_stmt(fs->init.get(), errors);
        ri_expr(fs->cond.get(), errors);
        ri_stmt(fs->post.get(), errors);
        ri_stmt(fs->body.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::ForEach) {
        auto *fes = static_cast<nari::ForEachStmt *>(s);
        ri_expr(fes->iterable.get(), errors);
        ri_stmt(fes->body.get(), errors);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Switch) {
        auto *ss = static_cast<nari::SwitchStmt *>(s);
        ri_expr(ss->value.get(), errors);
        for (auto &c : ss->cases) {
            ri_expr(c.match.get(), errors);
            ri_block(c.body.get(), errors);
        }
        ri_block(ss->default_body.get(), errors);
        return;
    }
}

void ri_block(nari::BlockStmt *blk, std::vector<ParseError> *errors) {
    if (!blk) {
        return;
    }
    for (auto &s : blk->stmts) {
        ri_stmt(s.get(), errors);
    }
}

void resolve_interp_exprs(std::vector<FunctionPtr> &funcs, std::vector<ParseError> *errors) {
    for (auto &fn : funcs) {
        if (fn && fn->body) {
            ri_block(fn->body.get(), errors);
        }
    }
}

} // namespace Parser
