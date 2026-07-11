#include "table.h"

#include <stdexcept>
#include "../row/serializer.h"
#include "../btree/key.h"

namespace {
// True when the schema's primary key is a single INT column with
// auto_increment set. That's the only shape auto-increment ever applies
// to — a composite key has no single "next value" to assign, and a
// non-INT column has no natural successor.
bool is_single_int_auto_increment(const TableSchema& schema) {
    return schema.primary_key_indices.size() == 1 &&
           schema.primary_key_column().type == ColumnType::INT &&
           schema.primary_key_column().auto_increment;
}
}

Table::Table(const TableSchema& schema, BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list,
             std::vector<Index*> indexes)
    : schema_(schema), btree_(buffer_pool, wal, free_list, schema.root_page), wal_(wal), next_auto_increment_(1)
{
    indexes_.reserve(indexes.size());
    for (Index* idx : indexes) {
        if (idx->schema().table_name != schema_.name) {
            throw std::runtime_error(
                "Table: index '" + idx->schema().name + "' belongs to table '" +
                idx->schema().table_name + "', not '" + schema_.name + "'");
        }

        IndexBinding binding;
        binding.index = idx;
        binding.column_indices.reserve(idx->schema().column_names.size());
        for (const std::string& col_name : idx->schema().column_names) {
            int col_idx = schema_.column_index(col_name);
            if (col_idx < 0) {
                throw std::runtime_error(
                    "Table: index '" + idx->schema().name + "' references unknown column '" +
                    col_name + "' on table '" + schema_.name + "'");
            }
            binding.column_indices.push_back(static_cast<uint32_t>(col_idx));
        }
        indexes_.push_back(std::move(binding));
    }

    init_auto_increment();
}

uint32_t Table::root_page() const {
    return btree_.root_page();
}

std::vector<std::pair<std::string, uint32_t>> Table::index_root_pages() const {
    std::vector<std::pair<std::string, uint32_t>> result;
    result.reserve(indexes_.size());
    for (const IndexBinding& binding : indexes_) {
        result.emplace_back(binding.index->schema().name, binding.index->root_page());
    }
    return result;
}

// ─────────────────────────────────────────────
// Auto-increment
// ─────────────────────────────────────────────

void Table::init_auto_increment() {
    // only a single-column INT auto-increment PK has a meaningful "next value"
    if (!is_single_int_auto_increment(schema_)) return;

    // scan all rows to find the current maximum primary key
    // so that after a restart, auto-increment continues from the right value
    auto all = btree_.scan_all();
    if (!all.empty()) {
        uint32_t max_key = static_cast<uint32_t>(get_int(all.back().first.at(0)));
        next_auto_increment_ = max_key + 1;
    }
}

Key Table::extract_primary_key(const Row& row) const {
    Key key;
    key.reserve(schema_.primary_key_indices.size());
    for (uint32_t col_idx : schema_.primary_key_indices) {
        const Value& pk_val = row.get(col_idx);
        if (is_null(pk_val)) {
            throw std::runtime_error(
                "Table::extract_primary_key: primary key column '" +
                schema_.columns[col_idx].name + "' cannot be NULL");
        }
        key.push_back(pk_val);
    }
    return key;
}

Key Table::extract_indexed_value(const Row& row, const IndexBinding& binding) const {
    Key value;
    value.reserve(binding.column_indices.size());
    for (uint32_t col_idx : binding.column_indices) {
        value.push_back(row.get(col_idx));
    }
    return value;
}

// ─────────────────────────────────────────────
// Index maintenance
// ─────────────────────────────────────────────

void Table::check_all_unique_constraints(const Row& row, const Key& primary_key) const {
    for (const IndexBinding& binding : indexes_) {
        Key indexed_value = extract_indexed_value(row, binding);
        binding.index->check_unique(indexed_value, primary_key);
    }
}

void Table::write_row_and_indexes(uint32_t transaction_id, const Key& primary_key,
                                   const Row& row, const std::vector<uint8_t>& bytes) {
    btree_.insert(transaction_id, primary_key, bytes);
    for (const IndexBinding& binding : indexes_) {
        Key indexed_value = extract_indexed_value(row, binding);
        binding.index->insert_entry(transaction_id, indexed_value, primary_key);
    }
}

void Table::remove_row_and_indexes(uint32_t transaction_id, const Key& primary_key, const Row& old_row) {
    btree_.remove(transaction_id, primary_key);
    for (const IndexBinding& binding : indexes_) {
        Key indexed_value = extract_indexed_value(old_row, binding);
        binding.index->remove_entry(transaction_id, indexed_value, primary_key);
    }
}

// ─────────────────────────────────────────────
// INSERT
// ─────────────────────────────────────────────

Key Table::insert(Row row) {
    // handle auto-increment — if PK is NULL and column is auto_increment, assign next value.
    // only ever applies to a single-column INT primary key.
    if (is_single_int_auto_increment(schema_)) {
        Value& pk_val = row.get(schema_.primary_key_indices[0]);
        if (is_null(pk_val)) {
            pk_val = static_cast<int32_t>(next_auto_increment_);
        }
    }

    Key primary_key = extract_primary_key(row);

    // verify key doesn't already exist
    if (btree_.search(primary_key).has_value()) {
        throw std::runtime_error("Table::insert: duplicate primary key");
    }

    // validate every UNIQUE index BEFORE writing anything — none of the
    // writes below can be rolled back once issued (see the class-level
    // "Index maintenance" note), so every check that can throw has to
    // happen first.
    check_all_unique_constraints(row, primary_key);

    auto bytes = RowSerializer::serialize(row, schema_);

    uint32_t transaction_id = wal_.begin();
    write_row_and_indexes(transaction_id, primary_key, row, bytes);
    wal_.commit(transaction_id);

    // advance auto-increment counter past the inserted key
    if (is_single_int_auto_increment(schema_)) {
        uint32_t key_val = static_cast<uint32_t>(get_int(primary_key.at(0)));
        if (key_val >= next_auto_increment_) {
            next_auto_increment_ = key_val + 1;
        }
    }

    return primary_key;
}

// ─────────────────────────────────────────────
// SELECT
// ─────────────────────────────────────────────

namespace {
bool is_primary_key_column(const TableSchema& schema, size_t col_idx) {
    for (uint32_t pk_idx : schema.primary_key_indices) {
        if (pk_idx == col_idx) return true;
    }
    return false;
}
}

std::optional<Row> Table::select_by_key(const Key& key) const {
    auto bytes = btree_.search(key);
    if (!bytes.has_value()) return std::nullopt;
    return RowSerializer::deserialize(*bytes, schema_);
}

std::vector<ScanResult> Table::scan(const Predicate& predicate) const {
    std::vector<ScanResult> results;
    auto all = btree_.scan_all();

    for (auto& [key, bytes] : all) {
        Row row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(row)) {
            results.push_back({key, std::move(row)});
        }
    }

    return results;
}

// ─────────────────────────────────────────────
// UPDATE
// ─────────────────────────────────────────────

void Table::update(const Key& primary_key,
                   const std::vector<std::pair<size_t, Value>>& new_values) {
    // verify key exists and fetch the current row
    auto bytes = btree_.search(primary_key);
    if (!bytes.has_value()) {
        throw std::runtime_error("Table::update: primary key not found");
    }

    Row old_row = RowSerializer::deserialize(*bytes, schema_);
    Row new_row = old_row;

    // apply new values to the copy
    for (auto& [col_idx, new_val] : new_values) {
        if (is_primary_key_column(schema_, col_idx)) {
            throw std::runtime_error(
                "Table::update: cannot change a primary key column");
        }
        if (col_idx >= schema_.columns.size()) {
            throw std::runtime_error(
                "Table::update: column index out of range " + std::to_string(col_idx));
        }
        new_row.get(col_idx) = new_val;
    }

    // validate every UNIQUE index against the NEW row before writing
    // anything. check_unique() naturally allows a row to "update to its
    // own current value" — an existing entry under the new value with
    // this same primary_key is not treated as a violation — so this is
    // safe to run before the old entry has even been removed yet.
    check_all_unique_constraints(new_row, primary_key);

    auto new_bytes = RowSerializer::serialize(new_row, schema_);

    // delete old blob + old index entries, reinsert new blob + new index
    // entries, all under one transaction.
    uint32_t transaction_id = wal_.begin();
    remove_row_and_indexes(transaction_id, primary_key, old_row);
    write_row_and_indexes(transaction_id, primary_key, new_row, new_bytes);
    wal_.commit(transaction_id);
}

// ─────────────────────────────────────────────
// DELETE
// ─────────────────────────────────────────────

void Table::delete_row(const Key& primary_key) {
    auto bytes = btree_.search(primary_key);
    if (!bytes.has_value()) {
        throw std::runtime_error("Table::delete_row: primary key not found");
    }

    Row old_row = RowSerializer::deserialize(*bytes, schema_);

    uint32_t transaction_id = wal_.begin();
    remove_row_and_indexes(transaction_id, primary_key, old_row);
    wal_.commit(transaction_id);
}

// ─────────────────────────────────────────────
// Scan + DELETE / UPDATE
// ─────────────────────────────────────────────

uint32_t Table::delete_where(const Predicate& predicate) {
    // phase 1: collect all matching rows (with their current values, for
    // index removal) without modifying any tree
    auto all = btree_.scan_all();
    std::vector<ScanResult> to_delete;

    for (auto& [key, bytes] : all) {
        Row row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(row)) {
            to_delete.push_back({key, std::move(row)});
        }
    }

    // phase 2: delete after scan is complete — one transaction per row,
    // covering that row's primary-key removal and every index's removal
    // together (same granularity delete_row() uses).
    for (const ScanResult& r : to_delete) {
        uint32_t transaction_id = wal_.begin();
        remove_row_and_indexes(transaction_id, r.primary_key, r.row);
        wal_.commit(transaction_id);
    }

    return static_cast<uint32_t>(to_delete.size());
}

uint32_t Table::update_where(const Predicate& predicate,
                              const std::vector<std::pair<size_t, Value>>& new_values) {
    // phase 1: collect matching keys, their old rows, and their updated
    // rows, without touching any tree. Validate every UNIQUE index for
    // every updated row up front — before any of them are written — same
    // as update() does for a single row.
    auto all = btree_.scan_all();
    std::vector<std::pair<Key, std::pair<Row, Row>>> to_update;  // key -> (old_row, new_row)

    // check_all_unique_constraints() only validates against what's
    // currently on disk — it has no way to know about OTHER rows in this
    // same batch that haven't been written yet. Two rows both being
    // updated to the same new value under a UNIQUE index would otherwise
    // each pass that check independently (neither is on disk yet when
    // the other is checked), and the collision would only surface once
    // phase 2 tried to write the second one — after the first was
    // already committed. So track, per unique index, every value already
    // claimed earlier in this same batch, and check new rows against
    // that too. One vector per index, parallel to indexes_.
    std::vector<std::vector<Key>> batch_claimed(indexes_.size());

    for (auto& [key, bytes] : all) {
        Row old_row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(old_row)) {
            Row new_row = old_row;
            for (auto& [col_idx, new_val] : new_values) {
                if (is_primary_key_column(schema_, col_idx)) {
                    throw std::runtime_error(
                        "Table::update_where: cannot change a primary key column");
                }
                new_row.get(col_idx) = new_val;
            }

            check_all_unique_constraints(new_row, key);

            for (size_t i = 0; i < indexes_.size(); i++) {
                const IndexBinding& binding = indexes_[i];
                if (!binding.index->schema().is_unique) continue;

                Key indexed_value = extract_indexed_value(new_row, binding);
                if (!Index::is_indexable(indexed_value)) continue;

                for (const Key& claimed : batch_claimed[i]) {
                    if (claimed == indexed_value) {
                        throw std::runtime_error(
                            "Table::update_where: two rows in this update would collide on unique index '" +
                            binding.index->schema().name + "'");
                    }
                }
                batch_claimed[i].push_back(indexed_value);
            }

            to_update.emplace_back(key, std::make_pair(std::move(old_row), std::move(new_row)));
        }
    }

    // phase 2: delete old + reinsert new, after scan and validation are
    // complete — one transaction per row, covering that row's
    // primary-key write and every index's write together.
    for (auto& [key, rows] : to_update) {
        auto& [old_row, new_row] = rows;
        auto new_bytes = RowSerializer::serialize(new_row, schema_);

        uint32_t transaction_id = wal_.begin();
        remove_row_and_indexes(transaction_id, key, old_row);
        write_row_and_indexes(transaction_id, key, new_row, new_bytes);
        wal_.commit(transaction_id);
    }

    return static_cast<uint32_t>(to_update.size());
}