#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include "../row/row.h"

// ─────────────────────────────────────────────
// Expressions — used in WHERE clauses and SET values
// ─────────────────────────────────────────────

// A literal value in SQL: 42, 3.14, 'hello', TRUE, NULL
struct LiteralExpr {
    Value value;  // uses the same Value variant from row.h
};

// A column reference: name, users.name, t.age
struct ColumnRef {
    std::string table_name;   // empty if unqualified (e.g. just "name")
    std::string column_name;
};

// Comparison operators used in WHERE
enum class CompareOp {
    EQ,    // =
    NEQ,   // !=
    LT,    // <
    GT,    // >
    LTE,   // <=
    GTE,   // >=
    LIKE,  // LIKE
    IS_NULL,     // IS NULL
    IS_NOT_NULL, // IS NOT NULL
};

// A comparison expression: age > 25, name = 'Alice', email IS NULL
struct CompareExpr {
    ColumnRef column;
    CompareOp op;
    Value     operand;  // not used for IS_NULL / IS_NOT_NULL
};

// Logical connectives: AND, OR, NOT
enum class LogicalOp { AND, OR, NOT };

// Forward declaration for recursive expression tree
struct WhereExpr;
using WhereExprPtr = std::unique_ptr<WhereExpr>;

// A WHERE expression node — either a comparison or a logical combination
struct WhereExpr {
    enum class Kind { COMPARE, LOGICAL } kind;

    // for COMPARE nodes
    CompareExpr compare;

    // for LOGICAL nodes
    LogicalOp          logical_op;
    WhereExprPtr       left;
    WhereExprPtr       right;  // null for NOT
};

// ─────────────────────────────────────────────
// SELECT statement
// ─────────────────────────────────────────────

// Aggregate functions usable in a SELECT column list.
// v1 scope: COUNT only, and only when every column in the SELECT list is
// an aggregate (no GROUP BY yet, so mixing aggregate + plain columns is
// not meaningful — that's exactly what GROUP BY exists to make meaningful).
enum class AggregateType { NONE, COUNT_STAR, COUNT_COLUMN };

// A selected column: *, name, users.name, COUNT(*), COUNT(name)
struct SelectColumn {
    bool          is_star   = false;    // SELECT *
    ColumnRef     column;               // used if not star, or if aggregate == COUNT_COLUMN
    AggregateType aggregate = AggregateType::NONE;
};

// JOIN types supported
enum class JoinType { INNER, LEFT, RIGHT };

struct JoinClause {
    JoinType    type;
    std::string table_name;
    std::string alias;            // empty if no alias
    ColumnRef   left_col;         // ON left_col = right_col
    ColumnRef   right_col;
};

struct OrderByClause {
    ColumnRef column;
    bool      ascending = true;
};

struct SelectStmt {
    std::vector<SelectColumn>  columns;
    std::string                table_name;
    std::string                table_alias;      // empty if no alias
    std::vector<JoinClause>    joins;
    WhereExprPtr               where;            // null if no WHERE
    std::vector<OrderByClause> order_by;
    std::optional<uint32_t>    limit;            // nullopt if no LIMIT
};

// ─────────────────────────────────────────────
// INSERT statement
// ─────────────────────────────────────────────

struct InsertStmt {
    std::string              table_name;
    std::vector<std::string> columns;    // empty = insert in schema order
    std::vector<Value>       values;
};

// ─────────────────────────────────────────────
// UPDATE statement
// ─────────────────────────────────────────────

struct SetClause {
    std::string column_name;
    Value       value;
};

struct UpdateStmt {
    std::string          table_name;
    std::vector<SetClause> assignments;
    WhereExprPtr         where;   // null if no WHERE
};

// ─────────────────────────────────────────────
// DELETE statement
// ─────────────────────────────────────────────

struct DeleteStmt {
    std::string  table_name;
    WhereExprPtr where;   // null if no WHERE
};

// ─────────────────────────────────────────────
// CREATE TABLE statement
// ─────────────────────────────────────────────

// A column definition inside CREATE TABLE
struct ColumnDef {
    std::string name;
    std::string type_name;    // "INT", "VARCHAR", "FLOAT", "BOOLEAN"
    uint32_t    varchar_len;  // only for VARCHAR(n)
    bool        is_primary_key   = false;
    bool        is_nullable      = true;
    bool        auto_increment   = false;
    WhereExprPtr check           = nullptr;  // column-level CHECK (...), null if none
};

struct CreateTableStmt {
    std::string             table_name;
    std::vector<ColumnDef>  columns;
    std::vector<WhereExprPtr> table_checks;  // table-level CHECK (...) clauses
};

// ─────────────────────────────────────────────
// DROP TABLE statement
// ─────────────────────────────────────────────

struct DropTableStmt {
    std::string table_name;
};

// ─────────────────────────────────────────────
// CREATE DATABASE / DROP DATABASE / USE / SHOW
// ─────────────────────────────────────────────

struct CreateDatabaseStmt {
    std::string name;
};

struct DropDatabaseStmt {
    std::string name;
};

struct UseStmt {
    std::string name;
};

enum class ShowTarget { TABLES, DATABASES };

struct ShowStmt {
    ShowTarget target;
};

// ─────────────────────────────────────────────
// Top-level statement variant
// All parse results are returned as a Statement.
// ─────────────────────────────────────────────

using Statement = std::variant<
    SelectStmt,
    InsertStmt,
    UpdateStmt,
    DeleteStmt,
    CreateTableStmt,
    DropTableStmt,
    CreateDatabaseStmt,
    DropDatabaseStmt,
    UseStmt,
    ShowStmt
>;