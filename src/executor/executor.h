#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <optional>

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

    // ── Subqueries (IN / NOT IN / EXISTS / NOT EXISTS) ──────────────────────
    //
    // Two kinds, distinguished per-node by precompute_subqueries:
    //
    // - Non-correlated: the subquery doesn't reference any column from the
    //   outer row, so it's the same result for every outer row. Executed
    //   exactly once per statement (in precompute_subqueries, before
    //   scanning) and cached — re-running an identical query per row would
    //   be a correctness-preserving but needlessly slow choice.
    //
    // - Correlated: the subquery's own WHERE references a column that isn't
    //   part of its own table (e.g. `EXISTS (SELECT 1 FROM orders WHERE
    //   orders.user_id = users.id)` — `users.id` only means something for
    //   "whichever outer row we're currently checking"). Its result can
    //   differ per outer row, so it CANNOT be cached — precompute_subqueries
    //   detects this case and marks it instead of computing it; the actual
    //   re-execution happens per-row in evaluate_where/evaluate_compare,
    //   with the current outer row threaded down via OuterContext.

    // Identifies the outer row a nested subquery's WHERE clause may need to
    // fall back to when a column isn't found in the subquery's own schema.
    // Passed down through build_predicate/evaluate_where/evaluate_compare
    // only when evaluating a correlated subquery; nullptr everywhere else.
    struct OuterContext {
        const Row*         row;
        const TableSchema* schema;
        const TableSchema* right_schema = nullptr;  // non-null if the outer query is itself a JOIN
        std::string        alias;                   // outer query's table alias, "" if none
        std::string        right_alias;              // outer query's JOIN alias, "" if none
    };

    // Cache of pre-computed subquery results for one WHERE tree, keyed by
    // the address of the AST node that owns the subquery. Node addresses
    // are stable for the cache's lifetime: every WhereExpr/CompareExpr is
    // heap-allocated once via unique_ptr and never copied or moved after
    // parsing, so &node stays valid as long as the AST does.
    struct SubqueryCache {
        std::unordered_map<const CompareExpr*, std::vector<Value>> in_values;      // IN_SUBQUERY / NOT_IN_SUBQUERY (non-correlated)
        std::unordered_map<const WhereExpr*,    bool>              exists_results; // EXISTS (non-correlated)
        std::unordered_set<const CompareExpr*>  correlated_in;      // IN_SUBQUERY / NOT_IN_SUBQUERY (correlated — re-run per row)
        std::unordered_set<const WhereExpr*>    correlated_exists;  // EXISTS (correlated — re-run per row)
    };

    // Walks a subquery's own WHERE tree and returns true if any ColumnRef in
    // it (LHS or RHS) does NOT resolve against sub_schema — meaning it must
    // refer to a column from an outer query. Used once per subquery node by
    // precompute_subqueries to decide cache-once vs. re-run-per-row.
    bool where_references_unknown_column(const WhereExprPtr& expr,
                                          const TableSchema&  sub_schema,
                                          const std::string&  sub_alias = "") const;

    // Walks a WHERE tree and runs every non-correlated subquery it contains
    // exactly once, storing results in cache. Correlated subqueries are
    // marked in cache but not executed here. A no-op for any subtree with
    // no subqueries.
    void precompute_subqueries(const WhereExprPtr& expr, SubqueryCache& cache) const;

    // Runs a subquery SELECT (no JOIN, no aggregate — v1 scope) and returns
    // its raw scan results. Honors the subquery's own WHERE (recursively,
    // via build_predicate) and LIMIT; ignores ORDER BY (irrelevant for
    // set membership / existence checks). 'outer' is non-null when this is
    // a correlated subquery being re-run for one specific outer row.
    std::vector<ScanResult> run_subquery_scan(const SelectStmt&    subquery,
                                               const OuterContext* outer = nullptr) const;

    // Runs a subquery for IN/NOT IN — requires exactly one plain (non-star,
    // non-aggregate) selected column, and returns that column's values.
    std::vector<Value> run_subquery_values(const SelectStmt&    subquery,
                                            const OuterContext* outer = nullptr) const;

    // Type-aware equality (int/float promotion, like evaluate_compare's EQ)
    // used to test row values against a precomputed IN candidate set.
    bool values_equal(const Value& a, const Value& b) const;

    // ── WHERE evaluation ─────────────────────────────────────────────────────

    // Recursively evaluates a WhereExpr tree (AND / OR / NOT / compare / EXISTS)
    // against a single row. Returns true if the row satisfies the expression.
    // right_schema is non-null for JOIN queries — enables two-schema column resolution.
    // cache holds pre-computed subquery results (see precompute_subqueries) —
    // required (non-null) if expr contains any IN-subquery or EXISTS node.
    // outer is non-null only while evaluating inside a correlated subquery's
    // own WHERE — it lets column resolution fall back to the outer row.
    bool evaluate_where(const WhereExprPtr&  expr,
                        const Row&           row,
                        const TableSchema&   left_schema,
                        const TableSchema*   right_schema = nullptr,
                        const SubqueryCache* cache         = nullptr,
                        const OuterContext*  outer         = nullptr,
                        const std::string&   left_alias    = "",
                        const std::string&   right_alias   = "") const;

    // Evaluates a single CompareExpr leaf node against a row.
    bool evaluate_compare(const CompareExpr&   expr,
                          const Row&           row,
                          const TableSchema&   left_schema,
                          const TableSchema*   right_schema = nullptr,
                          const SubqueryCache* cache         = nullptr,
                          const OuterContext*  outer         = nullptr,
                          const std::string&   left_alias    = "",
                          const std::string&   right_alias   = "") const;

    // Returns a Predicate that wraps evaluate_where().
    // If where is nullptr, returns a predicate that always returns true.
    // right_schema is non-null for JOIN queries. Precomputes any non-correlated
    // subqueries in 'where' exactly once and keeps the cache alive for the
    // predicate's lifetime (captured by shared_ptr in the returned lambda).
    // outer is forwarded through when this predicate is itself being built
    // for a correlated subquery's own scan (see run_subquery_scan).
    Predicate build_predicate(const WhereExprPtr& where,
                              const TableSchema&  schema,
                              const TableSchema*  right_schema = nullptr,
                              const OuterContext* outer        = nullptr,
                              const std::string&  left_alias   = "",
                              const std::string&  right_alias  = "") const;

    // Resolves a ColumnRef to a value against the current row, trying the
    // local schema(s) first and falling back to 'outer' (if provided) when
    // the column isn't found locally. This fallback is the entire mechanism
    // behind correlated subqueries — nothing more exotic than "if it's not
    // in my row, check the outer one."
    const Value& resolve_value(const ColumnRef&    ref,
                               const Row&           row,
                               const TableSchema&   schema,
                               const TableSchema*   right_schema,
                               const OuterContext*  outer,
                               const std::string&   left_alias  = "",
                               const std::string&   right_alias = "") const;

    // Non-throwing column lookups, used by resolve_value's fallback chain
    // and by where_references_unknown_column for correlation detection.
    // 'alias' is the query's table alias for 'schema' (e.g. the "o" in
    // "FROM orders o"), if any — a qualified ref matches on either the
    // alias or the real table name.
    std::optional<size_t> try_resolve_column(const ColumnRef&   ref,
                                             const TableSchema& schema,
                                             const std::string& alias = "") const;
    std::optional<size_t> try_resolve_column(const ColumnRef&   ref,
                                             const TableSchema& left_schema,
                                             const TableSchema& right_schema,
                                             const std::string& left_alias  = "",
                                             const std::string& right_alias = "") const;

    // True if 'ref_table' (a qualified column's table-name part) refers to
    // 'schema' — either by its real catalog name or by the query-supplied
    // alias. Centralizes the "alias OR real name" check used throughout
    // column resolution.
    bool table_name_matches(const std::string& ref_table,
                             const TableSchema& schema,
                             const std::string& alias) const;

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
        const JoinClause&              join,
        const std::string&             left_alias  = "",
        const std::string&             right_alias = "") const;

    // ── Column resolution ────────────────────────────────────────────────────

    // Returns the zero-based column index for ref in schema.
    // Throws if the column is not found or the table qualifier doesn't match.
    size_t resolve_column(const ColumnRef&   ref,
                          const TableSchema& schema,
                          const std::string& alias = "") const;

    // Two-schema overload for JOIN queries.
    // Qualified refs are checked against the correct table name (or alias).
    // Unqualified refs search both schemas and throw on ambiguity.
    size_t resolve_column(const ColumnRef&   ref,
                          const TableSchema& left_schema,
                          const TableSchema& right_schema,
                          const std::string& left_alias  = "",
                          const std::string& right_alias = "") const;

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
    // left_alias/right_alias let a qualified SELECT column (e.g. "c.name")
    // match a query alias, not just the real table name.
    std::vector<std::string> project_row(
        const std::vector<SelectColumn>& select_cols,
        const Row&                       row,
        const TableSchema&               schema,
        const TableSchema*               joined_schema = nullptr,
        const std::string&               left_alias    = "",
        const std::string&               right_alias   = "") const;
};