#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <memory>
#include <utility>

#include "../parser/ast.h"
#include "../catalog/catalog_manager.h"
#include "../catalog/schema.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"
#include "../btree/free_list_manager.h"
#include "../table/table.h"
#include "../index/index.h"
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
    // Forward declaration — OuterContext is fully defined further down
    // (see the "Subqueries" section), but find_index_prefix() and friends
    // need to accept a pointer to it up here for G's correlated-subquery
    // index seeding (see collect_and_constraints's comment). A pointer to
    // an incomplete type is fine; only the later, complete definition is
    // ever dereferenced.
    struct OuterContext;

    CatalogManager&   catalog_;
    BufferPool&       buffer_pool_;
    WALManager&       wal_;
    FreeListManager&  free_list_;

    // ── Index loading ────────────────────────────────────────────────────────
    //
    // Constructs one Index per secondary index CatalogManager has on-record
    // for `schema.name`, so a Table opened for writing can be handed them
    // via its `indexes` constructor parameter and keep them in sync through
    // write_row_and_indexes/remove_row_and_indexes. Called at the top of
    // every statement handler that opens a Table with intent to mutate it
    // (INSERT/UPDATE/DELETE) — without this, Table defaults to indexes={}
    // and every secondary index silently goes stale.
    //
    // Returned by value (owning) because Index holds a const IndexSchema&
    // into CatalogManager's own storage (stable — see CatalogManager::get_index)
    // but Index itself has no home to live in otherwise; the caller keeps
    // this vector alive for exactly as long as the Table that borrows raw
    // pointers from it, same lifetime relationship schema already has
    // between CatalogManager and Table.
    std::vector<std::unique_ptr<Index>> open_indexes(const TableSchema& schema) const;

    // Returns the raw non-owning pointers open_indexes' unique_ptrs manage —
    // exactly the shape Table's constructor wants. Kept as a separate step
    // (rather than folded into open_indexes) so the owning vector and the
    // pointer vector are visibly two different lifetimes at the call site.
    static std::vector<Index*> index_ptrs(const std::vector<std::unique_ptr<Index>>& owned);

    // Persists every index root page that may have changed after a
    // mutating Table call (insert/update_where/delete_where), mirroring
    // how table root pages are already persisted via
    // CatalogManager::update_table_root. Call once after the mutation,
    // passing the same Table the indexes were opened for.
    void persist_index_roots(const Table& tbl) const;

    // ── Index-assisted scans ─────────────────────────────────────────────────
    //
    // Speeds up WHERE evaluation by using a secondary index instead of a full
    // Table::scan() whenever the WHERE clause has a usable equality match (or,
    // for a composite index, a leftmost-prefix of equality matches) on an
    // indexed column. Falls back to Table::scan() whenever no index fits.
    // The index is only ever used to narrow *candidate* rows: every key it
    // returns is looked up via Table::select_by_key() and the resulting row
    // is still re-checked against the full WHERE predicate before being kept
    // — so a wrong or partial index match can only make this path slower
    // than it needs to be, never wrong.

    // Per-column constraints collected out of a WHERE tree's top-level AND
    // conjuncts: an equality pins the column to one value; lo/hi narrow it
    // to a range (each independently optional, with its own inclusivity).
    // A column can carry an equality OR a range, never both in the same
    // struct instance at once in practice (an EQ makes any range on the
    // same column redundant for index-seeding purposes), but nothing here
    // enforces that — find_index_prefix() only ever reads .eq when walking
    // an index's leftmost columns, and only reads lo/hi for the first
    // column that has no .eq, so a stray range alongside an eq is simply
    // never consulted, not incorrect.
    //
    // Collection deliberately does NOT try to pick the *tightest* bound
    // when a column has more than one LT/GT/etc. condition on it (e.g.
    // `age > 20 AND age > 25`) — it just keeps the last one seen. This is
    // safe, never wrong: scan_with_index()/execute_delete() still re-run
    // the full predicate against every fetched row, so a looser seed bound
    // only costs a few extra candidate fetches, never a wrong result.
    struct ColumnConstraint {
        std::optional<Value> eq;
        std::optional<Value> lo;
        bool                  lo_inclusive = false;
        std::optional<Value> hi;
        bool                  hi_inclusive = false;
    };

    // Walks a WHERE tree and collects every top-level AND-conjoined
    // equality (column = literal) and range (column </<=/>/>= literal)
    // comparison it can find, keyed by column name. Descends through
    // LOGICAL AND nodes only — an OR or NOT node contributes nothing from
    // its subtree, since neither side of an OR is a condition every
    // matching row must satisfy, and NOT's semantics aren't a simple
    // equality/range either. This makes the collected map always a set of
    // *necessary* (if not sufficient) conditions on any matching row,
    // which is exactly what's safe to use as an index seed.
    //
    // `outer`, when non-null, enables G — seeding an index from a
    // correlated column-to-column comparison (e.g. `o.customer_id = c.id`
    // inside `WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id =
    // c.id)`): a comparison with operand_column set is normally skipped
    // entirely (neither side is a literal), but if the operand_column
    // resolves against the OUTER row instead of this table, its value for
    // *this specific outer row* is a real literal as far as this table's
    // index is concerned — look it up via outer->row and treat it exactly
    // like a literal EQ/range operand. This turns a correlated EXISTS/IN
    // into an index nested-loop join: for the outer row currently being
    // checked, "o.customer_id = c.id" seeds orders' customer_id index with
    // c.id's actual value from `outer`.
    void collect_and_constraints(const WhereExprPtr& expr,
                                  const TableSchema&  schema,
                                  const std::string&  alias,
                                  std::unordered_map<std::string, ColumnConstraint>& out,
                                  const OuterContext* outer = nullptr) const;

    // A usable index pick from find_index_prefix(): `prefix` is the
    // leftmost run of equality-matched columns (in the index's own column
    // order, ready for Index::find()/find_range()'s prefix parameter);
    // range_lo/range_hi (with their inclusivity flags) are set only when
    // the column immediately after `prefix` was narrowed by a range
    // instead of (or in addition to reaching) an equality — i.e. exactly
    // when is_range() is true, this is a find_range() call instead of a
    // find() call.
    struct IndexMatch {
        const IndexSchema*   index = nullptr;
        Key                   prefix;
        std::optional<Value> range_lo;
        bool                  range_lo_inclusive = false;
        std::optional<Value> range_hi;
        bool                  range_hi_inclusive = false;

        bool is_range() const { return range_lo.has_value() || range_hi.has_value(); }
    };

    // Picks the best usable index for `where` against `schema`, if any: the
    // one whose column_names has the longest leftmost prefix fully covered
    // by collect_and_constraints()'s output — each equality value coerced
    // to the matching column's type (e.g. an int literal against a FLOAT
    // column), so a usable index isn't missed purely over a literal's
    // parsed type — optionally followed by a range on the very next
    // column (same coercion applied to lo/hi); ties broken by catalog
    // order, longer effective prefix (equality columns, plus one more if
    // a range extends it) wins. Returns nullopt if no index has even a
    // 1-column equality or range match — the caller falls back to
    // Table::scan().
    std::optional<IndexMatch> find_index_prefix(
        const WhereExprPtr& where,
        const TableSchema&  schema,
        const std::string&  alias = "",
        const OuterContext* outer = nullptr) const;

    // Looks up candidate primary keys for `match` against `idx` — find()
    // for a plain equality prefix, find_range() when the match extends
    // one column further via a range. Shared by scan_with_index() and
    // execute_delete()'s index-assisted path so the find()-vs-find_range()
    // dispatch lives in exactly one place.
    static std::vector<Key> index_lookup(const Index& idx, const IndexMatch& match);

    // Returns the rows matching `pred`, using find_index_prefix()'s pick (if
    // any) to fetch only candidate primary keys via Index::find() +
    // Table::select_by_key(), instead of a full Table::scan(). `where` is the
    // same WHERE tree `pred` was built from (see build_predicate) — passed
    // separately because Predicate is an opaque std::function and
    // find_index_prefix needs the AST shape; `pred` remains the single source
    // of truth for which rows actually match, since the index only narrows
    // candidates and every fetched row is still run through it.
    std::vector<ScanResult> scan_with_index(const Table&         tbl,
                                             const TableSchema&  schema,
                                             const WhereExprPtr& where,
                                             const Predicate&    pred,
                                             const std::string&  alias = "",
                                             const OuterContext* outer = nullptr) const;

    // ── Covering index reads (H) ─────────────────────────────────────────────
    //
    // A query is "coverable" by a given index when every column it touches
    // — SELECT list, ORDER BY, and every column referenced anywhere in
    // WHERE — lies within that index's own columns union the table's
    // primary key. When that holds, the index tuple returned alongside
    // each primary key (see Index::find_with_values/find_range_with_values)
    // already contains every value the query could possibly need, so the
    // usual extra Table::select_by_key() fetch per candidate is pure
    // waste — scan_covering() skips it entirely.
    //
    // where_is_coverable() walks the WHERE tree and bails conservatively
    // (returns false) on anything fiddly to prove safe here: EXISTS or an
    // IN/NOT IN subquery (a different table's columns are involved, and
    // subquery correctness lives in run_subquery_scan, not this fast
    // path), or a column=column comparison where the other side isn't
    // itself covered.
    bool where_is_coverable(const WhereExprPtr& expr,
                             const TableSchema&  schema,
                             const std::string&  alias,
                             const std::unordered_set<std::string>& covered) const;

    // Full coverability check: WHERE (via where_is_coverable) plus every
    // plain SELECT column and the ORDER BY column (if any). SELECT * and
    // any aggregate column make a query trivially non-coverable (a star
    // needs every column in the table, not just the indexed ones; an
    // aggregate is handled by execute_select_aggregate, a different path
    // entirely) — literal SELECT items (e.g. `SELECT 1`) are always free,
    // since they don't read any column at all.
    bool is_select_coverable(const std::vector<SelectColumn>&  columns,
                              const WhereExprPtr&               where,
                              const std::vector<OrderByClause>& order_by,
                              const TableSchema&                schema,
                              const std::string&                alias,
                              const std::unordered_set<std::string>& covered) const;

    // Attempts a covering-index read for a simple (non-JOIN, non-aggregate)
    // SELECT: returns nullopt if no index applies at all (same as
    // find_index_prefix) or the query isn't coverable by whichever index
    // find_index_prefix picked — the caller should fall back to
    // scan_with_index() in either case. When it does return, every row in
    // the result was built directly from the index tuple + primary key,
    // with every OTHER column in the Row left default-constructed
    // (monostate/NULL) — safe only because is_select_coverable() already
    // proved this query's WHERE/SELECT/ORDER BY never touches them.
    std::optional<std::vector<ScanResult>> scan_covering(
        const TableSchema&                 schema,
        const WhereExprPtr&                where,
        const Predicate&                   pred,
        const std::vector<SelectColumn>&   columns,
        const std::vector<OrderByClause>&  order_by,
        const std::string&                 alias = "") const;

    // ── Statement handlers ───────────────────────────────────────────────────

    QueryResult execute_select(const SelectStmt& stmt);

    // Handles SELECT lists where every column is an aggregate (COUNT/MAX/
    // MIN/AVG/MEDIAN) and there's no GROUP BY — a single-row result
    // computed over a WHERE-filtered scan. v1 scope: no JOIN.
    QueryResult execute_select_aggregate(const SelectStmt&  stmt,
                                          const TableSchema& schema,
                                          Table&              tbl) const;

    // Handles SELECT ... GROUP BY — one result row per distinct combination
    // of GROUP BY column values. Every plain SELECT column must also be a
    // GROUP BY column (validated here, not by the parser); aggregate
    // columns (COUNT/MAX/MIN/AVG/MEDIAN) are computed per group via
    // compute_aggregate_column. An optional HAVING clause filters groups
    // after they're built (see evaluate_having) and before LIMIT is
    // applied. v1 scope: no JOIN, no ORDER BY (ORDER BY on a grouped/
    // aggregate result needs to sort the projected output rather than a
    // schema-indexed Row, which the existing sort_by_column helper in
    // execute_select doesn't support yet).
    QueryResult execute_select_group_by(const SelectStmt&  stmt,
                                         const TableSchema& schema,
                                         Table&              tbl) const;

    // Computes one aggregate SELECT column's header text and string value
    // over `rows` — either the whole WHERE-filtered scan (execute_select_
    // aggregate) or a single group's rows (execute_select_group_by).
    // COUNT_STAR/COUNT_COLUMN count rows/non-NULLs; MAX/MIN compare via
    // value_less_than (same cross-int/float ordering WHERE and ORDER BY
    // use); AVG/MEDIAN require a numeric column and always report a FLOAT
    // result, even over an all-INT column. Any aggregate over zero
    // non-NULL values reports NULL (COUNT reports 0, matching real SQL).
    // Thin formatting wrapper around compute_aggregate_value.
    std::pair<std::string, std::string> compute_aggregate_column(
        const SelectColumn&             sc,
        const std::vector<ScanResult>&  rows,
        const TableSchema&              schema,
        const std::string&              alias) const;

    // Same computation as compute_aggregate_column, minus the string
    // formatting — returns the raw Value (monostate for NULL) so callers
    // that need to compare or further compute with the result, rather than
    // display it, don't have to parse a string back out. Used by
    // compute_aggregate_column itself and by evaluate_having (HAVING needs
    // the actual value to run compare_values against a literal).
    Value compute_aggregate_value(
        const SelectColumn&             sc,
        const std::vector<ScanResult>&  rows,
        const TableSchema&              schema,
        const std::string&              alias) const;

    // Returns the display header for an aggregate SELECT column, e.g.
    // "COUNT(*)", "MAX(age)". Single source of truth for aggregate header
    // text — used both by compute_aggregate_column and by
    // execute_select_group_by's own header-building pass (which needs
    // headers before any group's rows are available to compute a value).
    static std::string aggregate_header_label(AggregateType      type,
                                               const std::string& col_name);

    // Evaluates a HAVING expression against one group's rows (post-WHERE,
    // pre-projection) — true if the group passes. Mirrors evaluate_where's
    // COMPARE/LOGICAL recursion, but the COMPARE operand is computed via
    // compute_aggregate_value (which handles both aggregate and plain
    // grouped-column operands) instead of a direct row lookup, and the
    // actual comparison reuses the same compare_values helper evaluate_
    // where's evaluate_compare does — see executor.cpp.
    bool evaluate_having(const HavingExprPtr&            expr,
                          const std::vector<ScanResult>&  group_rows,
                          const TableSchema&               schema,
                          const std::string&               alias) const;
    QueryResult execute_insert(const InsertStmt& stmt);
    QueryResult execute_update(const UpdateStmt& stmt);
    QueryResult execute_delete(const DeleteStmt& stmt);
    QueryResult execute_create_table(const CreateTableStmt& stmt);
    QueryResult execute_drop_table(const DropTableStmt& stmt);
    QueryResult execute_create_index(const CreateIndexStmt& stmt);
    QueryResult execute_drop_index(const DropIndexStmt& stmt);
    QueryResult execute_show(const ShowStmt& stmt);

    // DESCRIBE table_name — returns one row per column (Field, Type, Null,
    // Key, Default, Extra), followed by a final row holding the fully
    // reconstructed CREATE TABLE statement (in the "Field" cell, labeled
    // "Create Table") so it can be copied and pasted to recreate the table
    // elsewhere, the same way SHOW CREATE TABLE / \d work in other
    // databases. See build_create_table_sql for how that statement is
    // reconstructed from the catalog's TableSchema.
    QueryResult execute_describe(const DescribeStmt& stmt);

    // Reconstructs a CREATE TABLE statement (plus any CREATE [UNIQUE]
    // INDEX statements for indexes not already implied by a constraint)
    // that would recreate `schema` from scratch. Used by execute_describe;
    // factored out separately since it only reads the catalog and never
    // touches storage.
    std::string build_create_table_sql(const TableSchema& schema) const;

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
        Row row;
        Key left_pk;
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

    // ── FOREIGN KEY constraint helpers ───────────────────────────────────────

    // Checks `row` (about to be written into `schema`, a child table) against
    // every FK constraint on `schema`. Returns "" if all pass, or a
    // human-readable description of the first violation found. A FK whose
    // component value(s) are NULL (any one, for a composite FK) is skipped
    // entirely for this row — MATCH SIMPLE semantics, the same convention
    // Index::is_indexable already uses. Otherwise looks up the referenced
    // value via the constraint's ref_index_name (or Table::select_by_key on
    // ref_table's primary key when ref_index_name is empty) — an indexed
    // lookup, not a scan.
    std::string check_foreign_keys_on_write(const TableSchema& schema, const Row& row) const;

    // One child table's rows that reference a specific parent row via one
    // of its FOREIGN KEY constraints.
    struct ReferencingChildren {
        std::string           child_table;
        ForeignKeyConstraint  constraint;   // which FK on child_table matched
        std::vector<Key>      child_primary_keys;
    };

    // Finds every child row across every table that references `ref_row`
    // (a row of `ref_schema`, the parent side) through some other table's
    // FOREIGN KEY — used before DELETE/UPDATE on a row that might be a
    // referenced parent. One entry per FK constraint that has at least one
    // match; a constraint with zero matching child rows is omitted. Looked
    // up via each constraint's auto-created child_index_name — an indexed
    // lookup per referencing table, not a full scan of the database. A FK
    // whose referenced column value(s) are NULL in ref_row can never have
    // been matched by any child (NULL is never indexed), so it's skipped.
    std::vector<ReferencingChildren> find_referencing_children(
        const TableSchema& ref_schema, const Row& ref_row) const;

    // Deletes exactly one row, first checking every FOREIGN KEY constraint
    // that references it (via find_referencing_children): a RESTRICT
    // constraint with any matching child row throws (caught by execute()'s
    // top-level try/catch, same as any other FK violation) instead of
    // deleting anything; a CASCADE constraint recursively deletes every
    // matching child row FIRST — so a chain of CASCADE FKs cascades
    // transitively through this same function, table by table. Returns
    // false (no-op, not an error) if the row was already gone — e.g.
    // removed a moment earlier by an overlapping cascade from a different
    // top-level match in the same DELETE statement — true if it deleted it.
    //
    // Re-reads `table_name`'s schema fresh from the catalog on every call
    // (rather than being handed one) and opens its own Table, since a
    // cascade can hop to a completely different table with its own schema
    // and indexes — there is no single long-lived Table this recursion can
    // share across tables the way execute_insert/update/delete's top-level
    // handlers share one Table for their own single-table operation.
    bool delete_row_cascading(const std::string& table_name, const Key& key);

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