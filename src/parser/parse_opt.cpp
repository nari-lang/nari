/*
  parse_opt.cpp
  Parser class methods split verbatim from parser.cpp.
*/

#include "parser/parser.h"

namespace Parser {

// constant propagation: replaces immutable `let x = <literal>` references with the literal value, then re-folds.
using ConstMap = std::unordered_map<std::string, const nari::Expr *>;
using NameSet = std::unordered_set<std::string>;

ExprPtr Parser::clone_literal(const nari::Expr *src, const nari::Expr *loc) {
    ExprPtr out;

    if (src) {
        if (src->kind == nari::ExprKind::Number) {
            const auto *number_expr = static_cast<const nari::NumberExpr *>(src);
            out = number_expr->is_float ? std::make_unique<nari::NumberExpr>(number_expr->f)
                                        : std::make_unique<nari::NumberExpr>(number_expr->i);
        } else if (src->kind == nari::ExprKind::Bool) {
            const auto *be = static_cast<const nari::BoolExpr *>(src);
            out = std::make_unique<nari::BoolExpr>(be->value);
        } else if (src->kind == nari::ExprKind::String) {
            const auto *se = static_cast<const nari::StringExpr *>(src);
            out = std::make_unique<nari::StringExpr>(se->value);
        }
    }
    if (out && loc) {
        copy_loc(out.get(), loc);
    }
    return out;
}

// Remove candidates that are assigned to or re-declared in a nested scope.
void Parser::eliminate_mutated_stmt(const nari::Stmt *s, NameSet &candidates) {
    if (!s || candidates.empty()) {
        return;
    }
    switch (s->stmt_kind) {
        case nari::StmtKind::Assign: {
            const auto *as = static_cast<const nari::AssignStmt *>(s);
            candidates.erase(as->target);
            eliminate_mutated_expr(as->value.get(), candidates);
            return;
        }
        case nari::StmtKind::Expr:
            eliminate_mutated_expr(static_cast<const nari::ExprStmt *>(s)->expr.get(), candidates);
            return;
        case nari::StmtKind::VarDecl:
            eliminate_mutated_expr(static_cast<const nari::VarDeclStmt *>(s)->initializerExpr.get(), candidates);
            return;
        case nari::StmtKind::IndexAssign: {
            const auto *ia = static_cast<const nari::IndexAssignStmt *>(s);
            eliminate_mutated_expr(ia->target.get(), candidates);
            eliminate_mutated_expr(ia->value.get(), candidates);
            return;
        }
        case nari::StmtKind::Return:
            eliminate_mutated_expr(static_cast<const nari::ReturnStmt *>(s)->value.get(), candidates);
            return;
        case nari::StmtKind::Block: {
            const auto *blk = static_cast<const nari::BlockStmt *>(s);
            for (const auto &st : blk->stmts) {
                // Any VarDeclStmt inside a nested block potentially shadows an outer name.
                if (st && st->stmt_kind == nari::StmtKind::VarDecl) {
                    candidates.erase(static_cast<const nari::VarDeclStmt *>(st.get())->name);
                }
                eliminate_mutated_stmt(st.get(), candidates);
            }
            return;
        }
        case nari::StmtKind::If: {
            const auto *is = static_cast<const nari::IfStmt *>(s);
            eliminate_mutated_expr(is->cond.get(), candidates);
            eliminate_mutated_stmt(is->then_branch.get(), candidates);
            eliminate_mutated_stmt(is->else_branch.get(), candidates);
            return;
        }
        case nari::StmtKind::While: {
            const auto *ws = static_cast<const nari::WhileStmt *>(s);
            eliminate_mutated_expr(ws->cond.get(), candidates);
            eliminate_mutated_stmt(ws->body.get(), candidates);
            return;
        }
        case nari::StmtKind::For: {
            const auto *fs = static_cast<const nari::ForStmt *>(s);
            // for-loop init may introduce a new binding (like `for (let i = 0; ...)`)
            if (fs->init && fs->init->stmt_kind == nari::StmtKind::VarDecl) {
                candidates.erase(static_cast<const nari::VarDeclStmt *>(fs->init.get())->name);
            } else {
                eliminate_mutated_stmt(fs->init.get(), candidates);
            }
            eliminate_mutated_expr(fs->cond.get(), candidates);
            eliminate_mutated_stmt(fs->post.get(), candidates);
            eliminate_mutated_stmt(fs->body.get(), candidates);
            return;
        }
        case nari::StmtKind::ForEach: {
            const auto *fes = static_cast<const nari::ForEachStmt *>(s);
            candidates.erase(fes->var); // loop variable is a new binding
            eliminate_mutated_expr(fes->iterable.get(), candidates);
            eliminate_mutated_stmt(fes->body.get(), candidates);
            return;
        }
        case nari::StmtKind::Switch: {
            const auto *ss = static_cast<const nari::SwitchStmt *>(s);
            eliminate_mutated_expr(ss->value.get(), candidates);
            for (const auto &c : ss->cases) {
                eliminate_mutated_expr(c.match.get(), candidates);
                if (c.body) {
                    for (const auto &st : c.body->stmts) {
                        eliminate_mutated_stmt(st.get(), candidates);
                    }
                }
            }
            if (ss->default_body) {
                for (const auto &st : ss->default_body->stmts) {
                    eliminate_mutated_stmt(st.get(), candidates);
                }
            }
            return;
        }
        default:
            return;
    }
}

void Parser::eliminate_mutated_expr(const nari::Expr *e, NameSet &candidates) {
    if (!e || candidates.empty()) {
        return;
    }
    switch (e->kind) {
        case nari::ExprKind::Unary: {
            const auto *ue = static_cast<const nari::UnaryExpr *>(e);
            if (ue->op == "++" || ue->op == "--" || ue->op == "post++" || ue->op == "post--") {
                if (ue->operand && ue->operand->kind == nari::ExprKind::Ident) {
                    candidates.erase(static_cast<const nari::IdentExpr *>(ue->operand.get())->name);
                }
            }
            eliminate_mutated_expr(ue->operand.get(), candidates);
            return;
        }
        case nari::ExprKind::Binary: {
            const auto *be = static_cast<const nari::BinaryExpr *>(e);
            eliminate_mutated_expr(be->left.get(), candidates);
            eliminate_mutated_expr(be->right.get(), candidates);
            return;
        }
        case nari::ExprKind::Call: {
            const auto *ce = static_cast<const nari::CallExpr *>(e);
            eliminate_mutated_expr(ce->callee.get(), candidates);
            for (const auto &a : ce->args) {
                eliminate_mutated_expr(a.get(), candidates);
            }
            return;
        }
        case nari::ExprKind::ArrayLiteral: {
            const auto *ae = static_cast<const nari::ArrayLiteralExpr *>(e);
            for (const auto &el : ae->elements) {
                eliminate_mutated_expr(el.get(), candidates);
            }
            return;
        }
        case nari::ExprKind::ObjectLiteral: {
            const auto *oe = static_cast<const nari::ObjectLiteralExpr *>(e);
            for (const auto &kv : oe->entries) {
                eliminate_mutated_expr(kv.second.get(), candidates);
            }
            return;
        }
        case nari::ExprKind::Index: {
            const auto *ie = static_cast<const nari::IndexExpr *>(e);
            eliminate_mutated_expr(ie->object.get(), candidates);
            eliminate_mutated_expr(ie->index.get(), candidates);
            return;
        }
        case nari::ExprKind::Member:
            eliminate_mutated_expr(static_cast<const nari::MemberExpr *>(e)->object.get(), candidates);
            return;
        case nari::ExprKind::New: {
            const auto *ne = static_cast<const nari::NewExpr *>(e);
            for (const auto &a : ne->args) {
                eliminate_mutated_expr(a.get(), candidates);
            }
            return;
        }
        case nari::ExprKind::Ternary: {
            const auto *te = static_cast<const nari::TernaryExpr *>(e);
            eliminate_mutated_expr(te->condition.get(), candidates);
            eliminate_mutated_expr(te->true_expr.get(), candidates);
            eliminate_mutated_expr(te->false_expr.get(), candidates);
            return;
        }
        // Scan closures and spawn blocks for assignments to outer variables.
        case nari::ExprKind::Function: {
            const auto *fe = static_cast<const nari::FunctionExpr *>(e);
            if (fe->body) {
                for (const auto &st : fe->body->stmts) {
                    eliminate_mutated_stmt(st.get(), candidates);
                }
            }
            return;
        }
        case nari::ExprKind::Spawn: {
            const auto *se = static_cast<const nari::SpawnExpr *>(e);
            if (se->body) {
                for (const auto &st : se->body->stmts) {
                    eliminate_mutated_stmt(st.get(), candidates);
                }
            }
            return;
        }
        default:
            return;
    }
}

// subst_fold: substitute known constants into an expression and re-fold.
ExprPtr Parser::subst_fold(ExprPtr expr, const ConstMap &m) {
    if (!expr || m.empty()) {
        return expr;
    }
    if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::Ident) {
        auto *id = static_cast<nari::IdentExpr *>(expr.get());
        auto it = m.find(id->name);
        if (it != m.end()) {
            auto lit = clone_literal(it->second, id);
            if (lit) {
                return lit;
            }
        }
        return expr;
    }
    if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::Binary) {
        auto *be = static_cast<nari::BinaryExpr *>(expr.get());
        be->left = subst_fold(std::move(be->left), m);
        be->right = subst_fold(std::move(be->right), m);
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::Unary) {
        auto *ue = static_cast<nari::UnaryExpr *>(expr.get());
        ue->operand = subst_fold(std::move(ue->operand), m);
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::Call) {
        auto *ce = static_cast<nari::CallExpr *>(expr.get());
        // Keep identifier callees intact so runtime call diagnostics retain
        // the source-level name instead of seeing only a folded literal.
        if ((ce->callee.get()) == nullptr || (ce->callee.get())->kind != nari::ExprKind::Ident) {
            ce->callee = subst_fold(std::move(ce->callee), m);
        }
        for (auto &a : ce->args) {
            a = subst_fold(std::move(a), m);
        }
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::ArrayLiteral) {
        auto *ae = static_cast<nari::ArrayLiteralExpr *>(expr.get());
        for (auto &el : ae->elements) {
            el = subst_fold(std::move(el), m);
        }
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::ObjectLiteral) {
        auto *oe = static_cast<nari::ObjectLiteralExpr *>(expr.get());
        for (auto &kv : oe->entries) {
            kv.second = subst_fold(std::move(kv.second), m);
        }
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::Index) {
        auto *ie = static_cast<nari::IndexExpr *>(expr.get());
        ie->object = subst_fold(std::move(ie->object), m);
        ie->index = subst_fold(std::move(ie->index), m);
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::Member) {
        auto *me = static_cast<nari::MemberExpr *>(expr.get());
        me->object = subst_fold(std::move(me->object), m);
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::New) {
        auto *ne = static_cast<nari::NewExpr *>(expr.get());
        for (auto &a : ne->args) {
            a = subst_fold(std::move(a), m);
        }
    } else if ((expr.get()) != nullptr && (expr.get())->kind == nari::ExprKind::Ternary) {
        auto *te = static_cast<nari::TernaryExpr *>(expr.get());
        te->condition = subst_fold(std::move(te->condition), m);
        te->true_expr = subst_fold(std::move(te->true_expr), m);
        te->false_expr = subst_fold(std::move(te->false_expr), m);
    }
    return fold_expr(std::move(expr));
}

void Parser::subst_stmt(nari::Stmt *s, const ConstMap &m) {
    if (!s || m.empty()) {
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Expr) {
        auto *es = static_cast<nari::ExprStmt *>(s);
        es->expr = subst_fold(std::move(es->expr), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::VarDecl) {
        auto *vd = static_cast<nari::VarDeclStmt *>(s);
        // do not substitute the init of a variable that is itself a const candidate.
        if (!m.count(vd->name) && vd->initializerExpr) {
            vd->initializerExpr = subst_fold(std::move(vd->initializerExpr), m);
        }
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Assign) {
        auto *as = static_cast<nari::AssignStmt *>(s);
        as->value = subst_fold(std::move(as->value), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::IndexAssign) {
        auto *ia = static_cast<nari::IndexAssignStmt *>(s);
        ia->target = subst_fold(std::move(ia->target), m);
        ia->value = subst_fold(std::move(ia->value), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Return) {
        auto *rs = static_cast<nari::ReturnStmt *>(s);
        rs->value = subst_fold(std::move(rs->value), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Block) {
        auto *blk = static_cast<nari::BlockStmt *>(s);
        subst_block(blk, m);
        return; // passes copy so nested re-decls are handled
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::If) {
        auto *is = static_cast<nari::IfStmt *>(s);
        is->cond = subst_fold(std::move(is->cond), m);
        subst_stmt(is->then_branch.get(), m);
        subst_stmt(is->else_branch.get(), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::While) {
        auto *ws = static_cast<nari::WhileStmt *>(s);
        ws->cond = subst_fold(std::move(ws->cond), m);
        subst_stmt(ws->body.get(), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::For) {
        auto *fs = static_cast<nari::ForStmt *>(s);
        subst_stmt(fs->init.get(), m);
        fs->cond = subst_fold(std::move(fs->cond), m);
        subst_stmt(fs->post.get(), m);
        subst_stmt(fs->body.get(), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::ForEach) {
        auto *fes = static_cast<nari::ForEachStmt *>(s);
        fes->iterable = subst_fold(std::move(fes->iterable), m);
        subst_stmt(fes->body.get(), m);
        return;
    }
    if ((s) != nullptr && (s)->stmt_kind == nari::StmtKind::Switch) {
        auto *ss = static_cast<nari::SwitchStmt *>(s);
        ss->value = subst_fold(std::move(ss->value), m);
        for (auto &c : ss->cases) {
            c.match = subst_fold(std::move(c.match), m);
            subst_block(c.body.get(), m);
        }
        subst_block(ss->default_body.get(), m);
        return;
    }
}

void Parser::subst_block(nari::BlockStmt *blk, ConstMap m) {
    if (!blk || m.empty()) {
        return;
    }
    for (auto &s : blk->stmts) {
        if (m.empty()) {
            break;
        }
        if ((s.get()) != nullptr && (s.get())->stmt_kind == nari::StmtKind::VarDecl) {
            auto *vd = static_cast<nari::VarDeclStmt *>(s.get());
            // substitute in the init first, then stop folding this name.
            if (!m.count(vd->name) && vd->initializerExpr) {
                vd->initializerExpr = subst_fold(std::move(vd->initializerExpr), m);
            }
            m.erase(vd->name);
            continue;
        }
        subst_stmt(s.get(), m);
    }
}

void Parser::propagate_consts(nari::BlockStmt *blk) {
    if (!blk) {
        return;
    }
    NameSet candidates;
    for (const auto &s : blk->stmts) {
        if ((s.get()) != nullptr && (s.get())->stmt_kind == nari::StmtKind::VarDecl) {
            const auto *vd = static_cast<const nari::VarDeclStmt *>(s.get());
            if (vd->destructure_kind == nari::DestructureKind::None && !vd->name.empty() && !vd->is_global && vd->initializerExpr &&
                literal_any(vd->initializerExpr.get())) {
                candidates.insert(vd->name);
            }
        }
    }
    if (candidates.empty()) {
        return;
    }
    for (const auto &s : blk->stmts) {
        eliminate_mutated_stmt(s.get(), candidates);
    }
    ConstMap consts;
    for (const auto &s : blk->stmts) {
        if ((s.get()) != nullptr && (s.get())->stmt_kind == nari::StmtKind::VarDecl) {
            const auto *vd = static_cast<const nari::VarDeclStmt *>(s.get());
            if (vd->destructure_kind == nari::DestructureKind::None && !vd->name.empty() && !vd->is_global && vd->initializerExpr &&
                literal_any(vd->initializerExpr.get()) && candidates.count(vd->name)) {
                consts[vd->name] = vd->initializerExpr.get();
            }
        }
    }
    if (consts.empty()) {
        return;
    }
    // substitute and fold.
    for (auto &s : blk->stmts) {
        subst_stmt(s.get(), consts);
    }
    // Re-run loop optimiser: newly constant loop bounds may now be collapsible.
    optimize_block(blk);
}

bool Parser::get_loop_init(nari::Stmt *init, std::string &var, int64_t &start) {
    if ((init) != nullptr && (init)->stmt_kind == nari::StmtKind::VarDecl) {
        auto *vd = static_cast<nari::VarDeclStmt *>(init);
        if (!vd->initializerExpr) {
            return false;
        }
        int64_t v = 0;
        if (!literal_int(vd->initializerExpr.get(), v)) {
            return false;
        }
        var = vd->name;
        start = v;
        return true;
    }
    if ((init) != nullptr && (init)->stmt_kind == nari::StmtKind::Assign) {
        auto *as = static_cast<nari::AssignStmt *>(init);
        if (!as->value) {
            return false;
        }
        int64_t v = 0;
        if (!literal_int(as->value.get(), v)) {
            return false;
        }
        var = as->target;
        start = v;
        return true;
    }
    return false;
}

bool Parser::get_loop_cond(nari::Expr *cond, const std::string &var, int64_t &end, bool &inclusive) {
    auto *be = ((cond) != nullptr && (cond)->kind == nari::ExprKind::Binary ? static_cast<nari::BinaryExpr *>(cond) : nullptr);
    if (!be || !be->left || !be->right) {
        return false;
    }
    if (be->op != "<" && be->op != "<=") {
        return false;
    }
    auto *id =
        ((be->left.get()) != nullptr && (be->left.get())->kind == nari::ExprKind::Ident ? static_cast<nari::IdentExpr *>(be->left.get())
                                                                                        : nullptr);
    if (!id || id->name != var) {
        return false;
    }
    int64_t v = 0;
    if (!literal_int(be->right.get(), v)) {
        return false;
    }
    inclusive = (be->op == "<=");
    end = v;
    return true;
}

bool Parser::get_loop_step(nari::Stmt *post, const std::string &var, int64_t &step) {
    if (!post) {
        return false;
    }
    if ((post) != nullptr && (post)->stmt_kind == nari::StmtKind::Assign) {
        auto *as = static_cast<nari::AssignStmt *>(post);
        if (as->target != var || !as->value) {
            return false;
        }
        auto *be =
            ((as->value.get()) != nullptr && (as->value.get())->kind == nari::ExprKind::Binary
                 ? static_cast<nari::BinaryExpr *>(as->value.get())
                 : nullptr);
        if (!be || !be->left || !be->right) {
            return false;
        }
        auto *id =
            ((be->left.get()) != nullptr && (be->left.get())->kind == nari::ExprKind::Ident ? static_cast<nari::IdentExpr *>(be->left.get())
                                                                                            : nullptr);
        if (!id || id->name != var) {
            return false;
        }
        int64_t v = 0;
        if (!literal_int(be->right.get(), v)) {
            return false;
        }
        if (be->op == "+") {
            step = v;
            return true;
        }
        if (be->op == "-") {
            step = -v;
            return true;
        }
        return false;
    }
    if ((post) != nullptr && (post)->stmt_kind == nari::StmtKind::Expr) {
        auto *es = static_cast<nari::ExprStmt *>(post);
        auto *ue =
            ((es->expr.get()) != nullptr && (es->expr.get())->kind == nari::ExprKind::Unary ? static_cast<nari::UnaryExpr *>(es->expr.get())
                                                                                            : nullptr);
        if (!ue) {
            return false;
        }
        auto *id =
            ((ue->operand.get()) != nullptr && (ue->operand.get())->kind == nari::ExprKind::Ident
                 ? static_cast<nari::IdentExpr *>(ue->operand.get())
                 : nullptr);
        if (!id || id->name != var) {
            return false;
        }
        if (ue->op == "post++") {
            step = 1;
            return true;
        }
        if (ue->op == "post--") {
            step = -1;
            return true;
        }
    }
    return false;
}

bool Parser::get_body_increment(nari::Stmt *body, std::string &target, int64_t &inc) {
    auto *blk = ((body) != nullptr && (body)->stmt_kind == nari::StmtKind::Block ? static_cast<nari::BlockStmt *>(body) : nullptr);
    if (!blk || blk->stmts.size() != 1) {
        return false;
    }

    if ((blk->stmts[0].get()) != nullptr && (blk->stmts[0].get())->stmt_kind == nari::StmtKind::Assign) {
        auto *as = static_cast<nari::AssignStmt *>(blk->stmts[0].get());
        auto *be =
            ((as->value.get()) != nullptr && (as->value.get())->kind == nari::ExprKind::Binary
                 ? static_cast<nari::BinaryExpr *>(as->value.get())
                 : nullptr);
        if (!be || !be->left || !be->right) {
            return false;
        }
        auto *id =
            ((be->left.get()) != nullptr && (be->left.get())->kind == nari::ExprKind::Ident ? static_cast<nari::IdentExpr *>(be->left.get())
                                                                                            : nullptr);
        if (!id || id->name != as->target) {
            return false;
        }
        int64_t v = 0;
        if (!literal_int(be->right.get(), v)) {
            return false;
        }
        if (be->op == "+") {
            target = as->target;
            inc = v;
            return true;
        }
        if (be->op == "-") {
            target = as->target;
            inc = -v;
            return true;
        }
        return false;
    }

    if ((blk->stmts[0].get()) != nullptr && (blk->stmts[0].get())->stmt_kind == nari::StmtKind::Expr) {
        auto *es = static_cast<nari::ExprStmt *>(blk->stmts[0].get());
        auto *ue =
            ((es->expr.get()) != nullptr && (es->expr.get())->kind == nari::ExprKind::Unary ? static_cast<nari::UnaryExpr *>(es->expr.get())
                                                                                            : nullptr);
        if (!ue) {
            return false;
        }
        auto *id =
            ((ue->operand.get()) != nullptr && (ue->operand.get())->kind == nari::ExprKind::Ident
                 ? static_cast<nari::IdentExpr *>(ue->operand.get())
                 : nullptr);
        if (!id) {
            return false;
        }
        if (ue->op == "post++") {
            target = id->name;
            inc = 1;
            return true;
        }
        if (ue->op == "post--") {
            target = id->name;
            inc = -1;
            return true;
        }
    }

    return false;
}

std::unique_ptr<nari::Stmt> Parser::try_optimize_for(nari::ForStmt *fs, nari::Stmt *prev) {
    if (!fs || !fs->init || !fs->cond || !fs->post || !fs->body) {
        return nullptr;
    }

    std::string loop_var;
    int64_t start = 0;
    if (!get_loop_init(fs->init.get(), loop_var, start)) {
        return nullptr;
    }

    int64_t end = 0;
    bool inclusive = false;
    if (!get_loop_cond(fs->cond.get(), loop_var, end, inclusive)) {
        return nullptr;
    }

    int64_t step = 0;
    if (!get_loop_step(fs->post.get(), loop_var, step)) {
        return nullptr;
    }

    if (step <= 0) {
        return nullptr;
    }
    if (step != 1) {
        return nullptr;
    }

    // check if body is empty - optimize to just setting the loop variable to the end value
    auto *blk =
        ((fs->body.get()) != nullptr && (fs->body.get())->stmt_kind == nari::StmtKind::Block
             ? static_cast<nari::BlockStmt *>(fs->body.get())
             : nullptr);
    if (blk && blk->stmts.empty()) {
        int64_t final_value = inclusive ? end + 1 : end;
        auto val_expr = std::make_unique<nari::NumberExpr>(final_value);
        val_expr->line = fs->line;
        val_expr->col = fs->col;
        val_expr->filename = fs->filename;

        auto assign = std::make_unique<nari::AssignStmt>(loop_var, std::move(val_expr));
        assign->line = fs->line;
        assign->col = fs->col;
        assign->filename = fs->filename;
        return assign;
    }

    std::string target;
    int64_t inc = 0;
    if (!get_body_increment(fs->body.get(), target, inc)) {
        return nullptr;
    }
    if (target == loop_var) {
        return nullptr;
    }

    if (!prev) {
        return nullptr;
    }
    bool prev_numeric = false;
    if ((prev) != nullptr && (prev)->stmt_kind == nari::StmtKind::VarDecl) {
        auto *vd = static_cast<nari::VarDeclStmt *>(prev);
        int64_t v = 0;
        if (vd->name == target && vd->initializerExpr && literal_int(vd->initializerExpr.get(), v)) {
            prev_numeric = true;
        }
    } else if ((prev) != nullptr && (prev)->stmt_kind == nari::StmtKind::Assign) {
        auto *as = static_cast<nari::AssignStmt *>(prev);
        int64_t v = 0;
        if (as->target == target && as->value && literal_int(as->value.get(), v)) {
            prev_numeric = true;
        }
    }
    if (!prev_numeric) {
        return nullptr;
    }

    int64_t diff;
    if (sub_overflow_i64(end, start, &diff)) {
        return nullptr;
    }
    if (inclusive) {
        if (add_overflow_i64(diff, INT64_C(1), &diff)) {
            return nullptr;
        }
    }
    if (diff <= 0) {
        diff = 0;
    }
    int64_t delta;
    if (mul_overflow_i64(diff, inc, &delta)) {
        return nullptr;
    }
    auto left = std::make_unique<nari::IdentExpr>(target);
    auto right = std::make_unique<nari::NumberExpr>(delta);
    auto be = std::make_unique<nari::BinaryExpr>("+", std::move(left), std::move(right));
    be->line = fs->line;
    be->col = fs->col;
    be->filename = fs->filename;
    auto assign = std::make_unique<nari::AssignStmt>(target, std::move(be));
    assign->line = fs->line;
    assign->col = fs->col;
    assign->filename = fs->filename;
    return assign;
}

bool Parser::literal_int(const nari::Expr *e, int64_t &out) {
    if (e && e->kind == nari::ExprKind::Number) {
        const auto *ne = static_cast<const nari::NumberExpr *>(e);
        if (!ne->is_float) {
            out = ne->i;
            return true;
        }
    }
    return false;
}

bool Parser::literal_float(const nari::Expr *e, double &out) {
    if (e && e->kind == nari::ExprKind::Number) {
        const auto *ne = static_cast<const nari::NumberExpr *>(e);
        if (ne->is_float) {
            out = ne->f;
            return true;
        }
    }
    return false;
}

double Parser::number_to_double(const nari::NumberExpr *ne) {
    if (!ne) {
        return 0.0;
    }
    if (ne->is_float) {
        return ne->f;
    }
    return static_cast<double>(ne->i);
}

bool Parser::literal_bool(const nari::Expr *e, bool &out) {
    if (e && e->kind == nari::ExprKind::Bool) {
        const auto *be = static_cast<const nari::BoolExpr *>(e);
        out = be->value;
        return true;
    }
    return false;
}

bool Parser::literal_string(const nari::Expr *e, std::string &out) {
    if (e && e->kind == nari::ExprKind::String) {
        const auto *se = static_cast<const nari::StringExpr *>(e);
        out = se->value;
        return true;
    }
    return false;
}

bool Parser::literal_any(const nari::Expr *e) {
    return e && (e->kind == nari::ExprKind::Number || e->kind == nari::ExprKind::String || e->kind == nari::ExprKind::Bool);
}

std::string Parser::literal_to_string(const nari::Expr *e) {
    if (!e) {
        return "";
    }
    if (e->kind == nari::ExprKind::String) {
        const auto *se = static_cast<const nari::StringExpr *>(e);
        return se->value;
    }
    if (e->kind == nari::ExprKind::Bool) {
        const auto *be = static_cast<const nari::BoolExpr *>(e);
        return be->value ? "true" : "false";
    }
    if (e->kind == nari::ExprKind::Number) {
        const auto *ne = static_cast<const nari::NumberExpr *>(e);
        if (ne->is_float) {
            return std::to_string(ne->f);
        } else {
            return std::to_string(ne->i);
        }
    }
    return "";
}

void Parser::copy_loc(nari::Expr *dst, const nari::Expr *src) {
    if (!dst || !src) {
        return;
    }
    dst->line = src->line;
    dst->col = src->col;
    dst->filename = src->filename;
}

ExprPtr Parser::fold_expr(ExprPtr expr) {
    if (!expr) {
        return expr;
    }

    if (expr->kind == nari::ExprKind::Unary) {
        auto *ue = static_cast<nari::UnaryExpr *>(expr.get());
        ue->operand = fold_expr(std::move(ue->operand));
        if (!ue->operand) {
            return expr;
        }
        int64_t ni = 0;
        double nf = 0.0;
        bool b = false;
        if (ue->op == "neg" && literal_int(ue->operand.get(), ni)) {
            auto result = std::make_unique<nari::NumberExpr>(-ni);
            copy_loc(result.get(), ue);
            return result;
        }
        // int48 min is only writable as -(2^47), and 2^47 on its own is out of int48
        // range so the literal was promoted to float. Negate the retained exact
        // integer instead of the double: the constructor narrows it back to an int
        // when the negated value fits, so -140737488355328 stays an int.
        if (ue->op == "neg" && ue->operand->kind == nari::ExprKind::Number) {
            const auto *ne = static_cast<const nari::NumberExpr *>(ue->operand.get());
            if (ne->promoted_from_int) {
                auto result = std::make_unique<nari::NumberExpr>(-ne->i);
                copy_loc(result.get(), ue);
                return result;
            }
        }
        if (ue->op == "+" && literal_int(ue->operand.get(), ni)) {
            auto result = std::make_unique<nari::NumberExpr>(ni);
            copy_loc(result.get(), ue);
            return result;
        }
        if (ue->op == "+" && literal_float(ue->operand.get(), nf)) {
            auto result = std::make_unique<nari::NumberExpr>(nf);
            copy_loc(result.get(), ue);
            return result;
        }
        if (ue->op == "!" && literal_bool(ue->operand.get(), b)) {
            auto result = std::make_unique<nari::BoolExpr>(!b);
            copy_loc(result.get(), ue);
            return result;
        }
        return expr;
    }

    if (expr->kind == nari::ExprKind::Binary) {
        auto *binaryExpr = static_cast<nari::BinaryExpr *>(expr.get());
        binaryExpr->left = fold_expr(std::move(binaryExpr->left));
        binaryExpr->right = fold_expr(std::move(binaryExpr->right));
        if (!binaryExpr->left || !binaryExpr->right) {
            return expr;
        }

        int64_t li = 0, ri = 0;
        double lf = 0.0, rf = 0.0;
        bool lb = false, rb = false;
        std::string ls, rs;

        bool lint = literal_int(binaryExpr->left.get(), li);
        bool rint = literal_int(binaryExpr->right.get(), ri);
        bool lfloat = literal_float(binaryExpr->left.get(), lf);
        bool rfloat = literal_float(binaryExpr->right.get(), rf);
        bool lbool = literal_bool(binaryExpr->left.get(), lb);
        bool rbool = literal_bool(binaryExpr->right.get(), rb);
        bool lstr = literal_string(binaryExpr->left.get(), ls);
        bool rstr = literal_string(binaryExpr->right.get(), rs);

        const std::string &op = binaryExpr->op;
        if (lint && rint) {
            if (op == "+") {
                int64_t v;
                if (add_overflow_i64(li, ri, &v)) {
                    return expr;
                }
                auto ne = std::make_unique<nari::NumberExpr>(v);
                copy_loc(ne.get(), binaryExpr);
                return ne;
            }
            if (op == "-") {
                int64_t v;
                if (sub_overflow_i64(li, ri, &v)) {
                    return expr;
                }
                auto ne = std::make_unique<nari::NumberExpr>(v);
                copy_loc(ne.get(), binaryExpr);
                return ne;
            }
            if (op == "*") {
                int64_t v;
                if (mul_overflow_i64(li, ri, &v)) {
                    return expr;
                }
                auto ne = std::make_unique<nari::NumberExpr>(v);
                copy_loc(ne.get(), binaryExpr);
                return ne;
            }
            if (op == "/") {
                double val = (ri == 0) ? std::numeric_limits<double>::quiet_NaN() : (static_cast<double>(li) / static_cast<double>(ri));
                auto numExpr = std::make_unique<nari::NumberExpr>(val);
                copy_loc(numExpr.get(), binaryExpr);
                return numExpr;
            }
            if (op == "%") {
                if (ri == 0) {
                    auto result = std::make_unique<nari::NumberExpr>(std::numeric_limits<double>::quiet_NaN());
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                // INT_MIN % -1 is UB and SIGFPEs on x86 idiv. Match runtime semantics.
                if (li == std::numeric_limits<int64_t>::min() && ri == -1) {
                    auto result = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(0));
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                auto result = std::make_unique<nari::NumberExpr>(li % ri);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "**") {
                if (ri < 0) {
                    auto result = std::make_unique<nari::NumberExpr>(std::pow(static_cast<double>(li), static_cast<double>(ri)));
                    copy_loc(result.get(), binaryExpr);
                    return result;
                }
                uint64_t exp = static_cast<uint64_t>(ri);
                int64_t base = li;
                int64_t total = 1;
                while (exp > 0) {
                    if (exp & 1ULL) {
                        if (mul_overflow_i64(total, base, &total)) {
                            return expr;
                        }
                    }
                    if (exp > 1) {
                        if (mul_overflow_i64(base, base, &base)) {
                            return expr;
                        }
                    }
                    exp >>= 1ULL;
                }
                auto result = std::make_unique<nari::NumberExpr>(static_cast<int64_t>(total));
                copy_loc(result.get(), binaryExpr);
                return result;
            }
        }

        if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
            double l =
                lfloat ? lf
                       : (lint ? static_cast<double>(li) : number_to_double(static_cast<const nari::NumberExpr *>(binaryExpr->left.get())));
            double r = rfloat ? rf
                              : (rint ? static_cast<double>(ri)
                                      : number_to_double(static_cast<const nari::NumberExpr *>(binaryExpr->right.get())));
            if (op == "+") {
                auto result = std::make_unique<nari::NumberExpr>(l + r);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "-") {
                auto result = std::make_unique<nari::NumberExpr>(l - r);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "/") {
                double v = (r == 0.0) ? std::numeric_limits<double>::quiet_NaN() : (l / r);
                auto result = std::make_unique<nari::NumberExpr>(v);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "%") {
                double v = (r == 0.0) ? std::numeric_limits<double>::quiet_NaN() : std::fmod(l, r);
                auto result = std::make_unique<nari::NumberExpr>(v);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "**") {
                auto result = std::make_unique<nari::NumberExpr>(std::pow(l, r));
                copy_loc(result.get(), binaryExpr);
                return result;
            }
        }

        // Only fold pairs of the same type class. Value::values_equal() reports a
        // string and a number as not equal, so folding a mixed pair through
        // literal_to_string() made `1 == "1"` true here but false at run time.
        if (op == "==" || op == "!=") {
            bool eq = false;
            if (lint && rint) {
                eq = (li == ri);
            } else if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
                double l = lfloat ? lf
                                  : (lint ? static_cast<double>(li)
                                          : number_to_double(static_cast<const nari::NumberExpr *>(binaryExpr->left.get())));
                double r = rfloat ? rf
                                  : (rint ? static_cast<double>(ri)
                                          : number_to_double(static_cast<const nari::NumberExpr *>(binaryExpr->right.get())));
                eq = std::fabs(l - r) < 1e-12;
            } else if (lbool && rbool) {
                eq = (lb == rb);
            } else if (lstr && rstr) {
                eq = (literal_to_string(binaryExpr->left.get()) == literal_to_string(binaryExpr->right.get()));
            } else {
                return expr;
            }
            auto result = std::make_unique<nari::BoolExpr>(op == "==" ? eq : !eq);
            copy_loc(result.get(), binaryExpr);
            return result;
        }

        if (lint && rint) {
            if (op == "<") {
                auto result = std::make_unique<nari::BoolExpr>(li < ri);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == ">") {
                auto result = std::make_unique<nari::BoolExpr>(li > ri);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "<=") {
                auto result = std::make_unique<nari::BoolExpr>(li <= ri);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == ">=") {
                auto result = std::make_unique<nari::BoolExpr>(li >= ri);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
        }

        if ((lfloat || rfloat) && (lint || lfloat) && (rint || rfloat)) {
            double l =
                lfloat ? lf
                       : (lint ? static_cast<double>(li) : number_to_double(static_cast<const nari::NumberExpr *>(binaryExpr->left.get())));
            double r = rfloat ? rf
                              : (rint ? static_cast<double>(ri)
                                      : number_to_double(static_cast<const nari::NumberExpr *>(binaryExpr->right.get())));
            // Same 1e-12 tolerance as Value::compare_ordered(), so a folded
            // float comparison agrees with the same comparison at run time.
            const bool feq = std::fabs(l - r) < 1e-12;
            if (op == "<") {
                auto result = std::make_unique<nari::BoolExpr>(!feq && l < r);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == ">") {
                auto result = std::make_unique<nari::BoolExpr>(!feq && l > r);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "<=") {
                auto result = std::make_unique<nari::BoolExpr>(feq || l < r);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == ">=") {
                auto result = std::make_unique<nari::BoolExpr>(feq || l > r);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
        }

        if (lbool && rbool) {
            if (op == "&&") {
                auto result = std::make_unique<nari::BoolExpr>(lb && rb);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
            if (op == "||") {
                auto result = std::make_unique<nari::BoolExpr>(lb || rb);
                copy_loc(result.get(), binaryExpr);
                return result;
            }
        }

        return expr;
    }

    if (expr->kind == nari::ExprKind::Ternary) {
        auto *te = static_cast<nari::TernaryExpr *>(expr.get());
        te->condition = fold_expr(std::move(te->condition));
        te->true_expr = fold_expr(std::move(te->true_expr));
        te->false_expr = fold_expr(std::move(te->false_expr));
        bool cond = false;
        if (literal_bool(te->condition.get(), cond)) {
            return cond ? std::move(te->true_expr) : std::move(te->false_expr);
        }
        return expr;
    }

    if (expr->kind == nari::ExprKind::Call) {
        auto *ce = static_cast<nari::CallExpr *>(expr.get());
        ce->callee = fold_expr(std::move(ce->callee));
        for (auto &arg : ce->args) {
            arg = fold_expr(std::move(arg));
        }
        return expr;
    }

    if (expr->kind == nari::ExprKind::ArrayLiteral) {
        auto *ae = static_cast<nari::ArrayLiteralExpr *>(expr.get());
        for (auto &el : ae->elements) {
            el = fold_expr(std::move(el));
        }
        return expr;
    }

    if (expr->kind == nari::ExprKind::ObjectLiteral) {
        auto *oe = static_cast<nari::ObjectLiteralExpr *>(expr.get());
        for (auto &entry : oe->entries) {
            entry.second = fold_expr(std::move(entry.second));
        }
        return expr;
    }

    if (expr->kind == nari::ExprKind::Index) {
        auto *ie = static_cast<nari::IndexExpr *>(expr.get());
        ie->object = fold_expr(std::move(ie->object));
        ie->index = fold_expr(std::move(ie->index));
        return expr;
    }

    if (expr->kind == nari::ExprKind::Member) {
        auto *me = static_cast<nari::MemberExpr *>(expr.get());
        me->object = fold_expr(std::move(me->object));
        return expr;
    }

    return expr;
}

} // namespace Parser
