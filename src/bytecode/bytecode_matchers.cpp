#include "bytecode_matchers.h"

#include <cstddef>
#include <string>

using namespace nari;

namespace nari {
namespace bytecode {

// Kind-checked downcasts. The AST nodes set kind/stmt_kind in their
// constructors, so these match by tag without RTTI.
static const VarDeclStmt *as_var_decl(const Stmt *s) {
    return s != nullptr && s->stmt_kind == StmtKind::VarDecl ? static_cast<const VarDeclStmt *>(s) : nullptr;
}
static const ReturnStmt *as_return(const Stmt *s) {
    return s != nullptr && s->stmt_kind == StmtKind::Return ? static_cast<const ReturnStmt *>(s) : nullptr;
}
static const BlockStmt *as_block(const Stmt *s) {
    return s != nullptr && s->stmt_kind == StmtKind::Block ? static_cast<const BlockStmt *>(s) : nullptr;
}
static const IfStmt *as_if(const Stmt *s) {
    return s != nullptr && s->stmt_kind == StmtKind::If ? static_cast<const IfStmt *>(s) : nullptr;
}
static const IndexAssignStmt *as_index_assign(const Stmt *s) {
    return s != nullptr && s->stmt_kind == StmtKind::IndexAssign ? static_cast<const IndexAssignStmt *>(s) : nullptr;
}
static const CallExpr *as_call(const Expr *e) {
    return e != nullptr && e->kind == ExprKind::Call ? static_cast<const CallExpr *>(e) : nullptr;
}
static const MemberExpr *as_member(const Expr *e) {
    return e != nullptr && e->kind == ExprKind::Member ? static_cast<const MemberExpr *>(e) : nullptr;
}
static const BinaryExpr *as_binary(const Expr *e) {
    return e != nullptr && e->kind == ExprKind::Binary ? static_cast<const BinaryExpr *>(e) : nullptr;
}
static const StringExpr *as_string(const Expr *e) {
    return e != nullptr && e->kind == ExprKind::String ? static_cast<const StringExpr *>(e) : nullptr;
}
static const IndexExpr *as_index(const Expr *e) {
    return e != nullptr && e->kind == ExprKind::Index ? static_cast<const IndexExpr *>(e) : nullptr;
}
static const UnaryExpr *as_unary(const Expr *e) {
    return e != nullptr && e->kind == ExprKind::Unary ? static_cast<const UnaryExpr *>(e) : nullptr;
}

static bool is_ident(const Expr *expr, const char *name) {
    if (expr == nullptr || expr->kind != ExprKind::Ident) {
        return false;
    }
    return static_cast<const IdentExpr *>(expr)->name == name;
}

static const CallExpr *is_call(const Expr *expr, const char *name, size_t argc) {
    if (expr == nullptr || expr->kind != ExprKind::Call) {
        return nullptr;
    }
    const auto *call = static_cast<const CallExpr *>(expr);
    return !call->has_spread && !call->optional && call->args.size() == argc && is_ident(call->callee.get(), name) ? call : nullptr;
}

static bool is_conversion(const Expr *expr, const char *conversion, const char *param) {
    const auto *call = is_call(expr, conversion, 1);
    return call && is_ident(call->args[0].get(), param);
}

static bool is_int(const Expr *expr, int64_t value) {
    if (expr == nullptr || expr->kind != ExprKind::Number) {
        return false;
    }
    const auto *number = static_cast<const NumberExpr *>(expr);
    return !number->is_float && number->i == value;
}

static const Expr *single_return(const Function *func) {
    if (!func->body || func->body->stmts.size() != 1) {
        return nullptr;
    }
    const Stmt *s = func->body->stmts[0].get();
    if (s == nullptr || s->stmt_kind != StmtKind::Return) {
        return nullptr;
    }
    return static_cast<const ReturnStmt *>(s)->value.get();
}

static bool matches_binary_helper(const Function *func, const char *op) {
    if (func->params.size() != 2 || func->params[0].name != "a" || func->params[1].name != "b") {
        return false;
    }
    const Expr *e = single_return(func);
    if (e == nullptr || e->kind != ExprKind::Binary) {
        return false;
    }
    const auto *binary = static_cast<const BinaryExpr *>(e);
    return binary->op == op && is_conversion(binary->left.get(), "__js_to_int32", "a") &&
           is_conversion(binary->right.get(), "__js_to_int32", "b");
}

static bool matches_shift_count(const Expr *expr, const char *param) {
    if (expr == nullptr || expr->kind != ExprKind::Binary) {
        return false;
    }
    const auto *mod = static_cast<const BinaryExpr *>(expr);
    return mod->op == "%" && is_conversion(mod->left.get(), "__js_to_uint32", param) && is_int(mod->right.get(), 32);
}

static bool matches_apply_array_helper(const Function *func) {
    if (func->params.size() != 2 || func->params[0].name != "f" || func->params[1].name != "args" || !func->body ||
        func->body->stmts.size() != 15) {
        return false;
    }
    const auto *length = as_var_decl(func->body->stmts[0].get());
    const auto *length_call = length ? as_call(length->initializerExpr.get()) : nullptr;
    const auto *length_member = length_call ? as_member(length_call->callee.get()) : nullptr;
    if (!length || length->name != "n" || !length_call || length_call->has_spread || length_call->optional || !length_call->args.empty() ||
        !length_member || length_member->member != "length" || !is_ident(length_member->object.get(), "args")) {
        return false;
    }
    for (int64_t arity = 0; arity <= 12; ++arity) {
        const auto *branch = as_if(func->body->stmts[arity + 1].get());
        const auto *condition = branch ? as_binary(branch->cond.get()) : nullptr;
        const auto *body = branch ? as_block(branch->then_branch.get()) : nullptr;
        const auto *ret = body && body->stmts.size() == 1 ? as_return(body->stmts[0].get()) : nullptr;
        const auto *call = ret ? as_call(ret->value.get()) : nullptr;
        if (!condition || condition->op != "==" || !is_ident(condition->left.get(), "n") || !is_int(condition->right.get(), arity) ||
            branch->else_branch || !call || call->has_spread || call->optional || !is_ident(call->callee.get(), "f") ||
            call->args.size() != static_cast<size_t>(arity)) {
            return false;
        }
        for (int64_t i = 0; i < arity; ++i) {
            const auto *index = as_index(call->args[i].get());
            if (!index || index->optional || !is_ident(index->object.get(), "args") || !is_int(index->index.get(), i)) {
                return false;
            }
        }
    }
    const auto *fallback = as_return(func->body->stmts[14].get());
    const auto *fallback_call = fallback ? is_call(fallback->value.get(), "__js_apply_array_spread", 2) : nullptr;
    return fallback_call && is_ident(fallback_call->args[0].get(), "f") && is_ident(fallback_call->args[1].get(), "args");
}

bool matches_extended_jsrt_helper(const Function *func, OpCode op) {
    if (op == OpCode::OP_CALL_SPREAD) {
        return matches_apply_array_helper(func);
    }
    if (op == OpCode::OP_JS_GET_PROP_STATIC) {
        const bool legacy = func->params.size() == 2 && func->body && func->body->stmts.size() == 10;
        const bool own_miss_aware =
            func->params.size() == 3 && func->params[2].name == "ownMiss" && func->body && func->body->stmts.size() == 8;
        if ((!legacy && !own_miss_aware) || func->params[0].name != "obj" || func->params[1].name != "key") {
            return false;
        }
        const auto *getter_key = as_var_decl(func->body->stmts[0].get());
        const auto *concat = getter_key ? as_binary(getter_key->initializerExpr.get()) : nullptr;
        const auto *prefix = concat ? as_string(concat->left.get()) : nullptr;
        return getter_key && getter_key->name == "getterKey" && concat && concat->op == "+" && prefix && prefix->value == "__js_getter__" &&
               is_ident(concat->right.get(), "key");
    }
    if (op == OpCode::OP_JS_SET_PROP_STATIC) {
        // confirm jsrt's helper still starts by deriving "__js_setter__" + key,
        // so editing jsrt silently disables the opcode instead of silently changing semantics.
        if (func->params.size() != 3 || func->params[0].name != "obj" || func->params[1].name != "key" || 
            func->params[2].name != "value" || !func->body || func->body->stmts.size() < 2
        ) {
            return false;
        }
        const auto *setter_key = as_var_decl(func->body->stmts[1].get());
        const auto *concat = setter_key ? as_binary(setter_key->initializerExpr.get()) : nullptr;
        const auto *prefix = concat ? as_string(concat->left.get()) : nullptr;
        return setter_key && setter_key->name == "setterKey" && concat && concat->op == "+" && prefix && prefix->value == "__js_setter__" && is_ident(concat->right.get(), "key");
    }
    if (op == OpCode::OP_JS_POSTINC) {
        // Gate on jsrt's exact body, same contract as the get/set gates: editing
        // jsrt silently disables the opcode rather than silently changing meaning.
        //   func __js_postinc(o, k) { let old = __js_to_number(o[k]); o[k] = old + 1; return old; }
        if (func->params.size() != 2 || func->params[0].name != "o" || func->params[1].name != "k" || !func->body ||
            func->body->stmts.size() != 3) {
            return false;
        }
        const auto *old_decl = as_var_decl(func->body->stmts[0].get());
        if (!old_decl || old_decl->name != "old") {
            return false;
        }
        const auto *to_number = is_call(old_decl->initializerExpr.get(), "__js_to_number", 1);
        const auto *read = to_number ? as_index(to_number->args[0].get()) : nullptr;
        if (!read || !is_ident(read->object.get(), "o") || !is_ident(read->index.get(), "k")) {
            return false;
        }
        const auto *store = as_index_assign(func->body->stmts[1].get());
        const auto *target = store ? as_index(store->target.get()) : nullptr;
        if (!target || !is_ident(target->object.get(), "o") || !is_ident(target->index.get(), "k")) {
            return false;
        }
        const auto *bump = as_binary(store->value.get());
        if (!bump || bump->op != "+" || !is_ident(bump->left.get(), "old") || !is_int(bump->right.get(), 1)) {
            return false;
        }
        const auto *ret = as_return(func->body->stmts[2].get());
        return ret && is_ident(ret->value.get(), "old");
    }
    if (op == OpCode::OP_JS_BIT_NOT) {
        if (func->params.size() != 1 || func->params[0].name != "value") {
            return false;
        }
        const auto *unary = as_unary(single_return(func));
        return unary && unary->op == "~" && is_conversion(unary->operand.get(), "__js_to_int32", "value");
    }
    if (op == OpCode::OP_JS_BIT_AND) {
        return matches_binary_helper(func, "&");
    }
    if (op == OpCode::OP_JS_BIT_OR) {
        return matches_binary_helper(func, "|");
    }
    if (op == OpCode::OP_JS_BIT_XOR) {
        return matches_binary_helper(func, "^");
    }
    if (op == OpCode::OP_JS_SHL || op == OpCode::OP_JS_SHR) {
        if (func->params.size() != 2 || func->params[0].name != "a" || func->params[1].name != "b") {
            return false;
        }
        const Expr *value = single_return(func);
        if (op == OpCode::OP_JS_SHL) {
            const auto *outer = is_call(value, "__js_to_int32", 1);
            value = outer ? outer->args[0].get() : nullptr;
        }
        const auto *shift = as_binary(value);
        return shift && shift->op == (op == OpCode::OP_JS_SHL ? "<<" : ">>") && is_conversion(shift->left.get(), "__js_to_int32", "a") &&
               matches_shift_count(shift->right.get(), "b");
    }
    if (op != OpCode::OP_JS_USHR || func->params.size() != 2 || func->params[0].name != "a" || func->params[1].name != "b" || !func->body ||
        func->body->stmts.size() != 3) {
        return false;
    }
    const auto *value = as_var_decl(func->body->stmts[0].get());
    const auto *shift = as_var_decl(func->body->stmts[1].get());
    const auto *ret = as_return(func->body->stmts[2].get());
    if (!value || value->name != "value" || !is_conversion(value->initializerExpr.get(), "__js_to_uint32", "a") || !shift ||
        shift->name != "shift" || !matches_shift_count(shift->initializerExpr.get(), "b") || !ret) {
        return false;
    }
    const auto *floor = as_call(ret->value.get());
    const auto *member = floor ? as_member(floor->callee.get()) : nullptr;
    if (!floor || floor->has_spread || floor->optional || floor->args.size() != 1 || !member || member->member != "floor" ||
        !is_ident(member->object.get(), "math")) {
        return false;
    }
    const auto *divide = as_binary(floor->args[0].get());
    const auto *power = divide ? is_call(divide->right.get(), "__js_pow", 2) : nullptr;
    return divide && divide->op == "/" && is_ident(divide->left.get(), "value") && power && is_int(power->args[0].get(), 2) &&
           is_ident(power->args[1].get(), "shift");
}

} // namespace bytecode
} // namespace nari
