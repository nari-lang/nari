#pragma once
// Parser class declaration. Method definitions live in the parse_*.cpp
// translation units; this header holds the class shape (members, nested
// types, template + inline helpers) so each TU sees the full type.

#include "ast.h"
#include "int_overflow.h"
#include "nari_fs.h"
#include "parser/lexer.h"
#include "parser/module_resolver.h"
#include "parser/parser_internal.h"
#include "parser/token.h"
#include "parser_api.h"

#include <cstdio>
#include <ctype.h>
#include <errno.h>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __linux__
#include <elf.h>
#endif

using nari::BlockPtr;
using nari::ExprPtr;
using nari::FunctionPtr;
using nari::StmtPtr;
using nari::VarDeclCtrl;

const std::string &nari_std_module_source(const std::string &name);
namespace Parser {

class Parser {
  public:
    // Standard mode: first error calls std::exit(1).
    explicit Parser(const std::vector<Token> &tokens) : toks(tokens), idx(0), recover_mode(false) {
    }
    // Recovery mode: errors are collected; parsing resumes after each bad site.
    Parser(const std::vector<Token> &tokens, bool recover_mode) : toks(tokens), idx(0), recover_mode(recover_mode) {
    }

    // Returns accumulated diagnostics (valid only when recover_mode == true).
    std::vector<ParseError> take_errors() {
        return std::move(errors_);
    }

    std::vector<FunctionPtr> parse_program(bool create_aggregator = true);

  private:
    const std::vector<Token> &toks;
    size_t idx;
    bool recover_mode;
    bool strict_mode = false; // set when "use strict" is the first statement
    std::vector<ParseError> errors_;
    // Set when error_and_exit records a recoverable error. Frames between the
    // error site and the per-iteration loop must bail out promptly (returning
    // their failure value) instead of continuing with inconsistent state.
    bool recover_failed_ = false;

    // error recovery
    // advance past tokens until a likely statement boundary so that the
    // top-level loop can attempt to parse the next construct cleanly.
    void synchronize();
    bool is_eof() const;
    const Token &peek(size_t n = 0) const;
    const Token &next();
    bool match(TokenKind k);
    void error_and_exit(const std::string &msg);
    bool expect(TokenKind k, const char *msg = nullptr);

    bool looks_like_func_keyword_decl();
    bool looks_like_foreach_loop();
    bool looks_like_type_decl();
    bool looks_like_union_decl();
    bool looks_like_enum_decl();
    bool looks_like_class_decl();

    template <typename T>
    void parse_generic_arguments(T *type) {
        if (recover_failed_) {
            return;
        }
        if (peek().kind == TokenKind::TK_LT) {
            next(); // consume '<'
            while (!is_eof() && peek().kind != TokenKind::TK_GT) {
                if (peek().kind != TokenKind::TK_IDENT) {
                    error_and_exit("Expected generic parameter name");
                    return;
                }
                type->generic_params.push_back(next().text);

                if (peek().kind == TokenKind::TK_COMMA) {
                    next(); // consume ','
                } else if (peek().kind != TokenKind::TK_GT) {
                    error_and_exit("Expected ',' or '>' in generic parameters");
                    return;
                }
            }
            if (!expect(TokenKind::TK_GT, "'>' after generic parameters")) {
                return;
            }
        }
    }

    nari::TypeDeclPtr parse_type_decl();
    std::unique_ptr<nari::TypeAnnotation> parse_type_annotation();
    nari::EnumDeclPtr parse_enum_decl();
    nari::ClassDeclPtr parse_class_decl();

    FunctionPtr parse_function();
    BlockPtr parse_block();
    void prune_dead_code(nari::BlockStmt *blk);
    void optimize_block(nari::BlockStmt *blk);

    // constant propagation: replaces never-reassigned `let x = <literal>` references with the literal value, then
    // re-folds.

    using ConstMap = std::unordered_map<std::string, const nari::Expr *>;
    using NameSet = std::unordered_set<std::string>;

    static ExprPtr clone_literal(const nari::Expr *src, const nari::Expr *loc);
    static void eliminate_mutated_stmt(const nari::Stmt *s, NameSet &candidates);
    static void eliminate_mutated_expr(const nari::Expr *e, NameSet &candidates);
    ExprPtr subst_fold(ExprPtr expr, const ConstMap &m);
    void subst_stmt(nari::Stmt *s, const ConstMap &m);
    void subst_block(nari::BlockStmt *blk, ConstMap m);
    void propagate_consts(nari::BlockStmt *blk);
    bool get_loop_init(nari::Stmt *init, std::string &var, int64_t &start);
    bool get_loop_cond(nari::Expr *cond, const std::string &var, int64_t &end, bool &inclusive);
    bool get_loop_step(nari::Stmt *post, const std::string &var, int64_t &step);
    bool get_body_increment(nari::Stmt *body, std::string &target, int64_t &inc);
    std::unique_ptr<nari::Stmt> try_optimize_for(nari::ForStmt *fs, nari::Stmt *prev);
    static bool literal_int(const nari::Expr *e, int64_t &out);
    static bool literal_float(const nari::Expr *e, double &out);
    static double number_to_double(const nari::NumberExpr *ne);
    static bool literal_bool(const nari::Expr *e, bool &out);
    static bool literal_string(const nari::Expr *e, std::string &out);
    static bool literal_any(const nari::Expr *e);
    static std::string literal_to_string(const nari::Expr *e);
    static void copy_loc(nari::Expr *dst, const nari::Expr *src);
    ExprPtr fold_expr(ExprPtr expr);

    bool looks_like_arrow_function();
    ExprPtr parse_function_expression();
    ExprPtr parse_spawn_expression();
    ExprPtr parse_arrow_function_expression();
    void check_statement_boundary();
    StmtPtr parse_stmt();
    StmtPtr parse_stmt_core();
    StmtPtr parse_switch_stmt();

    // higher value = tighter binding
    enum Precedence {
        PREC_NONE = 0,
        PREC_TERNARY = 1,       // ? :
        PREC_NULL_COALESCE = 2, // ??
        PREC_LOGICAL_OR = 3,    // ||
        PREC_LOGICAL_AND = 4,   // &&
        PREC_BITWISE_OR = 5,    // |
        PREC_BITWISE_XOR = 6,   // ^
        PREC_BITWISE_AND = 7,   // &
        PREC_EQUALITY = 8,      // == != === !==
        PREC_COMPARISON = 9,    // < > <= >=
        PREC_SHIFT = 10,        // << >>
        PREC_TERM = 11,         // + -
        PREC_FACTOR = 12,       // * / %
        PREC_EXPONENT = 13,     // **
        PREC_UNARY = 14,        // unary + - ! ~
        PREC_PRIMARY = 15
    };

    static Precedence token_precedence(const Token &t);

    // Guard against unbounded recursion in the expression parser when input
    // is deeply nested (e.g. `(((...(1)...)))` with thousands of parens)
    int expr_recursion_depth = 0;
    static constexpr int MAX_EXPR_RECURSION_DEPTH = 256;

    struct ExprRecursionGuard {
        int &depth;
        explicit ExprRecursionGuard(int &d) : depth(d) {
            ++depth;
        }
        ~ExprRecursionGuard() {
            --depth;
        }
    };

    ExprPtr parse_expression();
    ExprPtr parse_precedence(Precedence min_prec);
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    nari::PatternPtr parse_pattern();
    ExprPtr parse_match_expression();
    ExprPtr parse_primary();
};

} // namespace Parser
