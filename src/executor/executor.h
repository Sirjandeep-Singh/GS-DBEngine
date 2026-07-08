#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "../parser/ast.h"
#include "../catalog/catalog_manager.h"
#include "../catalog/schema.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"
#include "../btree/free_list_manager.h"
#include "../table/table.h"
#include "../row/row.h"

// Returned by every Executor::execute() call.
// success=false sets error_message; columns/rows are empty.
// rows_affected is set for INSERT / UPDATE / DELETE.
struct QueryResult {
    bool                                   success       = false;
    std::string                            error_message;
    std::vector<std::string>               columns;      // column header names, in order
    std::vector<std::vector<std::string>>  rows;         // result rows as display strings
    uint32_t                               rows_affected = 0;
};

// Executor walks a parsed Statement AST and calls the appropriate
// Table / CatalogManager methods to carry out the query.
//
// Table objects are created on demand per query and discarded on return —
// the Executor never owns them long-term.
//
// CREATE DATABASE / DROP DATABASE / USE are database-level operations
// handled by the Database class before reaching execute(). Receiving them
// here returns an error.
//
// All exceptions are caught inside execute() and returned as QueryResult
// with success=false — nothing propagates to the caller.

class Executor {
public:
    // catalog, buffer_pool, wal, and free_list must outlive the Executor.
    Executor(CatalogManager& catalog, BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list);

    // Execute any parsed Statement. Never throws.
    QueryResult execute(const Statement& stmt);

private:
    CatalogManager&   catalog_;
    BufferPool&       buffer_pool_;
    WALManager&       wal_;
    FreeListManager&  free_list_;

    // ── Statement handlers ───────────────────────────────────────────────────

    QueryResult execute_select(const SelectStmt& stmt);

    // Handles SELECT lists where every column is an aggregate (COUNT(*) /
    // COUNT(col)) — a single-row result computed over a WHERE-filtered scan.
    // v1 scope: no JOIN, no GROUP BY (see AggregateType comment in ast.h).
    QueryResult execute_select_aggregate(const SelectStmt&  stmt,
                                          const TableSchema& schema,
                                          Table&              tbl) const;
    QueryResult execute_insert(const InsertStmt& stmt);
    QueryResult execute_update(const UpdateStmt& stmt);
    QueryResult execute_delete(const DeleteStmt& stmt);
    QueryResult execute_create_table(const CreateTableStmt& stmt);
    QueryResult execute_drop_table(const DropTableStmt& stmt);
    QueryResult execute_show(const ShowStmt& stmt);

    // These are handled by the Database class — return an error if reached.
    QueryResult execute_create_database(const CreateDatabaseStmt& stmt);
    QueryResult execute_drop_database(const DropDatabaseStmt& stmt);
    QueryResult execute_use(const UseStmt& stmt);

    // ── WHERE evaluation ─────────────────────────────────────────────────────

    // Recursively evaluates a WhereExpr tree (AND / OR / NOT / compare)
    // against a single row. Returns true if the row satisfies the expression.
    // right_schema is non-null for JOIN queries — enables two-schema column resolution.
    bool evaluate_where(const WhereExprPtr& expr,
                        const Row&          row,
                        const TableSchema&  left_schema,
                        const TableSchema*  right_schema = nullptr) const;

    // Evaluates a single CompareExpr leaf node against a row.
    bool evaluate_compare(const CompareExpr& expr,
                          const Row&         row,
                          const TableSchema& left_schema,
                          const TableSchema* right_schema = nullptr) const;

    // Returns a Predicate that wraps evaluate_where().
    // If where is nullptr, returns a predicate that always returns true.
    // right_schema is non-null for JOIN queries.
    Predicate build_predicate(const WhereExprPtr& where,
                              const TableSchema&  schema,
                              const TableSchema*  right_schema = nullptr) const;

    // ── JOIN ─────────────────────────────────────────────────────────────────

    // Merged row from a left + right scan result.
    // row.values = left columns first, right columns appended.
    // For LEFT JOIN with no right match, right values are std::monostate (NULL).
    struct JoinedRow {
        Row      row;
        uint32_t left_pk;
    };

    // Nested-loop join of left_results against right_table using the ON clause.
    // Handles INNER, LEFT, and RIGHT join types.
    std::vector<JoinedRow> nested_loop_join(
        const std::vector<ScanResult>& left_results,
        const TableSchema&             left_schema,
        Table&                         right_table,
        const TableSchema&             right_schema,
        const JoinClause&              join) const;

    // ── Column resolution ────────────────────────────────────────────────────

    // Returns the zero-based column index for ref in schema.
    // Throws if the column is not found or the table qualifier doesn't match.
    size_t resolve_column(const ColumnRef&   ref,
                          const TableSchema& schema) const;

    // Two-schema overload for JOIN queries.
    // Qualified refs are checked against the correct table name.
    // Unqualified refs search both schemas and throw on ambiguity.
    size_t resolve_column(const ColumnRef&   ref,
                          const TableSchema& left_schema,
                          const TableSchema& right_schema) const;

    // ── CREATE TABLE helpers ─────────────────────────────────────────────────

    // Converts an AST ColumnDef to a catalog Column.
    // Validates type, VARCHAR length, and that AUTO_INCREMENT is INT only.
    Column column_def_to_column(const ColumnDef& def) const;

    // ── CHECK constraint helpers ─────────────────────────────────────────────

    // "Compiles" a parsed CHECK expression (column-level or table-level) into
    // the catalog's flat, serializable CheckConstraint list. Only supports an
    // AND-chain of simple `column OP literal` comparisons — SQL has no chained
    // comparisons (30 < x < 50 is invalid SQL), so CHECK (x > 30 AND x < 50)
    // is exactly this shape: two ANDed comparisons. Throws std::runtime_error
    // for anything else (OR, NOT, qualified/unknown columns, IS NULL, LIKE).
    void flatten_check_expr(const WhereExprPtr&           expr,
                             const TableSchema&            schema,
                             std::vector<CheckConstraint>& out) const;

    CheckOp    compare_op_to_check_op(CompareOp op) const;
    CompareOp  check_op_to_compare_op(CheckOp op) const;

    // Evaluates all of schema.checks against a row (all implicitly ANDed).
    // Returns the violated constraint's index, or -1 if the row passes.
    // Reuses evaluate_compare() so the same NULL/int-float semantics apply.
    int find_violated_check(const TableSchema& schema, const Row& row) const;

    // ── Result formatting ────────────────────────────────────────────────────

    // Converts a Row's values to strings via value_to_string().
    std::vector<std::string> row_to_strings(const Row& row) const;

    // Returns column header names for a SELECT result.
    // joined_schema is nullptr for non-JOIN queries.
    std::vector<std::string> build_column_headers(
        const std::vector<SelectColumn>& select_cols,
        const TableSchema&               schema,
        const TableSchema*               joined_schema = nullptr) const;

    // Projects the SELECT column list from a row to strings.
    // joined_row and joined_schema are provided for JOIN results.
    std::vector<std::string> project_row(
        const std::vector<SelectColumn>& select_cols,
        const Row&                       row,
        const TableSchema&               schema,
        const TableSchema*               joined_schema = nullptr) const;
};