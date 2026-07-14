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
    // Parallel to values — true at index i means the i-th VALUES slot was
    // written as the literal keyword DEFAULT rather than an actual value,
    // e.g. INSERT INTO t VALUES (1, DEFAULT, 'x'). values[i] is a
    // placeholder (NULL) in that case; Executor resolves it against the
    // column's schema default instead. Empty vector means no slot used
    // DEFAULT (the common case) — same as all-false.
    std::vector<bool>        value_is_default;
};

// ─────────────────────────────────────────────
// UPDATE statement
// ─────────────────────────────────────────────

struct SetClause {
    std::string column_name;
    Value       value;
    // true for `SET col = DEFAULT` — value is a placeholder (NULL) in that
    // case; Executor resolves it against the column's schema default.
    bool        is_default = false;
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
    bool        is_unique        = false;    // column-level UNIQUE — Executor backs this with
                                              // an auto-named unique secondary Index, same
                                              // mechanism CREATE UNIQUE INDEX already uses.
    bool        has_default      = false;    // was a DEFAULT (...) clause given?
    Value       default_value;               // literal used when a column is omitted from
                                              // an INSERT (or explicitly given as DEFAULT).
                                              // Only meaningful when has_default is true.
    WhereExprPtr check           = nullptr;  // column-level CHECK (...), null if none

    // column-level `REFERENCES parent_table(parent_column)` shorthand —
    // e.g. `customer_id INT REFERENCES customers(id)`. Empty string means
    // no inline FK on this column. Always ON DELETE RESTRICT — there's no
    // way to spell ON DELETE CASCADE inline; use the table-level FOREIGN
    // KEY (...) clause (fk_table_name below) for that.
    std::string fk_ref_table;
    std::string fk_ref_column;
};

// ON DELETE behavior for a FOREIGN KEY constraint when a referenced parent
// row is deleted (or updated in a way that changes the referenced column
// values). RESTRICT is the default and the only option for a column-level
// inline REFERENCES. There is deliberately no CASCADE-on-UPDATE distinct
// from CASCADE-on-DELETE — Executor always treats an UPDATE that changes a
// referenced column the same as RESTRICT, regardless of this setting; see
// Executor::execute_update's FK handling for why.
//
// Named distinctly from catalog/schema.h's FkOnDelete (not just reused)
// because this header and schema.h both end up included together in
// executor.cpp — two enums of the same name in the same translation unit
// would collide even though they live in different files.
enum class ForeignKeyOnDelete {
    RESTRICT = 1,
    CASCADE  = 2,
};

// A table-level FOREIGN KEY (col1, ...) REFERENCES parent (col1, ...)
// [ON DELETE CASCADE | ON DELETE RESTRICT] clause.
struct ForeignKeyDef {
    std::vector<std::string> columns;      // this table's FK columns, in order
    std::string               ref_table;   // parent table name
    std::vector<std::string>  ref_columns; // parent's referenced columns, same order/arity
    ForeignKeyOnDelete         on_delete = ForeignKeyOnDelete::RESTRICT;
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
    // table-level UNIQUE (col1, col2, ...) clauses — one entry per clause,
    // each naming the columns of one composite UNIQUE constraint. Distinct
    // from a column-level UNIQUE (ColumnDef::is_unique), which only ever
    // covers that single column. Multiple table-level UNIQUE clauses are
    // allowed, same as multiple CHECK clauses.
    std::vector<std::vector<std::string>> table_unique;
    // table-level FOREIGN KEY (...) REFERENCES ... clauses. Column-level
    // inline REFERENCES (ColumnDef::fk_ref_table) are folded into this
    // same list by Executor::execute_create_table before validation, so
    // downstream FK handling only ever has to look in one place.
    std::vector<ForeignKeyDef> foreign_keys;
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