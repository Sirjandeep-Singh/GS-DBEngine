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

Table::Table(const TableSchema& schema, BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list)
    : schema_(schema), btree_(buffer_pool, wal, free_list, schema.root_page), next_auto_increment_(1)
{
    init_auto_increment();
}

uint32_t Table::root_page() const {
    return btree_.root_page();
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

    auto bytes = RowSerializer::serialize(row, schema_);
    btree_.insert(primary_key, bytes);

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

    Row row = RowSerializer::deserialize(*bytes, schema_);

    // apply new values
    for (auto& [col_idx, new_val] : new_values) {
        if (is_primary_key_column(schema_, col_idx)) {
            throw std::runtime_error(
                "Table::update: cannot change a primary key column");
        }
        if (col_idx >= schema_.columns.size()) {
            throw std::runtime_error(
                "Table::update: column index out of range " + std::to_string(col_idx));
        }
        row.get(col_idx) = new_val;
    }

    // delete old blob, reinsert new blob
    btree_.remove(primary_key);
    auto new_bytes = RowSerializer::serialize(row, schema_);
    btree_.insert(primary_key, new_bytes);
}

// ─────────────────────────────────────────────
// DELETE
// ─────────────────────────────────────────────

void Table::delete_row(const Key& primary_key) {
    if (!btree_.search(primary_key).has_value()) {
        throw std::runtime_error("Table::delete_row: primary key not found");
    }
    btree_.remove(primary_key);
}

// ─────────────────────────────────────────────
// Scan + DELETE / UPDATE
// ─────────────────────────────────────────────

uint32_t Table::delete_where(const Predicate& predicate) {
    // phase 1: collect all matching primary keys without modifying the tree
    auto all = btree_.scan_all();
    std::vector<Key> to_delete;

    for (auto& [key, bytes] : all) {
        Row row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(row)) {
            to_delete.push_back(key);
        }
    }

    // phase 2: delete after scan is complete
    for (const Key& key : to_delete) {
        btree_.remove(key);
    }

    return static_cast<uint32_t>(to_delete.size());
}

uint32_t Table::update_where(const Predicate& predicate,
                              const std::vector<std::pair<size_t, Value>>& new_values) {
    // phase 1: collect matching keys and their updated rows without touching the tree
    auto all = btree_.scan_all();
    std::vector<std::pair<Key, Row>> to_update;

    for (auto& [key, bytes] : all) {
        Row row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(row)) {
            // apply new values to a copy of the row
            for (auto& [col_idx, new_val] : new_values) {
                if (is_primary_key_column(schema_, col_idx)) {
                    throw std::runtime_error(
                        "Table::update_where: cannot change a primary key column");
                }
                row.get(col_idx) = new_val;
            }
            to_update.emplace_back(key, std::move(row));
        }
    }

    // phase 2: delete + reinsert after scan is complete
    for (auto& [key, new_row] : to_update) {
        btree_.remove(key);
        auto new_bytes = RowSerializer::serialize(new_row, schema_);
        btree_.insert(key, new_bytes);
    }

    return static_cast<uint32_t>(to_update.size());
}