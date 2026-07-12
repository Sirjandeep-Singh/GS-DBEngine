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

// Forward declaration — SelectStmt is defined later (it contains a
// WhereExprPtr), but IN (SELECT ...) / EXISTS (SELECT ...) need to embed a
// SelectStmt inside an expression node. A pointer only needs the forward
// declaration, so this breaks the mutual-recursion cycle the same way
// WhereExprPtr already breaks WhereExpr's own self-recursion below.
struct SelectStmt;

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
    IN_SUBQUERY,     // column IN (SELECT ...)
    NOT_IN_SUBQUERY, // column NOT IN (SELECT ...)
};

// A comparison expression: age > 25, name = 'Alice', email IS NULL,
// dept_id IN (SELECT id FROM depts ...)
struct CompareExpr {
    ColumnRef column;
    CompareOp op = CompareOp::EQ;
    Value     operand;   // used when the RHS is a literal; not used for IS_NULL / IS_NOT_NULL / *_SUBQUERY
    std::optional<ColumnRef> operand_column;  // used when the RHS is itself a column, e.g.
                                               // `orders.user_id = users.id`. Mutually exclusive
                                               // with 'operand' — if set, 'operand' is ignored.
                                               // This is what makes correlated subqueries expressible:
                                               // the inner query's WHERE can reference a column that
                                               // only resolves against the outer row.
    std::unique_ptr<SelectStmt> subquery;  // only used for IN_SUBQUERY / NOT_IN_SUBQUERY
};

// Logical connectives: AND, OR, NOT
enum class LogicalOp { AND, OR, NOT };

// Forward declaration for recursive expression tree
struct WhereExpr;
using WhereExprPtr = std::unique_ptr<WhereExpr>;

// A WHERE expression node — a comparison, a logical combination, or an
// EXISTS (SELECT ...) check. NOT EXISTS is represented as a LOGICAL/NOT
// node wrapping an EXISTS node — no separate "negated" flag needed.
struct WhereExpr {
    enum class Kind { COMPARE, LOGICAL, EXISTS } kind;

    // for COMPARE nodes
    CompareExpr compare;

    // for LOGICAL nodes
    LogicalOp          logical_op;
    WhereExprPtr       left;
    WhereExprPtr       right;  // null for NOT

    // for EXISTS nodes
    std::unique_ptr<SelectStmt> subquery;
};

// ─────────────────────────────────────────────
// SELECT statement
// ─────────────────────────────────────────────

// Aggregate functions usable in a SELECT column list.
// v1 scope: COUNT only, and only when every column in the SELECT list is
// an aggregate (no GROUP BY yet, so mixing aggregate + plain columns is
// not meaningful — that's exactly what GROUP BY exists to make meaningful).
enum class AggregateType { NONE, COUNT_STAR, COUNT_COLUMN };

// A selected column: *, name, users.name, COUNT(*), COUNT(name), or a bare
// literal (1, 'x', TRUE, NULL) — most commonly seen as `SELECT 1` in an
// EXISTS subquery, where the projected value is irrelevant and only
// row-presence matters.
struct SelectColumn {
    bool          is_star   = false;    // SELECT *
    bool          is_literal = false;   // SELECT 1 / SELECT 'x' / SELECT NULL ...
    Value         literal;              // used if is_literal
    ColumnRef     column;               // used if not star/literal, or if aggregate == COUNT_COLUMN
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
    // table-level PRIMARY KEY (col1, col2, ...) clause — column names in
    // declared order. Empty if no table-level PRIMARY KEY clause was given
    // (the primary key may still be declared inline on a single ColumnDef
    // via is_primary_key). Mutually exclusive with an inline PRIMARY KEY —
    // Executor rejects CREATE TABLE statements that specify both.
    std::vector<std::string> table_primary_key;
};

// ─────────────────────────────────────────────
// DROP TABLE statement
// ─────────────────────────────────────────────

struct DropTableStmt {
    std::string table_name;
};

// ─────────────────────────────────────────────
// CREATE INDEX statement
// ─────────────────────────────────────────────

// CREATE [UNIQUE] INDEX index_name ON table_name (col1, col2, ...)
//
// column_names may be a single column or several (composite index), same
// "arity via vector" shape IndexSchema::column_names already uses.
// Executor builds the B+ tree and backfills it from the table's existing
// rows — CREATE INDEX is always valid on a table that already has data.
struct CreateIndexStmt {
    std::string              index_name;
    std::string              table_name;
    std::vector<std::string> column_names;
    bool                     is_unique = false;
};

// ─────────────────────────────────────────────
// DROP INDEX statement
// ─────────────────────────────────────────────

// DROP INDEX index_name
//
// Mirrors DropTableStmt/DROP TABLE's pattern exactly: removes catalog
// metadata only (CatalogManager::drop_index()) and leaves the index's
// B+ tree pages orphaned rather than reclaiming them via the free list —
// the same pre-existing gap DROP TABLE already has, not something new
// introduced here.
struct DropIndexStmt {
    std::string index_name;
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
    CreateIndexStmt,
    DropIndexStmt,
    CreateDatabaseStmt,
    DropDatabaseStmt,
    UseStmt,
    ShowStmt
>;