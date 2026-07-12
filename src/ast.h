#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nari {

struct Expr;
struct Stmt;
struct BlockStmt;
struct Function;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using BlockPtr = std::unique_ptr<BlockStmt>;
using FunctionPtr = std::unique_ptr<Function>;

inline void print_indent(int indent) {
    for (int i = 0; i < indent; ++i) {
        printf(" ");
    }
}

struct TypeAnnotation {
    std::string name; // "number", "string", "bool", etc.
    std::vector<std::string> generic_params;
    bool is_array = false; // type[]

    TypeAnnotation() = default;

    explicit TypeAnnotation(std::string n, bool arr = false) : name(std::move(n)), is_array(arr) {
    }

    std::string to_string() const {
        std::string result;
        if (generic_params.empty()) {
            result = name;
        } else {
            result = name + "<";
            for (size_t i = 0; i < generic_params.size(); i++) {
                if (i > 0) {
                    result += ", ";
                }
                result += generic_params[i];
            }
            result += ">";
        }
        if (is_array) {
            result += "[]";
        }
        return result;
    }
};

using TypeAnnotationPtr = std::unique_ptr<TypeAnnotation>;

struct ASTNode {
    std::string filename;
    int line = 0;
    int col = 0;

    virtual ~ASTNode() = default;
    virtual void pretty_print(int indent = 0) const = 0;

    std::string loc_str() const {
        if (filename.empty() && line == 0 && col == 0) {
            return "";
        }
        if (!filename.empty()) {
            return " (" + filename + ":" + std::to_string(line) + ":" + std::to_string(col) + ")";
        } else {
            return " (" + std::to_string(line) + ":" + std::to_string(col) + ")";
        }
    }
};

struct Param {
    std::string name;
    ExprPtr default_value;
    bool is_rest = false;
    TypeAnnotationPtr type;

    Param(std::string n, ExprPtr def = nullptr, bool rest = false, TypeAnnotationPtr t = nullptr)
        : name(std::move(n)), default_value(std::move(def)), is_rest(rest), type(std::move(t)) {
    }
};

// type tags for fast dispatch (avoids dynamic_cast in hot paths)
enum class ExprKind {
    Ident,
    String,
    Number,
    Bool,
    Null,
    Unary,
    Binary,
    Call,
    Ternary,
    Match,
    Regex,
    Spawn,
    StringInterpolation,
    ArrayLiteral,
    ObjectLiteral,
    Function,
    Index,
    Member,
    This,
    New,
    Spread,
    Other
};

struct Expr : ASTNode {
    ExprKind kind = ExprKind::Other;
    virtual ~Expr() = default;
};

struct IdentExpr : Expr {
    std::string name;

    explicit IdentExpr(std::string n) : name(std::move(n)) {
        kind = ExprKind::Ident;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Ident: %s%s\n", name.c_str(), loc_str().c_str());
    }
};

// func(params) { ... }
struct FunctionExpr : Expr {
    std::vector<Param> params;
    TypeAnnotationPtr return_type; // optional annotation
    BlockPtr body;

    FunctionExpr() {
        kind = ExprKind::Function;
    }

    void pretty_print(int indent) const override {
        print_indent(indent);
        printf("FunctionExpr");
        if (return_type) {
            printf(" -> %s", return_type->to_string().c_str());
        }
        printf(" {\n");
        print_indent(indent + 2);
        printf("params: [");
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) {
                printf(", ");
            }
            printf("%s", params[i].name.c_str());
            if (params[i].type) {
                printf(": %s", params[i].type->to_string().c_str());
            }
            if (params[i].default_value) {
                printf(" = ");
                params[i].default_value->pretty_print(0);
            }
            if (params[i].is_rest) {
                printf("...");
            }
        }
        printf("]\n");
        print_indent(indent + 2);
        printf("body: <block>\n");
        print_indent(indent);
        printf("}\n");
    }
};

struct StringExpr : Expr {
    std::string value;

    explicit StringExpr(std::string v) : value(std::move(v)) {
        kind = ExprKind::String;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("String: \"%s\"%s\n", value.c_str(), loc_str().c_str());
    }
};

struct NumberExpr : Expr {
    bool is_float = false;
    int64_t i = 0;
    double f = 0.0;

    explicit NumberExpr(double v) : is_float(true), f(v) {
        kind = ExprKind::Number;
    }
    explicit NumberExpr(int64_t v) : is_float(false), i(v) {
        kind = ExprKind::Number;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        if (is_float) {
            printf("Float: %f%s\n", f, loc_str().c_str());
        } else {
            printf("Int: %lld%s\n", (long long)i, loc_str().c_str());
        }
    }
};

struct BoolExpr : Expr {
    bool value;

    explicit BoolExpr(bool v) : value(v) {
        kind = ExprKind::Bool;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Bool: %s%s\n", value ? "true" : "false", loc_str().c_str());
    }
};

struct NullExpr : Expr {
    NullExpr() {
        kind = ExprKind::Null;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Null%s\n", loc_str().c_str());
    }
};

// regex literal: /pattern/flags
struct RegexLiteralExpr : Expr {
    std::string pattern;
    std::string flags;
    RegexLiteralExpr(std::string p, std::string f) : pattern(std::move(p)), flags(std::move(f)) {
        kind = ExprKind::Regex;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("RegexLiteral(/%s/%s)%s\n", pattern.c_str(), flags.c_str(), loc_str().c_str());
    }
};

struct UnaryExpr : Expr {
    std::string op;
    ExprPtr operand;

    UnaryExpr(std::string o, ExprPtr opd) : op(std::move(o)), operand(std::move(opd)) {
        kind = ExprKind::Unary;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("UnaryExpr: %s%s\n", op.c_str(), loc_str().c_str());
        if (operand) {
            operand->pretty_print(indent + 2);
        }
    }
};

struct BinaryExpr : Expr {
    std::string op;
    ExprPtr left;
    ExprPtr right;

    BinaryExpr(std::string o) : op(std::move(o)) {
        kind = ExprKind::Binary;
    }
    BinaryExpr(std::string o, ExprPtr l, ExprPtr r) : op(std::move(o)), left(std::move(l)), right(std::move(r)) {
        kind = ExprKind::Binary;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("BinaryExpr: %s%s\n", op.c_str(), loc_str().c_str());
        if (left) {
            left->pretty_print(indent + 2);
        }
        if (right) {
            right->pretty_print(indent + 2);
        }
    }
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    bool has_spread = false; // true if any arg is a SpreadExpr
    bool optional = false;   // true for ?.()
    explicit CallExpr(ExprPtr c) : callee(std::move(c)) {
        kind = ExprKind::Call;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("CallExpr:\n");
        callee->pretty_print(indent + 2);
        print_indent(indent);
        printf("Args:\n");
        for (const auto &a : args) {
            a->pretty_print(indent + 2);
        }
    }
};

// ...expr (spread)
struct SpreadExpr : Expr {
    ExprPtr operand;
    explicit SpreadExpr(ExprPtr op) : operand(std::move(op)) {
        kind = ExprKind::Spread;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("SpreadExpr:\n");
        if (operand) {
            operand->pretty_print(indent + 2);
        }
    }
};

// [elem1, elem2, ...]
struct ArrayLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
    bool has_spread = false; // true if any element is a SpreadExpr
    ArrayLiteralExpr() {
        kind = ExprKind::ArrayLiteral;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("ArrayLiteral%s [\n", loc_str().c_str());
        for (const auto &elem : elements) {
            if (elem) {
                elem->pretty_print(indent + 2);
            }
        }
        print_indent(indent);
        printf("]\n");
    }
};

// {key1: val1, key2: val2, ...}
struct ObjectLiteralExpr : Expr {
    std::vector<std::pair<std::string, ExprPtr>> entries;
    bool has_spread = false; // true if any entry is a spread (key == "")

    ObjectLiteralExpr() {
        kind = ExprKind::ObjectLiteral;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("ObjectLiteral%s {\n", loc_str().c_str());
        for (const auto &[key, val] : entries) {
            print_indent(indent + 2);
            printf("%s:\n", key.c_str());
            if (val) {
                val->pretty_print(indent + 4);
            }
        }
        print_indent(indent);
        printf("}\n");
    }
};

// array[index] or object[key]
struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    bool optional = false; // true for ?.[]

    IndexExpr(ExprPtr obj, ExprPtr idx)
        : object(std::move(obj)), index(std::move(idx)) {
        kind = ExprKind::Index;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("IndexExpr%s:\n", loc_str().c_str());
        print_indent(indent + 2);
        printf("Object:\n");
        if (object) {
            object->pretty_print(indent + 4);
        }
        print_indent(indent + 2);
        printf("Index:\n");
        if (index) {
            index->pretty_print(indent + 4);
        }
    }
};

// member access: object.member
struct MemberExpr : Expr {
    ExprPtr object;
    std::string member;
    bool optional = false; // true for ?.
    MemberExpr(ExprPtr obj, std::string mem) : object(std::move(obj)), member(std::move(mem)) {
        kind = ExprKind::Member;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("MemberExpr%s .%s:\n", loc_str().c_str(), member.c_str());
        if (object) {
            object->pretty_print(indent + 2);
        }
    }
};

// this expression for accessing current instance in class methods
struct ThisExpr : Expr {
    ThisExpr() {
        kind = ExprKind::This;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("This%s\n", loc_str().c_str());
    }
};

// new ClassName(args...) - class instantiation
struct NewExpr : Expr {
    std::string class_name;
    std::vector<ExprPtr> args;

    explicit NewExpr(std::string cn) : class_name(std::move(cn)) {
        kind = ExprKind::New;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("NewExpr: %s%s\n", class_name.c_str(), loc_str().c_str());
        for (size_t i = 0; i < args.size(); i++) {
            print_indent(indent + 2);
            printf("Arg[%zu]:\n", i);
            args[i]->pretty_print(indent + 4);
        }
    }
};

// string interpolation: `Hello {name}!`
struct StringInterpolationExpr : Expr {
    std::vector<std::string> parts;        // string literals between expressions
    std::vector<std::string> expr_sources; // raw source of each interpolated expression
    std::vector<std::string> format_specs; // Python-style format specs, e.g. ".3f"
    std::vector<ExprPtr> exprs;            // pre-parsed ASTs (filled at parse time)

    StringInterpolationExpr() {
        kind = ExprKind::StringInterpolation;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("StringInterpolation%s:\n", loc_str().c_str());
        for (size_t i = 0; i < parts.size(); ++i) {
            print_indent(indent + 2);
            printf("Part[%zu]: \"%s\"\n", i, parts[i].c_str());
            if (i < expr_sources.size()) {
                print_indent(indent + 2);
                if (i < format_specs.size() && !format_specs[i].empty()) {
                    printf("Expr[%zu]: {%s:%s}\n", i, expr_sources[i].c_str(), format_specs[i].c_str());
                } else {
                    printf("Expr[%zu]: {%s}\n", i, expr_sources[i].c_str());
                }
            }
        }
    }
};

// ternary conditional expression: condition ? trueExpr : falseExpr
struct TernaryExpr : Expr {
    ExprPtr condition;
    ExprPtr true_expr;
    ExprPtr false_expr;

    TernaryExpr(ExprPtr cond, ExprPtr true_val, ExprPtr false_val)
        : condition(std::move(cond)), true_expr(std::move(true_val)), false_expr(std::move(false_val)) {
        kind = ExprKind::Ternary;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("TernaryExpr%s ? :\n", loc_str().c_str());
        print_indent(indent + 2);
        printf("Condition:\n");
        if (condition) {
            condition->pretty_print(indent + 4);
        }
        print_indent(indent + 2);
        printf("True:\n");
        if (true_expr) {
            true_expr->pretty_print(indent + 4);
        }
        print_indent(indent + 2);
        printf("False:\n");
        if (false_expr) {
            false_expr->pretty_print(indent + 4);
        }
    }
};

// type tags for fast stmt dispatch
enum class StmtKind {
    Expr,
    VarDecl,
    Assign,
    IndexAssign,
    Block,
    If,
    While,
    For,
    ForEach,
    Switch,
    Break,
    Continue,
    Return,
    Other
};

struct Stmt : ASTNode {
    StmtKind stmt_kind = StmtKind::Other;
    virtual ~Stmt() = default;
};

struct ExprStmt : Stmt {
    ExprPtr expr;

    explicit ExprStmt(ExprPtr e) : expr(std::move(e)) {
        stmt_kind = StmtKind::Expr;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("ExprStmt%s:\n", loc_str().c_str());
        if (expr) {
            expr->pretty_print(indent + 2);
        }
    }
};

enum VarDeclCtrl : bool {
    GLOBAL = true,
    LOCAL = false
};

enum class DestructureKind {
    None,  // simple variable: let x = value
    Array, // array destructuring: let [a, b] = value
    Object // object destructuring: let {x, y} = value
};

struct VarDeclStmt : Stmt {
    std::string name;        // for simple declarations
    ExprPtr initializerExpr; // optional initializer
    VarDeclCtrl is_global = LOCAL;

    // Destructuring support
    DestructureKind destructure_kind = DestructureKind::None;
    std::vector<std::string> array_names;                             // for array destructuring: [a, b, c]
    std::vector<std::pair<std::string, std::string>> object_bindings; // for object destructuring: {key: name}

    VarDeclStmt(std::string n, ExprPtr i, VarDeclCtrl g = LOCAL) : name(std::move(n)), initializerExpr(std::move(i)), is_global(g) {
        stmt_kind = StmtKind::VarDecl;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        if (destructure_kind == DestructureKind::Array) {
            printf("VarDecl (array destructuring)%s: [", loc_str().c_str());
            for (size_t i = 0; i < array_names.size(); i++) {
                if (i > 0) {
                    printf(", ");
                }
                printf("%s", array_names[i].c_str());
            }
            printf("] =\n");
        } else if (destructure_kind == DestructureKind::Object) {
            printf("VarDecl (object destructuring)%s: {", loc_str().c_str());
            for (size_t i = 0; i < object_bindings.size(); i++) {
                if (i > 0) {
                    printf(", ");
                }
                if (object_bindings[i].first == object_bindings[i].second) {
                    printf("%s", object_bindings[i].first.c_str());
                } else {
                    printf("%s: %s", object_bindings[i].first.c_str(), object_bindings[i].second.c_str());
                }
            }
            printf("} =\n");
        } else {
            if (is_global) {
                printf("VarDecl (global): %s%s =\n", name.c_str(), loc_str().c_str());
            } else {
                printf("VarDecl: %s%s =\n", name.c_str(), loc_str().c_str());
            }
        }
        if (initializerExpr) {
            initializerExpr->pretty_print(indent + 2);
        }
    }
};

struct AssignStmt : Stmt {
    std::string target;
    ExprPtr value;

    AssignStmt(std::string t, ExprPtr v) : target(std::move(t)), value(std::move(v)) {
        stmt_kind = StmtKind::Assign;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Assign: %s%s =\n", target.c_str(), loc_str().c_str());
        if (value) {
            value->pretty_print(indent + 2);
        }
    }
};

// indexed assignment: arr[index] = value, obj[key] = value, or obj.member = value
struct IndexAssignStmt : Stmt {
    ExprPtr target; // should be IndexExpr or MemberExpr
    ExprPtr value;

    IndexAssignStmt(ExprPtr t, ExprPtr v) : target(std::move(t)), value(std::move(v)) {
        stmt_kind = StmtKind::IndexAssign;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("IndexAssign%s:\n", loc_str().c_str());
        print_indent(indent + 2);
        printf("Target:\n");
        if (target) {
            target->pretty_print(indent + 4);
        }
        print_indent(indent + 2);
        printf("Value:\n");
        if (value) {
            value->pretty_print(indent + 4);
        }
    }
};

// control flow
struct IfStmt : Stmt {
    ExprPtr cond;
    StmtPtr then_branch; // should be BlockStmt
    StmtPtr else_branch; // optional

    IfStmt(ExprPtr c, StmtPtr t, StmtPtr e = nullptr) : cond(std::move(c)), then_branch(std::move(t)), else_branch(std::move(e)) {
        stmt_kind = StmtKind::If;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("If%s:\n", loc_str().c_str());
        if (cond) {
            print_indent(indent + 2);
            printf("Condition:\n");
            cond->pretty_print(indent + 4);
        }
        if (then_branch) {
            print_indent(indent + 2);
            printf("Then:\n");
            then_branch->pretty_print(indent + 4);
        }
        if (else_branch) {
            print_indent(indent + 2);
            printf("Else:\n");
            else_branch->pretty_print(indent + 4);
        }
    }
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    StmtPtr body;

    WhileStmt(ExprPtr c, StmtPtr b) : cond(std::move(c)), body(std::move(b)) {
        stmt_kind = StmtKind::While;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("While%s:\n", loc_str().c_str());
        if (cond) {
            print_indent(indent + 2);
            printf("Condition:\n");
            cond->pretty_print(indent + 4);
        }
        if (body) {
            print_indent(indent + 2);
            printf("Body:\n");
            body->pretty_print(indent + 4);
        }
    }
};

struct ForStmt : Stmt {
    StmtPtr init;
    ExprPtr cond; // optional
    StmtPtr post; // optional
    StmtPtr body;

    ForStmt(StmtPtr i, ExprPtr c, StmtPtr p, StmtPtr b) : init(std::move(i)), cond(std::move(c)), post(std::move(p)), body(std::move(b)) {
        stmt_kind = StmtKind::For;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("For%s:\n", loc_str().c_str());
        if (init) {
            print_indent(indent + 2);
            printf("Init:\n");
            init->pretty_print(indent + 4);
        }
        if (cond) {
            print_indent(indent + 2);
            printf("Condition:\n");
            cond->pretty_print(indent + 4);
        }
        if (post) {
            print_indent(indent + 2);
            printf("Post:\n");
            post->pretty_print(indent + 4);
        }
        if (body) {
            print_indent(indent + 2);
            printf("Body:\n");
            body->pretty_print(indent + 4);
        }
    }
};

struct ForEachStmt : Stmt {
    std::string var;
    std::string val_var; // optional second variable: for (key, value in obj)
    ExprPtr iterable;
    StmtPtr body;

    ForEachStmt(std::string v, ExprPtr it, StmtPtr b) : var(std::move(v)), iterable(std::move(it)), body(std::move(b)) {
        stmt_kind = StmtKind::ForEach;
    }

    ForEachStmt(std::string v, std::string vv, ExprPtr it, StmtPtr b)
        : var(std::move(v)), val_var(std::move(vv)), iterable(std::move(it)), body(std::move(b)) {
        stmt_kind = StmtKind::ForEach;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("ForEach%s:\n", loc_str().c_str());
        print_indent(indent + 2);
        printf("Var: %s\n", var.c_str());
        if (!val_var.empty()) {
            print_indent(indent + 2);
            printf("ValVar: %s\n", val_var.c_str());
        }
        if (iterable) {
            print_indent(indent + 2);
            printf("Iterable:\n");
            iterable->pretty_print(indent + 4);
        }
        if (body) {
            print_indent(indent + 2);
            printf("Body:\n");
            body->pretty_print(indent + 4);
        }
    }
};

struct BreakStmt : Stmt {
    BreakStmt() {
        stmt_kind = StmtKind::Break;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Break%s\n", loc_str().c_str());
    }
};

struct ContinueStmt : Stmt {
    ContinueStmt() {
        stmt_kind = StmtKind::Continue;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Continue%s\n", loc_str().c_str());
    }
};

struct ReturnStmt : Stmt {
    ExprPtr value; // optional

    explicit ReturnStmt(ExprPtr v = nullptr) : value(std::move(v)) {
        stmt_kind = StmtKind::Return;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Return%s", loc_str().c_str());
        if (value) {
            printf(":\n");
            value->pretty_print(indent + 2);
        } else {
            printf("\n");
        }
    }
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> stmts;

    BlockStmt() {
        stmt_kind = StmtKind::Block;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Block%s\n", loc_str().c_str());
        for (const auto &s : stmts) {
            if (s) {
                s->pretty_print(indent + 2);
            }
        }
    }
};

// spawn expression: spawn { ... }
// this executes the block asynchronously and returns a Handle
struct SpawnExpr : Expr {
    BlockPtr body;

    SpawnExpr(BlockPtr b) : body(std::move(b)) {
        kind = ExprKind::Spawn;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("SpawnExpr%s:\n", loc_str().c_str());
        if (body) {
            print_indent(indent + 2);
            printf("Body:\n");
            body->pretty_print(indent + 4);
        }
    }
};

struct SwitchCase {
    ExprPtr match;
    BlockPtr body;

    SwitchCase(ExprPtr m, BlockPtr b) : match(std::move(m)), body(std::move(b)) {
    }
};

struct SwitchStmt : Stmt {
    ExprPtr value;
    std::vector<SwitchCase> cases;
    BlockPtr default_body;

    explicit SwitchStmt(ExprPtr v) : value(std::move(v)) {
        stmt_kind = StmtKind::Switch;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Switch%s:\n", loc_str().c_str());
        if (value) {
            print_indent(indent + 2);
            printf("Value:\n");
            value->pretty_print(indent + 4);
        }
        for (const auto &c : cases) {
            print_indent(indent + 2);
            printf("Case:\n");
            if (c.match) {
                c.match->pretty_print(indent + 4);
            }
            if (c.body) {
                c.body->pretty_print(indent + 4);
            }
        }
        if (default_body) {
            print_indent(indent + 2);
            printf("Default:\n");
            default_body->pretty_print(indent + 4);
        }
    }
};

// for custom type declarations
struct TypeField {
    std::string name;
    TypeAnnotationPtr type;

    TypeField(std::string n, TypeAnnotationPtr t) : name(std::move(n)), type(std::move(t)) {
    }
};

// custom type declaration: type Name<T, U> { field: type; ... }, and type aliases (type NameAlias Name)
struct TypeDecl : ASTNode {
    std::string name;
    std::vector<std::string> generic_params; // T, U, etc.
    std::vector<TypeField> fields;
    TypeAnnotationPtr alias_target; // if non-null, this is a type alias

    explicit TypeDecl(std::string n) : name(std::move(n)) {
    }

    bool is_alias() const {
        return alias_target != nullptr;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("TypeDecl: %s", name.c_str());
        if (!generic_params.empty()) {
            printf("<");
            for (size_t i = 0; i < generic_params.size(); i++) {
                if (i > 0) {
                    printf(", ");
                }
                printf("%s", generic_params[i].c_str());
            }
            printf(">");
        }
        if (is_alias()) {
            printf(" = %s", alias_target->to_string().c_str());
        }
        printf("%s\n", loc_str().c_str());
        for (const auto &field : fields) {
            print_indent(indent + 2);
            printf("%s: %s\n", field.name.c_str(), field.type->to_string().c_str());
        }
    }
};

// enum variant, can be a single unit or have tuple/struct data
struct EnumVariant {
    std::string name;
    std::vector<TypeAnnotationPtr> tuple_fields; // i.e Some(T)
    std::vector<TypeField> named_fields;         // i.e Point { x: T, y: T }

    explicit EnumVariant(std::string n) : name(std::move(n)) {
    }

    bool is_unit() const {
        return tuple_fields.empty() && named_fields.empty();
    }
    bool is_tuple() const {
        return !tuple_fields.empty();
    }
    bool is_struct() const {
        return !named_fields.empty();
    }
};

// enum Name<T> { Variant1, Variant2(T), Variant3 { field: T } }
struct EnumDecl : ASTNode {
    std::string name;
    std::vector<std::string> generic_params;
    std::vector<EnumVariant> variants;

    explicit EnumDecl(std::string n) : name(std::move(n)) {
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("EnumDecl: %s", name.c_str());
        if (!generic_params.empty()) {
            printf("<");
            for (size_t i = 0; i < generic_params.size(); i++) {
                if (i > 0) {
                    printf(", ");
                }
                printf("%s", generic_params[i].c_str());
            }
            printf(">");
        }
        printf("%s\n", loc_str().c_str());
        for (const auto &variant : variants) {
            print_indent(indent + 2);
            printf("%s", variant.name.c_str());
            if (variant.is_tuple()) {
                printf("(");
                for (size_t i = 0; i < variant.tuple_fields.size(); i++) {
                    if (i > 0) {
                        printf(", ");
                    }
                    printf("%s", variant.tuple_fields[i]->to_string().c_str());
                }
                printf(")");
            } else if (variant.is_struct()) {
                printf(" { ");
                for (size_t i = 0; i < variant.named_fields.size(); i++) {
                    if (i > 0) {
                        printf(", ");
                    }
                    printf("%s: %s", variant.named_fields[i].name.c_str(),
                           variant.named_fields[i].type->to_string().c_str());
                }
                printf(" }");
            }
            printf("\n");
        }
    }
};

using TypeDeclPtr = std::unique_ptr<TypeDecl>;
using EnumDeclPtr = std::unique_ptr<EnumDecl>;

enum class Visibility {
    Public,
    Private
};

// class field with visibility and optional default value
struct ClassField {
    std::string name;
    Visibility visibility;
    TypeAnnotationPtr type;
    ExprPtr default_value; // optional
    std::string filename;
    int line = 0;
    int col = 0;
    bool is_static = false;

    ClassField(std::string n, Visibility v, TypeAnnotationPtr t, ExprPtr def = nullptr)
        : name(std::move(n)), visibility(v), type(std::move(t)), default_value(std::move(def)) {
    }
};

// class method with visibility
struct ClassMethod {
    std::string name;
    Visibility visibility;
    std::vector<Param> params;
    TypeAnnotationPtr return_type;
    BlockPtr body;
    std::string filename;
    int line = 0;
    int col = 0;
    bool is_constructor = false;
    bool is_static = false;

    ClassMethod(std::string n, Visibility v) : name(std::move(n)), visibility(v) {
    }
};

// Class declaration: class Name<T> extends Parent { fields... methods... }
struct ClassDecl : ASTNode {
    std::string name;
    std::string parent_name; // for inheritance
    std::vector<std::string> generic_params;
    std::vector<ClassField> fields;
    std::vector<ClassMethod> methods;

    explicit ClassDecl(std::string n) : name(std::move(n)) {
    }

    ClassMethod *get_constructor() {
        for (ClassMethod &method : methods) {
            if (method.is_constructor) {
                return &method;
            }
        }
        return nullptr;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("ClassDecl: %s", name.c_str());
        if (!generic_params.empty()) {
            printf("<");
            for (size_t i = 0; i < generic_params.size(); i++) {
                if (i > 0) {
                    printf(", ");
                }
                printf("%s", generic_params[i].c_str());
            }
            printf(">");
        }
        printf("%s\n", loc_str().c_str());

        if (!fields.empty()) {
            print_indent(indent + 2);
            printf("Fields:\n");
            for (const auto &field : fields) {
                print_indent(indent + 4);
                printf(
                    "%s %s: %s\n",
                    field.visibility == Visibility::Public ? "public" : "private",
                    field.name.c_str(), field.type->to_string().c_str());
            }
        }

        if (!methods.empty()) {
            print_indent(indent + 2);
            printf("Methods:\n");
            for (const auto &method : methods) {
                print_indent(indent + 4);
                printf(
                    "%s %s%s(",
                    method.visibility == Visibility::Public ? "public" : "private",
                    method.is_constructor ? "constructor " : "",
                    method.name.c_str());
                for (size_t i = 0; i < method.params.size(); i++) {
                    if (i > 0) {
                        printf(", ");
                    }
                    printf("%s", method.params[i].name.c_str());
                }
                printf(")");
                if (method.return_type) {
                    printf(" -> %s", method.return_type->to_string().c_str());
                }
                printf("\n");
            }
        }
    }
};

using ClassDeclPtr = std::unique_ptr<ClassDecl>;

// type tags for fast pattern dispatch
enum class PatternKind {
    Wildcard,
    Binding,
    Literal,
    Variant,
    Other
};

struct Pattern : ASTNode {
    PatternKind pattern_kind = PatternKind::Other;
    virtual ~Pattern() = default;
};

using PatternPtr = std::unique_ptr<Pattern>;

// Wildcard pattern: _
struct WildcardPattern : Pattern {
    WildcardPattern() {
        pattern_kind = PatternKind::Wildcard;
    }
    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("_\n");
    }
};

// Variable binding pattern: x, value, etc.
struct BindingPattern : Pattern {
    std::string name;

    explicit BindingPattern(std::string n) : name(std::move(n)) {
        pattern_kind = PatternKind::Binding;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Binding: %s\n", name.c_str());
    }
};

// Literal pattern: 42, "hello", true, null
struct LiteralPattern : Pattern {
    ExprPtr value; // Uses existing literal expression types

    explicit LiteralPattern(ExprPtr v) : value(std::move(v)) {
        pattern_kind = PatternKind::Literal;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Literal:\n");
        if (value) {
            value->pretty_print(indent + 2);
        }
    }
};

// variant pattern, Ok(value), Err(e), Some(x), None
struct VariantPattern : Pattern {
    std::string enum_name;                   // Result, Option, etc.
    std::string variant_name;                // Ok, Err, Some, None
    std::vector<PatternPtr> fields;          // nested patterns for variant data (tuple)
    std::vector<std::string> named_bindings; // Named field bindings for struct destructuring
    bool is_struct_pattern = false;          // true for Text { content, sender }

    explicit VariantPattern(std::string v) : variant_name(std::move(v)) {
        pattern_kind = PatternKind::Variant;
    }
    VariantPattern(std::string e, std::string v) : enum_name(std::move(e)), variant_name(std::move(v)) {
        pattern_kind = PatternKind::Variant;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        if (!enum_name.empty()) {
            printf("%s::%s", enum_name.c_str(), variant_name.c_str());
        } else {
            printf("%s", variant_name.c_str());
        }
        if (!fields.empty()) {
            printf("(\n");
            for (const auto &field : fields) {
                if (field) {
                    field->pretty_print(indent + 2);
                }
            }
            print_indent(indent);
            printf(")");
        }
        printf("\n");
    }
};

// Match arm: pattern => expression
struct MatchArm {
    PatternPtr pattern;
    ExprPtr body;

    MatchArm(PatternPtr p, ExprPtr b) : pattern(std::move(p)), body(std::move(b)) {
    }
};

// match expression: match value { pattern => expr, ... }
struct MatchExpr : Expr {
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;

    explicit MatchExpr(ExprPtr s) : scrutinee(std::move(s)) {
        kind = ExprKind::Match;
    }

    void pretty_print(int indent = 0) const override {
        print_indent(indent);
        printf("Match%s\n", loc_str().c_str());
        print_indent(indent + 2);
        printf("Scrutinee:\n");
        if (scrutinee) {
            scrutinee->pretty_print(indent + 4);
        }
        print_indent(indent + 2);
        printf("Arms:\n");
        for (const auto &arm : arms) {
            print_indent(indent + 4);
            printf("Pattern:\n");
            if (arm.pattern) {
                arm.pattern->pretty_print(indent + 6);
            }
            print_indent(indent + 4);
            printf("Body:\n");
            if (arm.body) {
                arm.body->pretty_print(indent + 6);
            }
        }
    }
};

struct Function : ASTNode {
    std::string name;
    std::vector<Param> params;
    BlockPtr body;
    const FunctionExpr *function_expr = nullptr; // for lambda functions created from FunctionExpr
    TypeAnnotationPtr return_type;               // optional return type annotation
    void *closure_env_ptr = nullptr;             // pointer to captured environment
    void (*closure_deleter)(void *) = nullptr;   // custom deleter for closure
    bool strict_mode = false;                    // "use strict"

    Function() = default;

    explicit Function(std::string n) : name(std::move(n)) {
    }

    ~Function() {
        if (closure_env_ptr && closure_deleter) {
            closure_deleter(closure_env_ptr);
            closure_env_ptr = nullptr;
        }
    }

    void pretty_print(int indent) const {
        print_indent(indent);
        printf("Func %s", name.c_str());
        if (return_type) {
            printf(" -> %s", return_type->to_string().c_str());
        }
        printf("%s\n", loc_str().c_str());
        for (const auto &p : params) {
            print_indent(indent + 2);
            printf("Param: %s%s", (p.is_rest ? "..." : ""), p.name.c_str());
            if (p.type) {
                printf(": %s", p.type->to_string().c_str());
            }
            if (p.default_value) {
                printf(" =\n");
                p.default_value->pretty_print(indent + 4);
            } else {
                printf("\n");
            }
        }
        if (body) {
            body->pretty_print(indent + 2);
        }
    }
};

struct Program {
    std::vector<FunctionPtr> functions;
    std::vector<TypeDeclPtr> types;
    std::vector<EnumDeclPtr> enums;
    std::vector<ClassDeclPtr> classes;
    std::vector<StmtPtr> top_level_stmts;

    void pretty_print(int indent = 0) const {
        printf("Program\n");
        for (const auto &t : types) {
            if (t) {
                t->pretty_print(indent + 2);
            }
        }
        for (const auto &e : enums) {
            if (e) {
                e->pretty_print(indent + 2);
            }
        }
        for (const auto &c : classes) {
            if (c) {
                c->pretty_print(indent + 2);
            }
        }
        for (const auto &f : functions) {
            if (f) {
                f->pretty_print(indent + 2);
            }
        }
        if (!top_level_stmts.empty()) {
            print_indent(indent + 2);
            printf("Top-level statements:\n");
            for (const auto &s : top_level_stmts) {
                if (s) {
                    s->pretty_print(indent + 4);
                }
            }
        }
    }
};

} // namespace nari
