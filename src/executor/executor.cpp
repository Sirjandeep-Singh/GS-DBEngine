#include "executor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// File-scope helpers
// ─────────────────────────────────────────────────────────────────────────────

// Recursive SQL LIKE matcher.
//   '%' — matches any sequence of characters (including empty)
//   '_' — matches exactly one character
static bool like_match(const std::string& text,
                        const std::string& pattern,
                        size_t             ti = 0,
                        size_t             pi = 0)
{
    if (pi == pattern.size()) return ti == text.size();

    if (pattern[pi] == '%') {
        // Try consuming zero or more characters in text
        for (size_t i = ti; i <= text.size(); ++i) {
            if (like_match(text, pattern, i, pi + 1)) return true;
        }
        return false;
    }

    if (ti == text.size()) return false;

    if (pattern[pi] == '_' || pattern[pi] == text[ti]) {
        return like_match(text, pattern, ti + 1, pi + 1);
    }

    return false;
}

// Checks that a literal Value's active alternative matches the storage
// type a column actually expects. Deliberately as strict as
// RowSerializer::serialize (no int/float coercion) — a DEFAULT literal
// takes the exact same path into storage that an ordinary INSERT value
// does, so it must satisfy the exact same constraint or every row that
// falls back to it would fail to serialize.
static bool value_matches_type(const Value& v, ColumnType col_type)
{
    switch (col_type) {
        case ColumnType::INT:     return std::holds_alternative<int32_t>(v);
        case ColumnType::FLOAT:   return std::holds_alternative<float>(v);
        case ColumnType::BOOLEAN: return std::holds_alternative<bool>(v);
        case ColumnType::VARCHAR: return std::holds_alternative<std::string>(v);
    }
    return false;
}

// Coerces a WHERE literal to the type an indexed column actually stores,
// mirroring the int/float promotion evaluate_compare's EQ case already
// applies (see the std::visit block there). A raw B+ tree key comparison
// (unlike evaluate_compare) requires an exact Value-alternative match, so
// without this, a perfectly valid `WHERE float_col = 5` would silently fail
// to find an index entry stored as float 5.0f, even though evaluate_compare
// would call the two equal. Returns nullopt if the value can never equal
// anything stored in a column of this type (e.g. a fractional float against
// an INT column) — the caller treats that the same as "no equality found for
// this column", which is always safe (it just stops the prefix short).
static std::optional<Value> coerce_equality_value(const Value& v, ColumnType col_type)
{
    if (col_type == ColumnType::FLOAT && std::holds_alternative<int32_t>(v)) {
        return Value{static_cast<float>(std::get<int32_t>(v))};
    }
    if (col_type == ColumnType::INT && std::holds_alternative<float>(v)) {
        float f = std::get<float>(v);
        float rounded = std::round(f);
        if (rounded == f &&
            f >= static_cast<float>(std::numeric_limits<int32_t>::min()) &&
            f <= static_cast<float>(std::numeric_limits<int32_t>::max())) {
            return Value{static_cast<int32_t>(rounded)};
        }
        return std::nullopt;  // fractional/out-of-range float can never equal an INT column
    }
    // Already the matching type, or a combination evaluate_compare treats as
    // "never equal" anyway (e.g. VARCHAR vs INT) — an index lookup on such a
    // mismatched key correctly finds nothing either, so no coercion needed.
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

Executor::Executor(CatalogManager& catalog,
                   BufferPool&     buffer_pool,
                   WALManager&     wal,
                   FreeListManager& free_list)
    : catalog_(catalog), buffer_pool_(buffer_pool), wal_(wal), free_list_(free_list) {}

// ─────────────────────────────────────────────────────────────────────────────
// Index loading — see executor.h for why this exists
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::unique_ptr<Index>> Executor::open_indexes(const TableSchema& schema) const
{
    std::vector<std::unique_ptr<Index>> result;

    // get_indexes_for_table returns copies (see CatalogManager), so it's
    // only used here to discover *names*. The Index objects themselves are
    // built against catalog_.get_index(name), which returns a reference
    // into CatalogManager's own long-lived storage — the same stability
    // guarantee Table already relies on for its `schema` reference.
    for (const auto& summary : catalog_.get_indexes_for_table(schema.name)) {
        const IndexSchema& ischema = catalog_.get_index(summary.name);
        result.push_back(std::make_unique<Index>(
            ischema, schema.primary_key_indices.size(),
            buffer_pool_, wal_, free_list_));
    }
    return result;
}

std::vector<Index*> Executor::index_ptrs(const std::vector<std::unique_ptr<Index>>& owned)
{
    std::vector<Index*> ptrs;
    ptrs.reserve(owned.size());
    for (const auto& idx : owned) ptrs.push_back(idx.get());
    return ptrs;
}

void Executor::persist_index_roots(const Table& tbl) const
{
    for (const auto& [index_name, root_page] : tbl.index_root_pages()) {
        catalog_.update_index_root(index_name, root_page);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Index-assisted scans — see executor.h for why this exists
// ─────────────────────────────────────────────────────────────────────────────

void Executor::collect_and_constraints(const WhereExprPtr& expr,
                                        const TableSchema&  schema,
                                        const std::string&  alias,
                                        std::unordered_map<std::string, ColumnConstraint>& out,
                                        const OuterContext* outer) const
{
    if (!expr) return;

    if (expr->kind == WhereExpr::Kind::LOGICAL) {
        if (expr->logical_op == LogicalOp::AND) {
            collect_and_constraints(expr->left,  schema, alias, out, outer);
            collect_and_constraints(expr->right, schema, alias, out, outer);
        }
        // OR / NOT: neither side (or the negated subtree) is a condition
        // every matching row must satisfy, so nothing here is safe to use
        // as an index seed — contribute nothing.
        return;
    }

    // EXISTS carries no column-level equality/range of its own.
    if (expr->kind == WhereExpr::Kind::EXISTS) return;

    const CompareExpr& c = expr->compare;
    bool is_eq    = (c.op == CompareOp::EQ);
    bool is_range = (c.op == CompareOp::LT || c.op == CompareOp::GT ||
                      c.op == CompareOp::LTE || c.op == CompareOp::GTE);
    if (!is_eq && !is_range) return;  // only EQ/LT/GT/LTE/GTE narrow to a point or interval

    if (c.subquery)          return;  // (defensive — EQ/range never carry a subquery today)
    if (is_null(c.operand) && !c.operand_column) return;  // "col <op> NULL" never matches anything

    // Only a reference to THIS table's columns is usable — an unqualified
    // ref always is; a qualified one must match this table's name or alias.
    if (!c.column.table_name.empty() && !table_name_matches(c.column.table_name, schema, alias))
        return;

    Value seed_value;
    if (c.operand_column) {
        // column = column: normally unusable (neither side is a literal),
        // UNLESS the other side resolves against the OUTER row rather
        // than this table — see this function's header comment (G). Skip
        // if it resolves locally (an intra-table column comparison, e.g.
        // `a = b` on the same row, is never a literal seed regardless of
        // outer) or if it resolves nowhere at all.
        if (try_resolve_column(*c.operand_column, schema, alias)) return;

        if (!outer) return;
        std::optional<size_t> outer_idx;
        if (outer->right_schema) {
            outer_idx = try_resolve_column(*c.operand_column, *outer->schema, *outer->right_schema,
                                            outer->alias, outer->right_alias);
        } else {
            outer_idx = try_resolve_column(*c.operand_column, *outer->schema, outer->alias);
        }
        if (!outer_idx) return;  // doesn't resolve against the outer row either — unusable

        seed_value = outer->row->get(*outer_idx);
        if (is_null(seed_value)) return;  // "col <op> NULL" never matches anything
    } else {
        seed_value = c.operand;
    }

    ColumnConstraint& cc = out[c.column.column_name];
    if (is_eq) {
        cc.eq = seed_value;
        return;
    }
    switch (c.op) {
        case CompareOp::GT:  cc.lo = seed_value; cc.lo_inclusive = false; break;
        case CompareOp::GTE: cc.lo = seed_value; cc.lo_inclusive = true;  break;
        case CompareOp::LT:  cc.hi = seed_value; cc.hi_inclusive = false; break;
        case CompareOp::LTE: cc.hi = seed_value; cc.hi_inclusive = true;  break;
        default: break;  // unreachable — is_range already narrowed c.op to these four
    }
}

std::optional<Executor::IndexMatch> Executor::find_index_prefix(
    const WhereExprPtr& where,
    const TableSchema&  schema,
    const std::string&  alias,
    const OuterContext* outer) const
{
    if (!where) return std::nullopt;

    std::unordered_map<std::string, ColumnConstraint> constraints;
    collect_and_constraints(where, schema, alias, constraints, outer);
    if (constraints.empty()) return std::nullopt;

    const IndexSchema* best = nullptr;
    IndexMatch          best_match;
    size_t               best_effective_len = 0;  // prefix columns, +1 if extended by a range

    for (const auto& summary : catalog_.get_indexes_for_table(schema.name)) {
        Key   prefix;
        bool  extended_by_range = false;
        std::optional<Value> range_lo, range_hi;
        bool  range_lo_inclusive = false, range_hi_inclusive = false;

        for (const std::string& col_name : summary.column_names) {
            auto it = constraints.find(col_name);
            if (it == constraints.end()) break;  // leftmost prefix stops at the first uncovered column

            int col_idx = schema.column_index(col_name);
            if (col_idx < 0) break;  // shouldn't happen — defensive
            ColumnType col_type = schema.columns[static_cast<size_t>(col_idx)].type;

            if (it->second.eq.has_value()) {
                auto coerced = coerce_equality_value(*it->second.eq, col_type);
                if (!coerced) break;  // literal can never equal this column's type
                prefix.push_back(std::move(*coerced));
                continue;
            }

            // No equality on this column — if it has a range, that range
            // can extend the prefix by exactly one more column (the
            // leftmost-prefix rule: a range is only usable as the LAST
            // condition), then we're done regardless of what comes after.
            if (it->second.lo.has_value() || it->second.hi.has_value()) {
                if (it->second.lo.has_value()) {
                    range_lo = coerce_equality_value(*it->second.lo, col_type);
                    range_lo_inclusive = it->second.lo_inclusive;
                    if (!range_lo) { range_lo_inclusive = false; }
                }
                if (it->second.hi.has_value()) {
                    range_hi = coerce_equality_value(*it->second.hi, col_type);
                    range_hi_inclusive = it->second.hi_inclusive;
                    if (!range_hi) { range_hi_inclusive = false; }
                }
                if (range_lo.has_value() || range_hi.has_value()) extended_by_range = true;
            }
            break;  // whether or not a usable range was found, no further columns are usable
        }

        size_t effective_len = prefix.size() + (extended_by_range ? 1 : 0);
        if (effective_len == 0) continue;

        if (effective_len > best_effective_len) {
            best_effective_len = effective_len;
            best               = &catalog_.get_index(summary.name);  // stable reference, see open_indexes
            best_match.prefix  = std::move(prefix);
            best_match.range_lo = extended_by_range ? range_lo : std::nullopt;
            best_match.range_lo_inclusive = extended_by_range ? range_lo_inclusive : false;
            best_match.range_hi = extended_by_range ? range_hi : std::nullopt;
            best_match.range_hi_inclusive = extended_by_range ? range_hi_inclusive : false;
        }
    }

    if (!best) return std::nullopt;
    best_match.index = best;
    return best_match;
}

std::vector<Key> Executor::index_lookup(const Index& idx, const IndexMatch& match)
{
    if (match.is_range()) {
        return idx.find_range(match.prefix,
                               match.range_lo, match.range_lo_inclusive,
                               match.range_hi, match.range_hi_inclusive);
    }
    return idx.find(match.prefix);
}

std::vector<ScanResult> Executor::scan_with_index(const Table&         tbl,
                                                    const TableSchema&  schema,
                                                    const WhereExprPtr& where,
                                                    const Predicate&    pred,
                                                    const std::string&  alias,
                                                    const OuterContext* outer) const
{
    auto index_match = find_index_prefix(where, schema, alias, outer);
    if (!index_match) {
        return tbl.scan(pred);
    }

    const IndexSchema& ischema = *index_match->index;
    Index idx(ischema, schema.primary_key_indices.size(), buffer_pool_, wal_, free_list_);

    std::vector<ScanResult> results;
    for (const Key& pk : index_lookup(idx, *index_match)) {
        std::optional<Row> row = tbl.select_by_key(pk);
        // Every fetched row is still run through the full predicate — the
        // index only narrowed candidates by its equality prefix (and
        // possibly one trailing range column), but the WHERE clause may
        // have additional conditions beyond it.
        if (row && pred(*row)) {
            results.push_back({pk, std::move(*row)});
        }
    }
    return results;
}

bool Executor::where_is_coverable(const WhereExprPtr& expr,
                                   const TableSchema&  schema,
                                   const std::string&  alias,
                                   const std::unordered_set<std::string>& covered) const
{
    if (!expr) return true;

    // EXISTS and IN/NOT IN-subquery involve a different table's columns
    // (and their own correctness lives in run_subquery_scan) — bail
    // conservatively rather than try to prove those safe here too.
    if (expr->kind == WhereExpr::Kind::EXISTS) return false;

    if (expr->kind == WhereExpr::Kind::LOGICAL) {
        if (expr->logical_op == LogicalOp::NOT) {
            return where_is_coverable(expr->left, schema, alias, covered);
        }
        // AND and OR both require both sides coverable — even for OR,
        // evaluate_where still needs to read whichever columns either
        // side references to decide the row doesn't match.
        return where_is_coverable(expr->left,  schema, alias, covered) &&
               where_is_coverable(expr->right, schema, alias, covered);
    }

    const CompareExpr& c = expr->compare;
    if (c.subquery) return false;  // IN_SUBQUERY / NOT_IN_SUBQUERY — bail conservatively

    if (!c.column.table_name.empty() && !table_name_matches(c.column.table_name, schema, alias))
        return false;  // shouldn't normally happen for a single-table query — bail defensively
    if (!covered.count(c.column.column_name)) return false;

    if (c.operand_column) {
        // column = column: the other side must ALSO be covered (and be
        // this same table — a reference to an outer row in a correlated
        // subquery's own WHERE can't be answered from this table's index
        // tuple, though it's still a perfectly good index SEED elsewhere;
        // covering is a stricter question than seeding).
        if (!c.operand_column->table_name.empty() &&
            !table_name_matches(c.operand_column->table_name, schema, alias))
            return false;
        if (!covered.count(c.operand_column->column_name)) return false;
    }

    return true;
}

bool Executor::is_select_coverable(const std::vector<SelectColumn>&  columns,
                                    const WhereExprPtr&               where,
                                    const std::vector<OrderByClause>& order_by,
                                    const TableSchema&                schema,
                                    const std::string&                alias,
                                    const std::unordered_set<std::string>& covered) const
{
    auto ref_is_covered = [&](const ColumnRef& ref) {
        if (!ref.table_name.empty() && !table_name_matches(ref.table_name, schema, alias)) return false;
        return covered.count(ref.column_name) != 0;
    };

    for (const auto& sc : columns) {
        if (sc.is_star)                          return false;  // needs every column, not just indexed ones
        if (sc.is_literal)                        continue;      // reads no column at all
        if (sc.aggregate != AggregateType::NONE) return false;  // handled by execute_select_aggregate instead
        if (!ref_is_covered(sc.column)) return false;
    }
    for (const auto& ob : order_by) {
        if (!ref_is_covered(ob.column)) return false;
    }
    return where_is_coverable(where, schema, alias, covered);
}

std::optional<std::vector<ScanResult>> Executor::scan_covering(
    const TableSchema&                 schema,
    const WhereExprPtr&                where,
    const Predicate&                   pred,
    const std::vector<SelectColumn>&   columns,
    const std::vector<OrderByClause>&  order_by,
    const std::string&                 alias) const
{
    auto index_match = find_index_prefix(where, schema, alias);
    if (!index_match) return std::nullopt;

    const IndexSchema& ischema = *index_match->index;

    std::unordered_set<std::string> covered(ischema.column_names.begin(), ischema.column_names.end());
    for (uint32_t pk_idx : schema.primary_key_indices) {
        covered.insert(schema.columns[pk_idx].name);
    }

    if (!is_select_coverable(columns, where, order_by, schema, alias, covered)) {
        return std::nullopt;
    }

    Index idx(ischema, schema.primary_key_indices.size(), buffer_pool_, wal_, free_list_);

    std::vector<std::pair<Key, Key>> entries;
    if (index_match->is_range()) {
        entries = idx.find_range_with_values(index_match->prefix,
                                              index_match->range_lo, index_match->range_lo_inclusive,
                                              index_match->range_hi, index_match->range_hi_inclusive);
    } else {
        entries = idx.find_with_values(index_match->prefix);
    }

    std::vector<ScanResult> results;
    results.reserve(entries.size());
    for (auto& [index_key, pk] : entries) {
        // Every OTHER column stays default-constructed (monostate/NULL) —
        // is_select_coverable() already proved this query's WHERE/SELECT/
        // ORDER BY never reads them, so leaving them unpopulated is safe
        // for this query specifically, even though the resulting Row
        // would NOT be a valid full row for any other purpose.
        Row row;
        row.values.resize(schema.columns.size());
        for (size_t i = 0; i < ischema.column_names.size(); i++) {
            int col_idx = schema.column_index(ischema.column_names[i]);
            if (col_idx >= 0) row.values[static_cast<size_t>(col_idx)] = index_key[i];
        }
        for (size_t i = 0; i < schema.primary_key_indices.size(); i++) {
            row.values[schema.primary_key_indices[i]] = pk[i];
        }

        if (pred(row)) {
            results.push_back({pk, std::move(row)});
        }
    }
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// execute() — top-level dispatch, never throws
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute(const Statement& stmt)
{
    try {
        return std::visit([this](const auto& s) -> QueryResult {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, SelectStmt>)
                return execute_select(s);
            if constexpr (std::is_same_v<T, InsertStmt>)
                return execute_insert(s);
            if constexpr (std::is_same_v<T, UpdateStmt>)
                return execute_update(s);
            if constexpr (std::is_same_v<T, DeleteStmt>)
                return execute_delete(s);
            if constexpr (std::is_same_v<T, CreateTableStmt>)
                return execute_create_table(s);
            if constexpr (std::is_same_v<T, DropTableStmt>)
                return execute_drop_table(s);
            if constexpr (std::is_same_v<T, CreateIndexStmt>)
                return execute_create_index(s);
            if constexpr (std::is_same_v<T, DropIndexStmt>)
                return execute_drop_index(s);
            if constexpr (std::is_same_v<T, ShowStmt>)
                return execute_show(s);
            if constexpr (std::is_same_v<T, CreateDatabaseStmt>)
                return execute_create_database(s);
            if constexpr (std::is_same_v<T, DropDatabaseStmt>)
                return execute_drop_database(s);
            if constexpr (std::is_same_v<T, UseStmt>)
                return execute_use(s);
            return QueryResult{false, "Unknown statement type"};
        }, stmt);
    } catch (const std::exception& e) {
        return QueryResult{false, e.what()};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CREATE TABLE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_create_table(const CreateTableStmt& stmt)
{
    if (catalog_.table_exists(stmt.table_name)) {
        return {false, "Table '" + stmt.table_name + "' already exists"};
    }

    // Build the TableSchema from the AST column definitions
    TableSchema schema;
    schema.name             = stmt.table_name;
    schema.root_page        = INVALID_PAGE;  // BTree constructor will allocate

    int pk_count = 0;
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        Column col = column_def_to_column(stmt.columns[i]);  // throws on bad type
        if (col.is_primary_key) {
            pk_count++;
            schema.primary_key_indices.push_back(static_cast<uint32_t>(i));
        }
        schema.columns.push_back(std::move(col));
    }

    if (pk_count > 1) {
        return {false, "CREATE TABLE '" + stmt.table_name +
                       "': only one inline PRIMARY KEY column is allowed "
                       "(use a table-level PRIMARY KEY (col1, col2, ...) clause for a composite key)"};
    }

    // Table-level PRIMARY KEY (a, b, ...) clause — mutually exclusive with
    // an inline column-level PRIMARY KEY.
    if (!stmt.table_primary_key.empty()) {
        if (pk_count > 0) {
            return {false, "CREATE TABLE '" + stmt.table_name +
                           "': cannot combine an inline PRIMARY KEY column with a "
                           "table-level PRIMARY KEY (...) clause"};
        }
        for (const std::string& col_name : stmt.table_primary_key) {
            int idx = schema.column_index(col_name);
            if (idx < 0) {
                return {false, "CREATE TABLE '" + stmt.table_name +
                               "': PRIMARY KEY column '" + col_name + "' does not exist"};
            }
            schema.primary_key_indices.push_back(static_cast<uint32_t>(idx));
            schema.columns[static_cast<size_t>(idx)].is_primary_key = true;
            schema.columns[static_cast<size_t>(idx)].is_nullable    = false;
        }
        pk_count = static_cast<int>(stmt.table_primary_key.size());
    }

    if (pk_count == 0) {
        return {false, "CREATE TABLE '" + stmt.table_name +
                       "': no PRIMARY KEY column defined"};
    }

    // ── UNIQUE constraints (column-level `col TYPE UNIQUE` and table-level
    //    `UNIQUE (col1, col2, ...)`) are both backed by an auto-named
    //    unique secondary Index — the exact same machinery CREATE UNIQUE
    //    INDEX already uses (see execute_create_index below). Collect every
    //    such constraint here as a column-name group; validated and
    //    deduped now, actually built into Index objects further down once
    //    the table itself exists.
    std::vector<std::vector<std::string>> unique_groups;

    for (const auto& col_def : stmt.columns) {
        if (col_def.is_unique) {
            unique_groups.push_back({col_def.name});
        }
    }
    for (const auto& group : stmt.table_unique) {
        if (group.empty()) {
            return {false, "CREATE TABLE '" + stmt.table_name +
                           "': UNIQUE (...) clause names no columns"};
        }
        for (const std::string& col_name : group) {
            if (schema.column_index(col_name) < 0) {
                return {false, "CREATE TABLE '" + stmt.table_name +
                               "': UNIQUE column '" + col_name + "' does not exist"};
            }
        }
        unique_groups.push_back(group);
    }

    // Dedupe against the primary key (the primary B+ tree already enforces
    // that uniqueness — a second index over the exact same columns would
    // be pure waste) and against each other (the same column set declared
    // unique twice, e.g. once inline and once via a table-level clause).
    auto same_columns = [](std::vector<std::string> a, std::vector<std::string> b) {
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        return a == b;
    };
    std::vector<std::string> pk_names;
    for (uint32_t idx : schema.primary_key_indices) {
        pk_names.push_back(schema.columns[idx].name);
    }

    std::vector<std::vector<std::string>> unique_groups_to_build;
    for (auto& group : unique_groups) {
        if (same_columns(group, pk_names)) continue;
        bool dup = false;
        for (const auto& existing : unique_groups_to_build) {
            if (same_columns(group, existing)) { dup = true; break; }
        }
        if (!dup) unique_groups_to_build.push_back(std::move(group));
    }

    // ── FOREIGN KEY constraints (column-level `col TYPE REFERENCES
    //    parent(col)` shorthand and table-level `FOREIGN KEY (...)
    //    REFERENCES ... [ON DELETE CASCADE|RESTRICT]`) are validated here
    //    and folded into schema.foreign_keys. The child-side lookup Index
    //    is actually built further down, once the table exists — same
    //    two-step pattern the UNIQUE constraints above use.
    //
    // SCOPE / KNOWN LIMITATIONS (documented rather than silently wrong):
    //   - The parent table must already exist. No forward references, and
    //     no self-referencing FK — both would need this table's own (not
    //     yet created) schema to validate against itself.
    //   - REFERENCES columns must match, in the SAME declared order, either
    //     the parent's primary key or an existing UNIQUE constraint on the
    //     parent — this is what lets every parent-side lookup be a single
    //     indexed find() with no reordering logic. A same-columns-
    //     different-order REFERENCES is rejected with a clear error rather
    //     than silently reordered.
    //   - ON UPDATE has no CASCADE of its own: an UPDATE that changes a
    //     referenced parent column is always treated as RESTRICT regardless
    //     of the FK's ON DELETE action — see Executor::execute_update.
    std::vector<ForeignKeyDef> fk_defs = stmt.foreign_keys;
    for (const auto& col_def : stmt.columns) {
        if (!col_def.fk_ref_table.empty()) {
            ForeignKeyDef fk;
            fk.columns     = {col_def.name};
            fk.ref_table   = col_def.fk_ref_table;
            fk.ref_columns = {col_def.fk_ref_column};
            fk.on_delete   = ForeignKeyOnDelete::RESTRICT;
            fk_defs.push_back(std::move(fk));
        }
    }

    for (const auto& fk : fk_defs) {
        if (fk.ref_table == stmt.table_name) {
            return {false, "CREATE TABLE '" + stmt.table_name +
                           "': self-referencing FOREIGN KEY constraints are not yet supported"};
        }
        if (!catalog_.table_exists(fk.ref_table)) {
            return {false, "CREATE TABLE '" + stmt.table_name +
                           "': FOREIGN KEY references unknown table '" + fk.ref_table + "'"};
        }
        if (fk.columns.empty() || fk.columns.size() != fk.ref_columns.size()) {
            return {false, "CREATE TABLE '" + stmt.table_name +
                           "': FOREIGN KEY column count does not match REFERENCES column count"};
        }

        const TableSchema& ref_schema = catalog_.get_table(fk.ref_table);

        std::vector<uint32_t> col_indices, ref_col_indices;
        for (size_t i = 0; i < fk.columns.size(); ++i) {
            int idx = schema.column_index(fk.columns[i]);
            if (idx < 0) {
                return {false, "CREATE TABLE '" + stmt.table_name +
                               "': FOREIGN KEY column '" + fk.columns[i] + "' does not exist"};
            }
            int ref_idx = ref_schema.column_index(fk.ref_columns[i]);
            if (ref_idx < 0) {
                return {false, "CREATE TABLE '" + stmt.table_name +
                               "': REFERENCES column '" + fk.ref_columns[i] +
                               "' does not exist on '" + fk.ref_table + "'"};
            }
            if (schema.columns[static_cast<size_t>(idx)].type !=
                ref_schema.columns[static_cast<size_t>(ref_idx)].type) {
                return {false, "CREATE TABLE '" + stmt.table_name +
                               "': FOREIGN KEY column '" + fk.columns[i] +
                               "' type does not match referenced column '" +
                               fk.ref_columns[i] + "'"};
            }
            col_indices.push_back(static_cast<uint32_t>(idx));
            ref_col_indices.push_back(static_cast<uint32_t>(ref_idx));
        }

        // ref_columns must match, in this exact order, the parent's primary
        // key or an existing UNIQUE constraint — see the scope note above.
        // Whichever it is gets resolved to a concrete lookup target now
        // (ref_index_name empty = primary key; otherwise the matching
        // UNIQUE index's name), so runtime enforcement never re-searches.
        std::vector<std::string> ref_pk_names;
        for (uint32_t idx : ref_schema.primary_key_indices) {
            ref_pk_names.push_back(ref_schema.columns[idx].name);
        }
        bool matches_a_key = (fk.ref_columns == ref_pk_names);
        std::string ref_index_name;  // "" means "the primary key"
        if (!matches_a_key) {
            for (const auto& ref_idx_schema : catalog_.get_indexes_for_table(fk.ref_table)) {
                if (ref_idx_schema.is_unique && ref_idx_schema.column_names == fk.ref_columns) {
                    matches_a_key   = true;
                    ref_index_name  = ref_idx_schema.name;
                    break;
                }
            }
        }
        if (!matches_a_key) {
            return {false, "CREATE TABLE '" + stmt.table_name +
                           "': REFERENCES columns on '" + fk.ref_table +
                           "' must be its primary key or an existing UNIQUE constraint, "
                           "in the same declared order"};
        }

        ForeignKeyConstraint constraint;
        constraint.column_indices     = col_indices;
        constraint.ref_table          = fk.ref_table;
        constraint.ref_column_indices = ref_col_indices;
        constraint.on_delete          = static_cast<FkOnDelete>(static_cast<uint8_t>(fk.on_delete));
        constraint.ref_index_name     = ref_index_name;
        constraint.child_index_name   = "__fk_" + stmt.table_name;
        for (const std::string& col_name : fk.columns) {
            constraint.child_index_name += "_" + col_name;
        }

        schema.foreign_keys.push_back(std::move(constraint));
    }

    // Compile CHECK expressions (column-level, then table-level) into the
    // catalog's flat CheckConstraint list. Any unsupported shape (OR, NOT,
    // unknown column, etc.) aborts CREATE TABLE with a clear error instead
    // of silently dropping the constraint.
    try {
        for (const auto& col_def : stmt.columns) {
            if (col_def.check) {
                flatten_check_expr(col_def.check, schema, schema.checks);
            }
        }
        for (const auto& table_check : stmt.table_checks) {
            flatten_check_expr(table_check, schema, schema.checks);
        }
    } catch (const std::exception& e) {
        return {false, "CREATE TABLE '" + stmt.table_name + "': " + e.what()};
    }

    // Register in catalog (root_page = INVALID_PAGE at this point)
    catalog_.create_table(schema);

    // Constructing Table triggers BTree construction which allocates the
    // root leaf page via WAL when root_page == INVALID_PAGE.
    Table tbl(catalog_.get_table(stmt.table_name), buffer_pool_, wal_, free_list_);

    // Persist the newly allocated root page back into the catalog.
    catalog_.update_table_root(stmt.table_name, tbl.root_page());

    // Back every UNIQUE constraint collected above with its own empty
    // unique Index. No backfill scan is needed here (unlike CREATE INDEX
    // on a pre-existing table) since this table has zero rows right now —
    // that's what makes this simpler than execute_create_index, not harder.
    // The generated name is namespaced with a double-underscore prefix a
    // user-chosen CREATE INDEX name can't collide with, plus the (already
    // known-unique) table name, so no additional collision check is needed.
    for (const auto& group : unique_groups_to_build) {
        IndexSchema index_schema;
        index_schema.name       = "__unique_" + stmt.table_name;
        for (const std::string& col_name : group) {
            index_schema.name += "_" + col_name;
        }
        index_schema.table_name   = stmt.table_name;
        index_schema.column_names = group;
        index_schema.root_page    = INVALID_PAGE;
        index_schema.is_unique    = true;

        catalog_.create_index(index_schema);

        Index idx(catalog_.get_index(index_schema.name), schema.primary_key_indices.size(),
                   buffer_pool_, wal_, free_list_);
        catalog_.update_index_root(index_schema.name, idx.root_page());
    }

    // Back every FOREIGN KEY constraint with a plain (non-unique) child-side
    // Index over its column(s) — see ForeignKeyConstraint::child_index_name
    // in schema.h for why: it's what makes "find every child row
    // referencing this parent key" (needed for DELETE/UPDATE on the parent
    // side — see execute_delete/execute_update) an indexed lookup instead
    // of a full scan of every table that might reference this one. Same
    // empty-table, no-backfill-needed reasoning as the UNIQUE loop above.
    for (const auto& fk : schema.foreign_keys) {
        IndexSchema index_schema;
        index_schema.name = fk.child_index_name;
        for (uint32_t col_idx : fk.column_indices) {
            index_schema.column_names.push_back(schema.columns[col_idx].name);
        }
        index_schema.table_name = stmt.table_name;
        index_schema.root_page  = INVALID_PAGE;
        index_schema.is_unique  = false;  // many child rows may share one parent key

        catalog_.create_index(index_schema);

        Index idx(catalog_.get_index(index_schema.name), schema.primary_key_indices.size(),
                   buffer_pool_, wal_, free_list_);
        catalog_.update_index_root(index_schema.name, idx.root_page());
    }

    return {true, "", {}, {}, 0};
}

// Converts an AST ColumnDef into a catalog Column.
Column Executor::column_def_to_column(const ColumnDef& def) const
{
    Column col;
    col.name           = def.name;
    col.is_nullable    = def.is_nullable;
    col.is_primary_key = def.is_primary_key;
    col.auto_increment = def.auto_increment;
    col.max_length     = 0;
    col.has_default    = def.has_default;
    col.default_value  = def.default_value;

    if (def.type_name == "INT") {
        col.type = ColumnType::INT;
    } else if (def.type_name == "FLOAT") {
        col.type = ColumnType::FLOAT;
    } else if (def.type_name == "BOOLEAN") {
        col.type = ColumnType::BOOLEAN;
    } else if (def.type_name == "VARCHAR") {
        col.type       = ColumnType::VARCHAR;
        col.max_length = def.varchar_len;
        if (col.max_length == 0) {
            throw std::runtime_error("VARCHAR length must be greater than 0");
        }
    } else {
        throw std::runtime_error("Unknown column type: '" + def.type_name + "'");
    }

    if (def.auto_increment && col.type != ColumnType::INT) {
        throw std::runtime_error(
            "AUTO_INCREMENT is only valid on INT columns (column '" + def.name + "')");
    }

    if (def.auto_increment && col.has_default) {
        throw std::runtime_error(
            "AUTO_INCREMENT and DEFAULT cannot both be specified on column '" + def.name + "'");
    }

    if (col.has_default) {
        if (is_null(col.default_value)) {
            if (!col.is_nullable) {
                throw std::runtime_error(
                    "DEFAULT NULL is not valid on NOT NULL column '" + def.name + "'");
            }
        } else if (!value_matches_type(col.default_value, col.type)) {
            throw std::runtime_error(
                "DEFAULT value type does not match column '" + def.name +
                "' (" + column_type_name(col.type) + ")");
        } else if (col.type == ColumnType::VARCHAR &&
                   get_string(col.default_value).size() > col.max_length) {
            throw std::runtime_error(
                "DEFAULT value exceeds max_length for column '" + def.name + "'");
        }
    }

    return col;
}

// ─────────────────────────────────────────────────────────────────────────────
// DROP TABLE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_drop_table(const DropTableStmt& stmt)
{
    if (!catalog_.table_exists(stmt.table_name)) {
        return {false, "Table '" + stmt.table_name + "' does not exist"};
    }

    // RESTRICT-only, no CASCADE-drop: if some other table's FOREIGN KEY
    // still references this one, refuse rather than silently leaving that
    // constraint dangling (CatalogManager::drop_table only cleans up THIS
    // table's own indexes/schema, not other tables' FK metadata pointing
    // at it). The other table must drop its FK (or be dropped itself)
    // first.
    auto referencing = catalog_.get_foreign_keys_referencing(stmt.table_name);
    if (!referencing.empty()) {
        return {false, "DROP TABLE '" + stmt.table_name +
                       "': still referenced by a FOREIGN KEY on table '" +
                       referencing[0].first + "'"};
    }

    catalog_.drop_table(stmt.table_name);
    return {true, "", {}, {}, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// DROP INDEX
// ─────────────────────────────────────────────────────────────────────────────

// Mirrors execute_drop_table exactly: removes catalog metadata only. The
// index's B+ tree pages are left orphaned rather than reclaimed via the
// free list — same pre-existing gap DROP TABLE already has (see
// CatalogManager::drop_table), not something new introduced here.
QueryResult Executor::execute_drop_index(const DropIndexStmt& stmt)
{
    try {
        catalog_.get_index(stmt.index_name);
    } catch (const std::exception&) {
        return {false, "Index '" + stmt.index_name + "' does not exist"};
    }
    catalog_.drop_index(stmt.index_name);
    return {true, "", {}, {}, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// CREATE INDEX
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_create_index(const CreateIndexStmt& stmt)
{
    if (!catalog_.table_exists(stmt.table_name)) {
        return {false, "CREATE INDEX: table '" + stmt.table_name + "' does not exist"};
    }
    const TableSchema& schema = catalog_.get_table(stmt.table_name);

    if (stmt.column_names.empty()) {
        return {false, "CREATE INDEX '" + stmt.index_name + "': no columns specified"};
    }

    // Resolve column names to indices up front — this is also what lets us
    // read each existing row's indexed value below without re-resolving
    // per row.
    std::vector<int> col_indices;
    col_indices.reserve(stmt.column_names.size());
    for (const std::string& col_name : stmt.column_names) {
        int idx = schema.column_index(col_name);
        if (idx < 0) {
            return {false, "CREATE INDEX '" + stmt.index_name + "': column '" +
                           col_name + "' does not exist on table '" + stmt.table_name + "'"};
        }
        col_indices.push_back(idx);
    }

    // ── Scan the existing table and collect (indexed_value, primary_key)
    //    pairs for every row — this is the "backfill on an already-built
    //    table" step. Rows with a NULL in any indexed column are skipped
    //    entirely, matching Index::is_indexable semantics (NULLs are never
    //    indexed and never subject to uniqueness).
    Table no_index_tbl(schema, buffer_pool_, wal_, free_list_);  // plain scan, no indexes attached
    std::vector<std::pair<Key, Key>> entries;  // {indexed_value, primary_key}

    for (const auto& match : no_index_tbl.scan([](const Row&) { return true; })) {
        Key indexed_value;
        indexed_value.reserve(col_indices.size());
        bool has_null = false;
        for (int idx : col_indices) {
            const Value& v = match.row.values[static_cast<size_t>(idx)];
            if (std::holds_alternative<std::monostate>(v)) { has_null = true; break; }
            indexed_value.push_back(v);
        }
        if (has_null) continue;

        Key primary_key;
        primary_key.reserve(schema.primary_key_indices.size());
        for (uint32_t pk_idx : schema.primary_key_indices) {
            primary_key.push_back(match.row.values[pk_idx]);
        }
        entries.emplace_back(std::move(indexed_value), std::move(primary_key));
    }

    // ── Pre-validate UNIQUE across the existing data BEFORE anything is
    //    written — Index/BTree writes can't be rolled back once made (see
    //    Index::check_unique's comment), so a violation must be caught here,
    //    while the catalog and B+ tree for this index don't exist yet.
    if (stmt.is_unique) {
        std::set<Key> seen_values;
        for (const auto& [indexed_value, primary_key] : entries) {
            (void)primary_key;
            if (!seen_values.insert(indexed_value).second) {
                return {false, "CREATE INDEX '" + stmt.index_name +
                               "': cannot create UNIQUE index — table '" + stmt.table_name +
                               "' already has duplicate values for the indexed column(s)"};
            }
        }
    }

    // ── Register the index (root_page = INVALID_PAGE for now) and allocate
    //    its empty B+ tree — same two-step pattern execute_create_table uses
    //    for the table's own root page.
    IndexSchema index_schema;
    index_schema.name         = stmt.index_name;
    index_schema.table_name   = stmt.table_name;
    index_schema.column_names = stmt.column_names;
    index_schema.root_page    = INVALID_PAGE;
    index_schema.is_unique    = stmt.is_unique;

    catalog_.create_index(index_schema);  // throws (caught by execute()) if name is taken

    Index idx(catalog_.get_index(stmt.index_name), schema.primary_key_indices.size(),
               buffer_pool_, wal_, free_list_);

    // ── Backfill every collected entry in one WAL transaction, rather than
    //    one commit per row — the difference between one transaction and N
    //    of them for a large table.
    if (!entries.empty()) {
        uint32_t transaction_id = wal_.begin();
        for (const auto& [indexed_value, primary_key] : entries) {
            idx.insert_entry(transaction_id, indexed_value, primary_key);
        }
        wal_.commit(transaction_id);
    }

    catalog_.update_index_root(stmt.index_name, idx.root_page());

    return {true, "", {}, {}, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// INSERT
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_insert(const InsertStmt& stmt)
{
    const TableSchema& schema = catalog_.get_table(stmt.table_name);

    // Build the row — values ordered to match schema column positions.
    // Unspecified columns default to NULL (std::monostate), unless the
    // column has a DEFAULT clause — resolved below via `provided`.
    Row row;
    row.values.resize(schema.columns.size(), std::monostate{});

    // Tracks which columns were actually given a value by this statement.
    // false at index i means "fall back to schema default, or NULL if the
    // column has none" — either because the column was omitted from a
    // named column list, or because its VALUES slot was the literal
    // keyword DEFAULT.
    std::vector<bool> provided(schema.columns.size(), false);

    if (stmt.columns.empty()) {
        // INSERT INTO t VALUES (v1, v2, ...)  — positional, schema order
        if (stmt.values.size() != schema.columns.size()) {
            return {false,
                    "INSERT: value count (" + std::to_string(stmt.values.size()) +
                    ") does not match column count (" +
                    std::to_string(schema.columns.size()) + ")"};
        }
        for (size_t i = 0; i < stmt.values.size(); ++i) {
            bool is_default = i < stmt.value_is_default.size() && stmt.value_is_default[i];
            if (is_default) continue;  // provided[i] stays false — use the schema default
            row.values[i] = stmt.values[i];
            provided[i]   = true;
        }
    } else {
        // INSERT INTO t (col1, col2) VALUES (v1, v2)  — named columns
        if (stmt.columns.size() != stmt.values.size()) {
            return {false, "INSERT: column list and value list have different lengths"};
        }
        for (size_t i = 0; i < stmt.columns.size(); ++i) {
            int idx = schema.column_index(stmt.columns[i]);
            if (idx < 0) {
                return {false, "INSERT: unknown column '" + stmt.columns[i] + "'"};
            }
            bool is_default = i < stmt.value_is_default.size() && stmt.value_is_default[i];
            if (is_default) continue;  // provided[idx] stays false — use the schema default
            row.values[static_cast<size_t>(idx)] = stmt.values[i];
            provided[static_cast<size_t>(idx)]   = true;
        }
    }

    // Resolve every unprovided column against its DEFAULT clause, if any.
    // Columns with no DEFAULT keep the NULL they were pre-filled with above.
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (!provided[i] && schema.columns[i].has_default) {
            row.values[i] = schema.columns[i].default_value;
        }
    }

    int violated = find_violated_check(schema, row);
    if (violated >= 0) {
        const CheckConstraint& c = schema.checks[static_cast<size_t>(violated)];
        return {false, "INSERT: CHECK constraint violated on column '" +
                       schema.columns[c.column_index].name + "' (" +
                       schema.columns[c.column_index].name + " " +
                       check_op_symbol(c.op) + " " + value_to_string(c.operand) + ")"};
    }

    std::string fk_error = check_foreign_keys_on_write(schema, row);
    if (!fk_error.empty()) {
        return {false, "INSERT: " + fk_error};
    }

    auto  owned_indexes = open_indexes(schema);
    Table tbl(schema, buffer_pool_, wal_, free_list_, index_ptrs(owned_indexes));
    tbl.insert(row);  // throws on NOT NULL / duplicate PK / type mismatch / UNIQUE index violation

    // B+ tree root may have changed after a root split — keep catalog in sync
    if (tbl.root_page() != schema.root_page) {
        catalog_.update_table_root(stmt.table_name, tbl.root_page());
    }
    persist_index_roots(tbl);

    return {true, "", {}, {}, 1};
}

// ─────────────────────────────────────────────────────────────────────────────
// SELECT
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_select(const SelectStmt& stmt)
{
    const TableSchema& schema = catalog_.get_table(stmt.table_name);
    Table left_tbl(schema, buffer_pool_, wal_, free_list_);

    // ── Aggregate (COUNT) queries ───────────────────────────────────────────
    // No GROUP BY yet, so the only meaningful case is: every column in the
    // SELECT list is an aggregate (mixing aggregate + plain columns without
    // GROUP BY is what real SQL engines reject too — GROUP BY is exactly
    // what makes that combination well-defined).
    bool any_aggregate = false;
    bool all_aggregate  = true;
    for (const auto& sc : stmt.columns) {
        if (sc.aggregate != AggregateType::NONE) any_aggregate = true;
        else                                      all_aggregate = false;
    }
    if (any_aggregate) {
        if (!all_aggregate) {
            return {false, "SELECT: cannot mix aggregate functions with plain "
                           "columns without GROUP BY (not yet supported)"};
        }
        if (!stmt.joins.empty()) {
            return {false, "SELECT: aggregate functions are not yet supported with JOIN"};
        }
        return execute_select_aggregate(stmt, schema, left_tbl);
    }

    QueryResult result;
    result.success = true;

    // ── Helper: sort a vector of rows by a column value ───────────────────────
    auto sort_by_column = [](auto& vec,
                              size_t col_idx,
                              bool   ascending,
                              auto   get_row)
    {
        std::stable_sort(vec.begin(), vec.end(),
            [&](const auto& a, const auto& b) {
                const Value& va = get_row(a).get(col_idx);
                const Value& vb = get_row(b).get(col_idx);

                // NULLs always sort last, regardless of direction
                bool a_null = is_null(va);
                bool b_null = is_null(vb);
                if (a_null && b_null) return false;
                if (a_null)           return false; // a (NULL) goes after b
                if (b_null)           return true;  // b (NULL) goes after a

                bool less_than = std::visit(
                    [](const auto& x, const auto& y) -> bool {
                        using X = std::decay_t<decltype(x)>;
                        using Y = std::decay_t<decltype(y)>;
                        // Same-type comparison
                        if constexpr (std::is_same_v<X, Y> &&
                                      !std::is_same_v<X, std::monostate>) {
                            return x < y;
                        }
                        // Cross int/float comparison
                        if constexpr ((std::is_same_v<X, int32_t> ||
                                       std::is_same_v<X, float>) &&
                                      (std::is_same_v<Y, int32_t> ||
                                       std::is_same_v<Y, float>)) {
                            return static_cast<float>(x) < static_cast<float>(y);
                        }
                        return false;
                    }, va, vb);

                return ascending ? less_than : !less_than;
            });
    };

    if (stmt.joins.empty()) {
        // ── Simple (no JOIN) path ─────────────────────────────────────────────
        Predicate pred = build_predicate(stmt.where, schema, nullptr, nullptr, stmt.table_alias);

        // scan_covering() (H) answers the query straight from an index's
        // own tuple + primary key when every column this query touches is
        // covered by it — skipping the Table::select_by_key() fetch
        // scan_with_index() would otherwise do per candidate. Falls back
        // to scan_with_index() whenever it isn't applicable (no matching
        // index, SELECT *, or any WHERE/SELECT/ORDER BY column outside
        // what the index covers) — see scan_covering's own comment.
        std::vector<ScanResult> rows;
        if (auto covering = scan_covering(schema, stmt.where, pred, stmt.columns, stmt.order_by,
                                           stmt.table_alias)) {
            rows = std::move(*covering);
        } else {
            rows = scan_with_index(left_tbl, schema, stmt.where, pred, stmt.table_alias);
        }

        // ORDER BY (only first clause honoured — no secondary sort key)
        if (!stmt.order_by.empty()) {
            const OrderByClause& ob = stmt.order_by[0];
            size_t col_idx = resolve_column(ob.column, schema, stmt.table_alias);
            sort_by_column(rows, col_idx, ob.ascending,
                           [](const ScanResult& sr) -> const Row& { return sr.row; });
        }

        // LIMIT
        if (stmt.limit.has_value()) {
            size_t lim = static_cast<size_t>(stmt.limit.value());
            if (rows.size() > lim) rows.resize(lim);
        }

        result.columns = build_column_headers(stmt.columns, schema);
        for (const auto& sr : rows) {
            result.rows.push_back(project_row(stmt.columns, sr.row, schema, nullptr, stmt.table_alias));
        }

    } else {
        // ── JOIN path — one join clause, nested-loop ──────────────────────────
        const JoinClause&  join         = stmt.joins[0];
        const TableSchema& right_schema = catalog_.get_table(join.table_name);
        Table              right_tbl(right_schema, buffer_pool_, wal_, free_list_);
        const std::string& right_alias  = join.alias;  // "" if no alias given

        // Scan the left table completely first — WHERE is applied after joining
        Predicate all_rows = [](const Row&) { return true; };
        std::vector<ScanResult> left_results = left_tbl.scan(all_rows);

        std::vector<JoinedRow> joined =
            nested_loop_join(left_results, schema, right_tbl, right_schema, join,
                              stmt.table_alias, right_alias);

        // WHERE — two-schema resolve handles qualified right-table references.
        // Precompute any subqueries once, before the loop — same reasoning
        // as build_predicate() for the non-JOIN path.
        SubqueryCache where_cache;
        if (stmt.where) precompute_subqueries(stmt.where, where_cache);

        std::vector<JoinedRow> filtered;
        filtered.reserve(joined.size());
        for (auto& jr : joined) {
            if (!stmt.where ||
                evaluate_where(stmt.where, jr.row, schema, &right_schema, &where_cache,
                                nullptr, stmt.table_alias, right_alias)) {
                filtered.push_back(std::move(jr));
            }
        }

        // ORDER BY — two-schema resolve
        if (!stmt.order_by.empty()) {
            const OrderByClause& ob = stmt.order_by[0];
            size_t col_idx = resolve_column(ob.column, schema, right_schema,
                                             stmt.table_alias, right_alias);
            sort_by_column(filtered, col_idx, ob.ascending,
                           [](const JoinedRow& jr) -> const Row& { return jr.row; });
        }

        // LIMIT
        if (stmt.limit.has_value()) {
            size_t lim = static_cast<size_t>(stmt.limit.value());
            if (filtered.size() > lim) filtered.resize(lim);
        }

        result.columns = build_column_headers(stmt.columns, schema, &right_schema);
        for (const auto& jr : filtered) {
            result.rows.push_back(
                project_row(stmt.columns, jr.row, schema, &right_schema,
                            stmt.table_alias, right_alias));
        }
    }

    return result;
}

QueryResult Executor::execute_select_aggregate(const SelectStmt&  stmt,
                                                const TableSchema& schema,
                                                Table&              tbl) const
{
    Predicate pred = build_predicate(stmt.where, schema, nullptr, nullptr, stmt.table_alias);
    std::vector<ScanResult> rows = scan_with_index(tbl, schema, stmt.where, pred, stmt.table_alias);

    QueryResult result;
    result.success = true;

    std::vector<std::string> row_out;
    row_out.reserve(stmt.columns.size());

    for (const auto& sc : stmt.columns) {
        if (sc.aggregate == AggregateType::COUNT_STAR) {
            result.columns.push_back("COUNT(*)");
            row_out.push_back(std::to_string(rows.size()));
        } else {
            // COUNT(column) — count only non-NULL values at this column
            size_t   idx   = resolve_column(sc.column, schema, stmt.table_alias);  // throws on unknown column
            uint32_t count = 0;
            for (const auto& sr : rows) {
                if (!is_null(sr.row.get(idx))) count++;
            }
            result.columns.push_back("COUNT(" + sc.column.column_name + ")");
            row_out.push_back(std::to_string(count));
        }
    }

    result.rows.push_back(std::move(row_out));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// nested_loop_join
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Executor::JoinedRow> Executor::nested_loop_join(
    const std::vector<ScanResult>& left_results,
    const TableSchema&             left_schema,
    Table&                         right_table,
    const TableSchema&             right_schema,
    const JoinClause&              join,
    const std::string&             left_alias,
    const std::string&             right_alias) const
{
    // Resolve the ON clause columns against their respective schemas.
    // join.left_col  — column in the left table
    // join.right_col — column in the right table
    size_t left_col_idx  = resolve_column(join.left_col,  left_schema,  left_alias);
    size_t right_col_idx = resolve_column(join.right_col, right_schema, right_alias);

    // Materialise the entire right side once (no predicate)
    Predicate all_rows = [](const Row&) { return true; };
    std::vector<ScanResult> right_results = right_table.scan(all_rows);

    const size_t left_width  = left_schema.columns.size();
    const size_t right_width = right_schema.columns.size();

    std::vector<JoinedRow> out;

    if (join.type == JoinType::RIGHT) {
        // ── RIGHT JOIN — outer loop: right, inner loop: left ──────────────────
        // Unmatched right rows are kept; the left side is NULL-filled.
        for (const auto& rr : right_results) {
            bool matched = false;
            for (const auto& lr : left_results) {
                if (lr.row.get(left_col_idx) == rr.row.get(right_col_idx)) {
                    Row combined;
                    combined.values.reserve(left_width + right_width);
                    combined.values.insert(combined.values.end(),
                                           lr.row.values.begin(), lr.row.values.end());
                    combined.values.insert(combined.values.end(),
                                           rr.row.values.begin(), rr.row.values.end());
                    out.push_back({std::move(combined), lr.primary_key});
                    matched = true;
                }
            }
            if (!matched) {
                Row combined;
                combined.values.resize(left_width, std::monostate{});  // NULL left side
                combined.values.insert(combined.values.end(),
                                       rr.row.values.begin(), rr.row.values.end());
                out.push_back({std::move(combined), Key{}});  // no matching left row
            }
        }
    } else {
        // ── INNER or LEFT JOIN — outer loop: left, inner loop: right ──────────
        for (const auto& lr : left_results) {
            bool matched = false;
            for (const auto& rr : right_results) {
                if (lr.row.get(left_col_idx) == rr.row.get(right_col_idx)) {
                    Row combined;
                    combined.values.reserve(left_width + right_width);
                    combined.values.insert(combined.values.end(),
                                           lr.row.values.begin(), lr.row.values.end());
                    combined.values.insert(combined.values.end(),
                                           rr.row.values.begin(), rr.row.values.end());
                    out.push_back({std::move(combined), lr.primary_key});
                    matched = true;
                }
            }
            if (!matched && join.type == JoinType::LEFT) {
                // LEFT JOIN: emit left row with NULL-filled right side
                Row combined;
                combined.values.reserve(left_width + right_width);
                combined.values.insert(combined.values.end(),
                                       lr.row.values.begin(), lr.row.values.end());
                combined.values.resize(left_width + right_width, std::monostate{});
                out.push_back({std::move(combined), lr.primary_key});
            }
            // INNER JOIN with no match: row is simply dropped
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// UPDATE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_update(const UpdateStmt& stmt)
{
    const TableSchema& schema = catalog_.get_table(stmt.table_name);

    // Resolve SET column names to schema indices
    std::vector<std::pair<size_t, Value>> new_values;
    new_values.reserve(stmt.assignments.size());
    for (const auto& asgn : stmt.assignments) {
        int idx = schema.column_index(asgn.column_name);
        if (idx < 0) {
            return {false, "UPDATE: unknown column '" + asgn.column_name + "'"};
        }
        if (asgn.is_default) {
            const Column& col = schema.columns[static_cast<size_t>(idx)];
            new_values.emplace_back(static_cast<size_t>(idx),
                                     col.has_default ? col.default_value : Value{std::monostate{}});
        } else {
            new_values.emplace_back(static_cast<size_t>(idx), asgn.value);
        }
    }

    auto      owned_indexes = open_indexes(schema);
    Table     tbl(schema, buffer_pool_, wal_, free_list_, index_ptrs(owned_indexes));
    Predicate pred = build_predicate(stmt.where, schema);

    // Computed once and reused for both CHECK validation below and the
    // mutation itself — index-narrowed via scan_with_index() (falls back
    // to a full Table::scan() when no index fits), re-checked against the
    // full predicate already inside scan_with_index(), so every row here
    // is a genuine match regardless of how it was found.
    std::vector<ScanResult> matched = scan_with_index(tbl, schema, stmt.where, pred);

    // Validate CHECK constraints against every row's post-update values
    // *before* touching storage — a single violation aborts the whole
    // UPDATE with no rows changed, matching CHECK's statement-level atomicity.
    if (!schema.checks.empty()) {
        for (const auto& match : matched) {
            Row candidate = match.row;
            for (const auto& [idx, val] : new_values) {
                candidate.values[idx] = val;
            }
            int violated = find_violated_check(schema, candidate);
            if (violated >= 0) {
                const CheckConstraint& c = schema.checks[static_cast<size_t>(violated)];
                return {false, "UPDATE: CHECK constraint violated on column '" +
                               schema.columns[c.column_index].name + "' (" +
                               schema.columns[c.column_index].name + " " +
                               check_op_symbol(c.op) + " " + value_to_string(c.operand) + ")"};
            }
        }
    }

    // Validate FOREIGN KEY constraints against every row's post-update
    // values, same statement-level atomicity as CHECK above: a single
    // violation aborts the whole UPDATE with no rows changed.
    //   - child-side: if this table's OWN FK columns are being changed,
    //     the new value must still reference an existing parent row.
    //   - parent-side: if some OTHER table's FK references a column this
    //     UPDATE is changing, and the OLD value still has child rows
    //     pointing at it, the update is rejected — UPDATE always behaves
    //     as RESTRICT here regardless of the FK's ON DELETE action (see
    //     ForeignKeyConstraint::on_delete's comment for why: there's no
    //     ON UPDATE CASCADE support yet).
    auto inbound_fks = catalog_.get_foreign_keys_referencing(schema.name);
    if (!schema.foreign_keys.empty() || !inbound_fks.empty()) {
        for (const auto& match : matched) {
            Row candidate = match.row;
            for (const auto& [idx, val] : new_values) {
                candidate.values[idx] = val;
            }

            if (!schema.foreign_keys.empty()) {
                std::string fk_error = check_foreign_keys_on_write(schema, candidate);
                if (!fk_error.empty()) {
                    return {false, "UPDATE: " + fk_error};
                }
            }

            if (!inbound_fks.empty()) {
                bool touches_referenced_column = false;
                for (const auto& [idx, val] : new_values) {
                    if (match.row.get(idx) == val) continue;  // no actual change
                    for (const auto& [child_table, fk] : inbound_fks) {
                        (void)child_table;
                        if (std::find(fk.ref_column_indices.begin(), fk.ref_column_indices.end(),
                                      static_cast<uint32_t>(idx)) != fk.ref_column_indices.end()) {
                            touches_referenced_column = true;
                        }
                    }
                }
                if (touches_referenced_column) {
                    auto children = find_referencing_children(schema, match.row);
                    if (!children.empty()) {
                        return {false, "UPDATE: cannot change a value referenced by a FOREIGN "
                                       "KEY constraint on table '" + children[0].child_table + "'"};
                    }
                }
            }
        }
    }

    // The mutation itself goes through Table::update_matched(), seeded
    // from the same index-narrowed `matched` list gathered above — see
    // Table::update_matched's own comment for why this is safe: its
    // phase 1 still validates every matched row's UNIQUE indexes against
    // every OTHER row in the same batch (batch_claimed) before phase 2
    // writes any of them, so a multi-row UPDATE stays atomic on a
    // uniqueness collision exactly as before, regardless of whether
    // `matched` came from a full scan or an index. This also means the
    // WHERE clause is no longer re-scanned a second time internally —
    // update_matched() trusts `matched` (already predicate-checked by
    // scan_with_index() above) instead of re-deriving it from
    // btree_.scan_all().
    uint32_t count = tbl.update_matched(matched, new_values);

    if (tbl.root_page() != schema.root_page) {
        catalog_.update_table_root(stmt.table_name, tbl.root_page());
    }
    persist_index_roots(tbl);

    return {true, "", {}, {}, count};
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_delete(const DeleteStmt& stmt)
{
    const TableSchema& schema = catalog_.get_table(stmt.table_name);

    Table     read_tbl(schema, buffer_pool_, wal_, free_list_);
    Predicate pred = build_predicate(stmt.where, schema);

    // Gather every matching primary key BEFORE deleting anything — same
    // "match first, mutate second" shape execute_update already uses, and
    // needed here regardless of FK constraints so delete_row_cascading
    // below has a stable list to work through. Two paths exactly mirror
    // the pre-FK version of this function: index-assisted (narrow via a
    // usable WHERE-clause index, re-check the full predicate per
    // candidate — see find_index_prefix) or a full scan when no index fits.
    std::vector<Key> matched_keys;
    auto index_match = find_index_prefix(stmt.where, schema);
    if (index_match) {
        const IndexSchema& ischema = *index_match->index;
        Index idx(ischema, schema.primary_key_indices.size(), buffer_pool_, wal_, free_list_);
        for (const Key& pk : index_lookup(idx, *index_match)) {
            std::optional<Row> row = read_tbl.select_by_key(pk);
            if (row && pred(*row)) {
                matched_keys.push_back(pk);
            }
        }
    } else {
        for (const auto& sr : read_tbl.scan(pred)) {
            matched_keys.push_back(sr.primary_key);
        }
    }

    // Each row is deleted through delete_row_cascading, which checks every
    // FOREIGN KEY referencing this table before removing anything (RESTRICT
    // throws; CASCADE recursively removes referencing rows first — see its
    // own comment). This opens a fresh Table (and re-persists roots) per
    // row rather than batching one Table across the whole statement like
    // the pre-FK version did — a real cost, traded for cascades being able
    // to hop to a completely different table's B+ tree partway through.
    // Also note: unlike execute_update's FK check (which validates every
    // matched row up front, before mutating any of them), a RESTRICT
    // violation partway through a multi-row DELETE leaves earlier rows (and
    // their cascades) already deleted — this DELETE function never
    // guaranteed full cross-row atomicity even before FKs existed (see the
    // original comment this replaced: "DELETE has no cross-row validation
    // to preserve").
    uint32_t count = 0;
    for (const Key& pk : matched_keys) {
        if (delete_row_cascading(stmt.table_name, pk)) {
            count++;
        }
    }

    return {true, "", {}, {}, count};
}

// ─────────────────────────────────────────────────────────────────────────────
// SHOW
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_show(const ShowStmt& stmt)
{
    if (stmt.target == ShowTarget::DATABASES) {
        return {false, "SHOW DATABASES must be handled by the Database class"};
    }

    // SHOW TABLES
    QueryResult result;
    result.success = true;
    result.columns = {"Tables"};
    for (const auto& name : catalog_.list_tables()) {
        result.rows.push_back({name});
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Database-level stubs — these are handled by the Database class above us
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_create_database(const CreateDatabaseStmt&) {
    return {false, "CREATE DATABASE must be handled by the Database class"};
}
QueryResult Executor::execute_drop_database(const DropDatabaseStmt&) {
    return {false, "DROP DATABASE must be handled by the Database class"};
}
QueryResult Executor::execute_use(const UseStmt&) {
    return {false, "USE must be handled by the Database class"};
}

// ─────────────────────────────────────────────────────────────────────────────
// WHERE evaluation
// ─────────────────────────────────────────────────────────────────────────────

bool Executor::evaluate_where(const WhereExprPtr&  expr,
                               const Row&            row,
                               const TableSchema&    left_schema,
                               const TableSchema*    right_schema,
                               const SubqueryCache*  cache,
                               const OuterContext*   outer,
                               const std::string&    left_alias,
                               const std::string&    right_alias) const
{
    if (!expr) return true;

    if (expr->kind == WhereExpr::Kind::COMPARE) {
        return evaluate_compare(expr->compare, row, left_schema, right_schema, cache, outer,
                                 left_alias, right_alias);
    }

    if (expr->kind == WhereExpr::Kind::EXISTS) {
        if (!cache) {
            throw std::runtime_error(
                "internal error: EXISTS evaluated without a precomputed subquery cache");
        }
        // Correlated: this outer row's own values feed into the subquery's
        // WHERE (via OuterContext), so the inner scan has to happen fresh,
        // right now, for THIS row — it wasn't and couldn't be precomputed.
        if (cache->correlated_exists.count(expr.get())) {
            OuterContext this_row{&row, &left_schema, right_schema, left_alias, right_alias};
            return !run_subquery_scan(*expr->subquery, &this_row).empty();
        }
        return cache->exists_results.at(expr.get());
    }

    // LOGICAL node
    switch (expr->logical_op) {
        case LogicalOp::AND:
            return evaluate_where(expr->left,  row, left_schema, right_schema, cache, outer,
                                   left_alias, right_alias) &&
                   evaluate_where(expr->right, row, left_schema, right_schema, cache, outer,
                                   left_alias, right_alias);
        case LogicalOp::OR:
            return evaluate_where(expr->left,  row, left_schema, right_schema, cache, outer,
                                   left_alias, right_alias) ||
                   evaluate_where(expr->right, row, left_schema, right_schema, cache, outer,
                                   left_alias, right_alias);
        case LogicalOp::NOT:
            return !evaluate_where(expr->left, row, left_schema, right_schema, cache, outer,
                                    left_alias, right_alias);
    }
    return false;
}

bool Executor::evaluate_compare(const CompareExpr&   expr,
                                 const Row&            row,
                                 const TableSchema&    left_schema,
                                 const TableSchema*    right_schema,
                                 const SubqueryCache*  cache,
                                 const OuterContext*   outer,
                                 const std::string&    left_alias,
                                 const std::string&    right_alias) const
{
    // resolve_value tries left_schema/right_schema first, then falls back to
    // 'outer' if given — that fallback is what makes a correlated subquery's
    // WHERE able to see a column from the outer row.
    const Value& val = resolve_value(expr.column, row, left_schema, right_schema, outer,
                                      left_alias, right_alias);

    // IS NULL / IS NOT NULL do not examine the operand
    if (expr.op == CompareOp::IS_NULL)     return  is_null(val);
    if (expr.op == CompareOp::IS_NOT_NULL) return !is_null(val);

    // column [NOT] IN (SELECT ...) — uses the precomputed (or, if
    // correlated, freshly-run-for-this-row) candidate set, not expr.operand,
    // so this must be handled before the generic null-operand check below.
    if (expr.op == CompareOp::IN_SUBQUERY || expr.op == CompareOp::NOT_IN_SUBQUERY) {
        if (!cache) {
            throw std::runtime_error(
                "internal error: IN (SELECT ...) evaluated without a precomputed subquery cache");
        }
        // SQL: x IN (...) / x NOT IN (...) is UNKNOWN (never true) when x is NULL
        if (is_null(val)) return false;

        // Correlated: re-run the inner query for this specific outer row
        // rather than reusing a cached, once-computed candidate set.
        std::vector<Value> fresh_candidates;
        const std::vector<Value>* candidates;
        if (cache->correlated_in.count(&expr)) {
            OuterContext this_row{&row, &left_schema, right_schema, left_alias, right_alias};
            fresh_candidates = run_subquery_values(*expr.subquery, &this_row);
            candidates = &fresh_candidates;
        } else {
            candidates = &cache->in_values.at(&expr);
        }

        bool found    = false;
        bool any_null = false;
        for (const auto& c : *candidates) {
            if (is_null(c)) { any_null = true; continue; }
            if (values_equal(val, c)) { found = true; break; }
        }

        if (expr.op == CompareOp::IN_SUBQUERY) {
            return found;  // not-found-with-a-NULL-present is UNKNOWN, same
                           // WHERE-excluded outcome as a definite not-found
        }
        // NOT IN — the classic ANSI SQL gotcha: if x isn't found among the
        // non-NULL candidates but the subquery returned any NULL, the result
        // is UNKNOWN (not TRUE) — NOT IN can only be definitely TRUE when
        // every candidate was checked and none was NULL.
        if (found)    return false;
        if (any_null) return false;
        return true;
    }

    // RHS is either another column (orders.user_id = users.id) or a literal
    // (age > 25). Column-vs-column resolution uses the same outer fallback
    // as the LHS, so `users.id` on the right resolves against the outer row
    // exactly the same way `orders.user_id` on the left resolves locally.
    const Value& rhs = expr.operand_column
        ? resolve_value(*expr.operand_column, row, left_schema, right_schema, outer,
                         left_alias, right_alias)
        : expr.operand;

    // SQL NULL semantics: any comparison with NULL yields false
    if (is_null(val)) return false;
    if (is_null(rhs))  return false;

    // LIKE — only meaningful for VARCHAR
    if (expr.op == CompareOp::LIKE) {
        if (!std::holds_alternative<std::string>(val) ||
            !std::holds_alternative<std::string>(rhs)) {
            return false;
        }
        return like_match(get_string(val), get_string(rhs));
    }

    // General comparison via std::visit — handles same-type and int/float cross comparisons
    return std::visit([&](const auto& lhs, const auto& rhs_val) -> bool {
        using L = std::decay_t<decltype(lhs)>;
        using R = std::decay_t<decltype(rhs_val)>;

        // monostate (NULL) was already filtered above, but guard anyway
        if constexpr (std::is_same_v<L, std::monostate> ||
                      std::is_same_v<R, std::monostate>) {
            return false;
        }

        // Int/float cross comparison — promote both to float
        else if constexpr ((std::is_same_v<L, int32_t> || std::is_same_v<L, float>) &&
                           (std::is_same_v<R, int32_t> || std::is_same_v<R, float>) &&
                           !std::is_same_v<L, R>) {
            float fl = static_cast<float>(lhs);
            float fr = static_cast<float>(rhs_val);
            switch (expr.op) {
                case CompareOp::EQ:  return fl == fr;
                case CompareOp::NEQ: return fl != fr;
                case CompareOp::LT:  return fl <  fr;
                case CompareOp::GT:  return fl >  fr;
                case CompareOp::LTE: return fl <= fr;
                case CompareOp::GTE: return fl >= fr;
                default: return false;
            }
        }

        // Same-type comparison
        else if constexpr (std::is_same_v<L, R>) {
            switch (expr.op) {
                case CompareOp::EQ:  return lhs == rhs_val;
                case CompareOp::NEQ: return lhs != rhs_val;
                case CompareOp::LT:  return lhs <  rhs_val;
                case CompareOp::GT:  return lhs >  rhs_val;
                case CompareOp::LTE: return lhs <= rhs_val;
                case CompareOp::GTE: return lhs >= rhs_val;
                default: return false;
            }
        }

        // Incompatible types (e.g. comparing INT with VARCHAR)
        else {
            return false;
        }
    }, val, rhs);
}

Predicate Executor::build_predicate(const WhereExprPtr& where,
                                     const TableSchema&  schema,
                                     const TableSchema*  right_schema,
                                     const OuterContext* outer,
                                     const std::string&  left_alias,
                                     const std::string&  right_alias) const
{
    if (!where) {
        return [](const Row&) { return true; };
    }
    // Run any non-correlated subqueries in 'where' exactly once, up front —
    // not once per row. shared_ptr keeps the cache alive for as long as the
    // returned predicate lambda is (it's cheap to copy-capture into the
    // closure). Correlated subqueries are marked here but actually executed
    // per-row inside evaluate_where/evaluate_compare.
    auto cache = std::make_shared<SubqueryCache>();
    precompute_subqueries(where, *cache);

    // Capture by reference — safe because the predicate is only used within
    // the same execute_*() call frame, which keeps 'where' and 'schema' alive.
    // 'outer' is captured by value (it's just a pointer) — when this
    // predicate belongs to a correlated subquery's own scan, it points to
    // the single outer row this particular scan was run for. left_alias/
    // right_alias are captured by value (cheap strings) for the same reason.
    return [this, &where, &schema, right_schema, cache, outer, left_alias, right_alias](const Row& row) {
        return evaluate_where(where, row, schema, right_schema, cache.get(), outer,
                               left_alias, right_alias);
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Subqueries (IN / NOT IN / EXISTS / NOT EXISTS) — non-correlated
// ─────────────────────────────────────────────────────────────────────────────

bool Executor::where_references_unknown_column(const WhereExprPtr& expr,
                                                 const TableSchema&  sub_schema,
                                                 const std::string&  sub_alias) const
{
    if (!expr) return false;

    if (expr->kind == WhereExpr::Kind::LOGICAL) {
        return where_references_unknown_column(expr->left,  sub_schema, sub_alias) ||
               where_references_unknown_column(expr->right, sub_schema, sub_alias);
    }

    if (expr->kind == WhereExpr::Kind::EXISTS) {
        // A nested EXISTS's own correlation (if any) is against ITS
        // immediate parent schema, handled when precompute_subqueries
        // recurses into it separately — not something that makes the
        // current subquery correlated by itself.
        return false;
    }

    // COMPARE node — check both sides. expr.column not resolving locally
    // means it must be an outer column (or a genuine typo, which surfaces
    // as an "unknown column" error at runtime when the outer fallback also
    // fails to find it — same failure mode as today, just later).
    const CompareExpr& c = expr->compare;
    if (!try_resolve_column(c.column, sub_schema, sub_alias).has_value()) return true;
    if (c.operand_column && !try_resolve_column(*c.operand_column, sub_schema, sub_alias).has_value()) return true;
    return false;
}

void Executor::precompute_subqueries(const WhereExprPtr& expr, SubqueryCache& cache) const
{
    if (!expr) return;

    if (expr->kind == WhereExpr::Kind::LOGICAL) {
        precompute_subqueries(expr->left,  cache);
        precompute_subqueries(expr->right, cache);  // no-op if null (NOT node)
        return;
    }

    if (expr->kind == WhereExpr::Kind::EXISTS) {
        const TableSchema& sub_schema = catalog_.get_table(expr->subquery->table_name);
        if (where_references_unknown_column(expr->subquery->where, sub_schema,
                                             expr->subquery->table_alias)) {
            // Correlated — mark it, don't run it. Actual execution happens
            // per outer row in evaluate_where.
            cache.correlated_exists.insert(expr.get());
            return;
        }
        bool non_empty = !run_subquery_scan(*expr->subquery).empty();
        cache.exists_results[expr.get()] = non_empty;
        return;
    }

    // COMPARE — only IN_SUBQUERY / NOT_IN_SUBQUERY carry a subquery to run
    if (expr->compare.op == CompareOp::IN_SUBQUERY ||
        expr->compare.op == CompareOp::NOT_IN_SUBQUERY) {
        const TableSchema& sub_schema = catalog_.get_table(expr->compare.subquery->table_name);
        if (where_references_unknown_column(expr->compare.subquery->where, sub_schema,
                                             expr->compare.subquery->table_alias)) {
            cache.correlated_in.insert(&expr->compare);
            return;
        }
        cache.in_values[&expr->compare] = run_subquery_values(*expr->compare.subquery);
    }
}

std::vector<ScanResult> Executor::run_subquery_scan(const SelectStmt&    subquery,
                                                      const OuterContext* outer) const
{
    if (!subquery.joins.empty()) {
        throw std::runtime_error(
            "subqueries with JOIN are not yet supported");
    }

    const TableSchema& sub_schema = catalog_.get_table(subquery.table_name);
    Table sub_tbl(sub_schema, buffer_pool_, wal_, free_list_);

    // build_predicate() recursively precomputes any subqueries nested
    // inside this subquery's own WHERE, so IN/EXISTS nesting just works.
    // 'outer' is forwarded so THIS subquery's own WHERE can, in turn,
    // resolve a column against outer's outer row if it needs to (arbitrary
    // correlation depth works the same way one level deep does).
    Predicate pred = build_predicate(subquery.where, sub_schema, nullptr, outer,
                                      subquery.table_alias);

    // scan_with_index() (rather than a plain sub_tbl.scan(pred)) is what
    // makes a correlated subquery an index nested-loop join (G): 'outer'
    // is forwarded all the way down into collect_and_constraints, so a
    // correlated equality/range like `o.customer_id = c.id` can seed
    // orders' customer_id index with c.id's actual value for whichever
    // outer row is currently being checked, instead of falling back to a
    // full scan of `orders` per outer row. Non-correlated subqueries
    // (outer == nullptr) benefit the same way from any literal WHERE
    // condition, same as a top-level SELECT would.
    std::vector<ScanResult> rows =
        scan_with_index(sub_tbl, sub_schema, subquery.where, pred, subquery.table_alias, outer);

    if (subquery.limit.has_value()) {
        size_t lim = static_cast<size_t>(subquery.limit.value());
        if (rows.size() > lim) rows.resize(lim);
    }
    return rows;
}

std::vector<Value> Executor::run_subquery_values(const SelectStmt&    subquery,
                                                   const OuterContext* outer) const
{
    if (subquery.columns.size() != 1 ||
        subquery.columns[0].is_star ||
        subquery.columns[0].is_literal ||
        subquery.columns[0].aggregate != AggregateType::NONE) {
        throw std::runtime_error(
            "a subquery used with IN / NOT IN must select exactly one plain "
            "column (SELECT *, SELECT <literal>, and aggregate subqueries "
            "are not supported here)");
    }

    const TableSchema& sub_schema = catalog_.get_table(subquery.table_name);
    size_t col_idx = resolve_column(subquery.columns[0].column, sub_schema, subquery.table_alias);

    std::vector<Value> values;
    for (const auto& sr : run_subquery_scan(subquery, outer)) {
        values.push_back(sr.row.get(col_idx));
    }
    return values;
}

bool Executor::values_equal(const Value& a, const Value& b) const
{
    if (is_null(a) || is_null(b)) return false;
    return std::visit([](const auto& lhs, const auto& rhs) -> bool {
        using L = std::decay_t<decltype(lhs)>;
        using R = std::decay_t<decltype(rhs)>;
        if constexpr (std::is_same_v<L, std::monostate> || std::is_same_v<R, std::monostate>) {
            return false;
        } else if constexpr ((std::is_same_v<L, int32_t> || std::is_same_v<L, float>) &&
                              (std::is_same_v<R, int32_t> || std::is_same_v<R, float>) &&
                              !std::is_same_v<L, R>) {
            return static_cast<float>(lhs) == static_cast<float>(rhs);
        } else if constexpr (std::is_same_v<L, R>) {
            return lhs == rhs;
        } else {
            return false;
        }
    }, a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// CHECK constraints
// ─────────────────────────────────────────────────────────────────────────────

CheckOp Executor::compare_op_to_check_op(CompareOp op) const {
    switch (op) {
        case CompareOp::EQ:  return CheckOp::EQ;
        case CompareOp::NEQ: return CheckOp::NEQ;
        case CompareOp::LT:  return CheckOp::LT;
        case CompareOp::GT:  return CheckOp::GT;
        case CompareOp::LTE: return CheckOp::LTE;
        case CompareOp::GTE: return CheckOp::GTE;
        default:
            throw std::runtime_error(
                "unsupported operator in CHECK constraint — "
                "only =, !=, <, >, <=, >= are supported "
                "(not IS NULL / LIKE / IN / EXISTS)");
    }
}

CompareOp Executor::check_op_to_compare_op(CheckOp op) const {
    switch (op) {
        case CheckOp::EQ:  return CompareOp::EQ;
        case CheckOp::NEQ: return CompareOp::NEQ;
        case CheckOp::LT:  return CompareOp::LT;
        case CheckOp::GT:  return CompareOp::GT;
        case CheckOp::LTE: return CompareOp::LTE;
        case CheckOp::GTE: return CompareOp::GTE;
    }
    throw std::runtime_error("unknown CheckOp");
}

void Executor::flatten_check_expr(const WhereExprPtr&           expr,
                                   const TableSchema&            schema,
                                   std::vector<CheckConstraint>& out) const
{
    if (!expr) return;

    if (expr->kind == WhereExpr::Kind::LOGICAL) {
        // SQL has no chained comparisons (30 < x < 50 is invalid SQL), so
        // CHECK (x > 30 AND x < 50) is exactly an AND-chain of simple
        // comparisons — split it into separate, implicitly-ANDed entries.
        if (expr->logical_op == LogicalOp::AND) {
            flatten_check_expr(expr->left,  schema, out);
            flatten_check_expr(expr->right, schema, out);
            return;
        }
        throw std::runtime_error(
            "unsupported CHECK expression — only AND of simple comparisons "
            "is supported (no OR / NOT)");
    }

    if (expr->kind == WhereExpr::Kind::EXISTS) {
        throw std::runtime_error(
            "CHECK constraints cannot contain EXISTS / subquery expressions");
    }

    // COMPARE leaf: column OP literal
    const CompareExpr& cmp = expr->compare;
    if (cmp.operand_column.has_value()) {
        throw std::runtime_error(
            "CHECK constraints must compare a column against a literal, not another column");
    }
    if (!cmp.column.table_name.empty()) {
        throw std::runtime_error(
            "CHECK constraint column references must be unqualified: '" +
            cmp.column.column_name + "'");
    }
    int idx = schema.column_index(cmp.column.column_name);
    if (idx < 0) {
        throw std::runtime_error(
            "CHECK constraint references unknown column '" +
            cmp.column.column_name + "'");
    }

    CheckConstraint c;
    c.column_index = static_cast<uint32_t>(idx);
    c.op           = compare_op_to_check_op(cmp.op);  // throws for IS_NULL/LIKE
    c.operand      = cmp.operand;
    out.push_back(std::move(c));
}

int Executor::find_violated_check(const TableSchema& schema, const Row& row) const
{
    for (size_t i = 0; i < schema.checks.size(); ++i) {
        const CheckConstraint& c = schema.checks[i];

        CompareExpr cmp;
        cmp.column.column_name = schema.columns[c.column_index].name;
        cmp.op                 = check_op_to_compare_op(c.op);
        cmp.operand             = c.operand;

        // A CHECK is satisfied when the expression is TRUE or UNKNOWN (SQL
        // three-valued logic) — it only fails when the expression is FALSE.
        // evaluate_compare() already returns false for any NULL operand, so
        // a NULL column value passes the check here, matching ANSI semantics.
        if (is_null(row.get(c.column_index))) continue;
        if (!evaluate_compare(cmp, row, schema)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// FOREIGN KEY constraint enforcement — see executor.h for the two entry
// points' contracts.
// ─────────────────────────────────────────────────────────────────────────────

std::string Executor::check_foreign_keys_on_write(const TableSchema& schema, const Row& row) const
{
    for (const auto& fk : schema.foreign_keys) {
        Key child_values;
        bool any_null = false;
        for (uint32_t col_idx : fk.column_indices) {
            const Value& v = row.get(col_idx);
            if (is_null(v)) { any_null = true; break; }
            child_values.push_back(v);
        }
        // MATCH SIMPLE: any NULL component means "no reference" — the row
        // is exempt from this constraint entirely, same convention
        // Index::is_indexable already uses for NULL indexed values.
        if (any_null) continue;

        bool parent_exists;
        if (fk.ref_index_name.empty()) {
            // References the parent's primary key directly.
            const TableSchema& ref_schema = catalog_.get_table(fk.ref_table);
            Table ref_tbl(ref_schema, buffer_pool_, wal_, free_list_);
            parent_exists = ref_tbl.select_by_key(child_values).has_value();
        } else {
            // References an existing UNIQUE secondary index.
            const IndexSchema& ref_ischema = catalog_.get_index(fk.ref_index_name);
            const TableSchema& ref_schema  = catalog_.get_table(fk.ref_table);
            Index ref_idx(ref_ischema, ref_schema.primary_key_indices.size(),
                          buffer_pool_, wal_, free_list_);
            parent_exists = !ref_idx.find(child_values).empty();
        }

        if (!parent_exists) {
            std::string cols;
            for (size_t i = 0; i < fk.column_indices.size(); ++i) {
                if (i > 0) cols += ", ";
                cols += schema.columns[fk.column_indices[i]].name;
            }
            return "FOREIGN KEY violation: no row in '" + fk.ref_table +
                   "' matches (" + cols + ") = (" +
                   [&] {
                       std::string vals;
                       for (size_t i = 0; i < child_values.size(); ++i) {
                           if (i > 0) vals += ", ";
                           vals += value_to_string(child_values[i]);
                       }
                       return vals;
                   }() + ")";
        }
    }
    return "";
}

std::vector<Executor::ReferencingChildren> Executor::find_referencing_children(
    const TableSchema& ref_schema, const Row& ref_row) const
{
    std::vector<ReferencingChildren> result;

    for (const auto& [child_table, fk] : catalog_.get_foreign_keys_referencing(ref_schema.name)) {
        Key parent_values;
        bool any_null = false;
        for (uint32_t ref_idx : fk.ref_column_indices) {
            const Value& v = ref_row.get(ref_idx);
            if (is_null(v)) { any_null = true; break; }
            parent_values.push_back(v);
        }
        // A NULL referenced value was never indexed on the child side
        // either (Index::is_indexable), so it can't have any referencing
        // child rows — nothing to find.
        if (any_null) continue;

        const IndexSchema& child_ischema = catalog_.get_index(fk.child_index_name);
        const TableSchema& child_schema  = catalog_.get_table(child_table);
        Index child_idx(child_ischema, child_schema.primary_key_indices.size(),
                        buffer_pool_, wal_, free_list_);
        std::vector<Key> child_pks = child_idx.find(parent_values);

        if (!child_pks.empty()) {
            result.push_back({child_table, fk, std::move(child_pks)});
        }
    }

    return result;
}

bool Executor::delete_row_cascading(const std::string& table_name, const Key& key)
{
    const TableSchema& schema = catalog_.get_table(table_name);

    Table read_tbl(schema, buffer_pool_, wal_, free_list_);
    std::optional<Row> row = read_tbl.select_by_key(key);
    if (!row) return false;  // already removed by an earlier cascade this statement

    for (const auto& child : find_referencing_children(schema, *row)) {
        if (child.constraint.on_delete == FkOnDelete::RESTRICT) {
            throw std::runtime_error(
                "FOREIGN KEY violation: row in '" + table_name +
                "' is still referenced by table '" + child.child_table + "'");
        }
        // CASCADE — remove every referencing child row first. Recurses
        // through this same function, so a chain of CASCADE FKs (child has
        // its own children referencing IT) cascades transitively.
        for (const Key& child_pk : child.child_primary_keys) {
            delete_row_cascading(child.child_table, child_pk);
        }
    }

    auto  owned_indexes = open_indexes(schema);
    Table tbl(schema, buffer_pool_, wal_, free_list_, index_ptrs(owned_indexes));
    tbl.delete_row(key);

    if (tbl.root_page() != schema.root_page) {
        catalog_.update_table_root(table_name, tbl.root_page());
    }
    persist_index_roots(tbl);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Column resolution
// ─────────────────────────────────────────────────────────────────────────────

bool Executor::table_name_matches(const std::string& ref_table,
                                   const TableSchema& schema,
                                   const std::string& alias) const
{
    return ref_table == schema.name || (!alias.empty() && ref_table == alias);
}

std::optional<size_t> Executor::try_resolve_column(const ColumnRef&   ref,
                                                    const TableSchema& schema,
                                                    const std::string& alias) const
{
    if (!ref.table_name.empty() && !table_name_matches(ref.table_name, schema, alias))
        return std::nullopt;
    int idx = schema.column_index(ref.column_name);
    if (idx < 0) return std::nullopt;
    return static_cast<size_t>(idx);
}

std::optional<size_t> Executor::try_resolve_column(const ColumnRef&   ref,
                                                    const TableSchema& left_schema,
                                                    const TableSchema& right_schema,
                                                    const std::string& left_alias,
                                                    const std::string& right_alias) const
{
    const size_t left_width = left_schema.columns.size();

    if (!ref.table_name.empty()) {
        if (table_name_matches(ref.table_name, left_schema, left_alias)) {
            int idx = left_schema.column_index(ref.column_name);
            return idx < 0 ? std::nullopt : std::optional<size_t>(static_cast<size_t>(idx));
        }
        if (table_name_matches(ref.table_name, right_schema, right_alias)) {
            int idx = right_schema.column_index(ref.column_name);
            return idx < 0 ? std::nullopt : std::optional<size_t>(left_width + static_cast<size_t>(idx));
        }
        return std::nullopt;
    }

    int left_idx  = left_schema.column_index(ref.column_name);
    int right_idx = right_schema.column_index(ref.column_name);
    if (left_idx >= 0 && right_idx >= 0) {
        throw std::runtime_error(
            "Ambiguous column '" + ref.column_name +
            "' exists in both '" + left_schema.name +
            "' and '" + right_schema.name +
            "' — qualify with table name");
    }
    if (left_idx  >= 0) return static_cast<size_t>(left_idx);
    if (right_idx >= 0) return left_width + static_cast<size_t>(right_idx);
    return std::nullopt;
}

const Value& Executor::resolve_value(const ColumnRef&    ref,
                                      const Row&           row,
                                      const TableSchema&   schema,
                                      const TableSchema*   right_schema,
                                      const OuterContext*  outer,
                                      const std::string&   left_alias,
                                      const std::string&   right_alias) const
{
    // Try the local (current query's own) schema(s) first.
    if (right_schema) {
        if (auto idx = try_resolve_column(ref, schema, *right_schema, left_alias, right_alias))
            return row.get(*idx);
    } else {
        if (auto idx = try_resolve_column(ref, schema, left_alias)) return row.get(*idx);
    }

    // Not found locally — fall back to the outer row. This is the entire
    // correlated-subquery mechanism: a WHERE clause inside a subquery that
    // mentions a column not in the subquery's own table falls through to
    // whichever outer row is currently being checked.
    if (outer) {
        if (outer->right_schema) {
            if (auto idx = try_resolve_column(ref, *outer->schema, *outer->right_schema,
                                               outer->alias, outer->right_alias))
                return outer->row->get(*idx);
        } else {
            if (auto idx = try_resolve_column(ref, *outer->schema, outer->alias))
                return outer->row->get(*idx);
        }
    }

    throw std::runtime_error(
        "Unknown column '" + ref.column_name + "'" +
        (ref.table_name.empty() ? "" : " (qualified with '" + ref.table_name + "')"));
}

size_t Executor::resolve_column(const ColumnRef&   ref,
                                 const TableSchema& schema,
                                 const std::string& alias) const
{
    // If the column ref is table-qualified, it must match this schema's
    // name or its query alias (if any).
    if (!ref.table_name.empty() && !table_name_matches(ref.table_name, schema, alias)) {
        throw std::runtime_error(
            "Column '" + ref.column_name +
            "' is qualified with table '" + ref.table_name +
            "', which does not match table '" + schema.name + "'");
    }

    int idx = schema.column_index(ref.column_name);
    if (idx < 0) {
        throw std::runtime_error("Unknown column '" + ref.column_name + "'");
    }
    return static_cast<size_t>(idx);
}

size_t Executor::resolve_column(const ColumnRef&   ref,
                                 const TableSchema& left_schema,
                                 const TableSchema& right_schema,
                                 const std::string& left_alias,
                                 const std::string& right_alias) const
{
    const size_t left_width = left_schema.columns.size();

    if (!ref.table_name.empty()) {
        // Qualified reference — must match exactly one table's name or alias
        if (table_name_matches(ref.table_name, left_schema, left_alias)) {
            int idx = left_schema.column_index(ref.column_name);
            if (idx < 0)
                throw std::runtime_error("Unknown column '" + ref.column_name +
                                         "' in table '" + left_schema.name + "'");
            return static_cast<size_t>(idx);
        }
        if (table_name_matches(ref.table_name, right_schema, right_alias)) {
            int idx = right_schema.column_index(ref.column_name);
            if (idx < 0)
                throw std::runtime_error("Unknown column '" + ref.column_name +
                                         "' in table '" + right_schema.name + "'");
            return left_width + static_cast<size_t>(idx);
        }
        throw std::runtime_error("Unknown table '" + ref.table_name + "'");
    }

    // Unqualified — search both; throw on ambiguity
    int left_idx  = left_schema.column_index(ref.column_name);
    int right_idx = right_schema.column_index(ref.column_name);

    if (left_idx >= 0 && right_idx >= 0) {
        throw std::runtime_error(
            "Ambiguous column '" + ref.column_name +
            "' exists in both '" + left_schema.name +
            "' and '" + right_schema.name +
            "' — qualify with table name");
    }
    if (left_idx  >= 0) return static_cast<size_t>(left_idx);
    if (right_idx >= 0) return left_width + static_cast<size_t>(right_idx);

    throw std::runtime_error("Unknown column '" + ref.column_name + "'");
}

// ─────────────────────────────────────────────────────────────────────────────
// Result formatting
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string> Executor::row_to_strings(const Row& row) const
{
    std::vector<std::string> result;
    result.reserve(row.size());
    for (size_t i = 0; i < row.size(); ++i) {
        result.push_back(value_to_string(row.get(i)));
    }
    return result;
}

std::vector<std::string> Executor::build_column_headers(
    const std::vector<SelectColumn>& select_cols,
    const TableSchema&               schema,
    const TableSchema*               joined_schema) const
{
    // Check whether any SELECT column is a wildcard
    bool has_star = false;
    for (const auto& sc : select_cols) {
        if (sc.is_star) { has_star = true; break; }
    }

    if (has_star) {
        std::vector<std::string> headers;
        for (const auto& col : schema.columns) {
            headers.push_back(col.name);
        }
        if (joined_schema) {
            for (const auto& col : joined_schema->columns) {
                headers.push_back(col.name);
            }
        }
        return headers;
    }

    std::vector<std::string> headers;
    headers.reserve(select_cols.size());
    for (const auto& sc : select_cols) {
        // Header for a literal projection is its own text, e.g. `SELECT 1`
        // gets header "1" — matches how most SQL engines display it.
        headers.push_back(sc.is_literal ? value_to_string(sc.literal) : sc.column.column_name);
    }
    return headers;
}

std::vector<std::string> Executor::project_row(
    const std::vector<SelectColumn>& select_cols,
    const Row&                       row,
    const TableSchema&               schema,
    const TableSchema*               joined_schema,
    const std::string&               left_alias,
    const std::string&               right_alias) const
{
    // For JOIN queries, 'row' is the combined row (left cols + right cols).
    // 'joined_schema' is the right-side schema for column name resolution.

    bool has_star = false;
    for (const auto& sc : select_cols) {
        if (sc.is_star) { has_star = true; break; }
    }

    if (has_star) {
        // Return every value in the (possibly combined) row as strings
        return row_to_strings(row);
    }

    std::vector<std::string> out;
    out.reserve(select_cols.size());

    for (const auto& sc : select_cols) {
        if (sc.is_literal) {
            out.push_back(value_to_string(sc.literal));
            continue;
        }

        const std::string& col_name   = sc.column.column_name;
        const std::string& table_qual = sc.column.table_name;

        // ── Qualified reference with explicit table name (or alias) ────────
        if (!table_qual.empty()) {
            if (table_name_matches(table_qual, schema, left_alias)) {
                int idx = schema.column_index(col_name);
                if (idx < 0) throw std::runtime_error("SELECT: unknown column '" + col_name + "'");
                out.push_back(value_to_string(row.get(static_cast<size_t>(idx))));
                continue;
            }
            if (joined_schema && table_name_matches(table_qual, *joined_schema, right_alias)) {
                int idx = joined_schema->column_index(col_name);
                if (idx < 0) throw std::runtime_error("SELECT: unknown column '" + col_name + "'");
                size_t combined_idx = schema.columns.size() + static_cast<size_t>(idx);
                out.push_back(value_to_string(row.get(combined_idx)));
                continue;
            }
            throw std::runtime_error(
                "SELECT: column '" + col_name +
                "' qualified with unknown table '" + table_qual + "'");
        }

        // ── Unqualified reference — search left schema first ──────────────
        int idx = schema.column_index(col_name);
        if (idx >= 0) {
            out.push_back(value_to_string(row.get(static_cast<size_t>(idx))));
            continue;
        }

        // Fall back to right (joined) schema
        if (joined_schema) {
            idx = joined_schema->column_index(col_name);
            if (idx >= 0) {
                size_t combined_idx = schema.columns.size() + static_cast<size_t>(idx);
                out.push_back(value_to_string(row.get(combined_idx)));
                continue;
            }
        }

        throw std::runtime_error("SELECT: unknown column '" + col_name + "'");
    }

    return out;
}