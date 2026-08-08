#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nari {

inline constexpr size_t MAX_FIXED_ARRAY_COUNT = 1u << 20;

struct Expr;
struct Stmt;
struct BlockStmt;
struct Function;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using BlockPtr = std::unique_ptr<BlockStmt>;
using FunctionPtr = std::unique_ptr<Function>;

struct TypeAnnotation {
    std::string name; // "number", "string", "bool", etc.
    std::vector<std::string> generic_params;
    bool is_array = false;        // type[]
    size_t fixed_array_count = 0; // type[N], used for inline FFI fields

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
        } else if (fixed_array_count > 0) {
            result += "[" + std::to_string(fixed_array_count) + "]";
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

};

// func(params) { ... }
struct FunctionExpr : Expr {
    std::vector<Param> params;
    TypeAnnotationPtr return_type; // optional annotation
    BlockPtr body;

    FunctionExpr() {
        kind = ExprKind::Function;
    }

};

struct StringExpr : Expr {
    std::string value;

    explicit StringExpr(std::string v) : value(std::move(v)) {
        kind = ExprKind::String;
    }

};

struct NumberExpr : Expr {
    // Nari integers are NaN-boxed with a 48-bit payload, 
    // so this is the full range an int-typed number can represent.
    static constexpr int64_t AST_INT48_MIN = -(1LL << 47);
    static constexpr int64_t AST_INT48_MAX = (1LL << 47) - 1;

    bool is_float = false;
    // Set when an out-of-int48 integer literal was promoted to float by the constructor below.
    // `i` still holds the exact original integer in that case,
    // so a consumer that can make the value fit again
    bool promoted_from_int = false;
    int64_t i = 0;
    double f = 0.0;

    explicit NumberExpr(double v) : is_float(true), f(v) {
        kind = ExprKind::Number;
    }
    // an integer outside int48 is not representable as an int Value, Value::make_int() would silently mask it and flip its sign.
    explicit NumberExpr(int64_t v) {
        kind = ExprKind::Number;
        i = v; // kept exact even when promoting, see promoted_from_int
        if (v < AST_INT48_MIN || v > AST_INT48_MAX) {
            is_float = true;
            promoted_from_int = true;
            f = static_cast<double>(v);
        } else {
            is_float = false;
        }
    }

};

struct BoolExpr : Expr {
    bool value;

    explicit BoolExpr(bool v) : value(v) {
        kind = ExprKind::Bool;
    }

};

struct NullExpr : Expr {
    NullExpr() {
        kind = ExprKind::Null;
    }

};

// regex literal: /pattern/flags
struct RegexLiteralExpr : Expr {
    std::string pattern;
    std::string flags;
    RegexLiteralExpr(std::string p, std::string f) : pattern(std::move(p)), flags(std::move(f)) {
        kind = ExprKind::Regex;
    }
};

struct UnaryExpr : Expr {
    std::string op;
    ExprPtr operand;

    UnaryExpr(std::string o, ExprPtr opd) : op(std::move(o)), operand(std::move(opd)) {
        kind = ExprKind::Unary;
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

};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    bool has_spread = false; // true if any arg is a SpreadExpr
    bool optional = false;   // true for ?.()
    explicit CallExpr(ExprPtr c) : callee(std::move(c)) {
        kind = ExprKind::Call;
    }
};

// ...expr (spread)
struct SpreadExpr : Expr {
    ExprPtr operand;
    explicit SpreadExpr(ExprPtr op) : operand(std::move(op)) {
        kind = ExprKind::Spread;
    }
};

// [elem1, elem2, ...]
struct ArrayLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
    bool has_spread = false; // true if any element is a SpreadExpr
    ArrayLiteralExpr() {
        kind = ExprKind::ArrayLiteral;
    }
};

// {key1: val1, key2: val2, ...}
struct ObjectLiteralExpr : Expr {
    std::vector<std::pair<std::string, ExprPtr>> entries;
    bool has_spread = false; // true if any entry is a spread (key == "")

    ObjectLiteralExpr() {
        kind = ExprKind::ObjectLiteral;
    }

};

// array[index] or object[key]
struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    bool optional = false; // true for ?.[]

    IndexExpr(ExprPtr obj, ExprPtr idx) : object(std::move(obj)), index(std::move(idx)) {
        kind = ExprKind::Index;
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
};

// this expression for accessing current instance in class methods
struct ThisExpr : Expr {
    ThisExpr() {
        kind = ExprKind::This;
    }
};

// new ClassName(args...) - class instantiation
struct NewExpr : Expr {
    std::string class_name;
    std::vector<ExprPtr> args;

    explicit NewExpr(std::string cn) : class_name(std::move(cn)) {
        kind = ExprKind::New;
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
    bool is_const = false;

    // Destructuring support
    DestructureKind destructure_kind = DestructureKind::None;
    std::vector<std::string> array_names;                             // for array destructuring: [a, b, c]
    std::vector<std::pair<std::string, std::string>> object_bindings; // for object destructuring: {key: name}

    VarDeclStmt(std::string n, ExprPtr i, VarDeclCtrl g = LOCAL, bool c = false)
        : name(std::move(n)), initializerExpr(std::move(i)), is_global(g), is_const(c) {
        stmt_kind = StmtKind::VarDecl;
    }

};

struct AssignStmt : Stmt {
    std::string target;
    ExprPtr value;

    AssignStmt(std::string t, ExprPtr v) : target(std::move(t)), value(std::move(v)) {
        stmt_kind = StmtKind::Assign;
    }

};

// indexed assignment: arr[index] = value, obj[key] = value, or obj.member = value
struct IndexAssignStmt : Stmt {
    ExprPtr target; // should be IndexExpr or MemberExpr
    ExprPtr value;

    IndexAssignStmt(ExprPtr t, ExprPtr v) : target(std::move(t)), value(std::move(v)) {
        stmt_kind = StmtKind::IndexAssign;
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

};

struct WhileStmt : Stmt {
    ExprPtr cond;
    StmtPtr body;

    WhileStmt(ExprPtr c, StmtPtr b) : cond(std::move(c)), body(std::move(b)) {
        stmt_kind = StmtKind::While;
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

};

struct BreakStmt : Stmt {
    BreakStmt() {
        stmt_kind = StmtKind::Break;
    }
};

struct ContinueStmt : Stmt {
    ContinueStmt() {
        stmt_kind = StmtKind::Continue;
    }
};

struct ReturnStmt : Stmt {
    ExprPtr value; // optional

    explicit ReturnStmt(ExprPtr v = nullptr) : value(std::move(v)) {
        stmt_kind = StmtKind::Return;
    }

};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> stmts;

    BlockStmt() {
        stmt_kind = StmtKind::Block;
    }

};

// spawn expression: spawn { ... }
// this executes the block asynchronously and returns a Handle
struct SpawnExpr : Expr {
    BlockPtr body;

    SpawnExpr(BlockPtr b) : body(std::move(b)) {
        kind = ExprKind::Spawn;
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

};

// for custom type declarations
struct TypeField {
    std::string name;
    TypeAnnotationPtr type;

    TypeField(std::string n, TypeAnnotationPtr t) : name(std::move(n)), type(std::move(t)) {
    }
};

enum class TypeDeclKind : uint8_t {
    Struct,
    Union,
};

// custom aggregate declaration and type aliases (type NameAlias Name)
struct TypeDecl : ASTNode {
    std::string name;
    TypeDeclKind kind = TypeDeclKind::Struct;
    std::vector<std::string> generic_params; // T, U, etc.
    std::vector<TypeField> fields;
    TypeAnnotationPtr alias_target; // if non-null, this is a type alias

    explicit TypeDecl(std::string n, TypeDeclKind k = TypeDeclKind::Struct) : name(std::move(n)), kind(k) {
    }

    bool is_alias() const {
        return alias_target != nullptr;
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
};

// Variable binding pattern: x, value, etc.
struct BindingPattern : Pattern {
    std::string name;

    explicit BindingPattern(std::string n) : name(std::move(n)) {
        pattern_kind = PatternKind::Binding;
    }

};

// Literal pattern: 42, "hello", true, null
struct LiteralPattern : Pattern {
    ExprPtr value; // Uses existing literal expression types

    explicit LiteralPattern(ExprPtr v) : value(std::move(v)) {
        pattern_kind = PatternKind::Literal;
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

};

struct Function : ASTNode {
    std::string name;
    std::vector<Param> params;
    BlockPtr body;
    const FunctionExpr *function_expr = nullptr; // for lambda functions created from FunctionExpr
    TypeAnnotationPtr return_type;               // optional return type annotation
    void *closure_env_ptr = nullptr;             // pointer to captured environment
    void (*closure_deleter)(void *) = nullptr;   // custom deleter for closure
    void *closure_owner_env_ptr = nullptr;       // captured name -> declaring environment
    void (*closure_owner_deleter)(void *) = nullptr;
    void *closure_const_env_ptr = nullptr; // pointer to captured const names
    void (*closure_const_deleter)(void *) = nullptr;
    bool strict_mode = false;  // "use strict"
    bool is_enum_ctor = false; // synthesized from an enum variant, not written by the user

    Function() = default;

    explicit Function(std::string n) : name(std::move(n)) {
    }

    ~Function() {
        if (closure_env_ptr && closure_deleter) {
            closure_deleter(closure_env_ptr);
            closure_env_ptr = nullptr;
        }
        if (closure_owner_env_ptr && closure_owner_deleter) {
            closure_owner_deleter(closure_owner_env_ptr);
            closure_owner_env_ptr = nullptr;
        }
        if (closure_const_env_ptr && closure_const_deleter) {
            closure_const_deleter(closure_const_env_ptr);
            closure_const_env_ptr = nullptr;
        }
    }

};

struct Program {
    std::vector<FunctionPtr> functions;
    std::vector<TypeDeclPtr> types;
    std::vector<EnumDeclPtr> enums;
    std::vector<ClassDeclPtr> classes;
    std::vector<StmtPtr> top_level_stmts;

};

} // namespace nari
