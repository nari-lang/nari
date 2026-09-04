#include "ast.h"
#include "bytecode.h"
#include "bytecode_verify.h"
#include "parser_api.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>

using namespace nari;

// NumberExpr promotes out-of-int48 integer literals to float at parse time using its own copy of the int48 bounds
static_assert(
    NumberExpr::AST_INT48_MIN == Value::INT48_MIN && NumberExpr::AST_INT48_MAX == Value::INT48_MAX,
    "ast.h NumberExpr int48 bounds must match Value::INT48_MIN/MAX"
);

namespace nari {
namespace bytecode {

static bool is_js_undefined_param(const Param &param) {
    const auto *ident = dynamic_cast<const IdentExpr *>(param.default_value.get());
    return ident && ident->name == "__js_undefined";
}

static std::string callee_label(const Expr *expr) {
    if (const auto *ident = dynamic_cast<const IdentExpr *>(expr)) {
        return ident->name;
    }
    if (const auto *member = dynamic_cast<const MemberExpr *>(expr)) {
        return "." + member->member;
    }
    if (dynamic_cast<const IndexExpr *>(expr)) {
        return "indexed expression";
    }
    return "computed expression";
}

// compiler state for a single function
struct CompilerContext {
    FunctionMeta *function;
    Chunk *chunk;
    std::unordered_map<std::string, uint16_t> locals;      // name -> slot index
    std::vector<std::string> local_names;                  // for tracking order
    std::unordered_map<std::string, uint16_t> capture_map; // captured var name -> capture index
    std::set<std::string> const_locals;
    std::set<std::string> const_captures;
    std::set<std::string> lexical_bindings;
    uint16_t local_count;

    CompilerContext(FunctionMeta *f, Chunk *c) : function(f), chunk(c), local_count(0) {
    }

    // Block scoping.
    // `locals` is one entry per name for the whole function and slots are never reused,
    // so only the name to slot mapping needs unwinding at the end of a block.
    struct ShadowedBinding {
        std::string name;
        uint16_t prev_slot;
        bool had_prev;
        bool prev_const;
        bool prev_lexical;
    };
    std::vector<ShadowedBinding> shadow_stack;
    std::vector<size_t> scope_marks;

    void push_scope() {
        scope_marks.push_back(shadow_stack.size());
    }
    void pop_scope() {
        if (scope_marks.empty()) {
            return;
        }
        const size_t mark = scope_marks.back();
        scope_marks.pop_back();
        while (shadow_stack.size() > mark) {
            const ShadowedBinding &s = shadow_stack.back();
            if (s.had_prev) {
                locals[s.name] = s.prev_slot;
                if (s.prev_const) {
                    const_locals.insert(s.name);
                } else {
                    const_locals.erase(s.name);
                }
                if (s.prev_lexical) {
                    lexical_bindings.insert(s.name);
                } else {
                    lexical_bindings.erase(s.name);
                }
            } else {
                locals.erase(s.name);
                const_locals.erase(s.name);
                lexical_bindings.erase(s.name);
            }
            shadow_stack.pop_back();
        }
    }

    // Slots of this function's locals that some inner closure captures.
    // Loops check this to decide whether they need OP_CLOSE_UPVALUES
    std::vector<uint8_t> captured_locals;

    void note_local_captured(uint16_t slot) {
        if (captured_locals.size() <= slot) {
            captured_locals.resize(slot + 1, 0);
        }
        captured_locals[slot] = 1;
    }

    bool any_captured_local_at_or_above(uint16_t first) const {
        for (size_t i = first; i < captured_locals.size(); i++) {
            if (captured_locals[i]) {
                return true;
            }
        }
        return false;
    }

    uint16_t declare_local(const std::string &name) {
        if (local_count > 65534) {
            fprintf(stderr, "error: too many local variables in function (max 65534)!\n");
            return 0xFFFF;
        }
        if (!scope_marks.empty()) {
            auto it = locals.find(name);
            ShadowedBinding s{
                .name = name,
                .prev_slot = it != locals.end() ? it->second : 0,
                .had_prev = it != locals.end(),
                .prev_const = const_locals.count(name) != 0,
                .prev_lexical = lexical_bindings.count(name) != 0,
            };
            shadow_stack.push_back(std::move(s));
        }
        uint16_t idx = local_count++;
        locals[name] = idx;
        local_names.push_back(name);
        return idx;
    }

    uint16_t resolve_local(const std::string &name) {
        auto it = locals.find(name);
        if (it != locals.end()) {
            return it->second;
        }
        return 0xFFFF; // not found
    }

    uint16_t add_constant(const Constant &c) {
        for (uint16_t i = 0; i < function->constants.size(); ++i) {
            const Constant &existing = function->constants[i];
            if (existing.type != c.type) {
                continue;
            }
            bool equal = false;
            switch (c.type) {
                case Constant::Type::NONE:
                    equal = true;
                    break;
                case Constant::Type::INT:
                    equal = existing.as_int == c.as_int;
                    break;
                case Constant::Type::FLOAT:
                    equal = std::memcmp(&existing.as_float, &c.as_float, sizeof(c.as_float)) == 0;
                    break;
                case Constant::Type::STRING:
                    equal = existing.string_idx == c.string_idx;
                    break;
                case Constant::Type::FUNCTION:
                    equal = existing.func_idx == c.func_idx;
                    break;
            }
            if (equal) {
                return i;
            }
        }
        function->constants.push_back(c);
        return static_cast<uint16_t>(function->constants.size() - 1);
    }

    // track source lines for error messages / stack traces
    void emit_line(int line) {
        if (line <= 0) {
            return;
        }
        size_t pc = function->code.size();
        if (!function->line_map.empty() && function->line_map.back().line == line) {
            return; // same line, no new entry needed
        }
        function->line_map.push_back({ static_cast<uint32_t>(pc), line });
    }

    void emit_byte(uint8_t byte) {
        function->code.push_back(byte);
    }

    void emit_short(uint16_t val) {
        emit_byte((val >> 8) & 0xFF);
        emit_byte(val & 0xFF);
    }

    void emit_op(OpCode op) {
        emit_byte(static_cast<uint8_t>(op));
    }

    void emit_op_byte(OpCode op, uint8_t operand) {
        emit_op(op);
        emit_byte(operand);
    }

    void emit_op_short(OpCode op, uint16_t operand) {
        emit_op(op);
        emit_short(operand);
    }

    size_t emit_jump(OpCode op) {
        emit_op(op);
        emit_short(0xFFFF); // placeholder
        return function->code.size() - 2;
    }

    void patch_jump(size_t offset) {
        if (offset + 2 > function->code.size()) {
            fprintf(stderr, "error: invalid jump patch offset\n");
            return;
        }
        // calculate jump distance from the placeholder to current position
        int32_t jump = static_cast<int32_t>(function->code.size() - offset - 2);
        if (jump < -32768 || jump > 32767) {
            fprintf(stderr, "error: jump offset too large\n");
            return;
        }

        int16_t jump_short = static_cast<int16_t>(jump);
        function->code[offset] = (jump_short >> 8) & 0xFF;
        function->code[offset + 1] = jump_short & 0xFF;
    }
};

class Compiler {
  private:
    Chunk *chunk;
    CompilerContext *ctx;
    CompilerContext *parent_ctx; // for closure capture detection
    // stack of { break_patches, continue_target } for nested loops
    struct LoopInfo {
        // continue_target == kContinueForward means the target isn't known yet
        static constexpr size_t kContinueForward = SIZE_MAX;
        std::vector<size_t> break_patches;
        std::vector<size_t> continue_patches;
        size_t continue_target = 0;
        int outer_try_depth; // try_depth at the point the loop was entered
    };
    std::vector<LoopInfo> loop_stack;

    bool is_main_scope;               // true when compiling the <main> function body
    int try_depth = 0;                // nesting depth of try blocks (TCO forbidden when > 0)
    bool strict_mode = false;         // true when the current file has "use strict" at the top level
    std::string current_return_type_; // e.g. "int", "string", "bool"
    std::set<std::string> global_consts;
#ifdef NARI_EXTENDED_JSRT
    std::unordered_map<std::string, OpCode> extended_jsrt_helpers;
#endif

    void compile_expr(const Expr *expr);
    // Ends a loop iteration by dropping the upvalue cells for locals the loop declared,
    // so the next iteration's closures capture fresh cells.
    void emit_close_upvalues_for_loop(uint16_t first_slot) {
        if (!ctx->any_captured_local_at_or_above(first_slot)) {
            return;
        }
        ctx->emit_op_short(OpCode::OP_CLOSE_UPVALUES, first_slot);
    }

    void compile_stmt(const Stmt *stmt);
    // FnLike is nari::Function or nari::ClassMethod: both expose
    // name/params/body/return_type/filename/line.
    // Strict-mode annotation enforcement applies to Function only (see definition).
    template <typename FnLike> void compile_function_body(const FnLike *func, FunctionMeta &meta);
    void compile_classes();
    void emit_js_truthy();
    void collect_idents(const Stmt *stmt, std::set<std::string> &idents);
    void collect_idents_expr(const Expr *expr, std::set<std::string> &idents);
    void collect_bindings(const Stmt *stmt, std::set<std::string> &bindings);
    bool is_const_binding(const std::string &name) const;
    void emit_const_assignment_error(const std::string &name);

  public:
    Compiler() : chunk(nullptr), ctx(nullptr), parent_ctx(nullptr), is_main_scope(false) {
    }

    Chunk *compile(const FuncList &functions);
    AppendedCode compile_append(Chunk *existing, const FuncList &functions, const std::string &entry_name);
};

void Compiler::emit_js_truthy() {
    ctx->emit_op(OpCode::OP_DUP);
    uint32_t undefined_idx = chunk->add_string("__js_undefined");
    ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, static_cast<uint16_t>(undefined_idx));
    ctx->emit_op(OpCode::OP_STRICT_EQ);
    size_t native_truthy = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);
    ctx->emit_op(OpCode::OP_POP);
    ctx->emit_op(OpCode::OP_LOAD_FALSE);
    size_t end = ctx->emit_jump(OpCode::OP_JUMP);
    ctx->patch_jump(native_truthy);
    ctx->emit_op(OpCode::OP_JS_TRUTHY);
    ctx->patch_jump(end);
}

#ifdef NARI_EXTENDED_JSRT
static bool is_ident(const Expr *expr, const char *name) {
    const auto *ident = dynamic_cast<const IdentExpr *>(expr);
    return ident && ident->name == name;
}

static const CallExpr *is_call(const Expr *expr, const char *name, size_t argc) {
    const auto *call = dynamic_cast<const CallExpr *>(expr);
    return call && !call->has_spread && !call->optional && call->args.size() == argc && is_ident(call->callee.get(), name) ? call : nullptr;
}

static bool is_conversion(const Expr *expr, const char *conversion, const char *param) {
    const auto *call = is_call(expr, conversion, 1);
    return call && is_ident(call->args[0].get(), param);
}

static bool is_int(const Expr *expr, int64_t value) {
    const auto *number = dynamic_cast<const NumberExpr *>(expr);
    return number && !number->is_float && number->i == value;
}

static const Expr *single_return(const Function *func) {
    if (!func->body || func->body->stmts.size() != 1) {
        return nullptr;
    }
    const auto *ret = dynamic_cast<const ReturnStmt *>(func->body->stmts[0].get());
    return ret ? ret->value.get() : nullptr;
}

static bool matches_binary_helper(const Function *func, const char *op) {
    if (func->params.size() != 2 || func->params[0].name != "a" || func->params[1].name != "b") {
        return false;
    }
    const auto *binary = dynamic_cast<const BinaryExpr *>(single_return(func));
    return binary && binary->op == op && is_conversion(binary->left.get(), "__js_to_int32", "a") &&
           is_conversion(binary->right.get(), "__js_to_int32", "b");
}

static bool matches_shift_count(const Expr *expr, const char *param) {
    const auto *mod = dynamic_cast<const BinaryExpr *>(expr);
    return mod && mod->op == "%" && is_conversion(mod->left.get(), "__js_to_uint32", param) && is_int(mod->right.get(), 32);
}

static bool matches_apply_array_helper(const Function *func) {
    if (func->params.size() != 2 || func->params[0].name != "f" || func->params[1].name != "args" || !func->body ||
        func->body->stmts.size() != 15) {
        return false;
    }
    const auto *length = dynamic_cast<const VarDeclStmt *>(func->body->stmts[0].get());
    const auto *length_call = length ? dynamic_cast<const CallExpr *>(length->initializerExpr.get()) : nullptr;
    const auto *length_member = length_call ? dynamic_cast<const MemberExpr *>(length_call->callee.get()) : nullptr;
    if (!length || length->name != "n" || !length_call || length_call->has_spread || length_call->optional || !length_call->args.empty() ||
        !length_member || length_member->member != "length" || !is_ident(length_member->object.get(), "args")) {
        return false;
    }
    for (int64_t arity = 0; arity <= 12; ++arity) {
        const auto *branch = dynamic_cast<const IfStmt *>(func->body->stmts[arity + 1].get());
        const auto *condition = branch ? dynamic_cast<const BinaryExpr *>(branch->cond.get()) : nullptr;
        const auto *body = branch ? dynamic_cast<const BlockStmt *>(branch->then_branch.get()) : nullptr;
        const auto *ret = body && body->stmts.size() == 1 ? dynamic_cast<const ReturnStmt *>(body->stmts[0].get()) : nullptr;
        const auto *call = ret ? dynamic_cast<const CallExpr *>(ret->value.get()) : nullptr;
        if (!condition || condition->op != "==" || !is_ident(condition->left.get(), "n") || !is_int(condition->right.get(), arity) ||
            branch->else_branch || !call || call->has_spread || call->optional || !is_ident(call->callee.get(), "f") ||
            call->args.size() != static_cast<size_t>(arity)) {
            return false;
        }
        for (int64_t i = 0; i < arity; ++i) {
            const auto *index = dynamic_cast<const IndexExpr *>(call->args[i].get());
            if (!index || index->optional || !is_ident(index->object.get(), "args") || !is_int(index->index.get(), i)) {
                return false;
            }
        }
    }
    const auto *fallback = dynamic_cast<const ReturnStmt *>(func->body->stmts[14].get());
    const auto *fallback_call = fallback ? is_call(fallback->value.get(), "__js_apply_array_spread", 2) : nullptr;
    return fallback_call && is_ident(fallback_call->args[0].get(), "f") && is_ident(fallback_call->args[1].get(), "args");
}

static bool matches_extended_jsrt_helper(const Function *func, OpCode op) {
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
        const auto *getter_key = dynamic_cast<const VarDeclStmt *>(func->body->stmts[0].get());
        const auto *concat = getter_key ? dynamic_cast<const BinaryExpr *>(getter_key->initializerExpr.get()) : nullptr;
        const auto *prefix = concat ? dynamic_cast<const StringExpr *>(concat->left.get()) : nullptr;
        return getter_key && getter_key->name == "getterKey" && concat && concat->op == "@" && prefix && prefix->value == "__js_getter__" &&
               is_ident(concat->right.get(), "key");
    }
    if (op == OpCode::OP_JS_SET_PROP_STATIC) {
        // Same contract as the getter gate: confirm jsrt's helper still starts by
        // deriving "__js_setter__" @ key, so editing jsrt silently disables the
        // opcode instead of silently changing semantics.
        if (func->params.size() != 3 || func->params[0].name != "obj" || func->params[1].name != "key" || func->params[2].name != "value" ||
            !func->body || func->body->stmts.size() < 2) {
            return false;
        }
        const auto *setter_key = dynamic_cast<const VarDeclStmt *>(func->body->stmts[1].get());
        const auto *concat = setter_key ? dynamic_cast<const BinaryExpr *>(setter_key->initializerExpr.get()) : nullptr;
        const auto *prefix = concat ? dynamic_cast<const StringExpr *>(concat->left.get()) : nullptr;
        return setter_key && setter_key->name == "setterKey" && concat && concat->op == "@" && prefix && prefix->value == "__js_setter__" &&
               is_ident(concat->right.get(), "key");
    }
    if (op == OpCode::OP_JS_POSTINC) {
        // Gate on jsrt's exact body, same contract as the get/set gates: editing
        // jsrt silently disables the opcode rather than silently changing meaning.
        //   func __js_postinc(o, k) { let old = __js_to_number(o[k]); o[k] = old + 1; return old; }
        if (func->params.size() != 2 || func->params[0].name != "o" || func->params[1].name != "k" || !func->body ||
            func->body->stmts.size() != 3) {
            return false;
        }
        const auto *old_decl = dynamic_cast<const VarDeclStmt *>(func->body->stmts[0].get());
        if (!old_decl || old_decl->name != "old") {
            return false;
        }
        const auto *to_number = is_call(old_decl->initializerExpr.get(), "__js_to_number", 1);
        const auto *read = to_number ? dynamic_cast<const IndexExpr *>(to_number->args[0].get()) : nullptr;
        if (!read || !is_ident(read->object.get(), "o") || !is_ident(read->index.get(), "k")) {
            return false;
        }
        const auto *store = dynamic_cast<const IndexAssignStmt *>(func->body->stmts[1].get());
        const auto *target = store ? dynamic_cast<const IndexExpr *>(store->target.get()) : nullptr;
        if (!target || !is_ident(target->object.get(), "o") || !is_ident(target->index.get(), "k")) {
            return false;
        }
        const auto *bump = dynamic_cast<const BinaryExpr *>(store->value.get());
        if (!bump || bump->op != "+" || !is_ident(bump->left.get(), "old") || !is_int(bump->right.get(), 1)) {
            return false;
        }
        const auto *ret = dynamic_cast<const ReturnStmt *>(func->body->stmts[2].get());
        return ret && is_ident(ret->value.get(), "old");
    }
    if (op == OpCode::OP_JS_BIT_NOT) {
        if (func->params.size() != 1 || func->params[0].name != "value") {
            return false;
        }
        const auto *unary = dynamic_cast<const UnaryExpr *>(single_return(func));
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
        const auto *shift = dynamic_cast<const BinaryExpr *>(value);
        return shift && shift->op == (op == OpCode::OP_JS_SHL ? "<<" : ">>") && is_conversion(shift->left.get(), "__js_to_int32", "a") &&
               matches_shift_count(shift->right.get(), "b");
    }
    if (op != OpCode::OP_JS_USHR || func->params.size() != 2 || func->params[0].name != "a" || func->params[1].name != "b" || !func->body ||
        func->body->stmts.size() != 3) {
        return false;
    }
    const auto *value = dynamic_cast<const VarDeclStmt *>(func->body->stmts[0].get());
    const auto *shift = dynamic_cast<const VarDeclStmt *>(func->body->stmts[1].get());
    const auto *ret = dynamic_cast<const ReturnStmt *>(func->body->stmts[2].get());
    if (!value || value->name != "value" || !is_conversion(value->initializerExpr.get(), "__js_to_uint32", "a") || !shift ||
        shift->name != "shift" || !matches_shift_count(shift->initializerExpr.get(), "b") || !ret) {
        return false;
    }
    const auto *floor = dynamic_cast<const CallExpr *>(ret->value.get());
    const auto *member = floor ? dynamic_cast<const MemberExpr *>(floor->callee.get()) : nullptr;
    if (!floor || floor->has_spread || floor->optional || floor->args.size() != 1 || !member || member->member != "floor" ||
        !is_ident(member->object.get(), "math")) {
        return false;
    }
    const auto *divide = dynamic_cast<const BinaryExpr *>(floor->args[0].get());
    const auto *power = divide ? is_call(divide->right.get(), "__js_pow", 2) : nullptr;
    return divide && divide->op == "/" && is_ident(divide->left.get(), "value") && power && is_int(power->args[0].get(), 2) &&
           is_ident(power->args[1].get(), "shift");
}
#endif

bool Compiler::is_const_binding(const std::string &name) const {
    if (ctx->locals.count(name)) {
        return ctx->const_locals.count(name);
    }
    if (ctx->capture_map.count(name)) {
        return ctx->const_captures.count(name);
    }
    return global_consts.count(name);
}

void Compiler::emit_const_assignment_error(const std::string &name) {
    uint32_t str_idx = chunk->add_string("TypeError: Assignment to constant variable '" + name + "'");
    uint16_t const_idx = ctx->add_constant(Constant::make_string(str_idx));
    ctx->emit_op_short(OpCode::OP_LOAD_CONST, const_idx);
    ctx->emit_op(OpCode::OP_THROW);
}

// collect all identifier names referenced in an expression tree
void Compiler::collect_idents_expr(const Expr *expr, std::set<std::string> &idents) {
    if (!expr) {
        return;
    }
    if (auto *ident = dynamic_cast<const IdentExpr *>(expr)) {
        idents.insert(ident->name);
        return;
    }
    if (auto *binary = dynamic_cast<const BinaryExpr *>(expr)) {
        collect_idents_expr(binary->left.get(), idents);
        collect_idents_expr(binary->right.get(), idents);
        return;
    }
    if (auto *unary = dynamic_cast<const UnaryExpr *>(expr)) {
        collect_idents_expr(unary->operand.get(), idents);
        return;
    }
    if (auto *call = dynamic_cast<const CallExpr *>(expr)) {
        collect_idents_expr(call->callee.get(), idents);
        for (const auto &arg : call->args) {
            collect_idents_expr(arg.get(), idents);
        }
        return;
    }
    if (auto *spread = dynamic_cast<const SpreadExpr *>(expr)) {
        collect_idents_expr(spread->operand.get(), idents);
        return;
    }
    if (auto *index = dynamic_cast<const IndexExpr *>(expr)) {
        collect_idents_expr(index->object.get(), idents);
        collect_idents_expr(index->index.get(), idents);
        return;
    }
    if (auto *mem = dynamic_cast<const MemberExpr *>(expr)) {
        collect_idents_expr(mem->object.get(), idents);
        return;
    }
    if (auto *tern = dynamic_cast<const TernaryExpr *>(expr)) {
        collect_idents_expr(tern->condition.get(), idents);
        collect_idents_expr(tern->true_expr.get(), idents);
        collect_idents_expr(tern->false_expr.get(), idents);
        return;
    }
    if (auto *arr = dynamic_cast<const ArrayLiteralExpr *>(expr)) {
        for (const auto &e : arr->elements) {
            collect_idents_expr(e.get(), idents);
        }
        return;
    }
    if (auto *obj = dynamic_cast<const ObjectLiteralExpr *>(expr)) {
        for (const auto &[k, v] : obj->entries) {
            collect_idents_expr(v.get(), idents);
        }
        return;
    }
    if (auto *interp = dynamic_cast<const StringInterpolationExpr *>(expr)) {
        for (const auto &value : interp->exprs) {
            collect_idents_expr(value.get(), idents);
        }
        return;
    }
    if (auto *match = dynamic_cast<const MatchExpr *>(expr)) {
        collect_idents_expr(match->scrutinee.get(), idents);
        for (const auto &arm : match->arms) {
            if (auto *literal = dynamic_cast<const LiteralPattern *>(arm.pattern.get())) {
                collect_idents_expr(literal->value.get(), idents);
            }
            collect_idents_expr(arm.body.get(), idents);
        }
        return;
    }
    if (auto *new_expr = dynamic_cast<const NewExpr *>(expr)) {
        for (const auto &arg : new_expr->args) {
            collect_idents_expr(arg.get(), idents);
        }
        return;
    }
    if (auto *spawn = dynamic_cast<const SpawnExpr *>(expr)) {
        collect_idents(spawn->body.get(), idents);
        return;
    }
    // Note: we *do* descend into FunctionExpr to collect transitive captures
    if (auto *fn = dynamic_cast<const FunctionExpr *>(expr)) {
        for (const auto &param : fn->params) {
            collect_idents_expr(param.default_value.get(), idents);
        }
        if (fn->body) {
            collect_idents(fn->body.get(), idents);
        }
        return;
    }
}

// collect all identifier names referenced in a statement tree
void Compiler::collect_idents(const Stmt *stmt, std::set<std::string> &idents) {
    if (!stmt) {
        return;
    }
    if (auto *expr_stmt = dynamic_cast<const ExprStmt *>(stmt)) {
        collect_idents_expr(expr_stmt->expr.get(), idents);
        return;
    }
    if (auto *var_decl = dynamic_cast<const VarDeclStmt *>(stmt)) {
        collect_idents_expr(var_decl->initializerExpr.get(), idents);
        return;
    }
    if (auto *assign = dynamic_cast<const AssignStmt *>(stmt)) {
        // Include the target variable name as a referenced identifier
        idents.insert(assign->target);
        collect_idents_expr(assign->value.get(), idents);
        return;
    }
    if (auto *if_stmt = dynamic_cast<const IfStmt *>(stmt)) {
        collect_idents_expr(if_stmt->cond.get(), idents);
        collect_idents(if_stmt->then_branch.get(), idents);
        collect_idents(if_stmt->else_branch.get(), idents);
        return;
    }
    if (auto *while_stmt = dynamic_cast<const WhileStmt *>(stmt)) {
        collect_idents_expr(while_stmt->cond.get(), idents);
        collect_idents(while_stmt->body.get(), idents);
        return;
    }
    if (auto *for_stmt = dynamic_cast<const ForStmt *>(stmt)) {
        collect_idents(for_stmt->init.get(), idents);
        collect_idents_expr(for_stmt->cond.get(), idents);
        collect_idents(for_stmt->post.get(), idents);
        collect_idents(for_stmt->body.get(), idents);
        return;
    }
    if (auto *block = dynamic_cast<const BlockStmt *>(stmt)) {
        for (const auto &s : block->stmts) {
            collect_idents(s.get(), idents);
        }
        return;
    }
    if (auto *ret = dynamic_cast<const ReturnStmt *>(stmt)) {
        collect_idents_expr(ret->value.get(), idents);
        return;
    }
    if (auto *idx_assign = dynamic_cast<const IndexAssignStmt *>(stmt)) {
        collect_idents_expr(idx_assign->target.get(), idents);
        collect_idents_expr(idx_assign->value.get(), idents);
        return;
    }
    if (auto *foreach_stmt = dynamic_cast<const ForEachStmt *>(stmt)) {
        collect_idents_expr(foreach_stmt->iterable.get(), idents);
        collect_idents(foreach_stmt->body.get(), idents);
        return;
    }
    if (auto *switch_stmt = dynamic_cast<const SwitchStmt *>(stmt)) {
        collect_idents_expr(switch_stmt->value.get(), idents);
        for (const auto &_case : switch_stmt->cases) {
            collect_idents_expr(_case.match.get(), idents);
            collect_idents(_case.body.get(), idents);
        }
        collect_idents(switch_stmt->default_body.get(), idents);
        return;
    }
}

void Compiler::collect_bindings(const Stmt *stmt, std::set<std::string> &bindings) {
    if (!stmt) {
        return;
    }
    if (const auto *decl = dynamic_cast<const VarDeclStmt *>(stmt)) {
        if (decl->destructure_kind == DestructureKind::Array) {
            bindings.insert(decl->array_names.begin(), decl->array_names.end());
        } else if (decl->destructure_kind == DestructureKind::Object) {
            for (const auto &[_, name] : decl->object_bindings) {
                bindings.insert(name);
            }
        } else {
            bindings.insert(decl->name);
        }
        return;
    }
    if (const auto *block = dynamic_cast<const BlockStmt *>(stmt)) {
        for (const auto &child : block->stmts) {
            collect_bindings(child.get(), bindings);
        }
        return;
    }
    if (const auto *branch = dynamic_cast<const IfStmt *>(stmt)) {
        collect_bindings(branch->then_branch.get(), bindings);
        collect_bindings(branch->else_branch.get(), bindings);
        return;
    }
    if (const auto *loop = dynamic_cast<const WhileStmt *>(stmt)) {
        collect_bindings(loop->body.get(), bindings);
        return;
    }
    if (const auto *loop = dynamic_cast<const ForStmt *>(stmt)) {
        collect_bindings(loop->init.get(), bindings);
        collect_bindings(loop->post.get(), bindings);
        collect_bindings(loop->body.get(), bindings);
        return;
    }
    if (const auto *loop = dynamic_cast<const ForEachStmt *>(stmt)) {
        bindings.insert(loop->var);
        if (!loop->val_var.empty()) {
            bindings.insert(loop->val_var);
        }
        collect_bindings(loop->body.get(), bindings);
        return;
    }
    if (const auto *switch_stmt = dynamic_cast<const SwitchStmt *>(stmt)) {
        for (const auto &item : switch_stmt->cases) {
            collect_bindings(item.body.get(), bindings);
        }
        collect_bindings(switch_stmt->default_body.get(), bindings);
    }
}

void Compiler::compile_expr(const Expr *expr) {
    if (!expr) {
        return;
    }

    if (auto *num = dynamic_cast<const NumberExpr *>(expr)) {
        if (num->is_float) {
            uint16_t idx = ctx->add_constant(Constant::make_float(num->f));
            ctx->emit_op_short(OpCode::OP_LOAD_CONST, idx);
        } else {
            int64_t val = num->i;
            // use shortcuts for common values
            if (val == 0) {
                ctx->emit_op(OpCode::OP_LOAD_ZERO);
            } else if (val == 1) {
                ctx->emit_op(OpCode::OP_LOAD_ONE);
            } else {
                uint16_t idx = ctx->add_constant(Constant::make_int(val));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, idx);
            }
        }
        return;
    }

    if (auto *str = dynamic_cast<const StringExpr *>(expr)) {
        uint32_t str_idx = chunk->add_string(str->value);
        uint16_t const_idx = ctx->add_constant(Constant::make_string(str_idx));
        ctx->emit_op_short(OpCode::OP_LOAD_CONST, const_idx);
        return;
    }

    if (auto *b = dynamic_cast<const BoolExpr *>(expr)) {
        ctx->emit_op(b->value ? OpCode::OP_LOAD_TRUE : OpCode::OP_LOAD_FALSE);
        return;
    }

    if (auto *n = dynamic_cast<const NullExpr *>(expr)) {
        (void)n; // unused
        ctx->emit_op(OpCode::OP_LOAD_NONE);
        return;
    }

    if (auto *re = dynamic_cast<const RegexLiteralExpr *>(expr)) {
        uint32_t pattern_idx = chunk->add_string(re->pattern);
        uint32_t flags_idx = chunk->add_string(re->flags);
        ctx->emit_op(OpCode::OP_MAKE_REGEX);
        ctx->emit_short(static_cast<uint16_t>(pattern_idx));
        ctx->emit_short(static_cast<uint16_t>(flags_idx));
        return;
    }

    if (auto *var = dynamic_cast<const IdentExpr *>(expr)) {
        uint16_t idx = ctx->resolve_local(var->name);
        if (idx != 0xFFFF) {
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx);
            return;
        }
        // check if it's a captured variable
        auto cap_it = ctx->capture_map.find(var->name);
        if (cap_it != ctx->capture_map.end()) {
            ctx->emit_op_short(OpCode::OP_LOAD_CAPTURE, cap_it->second);
            return;
        }
        if (Parser::get_registered_type(var->name) || Parser::is_registered_class(var->name)) {
            // registered type names resolve at compile time to a string constant
            // so precompiled .naric files work without needing the parser's type registry at runtime.
            uint32_t str_idx = chunk->add_string(var->name);
            uint16_t const_idx = ctx->add_constant(Constant::make_string(str_idx));
            ctx->emit_op_short(OpCode::OP_LOAD_CONST, const_idx);
        } else {
            // global variable / builtin lookup
            uint32_t str_idx = chunk->add_string(var->name);
            ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, static_cast<uint16_t>(str_idx));
        }
        return;
    }

    if (auto *bin = dynamic_cast<const BinaryExpr *>(expr)) {
        // short-circuit operators need special codegen
        if (bin->op == "&&") {
            compile_expr(bin->left.get());
            size_t false_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);
            // left was truthy (consumed by jump), evaluate right
            compile_expr(bin->right.get());
            // convert to bool: NOT NOT
            ctx->emit_op(OpCode::OP_NOT);
            ctx->emit_op(OpCode::OP_NOT);
            size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(false_jump);
            // left was falsy (consumed by jump), push false
            ctx->emit_op(OpCode::OP_LOAD_FALSE);
            ctx->patch_jump(end_jump);
            return;
        }
        if (bin->op == "||") {
            compile_expr(bin->left.get());
            size_t true_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_TRUE);
            // left was falsy (consumed by jump), evaluate right
            compile_expr(bin->right.get());
            ctx->emit_op(OpCode::OP_NOT);
            ctx->emit_op(OpCode::OP_NOT);
            size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(true_jump);
            // left was truthy (consumed by jump), push true
            ctx->emit_op(OpCode::OP_LOAD_TRUE);
            ctx->patch_jump(end_jump);
            return;
        }
        if (bin->op == "??") {
            compile_expr(bin->left.get());
            ctx->emit_op(OpCode::OP_DUP);
            size_t none_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_NONE);
            size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(none_jump);
            // left was none (dup consumed), pop the original left value
            ctx->emit_op(OpCode::OP_POP);
            compile_expr(bin->right.get());
            ctx->patch_jump(end_jump);
            return;
        }

        // compile operands for non-short-circuit ops
        compile_expr(bin->left.get());
        compile_expr(bin->right.get());

        // emit operation based on operator string
        if (bin->op == "+") {
            ctx->emit_op(OpCode::OP_ADD);
        } else if (bin->op == "-") {
            ctx->emit_op(OpCode::OP_SUB);
        } else if (bin->op == "*") {
            ctx->emit_op(OpCode::OP_MUL);
        } else if (bin->op == "/") {
            ctx->emit_op(OpCode::OP_DIV);
        } else if (bin->op == "%") {
            ctx->emit_op(OpCode::OP_MOD);
        } else if (bin->op == "**") {
            ctx->emit_op(OpCode::OP_POW);
        } else if (bin->op == "&") {
            ctx->emit_op(OpCode::OP_BIT_AND);
        } else if (bin->op == "|") {
            ctx->emit_op(OpCode::OP_BIT_OR);
        } else if (bin->op == "^") {
            ctx->emit_op(OpCode::OP_BIT_XOR);
        } else if (bin->op == "<<") {
            ctx->emit_op(OpCode::OP_LSHIFT);
        } else if (bin->op == ">>") {
            ctx->emit_op(OpCode::OP_RSHIFT);
        } else if (bin->op == "==") {
            ctx->emit_op(OpCode::OP_EQ);
        } else if (bin->op == "!=") {
            ctx->emit_op(OpCode::OP_NE);
        } else if (bin->op == "===") {
            ctx->emit_op(OpCode::OP_STRICT_EQ);
        } else if (bin->op == "!==") {
            ctx->emit_op(OpCode::OP_STRICT_NE);
        } else if (bin->op == "<") {
            ctx->emit_op(OpCode::OP_LT);
        } else if (bin->op == "<=") {
            ctx->emit_op(OpCode::OP_LE);
        } else if (bin->op == ">") {
            ctx->emit_op(OpCode::OP_GT);
        } else if (bin->op == ">=") {
            ctx->emit_op(OpCode::OP_GE);
        } else if (bin->op == "@") {
            ctx->emit_op(OpCode::OP_STR_CONCAT);
        } else {
            fprintf(stderr, "unhandled binary op: %s\n", bin->op.c_str());
        }
        return;
    }

    if (auto *unary = dynamic_cast<const UnaryExpr *>(expr)) {
        // increment/decrement operators (++x, --x, x++, x--)
        if (unary->op == "++" || unary->op == "--" || unary->op == "post++" || unary->op == "post--") {
            auto *ident = dynamic_cast<const IdentExpr *>(unary->operand.get());
            if (!ident) {
                fprintf(stderr, "increment/decrement requires a variable\n");
                return;
            }
            if (is_const_binding(ident->name)) {
                emit_const_assignment_error(ident->name);
                return;
            }
            bool is_increment = (unary->op == "++" || unary->op == "post++");
            bool is_postfix = (unary->op == "post++" || unary->op == "post--");
            uint16_t local_idx = ctx->resolve_local(ident->name);
            bool is_local = (local_idx != 0xFFFF);
            auto cap_it = ctx->capture_map.find(ident->name);
            bool is_capture = !is_local && cap_it != ctx->capture_map.end();
            uint32_t global_str_idx = 0;
            if (!is_local && !is_capture) {
                global_str_idx = chunk->add_string(ident->name);
            }

            auto emit_load = [&]() {
                if (is_local) {
                    ctx->emit_op_short(OpCode::OP_LOAD_VAR, local_idx);
                } else if (is_capture) {
                    ctx->emit_op_short(OpCode::OP_LOAD_CAPTURE, cap_it->second);
                } else {
                    ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, static_cast<uint16_t>(global_str_idx));
                }
            };
            auto emit_store = [&]() {
                if (is_local) {
                    ctx->emit_op_short(OpCode::OP_STORE_VAR, local_idx);
                } else if (is_capture) {
                    ctx->emit_op_short(OpCode::OP_STORE_CAPTURE, cap_it->second);
                } else {
                    ctx->emit_op_short(OpCode::OP_STORE_GLOBAL, static_cast<uint16_t>(global_str_idx));
                }
            };

            if (is_postfix) {
                // postfix: result is the OLD value
                emit_load();
                // duplicate old value (keep one copy as result)
                ctx->emit_op(OpCode::OP_DUP);
                uint16_t one_idx = ctx->add_constant(Constant::make_int(1));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, one_idx);
                ctx->emit_op(is_increment ? OpCode::OP_ADD : OpCode::OP_SUB);
                // store new value (leaves new value on stack)
                emit_store();
                // pop the new value, leaving old value as result
                ctx->emit_op(OpCode::OP_POP);
            } else {
                // prefix: result is the NEW value
                emit_load();
                uint16_t one_idx = ctx->add_constant(Constant::make_int(1));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, one_idx);
                ctx->emit_op(is_increment ? OpCode::OP_ADD : OpCode::OP_SUB);
                // store new value (leaves new value on stack; this IS the result)
                emit_store();
            }
            return;
        }

        compile_expr(unary->operand.get());

        if (unary->op == "-" || unary->op == "neg") {
            ctx->emit_op(OpCode::OP_NEG);
        } else if (unary->op == "!") {
            ctx->emit_op(OpCode::OP_NOT);
        } else if (unary->op == "~") {
            ctx->emit_op(OpCode::OP_BIT_NOT);
        } else {
            fprintf(stderr, "unhandled unary op: %s\n", unary->op.c_str());
        }
        return;
    }

    if (auto *arr = dynamic_cast<const ArrayLiteralExpr *>(expr)) {
        if (arr->has_spread) {
            // Build array incrementally: start with empty array, push/spread each element
            ctx->emit_op_short(OpCode::OP_MAKE_ARRAY, 0);
            for (const auto &elem : arr->elements) {
                if (auto *spread = dynamic_cast<const SpreadExpr *>(elem.get())) {
                    compile_expr(spread->operand.get());
                    ctx->emit_op(OpCode::OP_ARRAY_SPREAD);
                } else {
                    compile_expr(elem.get());
                    ctx->emit_op(OpCode::OP_ARRAY_PUSH);
                }
            }
        } else {
            if (arr->elements.size() > 0xFFFF) {
                fprintf(stderr, "error: array literal too large (max 65535 elements)\n");
                return;
            }
            for (const auto &elem : arr->elements) {
                compile_expr(elem.get());
            }
            ctx->emit_op_short(OpCode::OP_MAKE_ARRAY, static_cast<uint16_t>(arr->elements.size()));
        }
        return;
    }

    if (auto *obj = dynamic_cast<const ObjectLiteralExpr *>(expr)) {
        if (obj->has_spread) {
            // build object incrementally
            ctx->emit_op_short(OpCode::OP_MAKE_OBJECT, 0);
            for (const auto &[key, value] : obj->entries) {
                if (key.empty()) {
                    // spread entry: ...expr
                    compile_expr(value.get());
                    ctx->emit_op(OpCode::OP_OBJECT_SPREAD);
                } else {
                    // regular key: value
                    compile_expr(value.get());
                    uint32_t str_idx = chunk->add_string(key);
                    ctx->emit_op_short(OpCode::OP_OBJECT_SET, static_cast<uint16_t>(str_idx));
                }
            }
        } else {
            if (obj->entries.size() > 0xFFFF) {
                fprintf(stderr, "error: object literal too large (max 65535 entries)\n");
                return;
            }
            for (const auto &[key, value] : obj->entries) {
                uint32_t str_idx = chunk->add_string(key);
                uint16_t const_idx = ctx->add_constant(Constant::make_string(str_idx));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, const_idx);
                compile_expr(value.get());
            }
            ctx->emit_op_short(OpCode::OP_MAKE_OBJECT, static_cast<uint16_t>(obj->entries.size()));
        }
        return;
    }

    if (auto *idx = dynamic_cast<const IndexExpr *>(expr)) {
        compile_expr(idx->object.get());
        if (idx->optional) {
            ctx->emit_op(OpCode::OP_DUP);
            size_t none_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_NONE);
            compile_expr(idx->index.get());
            ctx->emit_op(OpCode::OP_GET_INDEX);
            size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(none_jump);
            // obj was none, result is on stack
            ctx->patch_jump(end_jump);
        } else {
            compile_expr(idx->index.get());
            ctx->emit_op(OpCode::OP_GET_INDEX);
        }
        return;
    }

    if (auto *member = dynamic_cast<const MemberExpr *>(expr)) {
        compile_expr(member->object.get());
        if (member->optional) {
            ctx->emit_op(OpCode::OP_DUP);
            size_t none_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_NONE);
            uint32_t str_idx = chunk->add_string(member->member);
            ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(str_idx));
            size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(none_jump);
            ctx->patch_jump(end_jump);
        } else {
            uint32_t str_idx = chunk->add_string(member->member);
            ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(str_idx));
        }
        return;
    }

    if (auto *call = dynamic_cast<const CallExpr *>(expr)) {
        auto *callee = dynamic_cast<const IdentExpr *>(call->callee.get());
#ifdef NARI_EXTENDED_JSRT
        if (callee && !call->has_spread && !call->optional && !ctx->lexical_bindings.count(callee->name) &&
            !ctx->capture_map.count(callee->name)) {
            auto helper = extended_jsrt_helpers.find(callee->name);
            if (helper != extended_jsrt_helpers.end()) {
                if (helper->second == OpCode::OP_CALL_SPREAD && call->args.size() == 2) {
                    compile_expr(call->args[0].get());
                    compile_expr(call->args[1].get());
                    uint32_t label_idx = chunk->add_string("__js_apply_array");
                    ctx->emit_op_short(OpCode::OP_CALL_SPREAD, static_cast<uint16_t>(label_idx));
                    return;
                }
                if (helper->second == OpCode::OP_JS_SET_PROP_STATIC && call->args.size() == 3) {
                    const auto *key = dynamic_cast<const StringExpr *>(call->args[1].get());
                    if (key) {
                        compile_expr(call->args[0].get());
                        compile_expr(call->args[2].get());
                        uint32_t key_idx = chunk->add_string(key->value);
                        ctx->emit_op_short(OpCode::OP_JS_SET_PROP_STATIC, static_cast<uint16_t>(key_idx));
                        return;
                    }
                }
                if (helper->second == OpCode::OP_JS_POSTINC && call->args.size() == 2) {
                    const auto *key = dynamic_cast<const StringExpr *>(call->args[1].get());
                    if (key) {
                        compile_expr(call->args[0].get());
                        uint32_t key_idx = chunk->add_string(key->value);
                        ctx->emit_op_short(OpCode::OP_JS_POSTINC, static_cast<uint16_t>(key_idx));
                        return;
                    }
                }
                if (helper->second == OpCode::OP_JS_GET_PROP_STATIC && call->args.size() == 2) {
                    const auto *key = dynamic_cast<const StringExpr *>(call->args[1].get());
                    if (key) {
                        compile_expr(call->args[0].get());
                        uint32_t key_idx = chunk->add_string(key->value);
                        ctx->emit_op_short(OpCode::OP_JS_GET_PROP_STATIC, static_cast<uint16_t>(key_idx));
                        return;
                    }
                }
                // only the bit and shift helpers lower to an operand-free opcode that consumes exactly its arguments.
                const bool operand_free = helper->second == OpCode::OP_JS_BIT_AND || helper->second == OpCode::OP_JS_BIT_OR ||
                                          helper->second == OpCode::OP_JS_BIT_XOR || helper->second == OpCode::OP_JS_BIT_NOT ||
                                          helper->second == OpCode::OP_JS_SHL || helper->second == OpCode::OP_JS_SHR ||
                                          helper->second == OpCode::OP_JS_USHR;
                const size_t arity = helper->second == OpCode::OP_JS_BIT_NOT ? 1 : 2;
                if (operand_free && call->args.size() == arity) {
                    for (const auto &arg : call->args) {
                        compile_expr(arg.get());
                    }
                    ctx->emit_op(helper->second);
                    return;
                }
            }
        }
#endif
        if (callee && callee->name == "__js_strict_eq" && !call->has_spread && !call->optional && call->args.size() == 2) {
            compile_expr(call->args[0].get());
            compile_expr(call->args[1].get());
            ctx->emit_op(OpCode::OP_EQ);
            return;
        }
        if (callee && callee->name == "__js_truthy" && !call->has_spread && !call->optional && call->args.size() == 1) {
            compile_expr(call->args[0].get());
            emit_js_truthy();
            return;
        }
        if (callee && callee->name == "__js_omitted_has" && !call->has_spread && !call->optional && call->args.size() == 2) {
            compile_expr(call->args[0].get());
            compile_expr(call->args[1].get());
            uint32_t index_of_idx = chunk->add_string("index_of");
            ctx->emit_op(OpCode::OP_CALL_METHOD);
            ctx->emit_short(static_cast<uint16_t>(index_of_idx));
            ctx->emit_byte(1);
            ctx->emit_op(OpCode::OP_LOAD_ZERO);
            ctx->emit_op(OpCode::OP_GE);
            return;
        }
        if (callee && callee->name == "__js_set_function_length" && !call->has_spread && !call->optional && call->args.size() == 2) {
            compile_expr(call->args[0].get());
            ctx->emit_op(OpCode::OP_DUP);
            compile_expr(call->args[1].get());
            uint32_t length_idx = chunk->add_string("length");
            ctx->emit_op_short(OpCode::OP_SET_PROPERTY, static_cast<uint16_t>(length_idx));
            ctx->emit_op(OpCode::OP_POP);
            return;
        }
        bool is_js_and = callee && callee->name == "__js_and";
        bool is_js_or = callee && callee->name == "__js_or";
        if (!call->has_spread && !call->optional && (is_js_and || is_js_or) && call->args.size() == 2) {
            auto *right_thunk = dynamic_cast<const FunctionExpr *>(call->args[1].get());
            if (right_thunk && right_thunk->params.empty() && right_thunk->body && right_thunk->body->stmts.size() == 1) {
                auto *ret = dynamic_cast<const ReturnStmt *>(right_thunk->body->stmts[0].get());
                if (ret && ret->value) {
                    compile_expr(call->args[0].get());
                    ctx->emit_op(OpCode::OP_DUP);
                    emit_js_truthy();
                    size_t keep_left = ctx->emit_jump(is_js_and ? OpCode::OP_JUMP_IF_FALSE : OpCode::OP_JUMP_IF_TRUE);
                    ctx->emit_op(OpCode::OP_POP);
                    compile_expr(ret->value.get());
                    ctx->patch_jump(keep_left);
                    return;
                }
            }
        }

        if (call->has_spread) {
            // spread call: build args array, then use OP_CALL_SPREAD.
            // for method calls with spread, we still need the object on the stack
            auto *member = dynamic_cast<const MemberExpr *>(call->callee.get());
            if (member) {
                // method call with spread: obj.method(...args)
                compile_expr(member->object.get());
                compile_expr(member->object.get());
                uint32_t prop_idx = chunk->add_string(member->member);
                ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(prop_idx));
            } else {
                compile_expr(call->callee.get());
            }
            // A single spread operand is already the argument array expected
            // by OP_CALL_SPREAD, so avoid copying it into a temporary array.
            if (call->args.size() == 1) {
                if (auto *spread = dynamic_cast<const SpreadExpr *>(call->args[0].get())) {
                    compile_expr(spread->operand.get());
                } else {
                    ctx->emit_op_short(OpCode::OP_MAKE_ARRAY, 0);
                    compile_expr(call->args[0].get());
                    ctx->emit_op(OpCode::OP_ARRAY_PUSH);
                }
            } else {
                ctx->emit_op_short(OpCode::OP_MAKE_ARRAY, 0);
                for (const auto &arg : call->args) {
                    if (auto *spread = dynamic_cast<const SpreadExpr *>(arg.get())) {
                        compile_expr(spread->operand.get());
                        ctx->emit_op(OpCode::OP_ARRAY_SPREAD);
                    } else {
                        compile_expr(arg.get());
                        ctx->emit_op(OpCode::OP_ARRAY_PUSH);
                    }
                }
            }
            uint32_t label_idx = chunk->add_string(callee_label(call->callee.get()));
            ctx->emit_op_short(OpCode::OP_CALL_SPREAD, static_cast<uint16_t>(label_idx));
            return;
        }

        // check for method call (obj.method(args))
        auto *member = dynamic_cast<const MemberExpr *>(call->callee.get());
        if (member) {
            const std::string &method = member->member;

            // All member calls go through OP_CALL_METHOD so that type checking
            // can happen at runtime before any builtin is invoked.
            // non-builtin member call: use OP_CALL_METHOD for class instance support
            compile_expr(member->object.get());
            for (const auto &arg : call->args) {
                compile_expr(arg.get());
            }
            uint32_t prop_idx = chunk->add_string(method);
            ctx->emit_op(OpCode::OP_CALL_METHOD);
            ctx->emit_short(static_cast<uint16_t>(prop_idx));
            ctx->emit_byte(static_cast<uint8_t>(call->args.size()));
            return;
        }

        // regular function call
        compile_expr(call->callee.get());
        if (call->optional) {
            ctx->emit_op(OpCode::OP_DUP);
            size_t none_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_NONE);
            for (const auto &arg : call->args) {
                compile_expr(arg.get());
            }
            uint32_t label_idx = chunk->add_string(callee_label(call->callee.get()));
            ctx->emit_op_byte(OpCode::OP_CALL, static_cast<uint8_t>(call->args.size()));
            ctx->emit_short(static_cast<uint16_t>(label_idx));
            size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(none_jump);
            ctx->patch_jump(end_jump);
        } else {
            for (const auto &arg : call->args) {
                compile_expr(arg.get());
            }
            uint32_t label_idx = chunk->add_string(callee_label(call->callee.get()));
            ctx->emit_op_byte(OpCode::OP_CALL, static_cast<uint8_t>(call->args.size()));
            ctx->emit_short(static_cast<uint16_t>(label_idx));
        }
        return;
    }

    if (auto *func_expr = dynamic_cast<const FunctionExpr *>(expr)) {
        FunctionMeta meta;
        meta.name = "<lambda>";
        meta.source_file = func_expr->filename;
        meta.param_count = static_cast<uint8_t>(func_expr->params.size());
        meta.capture_count = 0;
        meta.is_lambda = true;
        meta.js_undefined_params = std::any_of(func_expr->params.begin(), func_expr->params.end(), is_js_undefined_param);

        // collect identifiers used in the body to detect captures
        CompilerContext lambda_ctx(&meta, chunk);
        CompilerContext *saved = ctx;
        CompilerContext *saved_parent = parent_ctx;
        parent_ctx = saved;
        ctx = &lambda_ctx;
        bool saved_main_scope = is_main_scope;
        auto saved_loop_stack = std::move(loop_stack);
        int saved_try_depth = try_depth;
        loop_stack.clear();
        try_depth = 0;
        is_main_scope = false;
        std::vector<std::string> captures;
        if (saved) {
            std::set<std::string> body_idents;
            for (const auto &param : func_expr->params) {
                collect_idents_expr(param.default_value.get(), body_idents);
            }
            collect_idents(func_expr->body.get(), body_idents);

            for (const auto &name : body_idents) {
                // skip if it's a parameter name
                bool is_param = false;
                for (const auto &param : func_expr->params) {
                    if (param.name == name) {
                        is_param = true;
                        break;
                    }
                }
                if (is_param) {
                    continue;
                }

                // check if it's a parent local or parent capture
                if (saved->resolve_local(name) != 0xFFFF || saved->capture_map.find(name) != saved->capture_map.end()) {
                    captures.push_back(name);
                }
            }
        }

        // Register captures in capture_map (NOT as locals)
        for (size_t i = 0; i < captures.size(); i++) {
            ctx->capture_map[captures[i]] = static_cast<uint16_t>(i);
            if (saved->const_locals.count(captures[i]) || saved->const_captures.count(captures[i])) {
                ctx->const_captures.insert(captures[i]);
            }
        }
        if (captures.size() > UINT16_MAX) {
            throw std::runtime_error("closure captures too many variables (max 65535)");
        }
        meta.capture_count = static_cast<uint16_t>(captures.size());

        // declare params as locals
        for (size_t i = 0; i < func_expr->params.size(); ++i) {
            const auto &param = func_expr->params[i];
            ctx->declare_local(param.name);
            ctx->lexical_bindings.insert(param.name);
            if (param.is_rest) {
                meta.rest_param_index = static_cast<int8_t>(i);
            }
        }
        collect_bindings(func_expr->body.get(), ctx->lexical_bindings);

        // compile default parameter values
        for (const auto &param : func_expr->params) {
            if (param.default_value && !is_js_undefined_param(param)) {
                uint16_t idx = ctx->resolve_local(param.name);
                ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx);
                size_t skip = ctx->emit_jump(OpCode::OP_JUMP_IF_NONE);
                size_t end = ctx->emit_jump(OpCode::OP_JUMP);
                ctx->patch_jump(skip);
                compile_expr(param.default_value.get());
                ctx->emit_op_short(OpCode::OP_STORE_VAR, idx);
                ctx->emit_op(OpCode::OP_POP);
                ctx->patch_jump(end);
            }
        }

        if (func_expr->body) {
            compile_stmt(func_expr->body.get());
        }
        ctx->emit_op(OpCode::OP_LOAD_NONE);
        ctx->emit_op(OpCode::OP_RETURN);
        meta.var_names = lambda_ctx.local_names;

        ctx = saved;
        parent_ctx = saved_parent;
        is_main_scope = saved_main_scope;
        loop_stack = std::move(saved_loop_stack);
        try_depth = saved_try_depth;

        uint32_t func_idx = static_cast<uint32_t>(chunk->functions.size());
        std::string lambda_name = "<lambda_" + std::to_string(func_idx) + ">";
        meta.name = lambda_name;
        chunk->functions.push_back(std::move(meta));

        if (!captures.empty()) {
            // emit OP_MAKE_CLOSURE: func_idx(2), capture_count(2), then for each
            // capture: source(1), index(2) source: 0=parent local, 1=parent capture
            ctx->emit_op(OpCode::OP_MAKE_CLOSURE);
            ctx->emit_short(static_cast<uint16_t>(func_idx));
            ctx->emit_short(static_cast<uint16_t>(captures.size()));
            for (const auto &cap : captures) {
                // a parent local shadows a parent capture of the same name
                uint16_t parent_idx = saved->resolve_local(cap);
                auto parent_cap = saved->capture_map.find(cap);
                if (parent_idx != 0xFFFF) {
                    ctx->emit_byte(0); // source: parent local
                    ctx->emit_short(parent_idx);
                    saved->note_local_captured(parent_idx);
                } else if (parent_cap != saved->capture_map.end()) {
                    ctx->emit_byte(1); // source: parent capture
                    ctx->emit_short(parent_cap->second);
                } else {
                    // fallback: try global
                    ctx->emit_byte(2); // source: global
                    uint32_t str_idx = chunk->add_string(cap);
                    ctx->emit_short(static_cast<uint16_t>(str_idx));
                }
            }
        } else {
            // no captures - just load as a global function reference
            uint32_t str_idx = chunk->add_string(lambda_name);
            ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, static_cast<uint16_t>(str_idx));
        }
        return;
    }

    if (auto *ternary = dynamic_cast<const TernaryExpr *>(expr)) {
        compile_expr(ternary->condition.get());
        size_t else_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);
        compile_expr(ternary->true_expr.get());
        size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
        ctx->patch_jump(else_jump);
        compile_expr(ternary->false_expr.get());
        ctx->patch_jump(end_jump);
        return;
    }

    if (auto *interp = dynamic_cast<const StringInterpolationExpr *>(expr)) {
        // build result by compiling parts and expressions, concatenating
        bool has_value = false;
        for (size_t i = 0; i < interp->parts.size(); ++i) {
            if (!interp->parts[i].empty() || !has_value) {
                uint32_t str_idx = chunk->add_string(interp->parts[i]);
                uint16_t const_idx = ctx->add_constant(Constant::make_string(str_idx));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, const_idx);
                if (has_value) {
                    ctx->emit_op(OpCode::OP_STR_CONCAT);
                }
                has_value = true;
            }

            if (i < interp->expr_sources.size()) {
                // Prefer the pre-parsed AST stored at parse time (fast path).
                const nari::Expr *frag_expr = nullptr;
                if (i < interp->exprs.size() && interp->exprs[i]) {
                    frag_expr = interp->exprs[i].get();
                } else {
                    // Fallback: parse the expression source on demand.
                    auto expr_funcs = Parser::parse_program_from_source(interp->expr_sources[i]);
                    if (expr_funcs.size() >= 2 && expr_funcs[1] && expr_funcs[1]->body && !expr_funcs[1]->body->stmts.empty()) {
                        auto *first_stmt = expr_funcs[1]->body->stmts[0].get();
                        if (auto *exprStmt = dynamic_cast<nari::ExprStmt *>(first_stmt)) {
                            frag_expr = exprStmt->expr.get();
                        }
                    }
                }
                if (frag_expr) {
                    compile_expr(frag_expr);
                    uint32_t spec_idx = 0xFFFF;
                    if (i < interp->format_specs.size() && !interp->format_specs[i].empty()) {
                        spec_idx = chunk->add_string(interp->format_specs[i]);
                    }
                    ctx->emit_op_short(OpCode::OP_FORMAT_VALUE, static_cast<uint16_t>(spec_idx));
                    if (has_value) {
                        ctx->emit_op(OpCode::OP_STR_CONCAT);
                    } else {
                        has_value = true;
                    }
                }
            }
        }
        if (!has_value) {
            // empty interpolation
            uint32_t str_idx = chunk->add_string("");
            uint16_t const_idx = ctx->add_constant(Constant::make_string(str_idx));
            ctx->emit_op_short(OpCode::OP_LOAD_CONST, const_idx);
        }
        return;
    }

    if (auto *match = dynamic_cast<const MatchExpr *>(expr)) {
        compile_expr(match->scrutinee.get());

        std::vector<size_t> end_jumps;
        for (const auto &arm : match->arms) {
            if (!arm.pattern) {
                continue;
            }

            if (arm.pattern->pattern_kind == PatternKind::Variant) {
                auto *vp = dynamic_cast<const VariantPattern *>(arm.pattern.get());
                if (vp) {
                    // check if obj.__variant == variant_name
                    ctx->emit_op(OpCode::OP_DUP); // dup scrutinee
                    uint32_t variant_str = chunk->add_string("__variant");
                    ctx->emit_op_short(OpCode::OP_GET_PROPERTY, (uint16_t)variant_str);
                    uint32_t name_str = chunk->add_string(vp->variant_name);
                    uint16_t name_const = ctx->add_constant(Constant::make_string(name_str));
                    ctx->emit_op_short(OpCode::OP_LOAD_CONST, name_const);
                    ctx->emit_op(OpCode::OP_EQ);
                    size_t skip = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

                    // match! bind any fields that exist.
                    // variant data is in obj.__data
                    if (vp->is_struct_pattern && !vp->named_bindings.empty()) {
                        // Struct variant: bind __data.fieldname for each named binding
                        for (const auto &field_name : vp->named_bindings) {
                            ctx->emit_op(OpCode::OP_DUP); // dup scrutinee
                            uint32_t data_str = chunk->add_string("__data");
                            ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(data_str));
                            uint32_t field_str = chunk->add_string(field_name);
                            ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(field_str));
                            uint16_t var_idx = ctx->declare_local(field_name);
                            ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
                            ctx->emit_op(OpCode::OP_POP);
                        }
                    } else if (!vp->fields.empty()) {
                        if (vp->fields.size() == 1) {
                            // Single field: bind __data directly
                            auto *binding = dynamic_cast<const BindingPattern *>(vp->fields[0].get());
                            if (binding) {
                                ctx->emit_op(OpCode::OP_DUP); // dup scrutinee
                                uint32_t data_str = chunk->add_string("__data");
                                ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(data_str));
                                uint16_t var_idx = ctx->declare_local(binding->name);
                                ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
                                ctx->emit_op(OpCode::OP_POP);
                            }
                        } else {
                            // multi-field tuple: bind __data[0], __data[1], etc.
                            for (size_t i = 0; i < vp->fields.size(); i++) {
                                auto *binding = dynamic_cast<const BindingPattern *>(vp->fields[i].get());
                                if (binding) {
                                    ctx->emit_op(OpCode::OP_DUP); // dup scrutinee
                                    uint32_t data_str = chunk->add_string("__data");
                                    ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(data_str));
                                    uint16_t idx_const = ctx->add_constant(Constant::make_int(static_cast<int64_t>(i)));
                                    ctx->emit_op_short(OpCode::OP_LOAD_CONST, idx_const);
                                    ctx->emit_op(OpCode::OP_GET_INDEX);
                                    uint16_t var_idx = ctx->declare_local(binding->name);
                                    ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
                                    ctx->emit_op(OpCode::OP_POP);
                                }
                            }
                        }
                    }

                    ctx->emit_op(OpCode::OP_POP);
                    compile_expr(arm.body.get());
                    end_jumps.push_back(ctx->emit_jump(OpCode::OP_JUMP));
                    ctx->patch_jump(skip);
                }
            } else if (arm.pattern->pattern_kind == PatternKind::Binding) {
                auto *bp = dynamic_cast<const BindingPattern *>(arm.pattern.get());
                if (bp) {
                    // check if this binding name matches a variant
                    ctx->emit_op(OpCode::OP_DUP);
                    uint32_t variant_str = chunk->add_string("__variant");
                    ctx->emit_op_short(OpCode::OP_GET_PROPERTY, (uint16_t)variant_str);
                    uint32_t name_str = chunk->add_string(bp->name);
                    uint16_t name_const = ctx->add_constant(Constant::make_string(name_str));
                    ctx->emit_op_short(OpCode::OP_LOAD_CONST, name_const);
                    ctx->emit_op(OpCode::OP_EQ);
                    size_t variant_match = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

                    // matched as a variant name
                    ctx->emit_op(OpCode::OP_POP); // pop scrutinee
                    compile_expr(arm.body.get());
                    end_jumps.push_back(ctx->emit_jump(OpCode::OP_JUMP));
                    ctx->patch_jump(variant_match);

                    // not a variant match; treat as a catch-all binding and bind the scrutinee value to the variable
                    // name
                    ctx->emit_op(OpCode::OP_DUP); // dup scrutinee for the binding
                    uint16_t var_idx = ctx->declare_local(bp->name);
                    ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
                    ctx->emit_op(OpCode::OP_POP);
                    ctx->emit_op(OpCode::OP_POP); // pop scrutinee
                    compile_expr(arm.body.get());
                    end_jumps.push_back(ctx->emit_jump(OpCode::OP_JUMP));
                }
            } else if (arm.pattern->pattern_kind == PatternKind::Literal) {
                auto *lp = dynamic_cast<const LiteralPattern *>(arm.pattern.get());
                if (lp) {
                    ctx->emit_op(OpCode::OP_DUP);
                    compile_expr(lp->value.get());
                    ctx->emit_op(OpCode::OP_EQ);

                    size_t skip = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);
                    ctx->emit_op(OpCode::OP_POP); // pop scrutinee
                    compile_expr(arm.body.get());
                    end_jumps.push_back(ctx->emit_jump(OpCode::OP_JUMP));
                    ctx->patch_jump(skip);
                }
            } else if (arm.pattern->pattern_kind == PatternKind::Wildcard) {
                // wildcard matches everything
                ctx->emit_op(OpCode::OP_POP); // pop scrutinee
                compile_expr(arm.body.get());
                end_jumps.push_back(ctx->emit_jump(OpCode::OP_JUMP));
            }
        }

        // fallthrough: pop scrutinee, push none
        ctx->emit_op(OpCode::OP_POP);
        ctx->emit_op(OpCode::OP_LOAD_NONE);

        for (size_t j : end_jumps) {
            ctx->patch_jump(j);
        }
        return;
    }

    if (auto *new_expr = dynamic_cast<const NewExpr *>(expr)) {
        for (const auto &arg : new_expr->args) {
            compile_expr(arg.get());
        }
        uint32_t name_idx = chunk->add_string(new_expr->class_name);
        ctx->emit_op(OpCode::OP_NEW_INSTANCE);
        ctx->emit_short(static_cast<uint16_t>(name_idx));
        ctx->emit_byte(static_cast<uint8_t>(new_expr->args.size()));
        return;
    }

    if (auto *this_expr = dynamic_cast<const ThisExpr *>(expr)) {
        (void)this_expr;
        ctx->emit_op(OpCode::OP_LOAD_THIS);
        return;
    }

    // compiles the body as an anonymous 0-param function and emits OP_SPAWN so the VM runs it asynchronously, returning
    // a Handle value.
    if (auto *spawn_expr = dynamic_cast<const SpawnExpr *>(expr)) {
        if (!spawn_expr->body) {
            ctx->emit_op(OpCode::OP_LOAD_NONE);
            return;
        }

        // compile spawn body as a 0-param anonymous function (same pattern as lambda compilation), capturing any
        // referenced parent local.
        FunctionMeta meta;
        meta.name = "<spawn>";
        meta.source_file = spawn_expr->filename;
        meta.param_count = 0;
        meta.capture_count = 0;
        meta.is_lambda = true;

        CompilerContext spawn_ctx(&meta, chunk);
        CompilerContext *saved = ctx;
        CompilerContext *saved_parent = parent_ctx;
        parent_ctx = saved;
        ctx = &spawn_ctx;
        bool saved_main_scope = is_main_scope;
        auto saved_loop_stack = std::move(loop_stack);
        int saved_try_depth = try_depth;
        loop_stack.clear();
        try_depth = 0;
        is_main_scope = false;
        collect_bindings(spawn_expr->body.get(), ctx->lexical_bindings);
        std::vector<std::string> captures;
        if (saved) {
            std::set<std::string> body_idents;
            collect_idents(spawn_expr->body.get(), body_idents);

            for (const auto &name : body_idents) {
                if (saved->resolve_local(name) != 0xFFFF || saved->capture_map.find(name) != saved->capture_map.end()) {
                    captures.push_back(name);
                }
            }
        }

        // Register captures in the spawn function context
        for (size_t i = 0; i < captures.size(); i++) {
            ctx->capture_map[captures[i]] = static_cast<uint16_t>(i);
            if (saved->const_locals.count(captures[i]) || saved->const_captures.count(captures[i])) {
                ctx->const_captures.insert(captures[i]);
            }
        }
        if (captures.size() > UINT16_MAX) {
            throw std::runtime_error("spawn captures too many variables (max 65535)");
        }
        meta.capture_count = static_cast<uint16_t>(captures.size());

        // Compile the body block; a missing/no-value return falls through to none
        compile_stmt(spawn_expr->body.get());
        ctx->emit_op(OpCode::OP_LOAD_NONE);
        ctx->emit_op(OpCode::OP_RETURN);
        meta.var_names = spawn_ctx.local_names;

        ctx = saved;
        parent_ctx = saved_parent;
        is_main_scope = saved_main_scope;
        loop_stack = std::move(saved_loop_stack);
        try_depth = saved_try_depth;

        // Add function to chunk and assign a unique name
        uint32_t func_idx = static_cast<uint32_t>(chunk->functions.size());
        std::string spawn_name = "<spawn_" + std::to_string(func_idx) + ">";
        meta.name = spawn_name;
        chunk->functions.push_back(std::move(meta));

        if (!captures.empty()) {
            // Emit OP_MAKE_CLOSURE: func_idx(2), capture_count(2), then for each
            // capture: source(1), index(2)
            ctx->emit_op(OpCode::OP_MAKE_CLOSURE);
            ctx->emit_short(static_cast<uint16_t>(func_idx));
            ctx->emit_short(static_cast<uint16_t>(captures.size()));
            for (const auto &cap : captures) {
                auto parent_cap = saved->capture_map.find(cap);
                if (parent_cap != saved->capture_map.end()) {
                    ctx->emit_byte(1); // source is parent capture
                    ctx->emit_short(parent_cap->second);
                } else {
                    uint16_t parent_idx = saved->resolve_local(cap);
                    if (parent_idx != 0xFFFF) {
                        ctx->emit_byte(0); // source is parent local
                        ctx->emit_short(parent_idx);
                        saved->note_local_captured(parent_idx);
                    } else {
                        ctx->emit_byte(2); // source is global
                        uint32_t str_idx = chunk->add_string(cap);
                        ctx->emit_short(static_cast<uint16_t>(str_idx));
                    }
                }
            }
        } else {
            // no captures, load the function by its registered global name
            uint32_t str_idx = chunk->add_string(spawn_name);
            ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, static_cast<uint16_t>(str_idx));
        }

        // OP_SPAWN pops the function value and pushes an async Handle
        ctx->emit_op(OpCode::OP_SPAWN);
        return;
    }

    fprintf(stderr, "unhandled expression type\n");
}

void Compiler::compile_stmt(const Stmt *stmt) {
    if (!stmt) {
        return;
    }

    // Record source line for stack traces / error messages.
    if (stmt->line > 0) {
        ctx->emit_line(stmt->line);
    }

    if (auto *expr_stmt = dynamic_cast<const ExprStmt *>(stmt)) {
        compile_expr(expr_stmt->expr.get());
        ctx->emit_op(OpCode::OP_POP); // discard result
        return;
    }

    if (auto *var_decl = dynamic_cast<const VarDeclStmt *>(stmt)) {
        // handle destructuring
        if (var_decl->destructure_kind == DestructureKind::Array) {
            if (var_decl->initializerExpr) {
                compile_expr(var_decl->initializerExpr.get());
            } else {
                ctx->emit_op(OpCode::OP_LOAD_NONE);
            }
            // for each array_names[i], get element i from the array
            for (size_t i = 0; i < var_decl->array_names.size(); i++) {
                ctx->emit_op(OpCode::OP_DUP); // dup array
                uint16_t idx_const = ctx->add_constant(Constant::make_int(i));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, idx_const);
                ctx->emit_op(OpCode::OP_GET_INDEX);
                uint16_t local_idx = ctx->declare_local(var_decl->array_names[i]);
                if (var_decl->is_const) {
                    ctx->const_locals.insert(var_decl->array_names[i]);
                }
                ctx->emit_op_short(OpCode::OP_STORE_VAR, local_idx);
                ctx->emit_op(OpCode::OP_POP);
            }
            ctx->emit_op(OpCode::OP_POP); // pop the array
            return;
        }
        if (var_decl->destructure_kind == DestructureKind::Object) {
            if (var_decl->initializerExpr) {
                compile_expr(var_decl->initializerExpr.get());
            } else {
                ctx->emit_op(OpCode::OP_LOAD_NONE);
            }
            // for each binding {key: varName}, get property key from the object
            for (const auto &[key, var_name] : var_decl->object_bindings) {
                ctx->emit_op(OpCode::OP_DUP); // dup object
                uint32_t str_idx = chunk->add_string(key);
                ctx->emit_op_short(OpCode::OP_GET_PROPERTY, (uint16_t)str_idx);
                uint16_t local_idx = ctx->declare_local(var_name);
                if (var_decl->is_const) {
                    ctx->const_locals.insert(var_name);
                }
                ctx->emit_op_short(OpCode::OP_STORE_VAR, local_idx);
                ctx->emit_op(OpCode::OP_POP);
            }
            ctx->emit_op(OpCode::OP_POP); // pop the object
            return;
        }

        if (var_decl->is_global) {
            if (var_decl->initializerExpr) {
                compile_expr(var_decl->initializerExpr.get());
            } else {
                ctx->emit_op(OpCode::OP_LOAD_NONE);
            }
            uint32_t str_idx = chunk->add_string(var_decl->name);
            ctx->emit_op_short(OpCode::OP_STORE_GLOBAL, (uint16_t)str_idx);
            ctx->emit_op(OpCode::OP_POP);
            // also declare a local so it can be referenced within the current scope
            uint16_t idx = ctx->declare_local(var_decl->name);
            if (var_decl->is_const) {
                ctx->const_locals.insert(var_decl->name);
                global_consts.insert(var_decl->name);
            }
            ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, (uint16_t)str_idx);
            ctx->emit_op_short(OpCode::OP_STORE_VAR, idx);
            ctx->emit_op(OpCode::OP_POP);
        } else {
            // pre-declare the local so closures in the initializer can capture it
            uint16_t idx = ctx->declare_local(var_decl->name);
            if (var_decl->is_const) {
                ctx->const_locals.insert(var_decl->name);
                if (is_main_scope) {
                    global_consts.insert(var_decl->name);
                }
            }
            if (var_decl->initializerExpr) {
                compile_expr(var_decl->initializerExpr.get());
            } else {
                ctx->emit_op(OpCode::OP_LOAD_NONE);
            }
            ctx->emit_op_short(OpCode::OP_STORE_VAR, idx);
            // in main scope, also store as global so named functions can access it
            if (is_main_scope) {
                ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx);
                uint32_t str_idx = chunk->add_string(var_decl->name);
                ctx->emit_op_short(OpCode::OP_STORE_GLOBAL, (uint16_t)str_idx);
                ctx->emit_op(OpCode::OP_POP);
            }
            ctx->emit_op(OpCode::OP_POP); // var decl doesn't produce a value
        }
        return;
    }

    if (auto *if_stmt = dynamic_cast<const IfStmt *>(stmt)) {
        compile_expr(if_stmt->cond.get());

        size_t else_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

        compile_stmt(if_stmt->then_branch.get());

        if (if_stmt->else_branch) {
            size_t end_jump = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(else_jump);

            compile_stmt(if_stmt->else_branch.get());
            ctx->patch_jump(end_jump);
        } else {
            ctx->patch_jump(else_jump);
        }
        return;
    }

    if (auto *while_stmt = dynamic_cast<const WhileStmt *>(stmt)) {
        size_t loop_start = ctx->function->code.size();
        // locals the body declares get slots at or above this, so closing from here
        // never touches a binding that lives outside the loop
        uint16_t loop_slot_watermark = ctx->local_count;

        loop_stack.push_back(LoopInfo());
        // `continue` is patched forward to the end-of-iteration point rather than
        // jumping straight back to loop_start, so it cannot skip OP_CLOSE_UPVALUES
        loop_stack.back().continue_target = LoopInfo::kContinueForward;
        loop_stack.back().outer_try_depth = this->try_depth;

        compile_expr(while_stmt->cond.get());

        size_t exit_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

        compile_stmt(while_stmt->body.get());

        for (size_t cp : loop_stack.back().continue_patches) {
            ctx->patch_jump(cp);
        }

        emit_close_upvalues_for_loop(loop_slot_watermark);

        // jump back to start (offset is relative to position after jump instruction)
        size_t current_pos = ctx->function->code.size();
        int32_t offset = loop_start - current_pos - 3;
        ctx->emit_op(OpCode::OP_JUMP);
        ctx->emit_short(offset);

        ctx->patch_jump(exit_jump);

        for (size_t bp : loop_stack.back().break_patches) {
            ctx->patch_jump(bp);
        }
        loop_stack.pop_back();
        return;
    }

    if (auto *block = dynamic_cast<const BlockStmt *>(stmt)) {
        ctx->push_scope();
        for (const auto &s : block->stmts) {
            compile_stmt(s.get());
        }
        ctx->pop_scope();
        return;
    }

    if (auto *assign = dynamic_cast<const AssignStmt *>(stmt)) {
        if (is_const_binding(assign->target)) {
            emit_const_assignment_error(assign->target);
            return;
        }
        // peephole: `v = v @ expr` -> OP_STR_APPEND_VAR / OP_STR_APPEND_GLOBAL
        // detects the pattern `target = target @ rhs` and emits a single string-append opcode
        if (auto *bin = dynamic_cast<const BinaryExpr *>(assign->value.get())) {
            if (bin->op == "@") {
                if (auto *lhs_ident = dynamic_cast<const IdentExpr *>(bin->left.get())) {
                    if (lhs_ident->name == assign->target) {
                        // Local always wins over capture/global (shadowing).
                        uint16_t idx = ctx->resolve_local(assign->target);
                        if (idx != 0xFFFF) {
                            // emit only the RHS, then in-place append
                            compile_expr(bin->right.get());
                            ctx->emit_op_short(OpCode::OP_STR_APPEND_VAR, idx);
                            ctx->emit_op(OpCode::OP_POP);
                            return;
                        }
                        // skip the peephole when captured
                        if (ctx->capture_map.find(assign->target) == ctx->capture_map.end() && !is_main_scope) {
                            // global variable
                            uint32_t str_idx = chunk->add_string(assign->target);
                            compile_expr(bin->right.get());
                            ctx->emit_op_short(OpCode::OP_STR_APPEND_GLOBAL, static_cast<uint16_t>(str_idx));
                            ctx->emit_op(OpCode::OP_POP);
                            return;
                        }
                    }
                }
            }
        }

        compile_expr(assign->value.get());

        // Locals shadow captures (see IdentExpr handling above).
        uint16_t idx = ctx->resolve_local(assign->target);
        if (idx != 0xFFFF) {
            ctx->emit_op_short(OpCode::OP_STORE_VAR, idx);
            // in main scope, sync to globals so named functions see updates
            if (is_main_scope) {
                ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx);
                uint32_t str_idx = chunk->add_string(assign->target);
                ctx->emit_op_short(OpCode::OP_STORE_GLOBAL, static_cast<uint16_t>(str_idx));
                ctx->emit_op(OpCode::OP_POP);
            }
        } else if (auto cap_it = ctx->capture_map.find(assign->target); cap_it != ctx->capture_map.end()) {
            ctx->emit_op_short(OpCode::OP_STORE_CAPTURE, cap_it->second);
        } else {
            uint32_t str_idx = chunk->add_string(assign->target);
            ctx->emit_op_short(OpCode::OP_STORE_GLOBAL, static_cast<uint16_t>(str_idx));
        }
        ctx->emit_op(OpCode::OP_POP); // assignment doesn't produce value
        return;
    }

    if (auto *idx_assign = dynamic_cast<const IndexAssignStmt *>(stmt)) {
        // check if target is a member expression (obj.prop = v)
        if (auto *member = dynamic_cast<const MemberExpr *>(idx_assign->target.get())) {
            compile_expr(member->object.get());
            compile_expr(idx_assign->value.get());
            uint32_t str_idx = chunk->add_string(member->member);
            ctx->emit_op_short(OpCode::OP_SET_PROPERTY, static_cast<uint16_t>(str_idx));
            ctx->emit_op(OpCode::OP_POP);
        }
        // check if target is an index expression (arr[i] = v)
        else if (auto *index = dynamic_cast<const IndexExpr *>(idx_assign->target.get())) {
            compile_expr(index->object.get());
            compile_expr(index->index.get());
            compile_expr(idx_assign->value.get());
            ctx->emit_op(OpCode::OP_SET_INDEX);
            ctx->emit_op(OpCode::OP_POP);
        }
        return;
    }

    if (auto *ret = dynamic_cast<const ReturnStmt *>(stmt)) {
        if (ret->value) {
            // tail-call self-recursion -> OP_SELF_TAIL_CALL
            // Detect `return current_func(arg0, arg1, ...)` with no rest params not
            // inside a try block.
            if (this->try_depth == 0 && ctx->function->rest_param_index < 0) {
                if (auto *tcall = dynamic_cast<const CallExpr *>(ret->value.get())) {
                    if (auto *ident = dynamic_cast<const IdentExpr *>(tcall->callee.get())) {
                        if (ident->name == ctx->function->name && !ctx->function->name.empty() &&
                            ctx->function->name[0] != '<' && // not a lambda
                            tcall->args.size() <= 255) {
                            // evaluate all new arg values onto stack, then overwrite params.
                            for (const auto &arg : tcall->args) {
                                compile_expr(arg.get());
                            }
                            ctx->emit_op_byte(OpCode::OP_SELF_TAIL_CALL, static_cast<uint8_t>(tcall->args.size()));
                            return;
                        }
                    }
                }
            }
            compile_expr(ret->value.get());
        } else {
            ctx->emit_op(OpCode::OP_LOAD_NONE);
        }
        // in strict mode, check the return value against the declared return type.
        // context byte 1 = return-value check. Value is already on the top of stack, and OP_CHECK_TYPE will
        // peek/validate/continue or throw.
        if (strict_mode && !current_return_type_.empty()) {
            uint32_t type_str_idx = chunk->add_string(current_return_type_);
            ctx->emit_op(OpCode::OP_CHECK_TYPE);
            ctx->emit_short(static_cast<uint16_t>(type_str_idx));
            ctx->emit_byte(1); // context: return-value check
        }
        // pop any enclosing try frames before returning
        for (int i = 0; i < this->try_depth; i++) {
            ctx->emit_op(OpCode::OP_POP_TRY);
        }
        ctx->emit_op(OpCode::OP_RETURN);
        return;
    }

    if (auto *for_stmt = dynamic_cast<const ForStmt *>(stmt)) {
        // taken before init so a `for (let i = ...)` binding is covered too
        uint16_t loop_slot_watermark = ctx->local_count;
        if (for_stmt->init) {
            compile_stmt(for_stmt->init.get());
        }

        size_t loop_start = ctx->function->code.size();

        size_t exit_jump = 0;
        bool has_cond = false;
        if (for_stmt->cond) {
            compile_expr(for_stmt->cond.get());
            exit_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);
            has_cond = true;
        }

        loop_stack.push_back(LoopInfo());
        loop_stack.back().outer_try_depth = this->try_depth;
        // continue must land on the post clause, whose position isn't known until the body is compiled
        loop_stack.back().continue_target = LoopInfo::kContinueForward;

        compile_stmt(for_stmt->body.get());

        for (size_t cp : loop_stack.back().continue_patches) {
            ctx->patch_jump(cp);
        }

        // before `post`: the update must land in the slot only, leaving the cell the
        // just-finished iteration handed to its closures holding that iteration's value
        emit_close_upvalues_for_loop(loop_slot_watermark);

        if (for_stmt->post) {
            compile_stmt(for_stmt->post.get());
        }

        size_t current_pos = ctx->function->code.size();
        int32_t offset = loop_start - current_pos - 3;
        ctx->emit_op(OpCode::OP_JUMP);
        ctx->emit_short(static_cast<uint16_t>(offset));

        if (has_cond) {
            ctx->patch_jump(exit_jump);
        }

        for (size_t bp : loop_stack.back().break_patches) {
            ctx->patch_jump(bp);
        }
        loop_stack.pop_back();
        return;
    }

    if (auto *foreach_stmt = dynamic_cast<const ForEachStmt *>(stmt)) {
        bool is_kv = !foreach_stmt->val_var.empty();

        // Single-variable for-in is desugared onto an index loop over hidden locals
        if (!is_kv) {
            // Desugar `for (v in iterable) { body }` into:
            //     __arr = normalize(iterable)               ; OP_ITER_ARRAY (array->self, obj->keys)
            //     __idx = -1
            //   loop_start (continue target):
            //     __idx = __idx + 1                         ; increment-first keeps `continue`
            //                                                  a backward jump to the top
            //     if !(__idx < __arr.length) goto exit      ; length re-read each iteration
            //     v = __arr[__idx]
            //     body
            //     goto loop_start
            //   exit:
            //
            static uint32_t forin_seq = 0;
            uint32_t seq = forin_seq++;
            uint16_t arr_idx = ctx->declare_local("$forin_arr$" + std::to_string(seq));
            uint16_t idx_idx = ctx->declare_local("$forin_idx$" + std::to_string(seq));
            uint16_t var_idx = ctx->declare_local(foreach_stmt->var);
            uint32_t length_str = ctx->chunk->add_string("length");

            // __arr = normalize(iterable)
            compile_expr(foreach_stmt->iterable.get());
            ctx->emit_op(OpCode::OP_ITER_ARRAY);
            ctx->emit_op_short(OpCode::OP_STORE_VAR, arr_idx);
            ctx->emit_op(OpCode::OP_POP);

            // __idx = -1
            ctx->emit_op(OpCode::OP_LOAD_ONE);
            ctx->emit_op(OpCode::OP_NEG);
            ctx->emit_op_short(OpCode::OP_STORE_VAR, idx_idx);
            ctx->emit_op(OpCode::OP_POP);

            // the loop var and any body local sit at or above this; the hidden $forin_arr$
            // / $forin_idx$ slots are below it and must survive the whole loop
            uint16_t loop_slot_watermark = var_idx;

            loop_stack.push_back(LoopInfo());
            loop_stack.back().outer_try_depth = this->try_depth;

            size_t loop_start = ctx->function->code.size();
            // forward-patched so `continue` cannot skip the end-of-iteration close
            loop_stack.back().continue_target = LoopInfo::kContinueForward;

            // __idx = __idx + 1
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx_idx);
            ctx->emit_op(OpCode::OP_LOAD_ONE);
            ctx->emit_op(OpCode::OP_ADD);
            ctx->emit_op_short(OpCode::OP_STORE_VAR, idx_idx);
            ctx->emit_op(OpCode::OP_POP);

            // if !(__idx < __arr.length) goto exit
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx_idx);
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, arr_idx);
            ctx->emit_op_short(OpCode::OP_GET_PROPERTY, static_cast<uint16_t>(length_str));
            ctx->emit_op(OpCode::OP_LT);
            size_t exit_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

            // v = __arr[__idx]
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, arr_idx);
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx_idx);
            ctx->emit_op(OpCode::OP_GET_INDEX);
            ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
            ctx->emit_op(OpCode::OP_POP);

            compile_stmt(foreach_stmt->body.get());

            for (size_t cp : loop_stack.back().continue_patches) {
                ctx->patch_jump(cp);
            }

            emit_close_upvalues_for_loop(loop_slot_watermark);

            // goto loop_start
            size_t current_pos = ctx->function->code.size();
            int32_t offset = static_cast<int32_t>(loop_start) - static_cast<int32_t>(current_pos) - 3;
            ctx->emit_op(OpCode::OP_JUMP);
            ctx->emit_short(static_cast<uint16_t>(offset));

            // exit: (stack is empty here, so no iterator cleanup is needed)
            ctx->patch_jump(exit_jump);
            for (size_t bp : loop_stack.back().break_patches) {
                ctx->patch_jump(bp);
            }

            loop_stack.pop_back();
            return;
        }

        compile_expr(foreach_stmt->iterable.get());
        ctx->emit_op(is_kv ? OpCode::OP_MAKE_ITERATOR_KV : OpCode::OP_MAKE_ITERATOR);

        uint16_t var_idx = ctx->declare_local(foreach_stmt->var);
        uint16_t val_idx = 0;
        if (is_kv) {
            val_idx = ctx->declare_local(foreach_stmt->val_var);
        }

        // key/value vars and body locals are at or above this slot
        uint16_t loop_slot_watermark = var_idx;

        loop_stack.push_back(LoopInfo());
        loop_stack.back().outer_try_depth = this->try_depth;

        size_t loop_start = ctx->function->code.size();
        // forward-patched so `continue` cannot skip the end-of-iteration close
        loop_stack.back().continue_target = LoopInfo::kContinueForward;

        ctx->emit_op(is_kv ? OpCode::OP_ITER_NEXT_KV : OpCode::OP_ITER_NEXT);

        // exit if done (pops false)
        size_t exit_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

        if (is_kv) {
            // stack looks like [..., pairs, index, key, value]
            ctx->emit_op_short(OpCode::OP_STORE_VAR, val_idx);
            ctx->emit_op(OpCode::OP_POP);
            ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
            ctx->emit_op(OpCode::OP_POP);
        } else {
            // store value in loop variable (STORE_VAR peeks, doesn't pop)
            ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
            // pop value from stack, leaving [..., array, index]
            ctx->emit_op(OpCode::OP_POP);
        }

        compile_stmt(foreach_stmt->body.get());

        for (size_t cp : loop_stack.back().continue_patches) {
            ctx->patch_jump(cp);
        }

        emit_close_upvalues_for_loop(loop_slot_watermark);

        size_t current_pos = ctx->function->code.size();
        int32_t offset = static_cast<int32_t>(loop_start) - static_cast<int32_t>(current_pos) - 3;
        ctx->emit_op(OpCode::OP_JUMP);
        ctx->emit_short(static_cast<uint16_t>(offset));

        ctx->patch_jump(exit_jump);

        // patch breaks to same location (before cleanup, so POPs also run)
        for (size_t bp : loop_stack.back().break_patches) {
            ctx->patch_jump(bp);
        }

        // clean up iterator from stack (array/pairs + index = 2 entries)
        ctx->emit_op(OpCode::OP_POP);
        ctx->emit_op(OpCode::OP_POP);

        loop_stack.pop_back();
        return;
    }

    if (dynamic_cast<const BreakStmt *>(stmt)) {
        if (!loop_stack.empty()) {
            // pop any try frames entered since the loop started
            int frames_to_pop = this->try_depth - loop_stack.back().outer_try_depth;
            for (int i = 0; i < frames_to_pop; i++) {
                ctx->emit_op(OpCode::OP_POP_TRY);
            }
            size_t patch = ctx->emit_jump(OpCode::OP_JUMP);
            loop_stack.back().break_patches.push_back(patch);
        }
        return;
    }

    if (dynamic_cast<const ContinueStmt *>(stmt)) {
        if (!loop_stack.empty()) {
            // pop any try frames entered since the loop started
            int frames_to_pop = this->try_depth - loop_stack.back().outer_try_depth;
            for (int i = 0; i < frames_to_pop; i++) {
                ctx->emit_op(OpCode::OP_POP_TRY);
            }
            if (loop_stack.back().continue_target == LoopInfo::kContinueForward) {
                // C-style for: target (post clause) not emitted yet
                size_t patch = ctx->emit_jump(OpCode::OP_JUMP);
                loop_stack.back().continue_patches.push_back(patch);
            } else {
                size_t current_pos = ctx->function->code.size();
                int32_t offset = loop_stack.back().continue_target - current_pos - 3;
                ctx->emit_op(OpCode::OP_JUMP);
                ctx->emit_short(static_cast<uint16_t>(offset));
            }
        }
        return;
    }

    if (auto *switch_stmt = dynamic_cast<const SwitchStmt *>(stmt)) {
        compile_expr(switch_stmt->value.get());

        std::vector<size_t> end_jumps;

        for (const auto &_case : switch_stmt->cases) {
            ctx->emit_op(OpCode::OP_DUP);
            compile_expr(_case.match.get());
            ctx->emit_op(OpCode::OP_EQ);
            size_t skip_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

            ctx->emit_op(OpCode::OP_POP);
            if (_case.body) {
                compile_stmt(_case.body.get());
            }
            end_jumps.push_back(ctx->emit_jump(OpCode::OP_JUMP));
            ctx->patch_jump(skip_jump);
        }

        ctx->emit_op(OpCode::OP_POP); // pop switch value
        if (switch_stmt->default_body) {
            compile_stmt(switch_stmt->default_body.get());
        }

        for (size_t j : end_jumps) {
            ctx->patch_jump(j);
        }
        return;
    }

    fprintf(stderr, "unhandled statement type\n");
}

template <typename FnLike> void Compiler::compile_function_body(const FnLike *func, FunctionMeta &meta) {
    constexpr bool is_plain_function = std::is_same<FnLike, Function>::value;

    CompilerContext context(&meta, chunk);
    CompilerContext *saved_ctx = ctx;
    ctx = &context;

    // Propagate strict_mode from the AST Function node.
    bool saved_strict = strict_mode;
    if constexpr (is_plain_function) {
        if (func->strict_mode) {
            strict_mode = true;
        }
    }
    meta.strict_mode = strict_mode;
    meta.source_file = func->filename;

    // Seed the line map with the function's definition line
    // so that errors inside the parameter-check preamble point at the function header.
    if (func->line > 0) {
        ctx->emit_line(func->line);
    }

    // strict mode enforcement: named non-lambda functions
    // MUST have type annotations on all non-rest, non-ignored parameters and a return type.
    if constexpr (is_plain_function) {
        if (strict_mode && !meta.is_lambda && !func->name.empty() && func->name.find("__top_level__") == std::string::npos &&
            func->name.find("__module_") == std::string::npos) {
            bool had_error = false;
            for (const auto &param : func->params) {
                if (param.is_rest) {
                    continue;
                }
                if (!param.name.empty() && param.name[0] == '_') {
                    continue;
                }
                if (!param.type) {
                    fprintf(
                        stderr, "StrictModeError: parameter '%s' of function '%s' has no type annotation%s\n", param.name.c_str(),
                        func->name.c_str(), func->loc_str().c_str()
                    );
                    had_error = true;
                }
            }
            if (!func->return_type) {
                fprintf(
                    stderr, "StrictModeError: function '%s' has no return type annotation (add '-> <type>')%s\n", func->name.c_str(),
                    func->loc_str().c_str()
                );
                had_error = true;
            }
            if (had_error) {
                exit(1);
            }
        }
    } // if constexpr (is_plain_function)

    // track return type for OP_RETURN injection.
    std::string saved_return_type = current_return_type_;
    current_return_type_ = (strict_mode && func->return_type) ? func->return_type->name : "";
    // Store the JIT vt of the return value for call-site optimization.
    if (current_return_type_ == "int") {
        meta.return_vt = 1;
    } else if (current_return_type_ == "float" || current_return_type_ == "number") {
        meta.return_vt = 2;
    } else {
        meta.return_vt = 0;
    }

    for (const auto &param : func->params) {
        ctx->declare_local(param.name);
        ctx->lexical_bindings.insert(param.name);
    }
    collect_bindings(func->body.get(), ctx->lexical_bindings);

    // in strict mode: emit a type check for each typed parameter right after
    // the function frame is set up.
    //  LOAD_VAR(i)  CHECK_TYPE(type_str, 0)  STORE_VAR(i)  POP
    // context byte 0 = parameter check
    if (strict_mode) {
        for (size_t i = 0; i < func->params.size(); i++) {
            const auto &param = func->params[i];
            if (!param.type || param.is_rest) {
                continue; // untyped or rest param
            }
            uint16_t var_idx = ctx->resolve_local(param.name);
            uint32_t type_str_idx = chunk->add_string(param.type->name);
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, var_idx);
            ctx->emit_op(OpCode::OP_CHECK_TYPE);
            ctx->emit_short(static_cast<uint16_t>(type_str_idx));
            ctx->emit_byte(0); // context: param check
            ctx->emit_op_short(OpCode::OP_STORE_VAR, var_idx);
            ctx->emit_op(OpCode::OP_POP);
        }
    }

    for (const auto &param : func->params) {
        if (param.default_value && !is_js_undefined_param(param)) {
            uint16_t idx = ctx->resolve_local(param.name);
            ctx->emit_op_short(OpCode::OP_LOAD_VAR, idx);
            size_t skip = ctx->emit_jump(OpCode::OP_JUMP_IF_NONE);
            size_t end = ctx->emit_jump(OpCode::OP_JUMP);
            ctx->patch_jump(skip);
            compile_expr(param.default_value.get());
            ctx->emit_op_short(OpCode::OP_STORE_VAR, idx);
            ctx->emit_op(OpCode::OP_POP);
            ctx->patch_jump(end);
        }
    }

    // handle rest params - record the index for the VM
    for (size_t i = 0; i < func->params.size(); i++) {
        if (func->params[i].is_rest) {
            meta.rest_param_index = static_cast<int8_t>(i);
            break;
        }
    }

    if (func->body) {
        compile_stmt(func->body.get());
    }

    // implicit return none at end
    ctx->emit_op(OpCode::OP_LOAD_NONE);
    ctx->emit_op(OpCode::OP_RETURN);

    meta.var_names = context.local_names;

    current_return_type_ = saved_return_type;
    strict_mode = saved_strict;
    ctx = saved_ctx;
}

// Compile every registered class into chunk functions.
// Per class we emit:
//   - one function per declared method (instance, static, and the 'init' ctor,
//     which is separately callable as a normal method: `a.init(x)` works)
//   - <Class>.__init__  : field defaults, then a forward call to 'init' if present
//   - <Class>.__static_init__ : static field defaults (only when some static has one)
void Compiler::compile_classes() {
    for (const auto &entry : Parser::get_all_registered_classes()) {
        const nari::ClassDecl *cd = entry.second;
        if (!cd) {
            continue;
        }
        // __init__ is emitted for every class, so this doubles as "already compiled".
        // Matters for eval(): compile_classes() walks the whole parser registry each call.
        if (chunk->class_init_idx.count(cd)) {
            continue;
        }
        const std::string &cname = entry.first;

        for (const auto &method : cd->methods) {
            FunctionMeta meta;
            meta.name = cname + "." + method.name;
            meta.param_count = static_cast<uint8_t>(method.params.size());
            meta.capture_count = 0;
            meta.is_lambda = false;
            meta.js_undefined_params = std::any_of(method.params.begin(), method.params.end(), is_js_undefined_param);
            compile_function_body(&method, meta);
            chunk->method_func_idx[&method] = static_cast<uint32_t>(chunk->functions.size());
            chunk->functions.push_back(std::move(meta));
        }

        // <Class>.__init__: run field defaults against `this`, then forward to the ctor.
        // Fields come from bc_collect_all_fields so inherited fields are included,
        // in the same parent-first order the ClassLayout uses.
        std::vector<const nari::ClassField *> all_fields;
        bc_collect_all_fields(cd, all_fields);
        const nari::ClassMethod *ctor = bc_find_method(cd, "init");
        if (ctor && !ctor->is_constructor) {
            ctor = nullptr;
        }

        {
            FunctionMeta meta;
            meta.name = cname + ".__init__";
            meta.param_count = static_cast<uint8_t>(ctor ? ctor->params.size() : 0);
            meta.capture_count = 0;
            meta.is_lambda = false;
            meta.source_file = cd->filename;

            CompilerContext context(&meta, chunk);
            CompilerContext *saved_ctx = ctx;
            ctx = &context;
            if (cd->line > 0) {
                ctx->emit_line(cd->line);
            }

            // Ctor params occupy the leading local slots so the forwarding call can
            // reload them; declared before the field defaults so both see one frame.
            if (ctor) {
                for (const auto &param : ctor->params) {
                    ctx->declare_local(param.name);
                    ctx->lexical_bindings.insert(param.name);
                }
            }

            for (const nari::ClassField *field : all_fields) {
                if (!field->default_value) {
                    continue; // slot already defaults to none
                }
                ctx->emit_op(OpCode::OP_LOAD_THIS);
                compile_expr(field->default_value.get());
                uint32_t name_idx = chunk->add_string(field->name);
                ctx->emit_op_short(OpCode::OP_SET_PROPERTY, static_cast<uint16_t>(name_idx));
                ctx->emit_op(OpCode::OP_POP); // SET_PROPERTY leaves the value
            }

            if (ctor) {
                ctx->emit_op(OpCode::OP_LOAD_THIS);
                for (size_t i = 0; i < ctor->params.size(); i++) {
                    ctx->emit_op_short(OpCode::OP_LOAD_VAR, static_cast<uint16_t>(i));
                }
                uint32_t init_idx = chunk->add_string("init");
                ctx->emit_op(OpCode::OP_CALL_METHOD);
                ctx->emit_short(static_cast<uint16_t>(init_idx));
                ctx->emit_byte(static_cast<uint8_t>(ctor->params.size()));
                ctx->emit_op(OpCode::OP_POP); // ctor return value is discarded
            }

            ctx->emit_op(OpCode::OP_LOAD_NONE);
            ctx->emit_op(OpCode::OP_RETURN);
            meta.var_names = context.local_names;
            ctx = saved_ctx;

            chunk->class_init_idx[cd] = static_cast<uint32_t>(chunk->functions.size());
            chunk->functions.push_back(std::move(meta));
        }

        // <Class>.__static_init__: `<Class>.field = <default>` per static field with a default.
        // Assigning through the class name is what user code already compiles to
        // (OP_SET_PROPERTY's is_string branch writes Parser::get_static_fields()).
        bool any_static_default = false;
        for (const auto &field : cd->fields) {
            if (field.is_static && field.default_value) {
                any_static_default = true;
                break;
            }
        }
        if (any_static_default) {
            FunctionMeta meta;
            meta.name = cname + ".__static_init__";
            meta.param_count = 0;
            meta.capture_count = 0;
            meta.is_lambda = false;
            meta.source_file = cd->filename;

            CompilerContext context(&meta, chunk);
            CompilerContext *saved_ctx = ctx;
            ctx = &context;
            if (cd->line > 0) {
                ctx->emit_line(cd->line);
            }

            uint32_t cname_idx = chunk->add_string(cname);
            for (const auto &field : cd->fields) {
                if (!field.is_static || !field.default_value) {
                    continue;
                }
                uint16_t const_idx = ctx->add_constant(Constant::make_string(cname_idx));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, const_idx);
                compile_expr(field.default_value.get());
                uint32_t name_idx = chunk->add_string(field.name);
                ctx->emit_op_short(OpCode::OP_SET_PROPERTY, static_cast<uint16_t>(name_idx));
                ctx->emit_op(OpCode::OP_POP);
            }

            ctx->emit_op(OpCode::OP_LOAD_NONE);
            ctx->emit_op(OpCode::OP_RETURN);
            meta.var_names = context.local_names;
            ctx = saved_ctx;

            chunk->class_static_init_idx[cd] = static_cast<uint32_t>(chunk->functions.size());
            chunk->functions.push_back(std::move(meta));
        }
    }
}

Chunk *Compiler::compile(const FuncList &functions) {
    chunk = new Chunk();

#ifdef NARI_EXTENDED_JSRT
    const std::unordered_map<std::string, OpCode> candidates = {
        { "__js_bitand", OpCode::OP_JS_BIT_AND },
        { "__js_bitor", OpCode::OP_JS_BIT_OR },
        { "__js_bitxor", OpCode::OP_JS_BIT_XOR },
        { "__js_bitnot", OpCode::OP_JS_BIT_NOT },
        { "__js_shl", OpCode::OP_JS_SHL },
        { "__js_shr", OpCode::OP_JS_SHR },
        { "__js_ushr", OpCode::OP_JS_USHR },
        { "__js_get_prop_static", OpCode::OP_JS_GET_PROP_STATIC },
        { "__js_set_prop_static", OpCode::OP_JS_SET_PROP_STATIC },
        { "__js_apply_array", OpCode::OP_CALL_SPREAD },
        { "__js_postinc", OpCode::OP_JS_POSTINC },
    };
    std::unordered_map<std::string, size_t> definitions;
    std::set<std::string> assigned;
    for (const auto &func : functions) {
        if (!func) {
            continue;
        }
        if (candidates.count(func->name)) {
            definitions[func->name]++;
        }
        const auto scan_assignments = [&](const auto &self, const Stmt *stmt) -> void {
            if (!stmt) {
                return;
            }
            if (const auto *assign = dynamic_cast<const AssignStmt *>(stmt)) {
                assigned.insert(assign->target);
            }
            if (const auto *block = dynamic_cast<const BlockStmt *>(stmt)) {
                for (const auto &child : block->stmts) {
                    self(self, child.get());
                }
            } else if (const auto *branch = dynamic_cast<const IfStmt *>(stmt)) {
                self(self, branch->then_branch.get());
                self(self, branch->else_branch.get());
            } else if (const auto *loop = dynamic_cast<const WhileStmt *>(stmt)) {
                self(self, loop->body.get());
            } else if (const auto *loop = dynamic_cast<const ForStmt *>(stmt)) {
                self(self, loop->init.get());
                self(self, loop->post.get());
                self(self, loop->body.get());
            } else if (const auto *loop = dynamic_cast<const ForEachStmt *>(stmt)) {
                self(self, loop->body.get());
            } else if (const auto *switch_stmt = dynamic_cast<const SwitchStmt *>(stmt)) {
                for (const auto &item : switch_stmt->cases) {
                    self(self, item.body.get());
                }
                self(self, switch_stmt->default_body.get());
            }
        };
        scan_assignments(scan_assignments, func->body.get());
    }
    const bool no_postinc_op = std::getenv("NARI_NO_POSTINC_OP") != nullptr;
    for (const auto &func : functions) {
        if (!func) {
            continue;
        }
        auto candidate = candidates.find(func->name);
        if (candidate != candidates.end() && no_postinc_op && candidate->second == OpCode::OP_JS_POSTINC) {
            continue;
        }
        if (candidate != candidates.end() && definitions[func->name] == 1 && !assigned.count(func->name) &&
            matches_extended_jsrt_helper(func.get(), candidate->second)) {
            extended_jsrt_helpers.emplace(func->name, candidate->second);
        }
    }
#endif

    // named functions are compiled before <main>, so collect top-level consts first.
    for (const auto &func : functions) {
        if (!func || func->name.find("__top_level__@") != 0 || !func->body) {
            continue;
        }
        for (const auto &stmt : func->body->stmts) {
            const auto *decl = dynamic_cast<const VarDeclStmt *>(stmt.get());
            if (!decl || !decl->is_const) {
                continue;
            }
            if (decl->destructure_kind == DestructureKind::Array) {
                global_consts.insert(decl->array_names.begin(), decl->array_names.end());
            } else if (decl->destructure_kind == DestructureKind::Object) {
                for (const auto &[_, name] : decl->object_bindings) {
                    global_consts.insert(name);
                }
            } else {
                global_consts.insert(decl->name);
            }
        }
    }

    {
        std::set<std::string> global_names;
        for (const auto &func : functions) {
            if (!func || !func->body) {
                continue;
            }
            for (const auto &stmt : func->body->stmts) {
                const auto *decl = dynamic_cast<const VarDeclStmt *>(stmt.get());
                if (decl && decl->is_global) {
                    global_names.insert(decl->name);
                }
            }
        }
        for (const auto &func : functions) {
            // a lambda assigned straight to a global (`global Ok = func(...)`) is named after that global on purpose,
            // and an enum variant ctor is synthesized rather than user-written to avoid collision
            if (!func || func->function_expr != nullptr || func->is_enum_ctor) {
                continue;
            }
            const std::string &fname = func->name;
            if (fname.empty() || fname[0] == '<' || fname.find('.') != std::string::npos || fname.compare(0, 2, "__") == 0) {
                continue; // <main>/<lambda_N>, class methods, and compiler internals
            }
            if (global_names.count(fname)) {
                fprintf(
                    stderr,
                    "warning: top-level func '%s' is shadowed by a global of the same name and will never be "
                    "called; rename it\n",
                    fname.c_str()
                );
            }
        }
    }

    std::vector<const Function *> top_level_bodies;
    for (size_t i = 0; i < functions.size(); i++) {
        const Function *func = functions[i].get();
        if (!func) {
            continue;
        }

        if (func->name.find("__top_level__@") == 0) {
            top_level_bodies.push_back(func);
            continue;
        }
        if (func->name == "__top_level__") {
            continue;
        }

        FunctionMeta meta;
        meta.name = func->name;
        meta.param_count = static_cast<uint8_t>(func->params.size());
        meta.capture_count = 0;
        meta.is_lambda = false;
        meta.js_undefined_params = std::any_of(func->params.begin(), func->params.end(), is_js_undefined_param);
        // strict_mode is propagated inside compile_function_body from func->strict_mode

        compile_function_body(func, meta);

        chunk->functions.push_back(std::move(meta));
    }

    compile_classes();

    FunctionMeta main_func;
    main_func.name = "<main>";
    main_func.param_count = 0;
    main_func.capture_count = 0;
    main_func.is_lambda = false;
    // inherit source_file from the first top-level body that has one.
    for (const Function *body : top_level_bodies) {
        if (body && !body->filename.empty()) {
            main_func.source_file = body->filename;
            break;
        }
    }

    CompilerContext context(&main_func, chunk);
    ctx = &context;
    for (const Function *body : top_level_bodies) {
        if (body && body->body) {
            collect_bindings(body->body.get(), ctx->lexical_bindings);
        }
    }

    // compile all top-level statements from all top-level functions (imports first, then main)
    is_main_scope = true;
    for (const Function *body : top_level_bodies) {
        if (body && body->body) {
            compile_stmt(body->body.get());
        }
    }
    is_main_scope = false;

    // implicit return none at end
    ctx->emit_op(OpCode::OP_LOAD_NONE);
    ctx->emit_op(OpCode::OP_RETURN);

    main_func.var_names = context.local_names;

    uint32_t main_idx = static_cast<uint32_t>(chunk->functions.size());
    chunk->functions.push_back(std::move(main_func));
    chunk->main_func_idx = main_idx;

    // snapshot registered type declarations into the chunk so they survive serialization to .naric
    for (const auto &[name, decl] : Parser::get_all_registered_types()) {
        TypeInfo typeInfo;
        typeInfo.name = name;
        typeInfo.is_union = decl->kind == TypeDeclKind::Union;
        if (decl->is_alias() && decl->alias_target) {
            typeInfo.alias_target = decl->alias_target->name;
        }
        for (const auto &field : decl->fields) {
            typeInfo.fields.emplace_back(
                field.name, field.type ? field.type->name : "number", field.type ? field.type->is_array : false,
                field.type ? field.type->fixed_array_count : 0
            );
        }
        chunk->types.push_back(std::move(typeInfo));
    }

    return chunk;
}

Chunk *compile_bytecode(const FuncList &functions) {
    Compiler compiler;
    Chunk *chunk = compiler.compile(functions);
    if (chunk != nullptr) {
        if (!BytecodeVerifier::verify(*chunk)) {
            delete chunk;
            return nullptr;
        }
    }
    return chunk;
}

AppendedCode Compiler::compile_append(Chunk *existing, const FuncList &functions, const std::string &entry_name) {
    AppendedCode out;
    chunk = existing; // borrowed: never deleted here, and main_func_idx is left alone

    for (const auto &fptr : functions) {
        const Function *func = fptr.get();
        if (!func || func->name == "__top_level__") {
            continue;
        }

        FunctionMeta meta;
        meta.name = func->name;
        meta.param_count = static_cast<uint8_t>(func->params.size());
        meta.capture_count = 0;
        meta.is_lambda = false;
        meta.js_undefined_params = std::any_of(func->params.begin(), func->params.end(), is_js_undefined_param);

        compile_function_body(func, meta);

        uint32_t idx = static_cast<uint32_t>(chunk->functions.size());
        chunk->functions.push_back(std::move(meta));
        if (!entry_name.empty() && func->name == entry_name) {
            out.entry_idx = idx;
        } else if (func->name.compare(0, 14, "__top_level__@") == 0) {
            out.toplevel_idxs.push_back(idx);
        }
    }

    compile_classes(); // classes declared inside the eval'd source
    return out;
}

AppendedCode compile_bytecode_append(Chunk *existing, const FuncList &functions, const std::string &entry_name) {
    Compiler compiler;
    return compiler.compile_append(existing, functions, entry_name);
}

} // namespace bytecode
} // namespace nari
