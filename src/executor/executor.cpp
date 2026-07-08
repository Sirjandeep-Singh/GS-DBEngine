#include "executor.h"

#include <algorithm>
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

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

Executor::Executor(CatalogManager& catalog,
                   BufferPool&     buffer_pool,
                   WALManager&     wal,
                   FreeListManager& free_list)
    : catalog_(catalog), buffer_pool_(buffer_pool), wal_(wal), free_list_(free_list) {}

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
    schema.primary_key_index = UINT32_MAX;

    int pk_count = 0;
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        Column col = column_def_to_column(stmt.columns[i]);  // throws on bad type
        if (col.is_primary_key) {
            pk_count++;
            schema.primary_key_index = static_cast<uint32_t>(i);
        }
        schema.columns.push_back(std::move(col));
    }

    if (pk_count == 0) {
        return {false, "CREATE TABLE '" + stmt.table_name +
                       "': no PRIMARY KEY column defined"};
    }
    if (pk_count > 1) {
        return {false, "CREATE TABLE '" + stmt.table_name +
                       "': only one PRIMARY KEY column is allowed"};
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
    catalog_.drop_table(stmt.table_name);
    return {true, "", {}, {}, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// INSERT
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_insert(const InsertStmt& stmt)
{
    const TableSchema& schema = catalog_.get_table(stmt.table_name);

    // Build the row — values ordered to match schema column positions.
    // Unspecified columns default to NULL (std::monostate).
    Row row;
    row.values.resize(schema.columns.size(), std::monostate{});

    if (stmt.columns.empty()) {
        // INSERT INTO t VALUES (v1, v2, ...)  — positional, schema order
        if (stmt.values.size() != schema.columns.size()) {
            return {false,
                    "INSERT: value count (" + std::to_string(stmt.values.size()) +
                    ") does not match column count (" +
                    std::to_string(schema.columns.size()) + ")"};
        }
        for (size_t i = 0; i < stmt.values.size(); ++i) {
            row.values[i] = stmt.values[i];
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
            row.values[static_cast<size_t>(idx)] = stmt.values[i];
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

    Table tbl(schema, buffer_pool_, wal_, free_list_);
    tbl.insert(row);  // throws on NOT NULL / duplicate PK / type mismatch

    // B+ tree root may have changed after a root split — keep catalog in sync
    if (tbl.root_page() != schema.root_page) {
        catalog_.update_table_root(stmt.table_name, tbl.root_page());
    }

    return {true, "", {}, {}, 1};
}

// ─────────────────────────────────────────────────────────────────────────────
// SELECT
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_select(const SelectStmt& stmt)
{
    const TableSchema& schema = catalog_.get_table(stmt.table_name);
    Table left_tbl(schema, buffer_pool_, wal_, free_list_);

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
        Predicate pred = build_predicate(stmt.where, schema);
        std::vector<ScanResult> rows = left_tbl.scan(pred);

        // ORDER BY (only first clause honoured — no secondary sort key)
        if (!stmt.order_by.empty()) {
            const OrderByClause& ob = stmt.order_by[0];
            size_t col_idx = resolve_column(ob.column, schema);
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
            result.rows.push_back(project_row(stmt.columns, sr.row, schema));
        }

    } else {
        // ── JOIN path — one join clause, nested-loop ──────────────────────────
        const JoinClause&  join         = stmt.joins[0];
        const TableSchema& right_schema = catalog_.get_table(join.table_name);
        Table              right_tbl(right_schema, buffer_pool_, wal_, free_list_);

        // Scan the left table completely first — WHERE is applied after joining
        Predicate all_rows = [](const Row&) { return true; };
        std::vector<ScanResult> left_results = left_tbl.scan(all_rows);

        std::vector<JoinedRow> joined =
            nested_loop_join(left_results, schema, right_tbl, right_schema, join);

        // WHERE — two-schema resolve handles qualified right-table references
        std::vector<JoinedRow> filtered;
        filtered.reserve(joined.size());
        for (auto& jr : joined) {
            if (!stmt.where || evaluate_where(stmt.where, jr.row, schema, &right_schema)) {
                filtered.push_back(std::move(jr));
            }
        }

        // ORDER BY — two-schema resolve
        if (!stmt.order_by.empty()) {
            const OrderByClause& ob = stmt.order_by[0];
            size_t col_idx = resolve_column(ob.column, schema, right_schema);
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
                project_row(stmt.columns, jr.row, schema, &right_schema));
        }
    }

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
    const JoinClause&              join) const
{
    // Resolve the ON clause columns against their respective schemas.
    // join.left_col  — column in the left table
    // join.right_col — column in the right table
    size_t left_col_idx  = resolve_column(join.left_col,  left_schema);
    size_t right_col_idx = resolve_column(join.right_col, right_schema);

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
                out.push_back({std::move(combined), 0});
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
        new_values.emplace_back(static_cast<size_t>(idx), asgn.value);
    }

    Table     tbl(schema, buffer_pool_, wal_, free_list_);
    Predicate pred = build_predicate(stmt.where, schema);

    // Validate CHECK constraints against every row's post-update values
    // *before* touching storage — a single violation aborts the whole
    // UPDATE with no rows changed, matching CHECK's statement-level atomicity.
    if (!schema.checks.empty()) {
        for (const auto& match : tbl.scan(pred)) {
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

    uint32_t count = tbl.update_where(pred, new_values);

    if (tbl.root_page() != schema.root_page) {
        catalog_.update_table_root(stmt.table_name, tbl.root_page());
    }

    return {true, "", {}, {}, count};
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Executor::execute_delete(const DeleteStmt& stmt)
{
    const TableSchema& schema = catalog_.get_table(stmt.table_name);

    Table     tbl(schema, buffer_pool_, wal_, free_list_);
    Predicate pred  = build_predicate(stmt.where, schema);
    uint32_t  count = tbl.delete_where(pred);

    if (tbl.root_page() != schema.root_page) {
        catalog_.update_table_root(stmt.table_name, tbl.root_page());
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

bool Executor::evaluate_where(const WhereExprPtr& expr,
                               const Row&          row,
                               const TableSchema&  left_schema,
                               const TableSchema*  right_schema) const
{
    if (!expr) return true;

    if (expr->kind == WhereExpr::Kind::COMPARE) {
        return evaluate_compare(expr->compare, row, left_schema, right_schema);
    }

    // LOGICAL node
    switch (expr->logical_op) {
        case LogicalOp::AND:
            return evaluate_where(expr->left,  row, left_schema, right_schema) &&
                   evaluate_where(expr->right, row, left_schema, right_schema);
        case LogicalOp::OR:
            return evaluate_where(expr->left,  row, left_schema, right_schema) ||
                   evaluate_where(expr->right, row, left_schema, right_schema);
        case LogicalOp::NOT:
            return !evaluate_where(expr->left, row, left_schema, right_schema);
    }
    return false;
}

bool Executor::evaluate_compare(const CompareExpr& expr,
                                 const Row&         row,
                                 const TableSchema& left_schema,
                                 const TableSchema* right_schema) const
{
    size_t col_idx = right_schema
        ? resolve_column(expr.column, left_schema, *right_schema)
        : resolve_column(expr.column, left_schema);
    const Value& val = row.get(col_idx);

    // IS NULL / IS NOT NULL do not examine the operand
    if (expr.op == CompareOp::IS_NULL)     return  is_null(val);
    if (expr.op == CompareOp::IS_NOT_NULL) return !is_null(val);

    // SQL NULL semantics: any comparison with NULL yields false
    if (is_null(val))          return false;
    if (is_null(expr.operand)) return false;

    // LIKE — only meaningful for VARCHAR
    if (expr.op == CompareOp::LIKE) {
        if (!std::holds_alternative<std::string>(val) ||
            !std::holds_alternative<std::string>(expr.operand)) {
            return false;
        }
        return like_match(get_string(val), get_string(expr.operand));
    }

    // General comparison via std::visit — handles same-type and int/float cross comparisons
    return std::visit([&](const auto& lhs, const auto& rhs) -> bool {
        using L = std::decay_t<decltype(lhs)>;
        using R = std::decay_t<decltype(rhs)>;

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
            float fr = static_cast<float>(rhs);
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
                case CompareOp::EQ:  return lhs == rhs;
                case CompareOp::NEQ: return lhs != rhs;
                case CompareOp::LT:  return lhs <  rhs;
                case CompareOp::GT:  return lhs >  rhs;
                case CompareOp::LTE: return lhs <= rhs;
                case CompareOp::GTE: return lhs >= rhs;
                default: return false;
            }
        }

        // Incompatible types (e.g. comparing INT with VARCHAR)
        else {
            return false;
        }
    }, val, expr.operand);
}

Predicate Executor::build_predicate(const WhereExprPtr& where,
                                     const TableSchema&  schema,
                                     const TableSchema*  right_schema) const
{
    if (!where) {
        return [](const Row&) { return true; };
    }
    // Capture by reference — safe because the predicate is only used within
    // the same execute_*() call frame, which keeps 'where' and 'schema' alive.
    return [this, &where, &schema, right_schema](const Row& row) {
        return evaluate_where(where, row, schema, right_schema);
    };
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
                "only =, !=, <, >, <=, >= are supported (not IS NULL / LIKE)");
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

    // COMPARE leaf: column OP literal
    const CompareExpr& cmp = expr->compare;
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
// Column resolution
// ─────────────────────────────────────────────────────────────────────────────

size_t Executor::resolve_column(const ColumnRef&   ref,
                                 const TableSchema& schema) const
{
    // If the column ref is table-qualified, it must match this schema's name
    if (!ref.table_name.empty() && ref.table_name != schema.name) {
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
                                 const TableSchema& right_schema) const
{
    const size_t left_width = left_schema.columns.size();

    if (!ref.table_name.empty()) {
        // Qualified reference — must match exactly one table's name
        if (ref.table_name == left_schema.name) {
            int idx = left_schema.column_index(ref.column_name);
            if (idx < 0)
                throw std::runtime_error("Unknown column '" + ref.column_name +
                                         "' in table '" + left_schema.name + "'");
            return static_cast<size_t>(idx);
        }
        if (ref.table_name == right_schema.name) {
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
        headers.push_back(sc.column.column_name);
    }
    return headers;
}

std::vector<std::string> Executor::project_row(
    const std::vector<SelectColumn>& select_cols,
    const Row&                       row,
    const TableSchema&               schema,
    const TableSchema*               joined_schema) const
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
        const std::string& col_name   = sc.column.column_name;
        const std::string& table_qual = sc.column.table_name;

        // ── Qualified reference with explicit table name ───────────────────
        if (!table_qual.empty()) {
            if (table_qual == schema.name) {
                int idx = schema.column_index(col_name);
                if (idx < 0) throw std::runtime_error("SELECT: unknown column '" + col_name + "'");
                out.push_back(value_to_string(row.get(static_cast<size_t>(idx))));
                continue;
            }
            if (joined_schema && table_qual == joined_schema->name) {
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