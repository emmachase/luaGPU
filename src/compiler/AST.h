#pragma once
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <optional>

// Forward declarations
struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using Block   = std::vector<StmtPtr>;

// ── Source location ───────────────────────────────────────────────────────────
struct SrcLoc {
    std::string file;
    int         line = 0;
};

// ── Expressions ───────────────────────────────────────────────────────────────

struct NumberLit  { double value; };
struct StringLit  { std::string value; };
struct BoolLit    { bool value; };
struct NilLit     {};

// Variable reference: a plain name
struct NameExpr   { std::string name; };

// Field access: expr.field  (covers swizzle and struct fields)
struct FieldExpr  {
    ExprPtr base;
    std::string field;
};

// Function call: callee(args)
// callee is either a NameExpr or a FieldExpr (for lib.fn(...))
struct CallExpr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
};

// Unary op: op expr
struct UnaryExpr {
    int     op;   // '-' or TK::Not (cast to int)
    ExprPtr operand;
};

// Binary op: left op right
struct BinaryExpr {
    int     op;   // ASCII or TK value (cast to int)
    ExprPtr left;
    ExprPtr right;
};

// Table constructor: { field = expr, ... }
// Only named fields are supported (no positional / computed keys)
struct TableField {
    std::string key;
    ExprPtr     value;
};
struct TableExpr {
    std::vector<TableField> fields;
};

// Function expression: function(params) body end
// Used for helper functions and the main entry point.
struct FuncExpr {
    std::string              name;    // optional — from "return function NAME"
    std::vector<std::string> params;
    Block                    body;
};

// Variant over all expression kinds
using ExprKind = std::variant<
    NumberLit,
    StringLit,
    BoolLit,
    NilLit,
    NameExpr,
    FieldExpr,
    CallExpr,
    UnaryExpr,
    BinaryExpr,
    TableExpr,
    FuncExpr
>;

struct Expr {
    ExprKind kind;
    SrcLoc   loc;
};

// ── Statements ────────────────────────────────────────────────────────────────

// local x = expr
struct LocalStmt {
    std::string           name;
    std::optional<ExprPtr> init;   // always present in valid shader code
};

// local function f(params) body end
struct LocalFuncStmt {
    std::string              name;
    std::vector<std::string> params;
    Block                    body;
};

// assignment: name = expr  (no multi-assign, no field assign in shader body)
struct AssignStmt {
    std::string target;
    ExprPtr     value;
};

// expr statement (function call as statement)
struct ExprStmt {
    ExprPtr expr;
};

// return expr
struct ReturnStmt {
    std::optional<ExprPtr> value;
};

// if / elseif / else
struct IfBranch {
    ExprPtr cond;   // null for else branch
    Block   body;
};
struct IfStmt {
    std::vector<IfBranch> branches;  // last may have null cond (else)
};

// while cond do body end
struct WhileStmt {
    ExprPtr cond;
    Block   body;
};

// for i = start, stop [, step] do body end
struct ForStmt {
    std::string var;
    ExprPtr     start;
    ExprPtr     stop;
    std::optional<ExprPtr> step;
    Block       body;
};

// break
struct BreakStmt {};

using StmtKind = std::variant<
    LocalStmt,
    LocalFuncStmt,
    AssignStmt,
    ExprStmt,
    ReturnStmt,
    IfStmt,
    WhileStmt,
    ForStmt,
    BreakStmt
>;

struct Stmt {
    StmtKind kind;
    SrcLoc   loc;
};

// ── Top-level shader function ─────────────────────────────────────────────────
// Represents the outer closure passed to shader():
//   function(u_time, u_resolution)
//       local function helper(...) ... end
//       return function main(uv) ... end
//   end
struct ShaderFunc {
    std::string              src_name;
    std::vector<std::string> uniform_params;   // outer fn params
    Block                    body;             // contains LocalFuncStmt + ReturnStmt
};
