#include "ast.h"
#include "bytecode.h"
#include "parser_api.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace nari;

namespace nari {
namespace bytecode {

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
    uint16_t local_count;

    CompilerContext(FunctionMeta *f, Chunk *c) : function(f), chunk(c), local_count(0) {
    }

    uint16_t declare_local(const std::string &name) {
        if (local_count >= 0xFFFE) {
            fprintf(stderr, "error: too many local variables in function (max 65534)!\n");
            return 0xFFFF;
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
        // (C-style for: continue lands on the post clause, which is emitted after the body)
        static constexpr size_t kContinueForward = SIZE_MAX;
        std::vector<size_t> break_patches;
        std::vector<size_t> continue_patches;
        size_t continue_target = 0;
        int outer_try_depth; // try_depth at the point the loop was entered
    };
    std::vector<LoopInfo> loop_stack;

    bool is_main_scope; // true when compiling the <main> function body
    int try_depth = 0; // nesting depth of try blocks (TCO forbidden when > 0)
    bool strict_mode = false; // true when the current source file has "use strict" at the top level
    std::string current_return_type_; // e.g. "int", "string", "bool"
    std::set<std::string> global_consts;

    void compile_expr(const Expr *expr);
    void compile_stmt(const Stmt *stmt);
    void compile_function_body(const Function *func, FunctionMeta &meta);
    void collect_idents(const Stmt *stmt, std::set<std::string> &idents);
    void collect_idents_expr(const Expr *expr, std::set<std::string> &idents);
    bool is_const_binding(const std::string &name) const;
    void emit_const_assignment_error(const std::string &name);

  public:
    Compiler() : chunk(nullptr), ctx(nullptr), parent_ctx(nullptr), is_main_scope(false) {
    }

    Chunk *compile(const FuncList &functions);
};

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
    if (dynamic_cast<const StringInterpolationExpr *>(expr)) {
        // StringInterpolationExpr has parts/expr_sources (strings), no ExprPtr children
        return;
    }
    // Note: we *do* descend into FunctionExpr to collect transitive captures
    if (auto *fn = dynamic_cast<const FunctionExpr *>(expr)) {
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
        // Local declarations shadow captured upvalues with the same name.
        // (A `let x = ...;` inside a function body must override an outer `x`
        //  that happened to be captured at function-entry capture-collection.)
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
            // Registered type names resolve at compile time to a string constant
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
        if (unary->op == "++" || unary->op == "--" || unary->op == "post++" ||
            unary->op == "post--") {
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
            uint32_t global_str_idx = 0;
            if (!is_local) {
                global_str_idx = chunk->add_string(ident->name);
            }

            if (is_postfix) {
                // postfix: result is the OLD value
                // load old value (this will be the result)
                if (is_local) {
                    ctx->emit_op_short(OpCode::OP_LOAD_VAR, local_idx);
                } else {
                    ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, static_cast<uint16_t>(global_str_idx));
                }
                // duplicate old value (keep one copy as result)
                ctx->emit_op(OpCode::OP_DUP);
                uint16_t one_idx = ctx->add_constant(Constant::make_int(1));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, one_idx);
                ctx->emit_op(is_increment ? OpCode::OP_ADD : OpCode::OP_SUB);
                // store new value (leaves new value on stack)
                if (is_local) {
                    ctx->emit_op_short(OpCode::OP_STORE_VAR, local_idx);
                } else {
                    ctx->emit_op_short(OpCode::OP_STORE_GLOBAL, static_cast<uint16_t>(global_str_idx));
                }
                // pop the new value, leaving old value as result
                ctx->emit_op(OpCode::OP_POP);
            } else {
                // prefix: result is the NEW value
                // load current value
                if (is_local) {
                    ctx->emit_op_short(OpCode::OP_LOAD_VAR, local_idx);
                } else {
                    ctx->emit_op_short(OpCode::OP_LOAD_GLOBAL, static_cast<uint16_t>(global_str_idx));
                }
                uint16_t one_idx = ctx->add_constant(Constant::make_int(1));
                ctx->emit_op_short(OpCode::OP_LOAD_CONST, one_idx);
                ctx->emit_op(is_increment ? OpCode::OP_ADD : OpCode::OP_SUB);
                // store new value (leaves new value on stack; this IS the result)
                if (is_local) {
                    ctx->emit_op_short(OpCode::OP_STORE_VAR, local_idx);
                } else {
                    ctx->emit_op_short(OpCode::OP_STORE_GLOBAL, static_cast<uint16_t>(global_str_idx));
                }
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
            // Build object incrementally: start with empty object, set/spread each
            // entry
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
        meta.param_count = static_cast<uint8_t>(func_expr->params.size());
        meta.capture_count = 0;
        meta.is_lambda = true;

        // collect identifiers used in the body to detect captures
        CompilerContext lambda_ctx(&meta, chunk);
        CompilerContext *saved = ctx;
        CompilerContext *saved_parent = parent_ctx;
        parent_ctx = saved;
        ctx = &lambda_ctx;
        bool saved_main_scope = is_main_scope;
        is_main_scope = false;
        std::vector<std::string> captures;
        if (saved) {
            std::set<std::string> body_idents;
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
                if (saved->resolve_local(name) != 0xFFFF ||
                    saved->capture_map.find(name) != saved->capture_map.end()) {
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
        meta.capture_count = static_cast<uint8_t>(captures.size());
        if (captures.size() > 255) {
            throw std::runtime_error("closure captures too many variables (max 255)");
        }

        // declare params as locals
        for (const auto &param : func_expr->params) {
            ctx->declare_local(param.name);
        }

        // compile default parameter values
        for (const auto &param : func_expr->params) {
            if (param.default_value) {
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

        uint32_t func_idx = static_cast<uint32_t>(chunk->functions.size());
        std::string lambda_name = "<lambda_" + std::to_string(func_idx) + ">";
        meta.name = lambda_name;
        chunk->functions.push_back(std::move(meta));

        if (!captures.empty()) {
            // emit OP_MAKE_CLOSURE: func_idx(2), capture_count(1), then for each
            // capture: source(1), index(2) source: 0=parent local, 1=parent capture
            ctx->emit_op(OpCode::OP_MAKE_CLOSURE);
            ctx->emit_short(static_cast<uint16_t>(func_idx));
            ctx->emit_byte(static_cast<uint8_t>(captures.size()));
            for (const auto &cap : captures) {
                auto parent_cap = saved->capture_map.find(cap);
                if (parent_cap != saved->capture_map.end()) {
                    ctx->emit_byte(1); // source: parent capture
                    ctx->emit_short(parent_cap->second);
                } else {
                    uint16_t parent_idx = saved->resolve_local(cap);
                    if (parent_idx != 0xFFFF) {
                        ctx->emit_byte(0); // source: parent local
                        ctx->emit_short(parent_idx);
                    } else {
                        // fallback: try global
                        ctx->emit_byte(2); // source: global
                        uint32_t str_idx = chunk->add_string(cap);
                        ctx->emit_short(static_cast<uint16_t>(str_idx));
                    }
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
                    uint16_t name_const =
                        ctx->add_constant(Constant::make_string(name_str));
                    ctx->emit_op_short(OpCode::OP_LOAD_CONST, name_const);
                    ctx->emit_op(OpCode::OP_EQ);
                    size_t variant_match = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

                    // matched as a variant name
                    ctx->emit_op(OpCode::OP_POP); // pop scrutinee
                    compile_expr(arm.body.get());
                    end_jumps.push_back(ctx->emit_jump(OpCode::OP_JUMP));
                    ctx->patch_jump(variant_match);

                    // not a variant match; treat as a catch-all binding and bind the scrutinee value to the variable name
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

    // compiles the body as an anonymous 0-param function and emits OP_SPAWN so the VM runs it asynchronously, returning a Handle value.
    if (auto *spawn_expr = dynamic_cast<const SpawnExpr *>(expr)) {
        if (!spawn_expr->body) {
            ctx->emit_op(OpCode::OP_LOAD_NONE);
            return;
        }

        // compile spawn body as a 0-param anonymous function (same pattern as lambda compilation), capturing any referenced parent local.
        FunctionMeta meta;
        meta.name = "<spawn>";
        meta.param_count = 0;
        meta.capture_count = 0;
        meta.is_lambda = true;

        CompilerContext spawn_ctx(&meta, chunk);
        CompilerContext *saved = ctx;
        CompilerContext *saved_parent = parent_ctx;
        parent_ctx = saved;
        ctx = &spawn_ctx;
        bool saved_main_scope = is_main_scope;
        is_main_scope = false;
        std::vector<std::string> captures;
        if (saved) {
            std::set<std::string> body_idents;
            collect_idents(spawn_expr->body.get(), body_idents);

            for (const auto &name : body_idents) {
                if (saved->resolve_local(name) != 0xFFFF ||
                    saved->capture_map.find(name) != saved->capture_map.end()) {
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
        meta.capture_count = static_cast<uint8_t>(captures.size());
        if (captures.size() > 255) {
            throw std::runtime_error("spawn captures too many variables (max 255)");
        }

        // Compile the body block; a missing/no-value return falls through to none
        compile_stmt(spawn_expr->body.get());
        ctx->emit_op(OpCode::OP_LOAD_NONE);
        ctx->emit_op(OpCode::OP_RETURN);
        meta.var_names = spawn_ctx.local_names;

        ctx = saved;
        parent_ctx = saved_parent;
        is_main_scope = saved_main_scope;

        // Add function to chunk and assign a unique name
        uint32_t func_idx = static_cast<uint32_t>(chunk->functions.size());
        std::string spawn_name = "<spawn_" + std::to_string(func_idx) + ">";
        meta.name = spawn_name;
        chunk->functions.push_back(std::move(meta));

        if (!captures.empty()) {
            // Emit OP_MAKE_CLOSURE: func_idx(2), capture_count(1), then for each
            // capture: source(1), index(2)
            ctx->emit_op(OpCode::OP_MAKE_CLOSURE);
            ctx->emit_short(static_cast<uint16_t>(func_idx));
            ctx->emit_byte(static_cast<uint8_t>(captures.size()));
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

        loop_stack.push_back(LoopInfo());
        loop_stack.back().continue_target = loop_start;
        loop_stack.back().outer_try_depth = this->try_depth;

        compile_expr(while_stmt->cond.get());

        size_t exit_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

        compile_stmt(while_stmt->body.get());

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
        for (const auto &s : block->stmts) {
            compile_stmt(s.get());
        }
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
                        // Not local: skip the peephole when captured (no
                        // STR_APPEND_CAPTURE opcode); fall through to general
                        // assignment path below.
                        if (ctx->capture_map.find(assign->target) == ctx->capture_map.end() &&
                            !is_main_scope) {
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
        } else if (auto cap_it = ctx->capture_map.find(assign->target);
                   cap_it != ctx->capture_map.end()) {
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
                        if (ident->name == ctx->function->name &&
                            !ctx->function->name.empty() &&
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
        // context byte 1 = return-value check. Value is already on the top of stack, and OP_CHECK_TYPE will peek/validate/continue or throw.
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
            //
            //     __arr = normalize(iterable)   ; OP_ITER_ARRAY (array->self, obj->keys)
            //     __idx = -1
            //   loop_start (continue target):
            //     __idx = __idx + 1             ; increment-first keeps `continue`
            //                                     a backward jump to the top
            //     if !(__idx < __arr.length) goto exit   ; length re-read each iteration
            //     v = __arr[__idx]
            //     body
            //     goto loop_start
            //   exit:
            //
            // All loop state lives in hidden locals, so the loop header has an
            // entry stack depth of 0 and every opcode is in the JIT's P1 subset.
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

            loop_stack.push_back(LoopInfo());
            loop_stack.back().outer_try_depth = this->try_depth;

            size_t loop_start = ctx->function->code.size();
            loop_stack.back().continue_target = loop_start;

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

        loop_stack.push_back(LoopInfo());
        loop_stack.back().outer_try_depth = this->try_depth;

        size_t loop_start = ctx->function->code.size();
        loop_stack.back().continue_target = loop_start;

        ctx->emit_op(is_kv ? OpCode::OP_ITER_NEXT_KV : OpCode::OP_ITER_NEXT);

        // exit if done (pops false)
        size_t exit_jump = ctx->emit_jump(OpCode::OP_JUMP_IF_FALSE);

        if (is_kv) {
            // stack: [..., pairs, index, key, value]
            // store value first (it's on top), then key
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

void Compiler::compile_function_body(const Function *func, FunctionMeta &meta) {
    CompilerContext context(&meta, chunk);
    CompilerContext *saved_ctx = ctx;
    ctx = &context;

    // Propagate strict_mode from the AST Function node.
    bool saved_strict = strict_mode;
    if (func->strict_mode) {
        strict_mode = true;
    }
    meta.strict_mode = strict_mode;
    meta.source_file = func->filename;

    // Seed the line map with the function's definition line so that errors inside the parameter-check preamble point at the function header.
    if (func->line > 0) {
        ctx->emit_line(func->line);
    }

    // strict mode enforcement: named non-lambda functions MUST have type annotations on all non-rest, non-ignored parameters and a return type.
    // Lambdas/anonymous functions and internal compiler-generated functions are exempt.
    if (strict_mode && !meta.is_lambda && !func->name.empty() &&
        func->name.find("__top_level__") == std::string::npos &&
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
                    stderr,
                    "StrictModeError: parameter '%s' of function '%s' has no type annotation%s\n",
                    param.name.c_str(), func->name.c_str(),
                    func->loc_str().c_str());
                had_error = true;
            }
        }
        if (!func->return_type) {
            fprintf(
                stderr,
                "StrictModeError: function '%s' has no return type annotation (add '-> <type>')%s\n",
                func->name.c_str(), func->loc_str().c_str());
            had_error = true;
        }
        if (had_error) {
            exit(1);
        }
    }

    // Track return type for OP_RETURN injection.
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
    }

    // In strict mode: emit a type check for each typed parameter right after
    // the function frame is set up (before any user code runs).
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
        if (param.default_value) {
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

Chunk *Compiler::compile(const FuncList &functions) {
    chunk = new Chunk();

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
        // strict_mode is propagated inside compile_function_body from func->strict_mode

        compile_function_body(func, meta);

        chunk->functions.push_back(std::move(meta));
    }

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
                field.name, field.type ? field.type->name : "number",
                field.type ? field.type->is_array : false,
                field.type ? field.type->fixed_array_count : 0);
        }
        chunk->types.push_back(std::move(typeInfo));
    }

    return chunk;
}

Chunk *compile_bytecode(const FuncList &functions) {
    Compiler compiler;
    return compiler.compile(functions);
}

} // namespace bytecode
} // namespace nari
