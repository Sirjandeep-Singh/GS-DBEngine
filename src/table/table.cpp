#include "table.h"

#include <stdexcept>
#include "../row/serializer.h"
#include "../btree/key.h"

namespace {
// The BTree layer now stores generic Key (vector<Value>) — a 1-element
// Key for a scalar column, or an N-element Key for a composite one. Table
// still only exposes single-column INT primary keys in this pass (that's
// what extract_primary_key() enforces below); composite/non-int PRIMARY
// KEY support is wired up at the schema/parser layer in a follow-up. These
// two helpers are the adapter between Table's existing uint32_t PK API and
// the BTree's new generic Key API, so nothing above Table needs to change
// yet.
Key to_key(uint32_t primary_key) {
    return Key{ static_cast<int32_t>(primary_key) };
}

uint32_t from_key(const Key& key) {
    return static_cast<uint32_t>(get_int(key.at(0)));
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
    // scan all rows to find the current maximum primary key
    // so that after a restart, auto-increment continues from the right value
    auto all = btree_.scan_all();
    if (!all.empty()) {
        next_auto_increment_ = from_key(all.back().first) + 1;
    }
}

uint32_t Table::extract_primary_key(const Row& row) const {
    const Value& pk_val = row.get(schema_.primary_key_index);
    if (!std::holds_alternative<int32_t>(pk_val)) {
        throw std::runtime_error("Table::extract_primary_key: primary key must be INT");
    }
    int32_t key = std::get<int32_t>(pk_val);
    if (key < 0) {
        throw std::runtime_error("Table::extract_primary_key: primary key must be non-negative");
    }
    return static_cast<uint32_t>(key);
}

// ─────────────────────────────────────────────
// INSERT
// ─────────────────────────────────────────────

uint32_t Table::insert(Row row) {
    const Column& pk_col = schema_.primary_key_column();

    // handle auto-increment — if PK is NULL and column is auto_increment, assign next value
    if (pk_col.auto_increment) {
        Value& pk_val = row.get(schema_.primary_key_index);
        if (is_null(pk_val)) {
            pk_val = static_cast<int32_t>(next_auto_increment_);
        }
    }

    uint32_t primary_key = extract_primary_key(row);

    // verify key doesn't already exist
    if (btree_.search(to_key(primary_key)).has_value()) {
        throw std::runtime_error(
            "Table::insert: duplicate primary key " + std::to_string(primary_key));
    }

    auto bytes = RowSerializer::serialize(row, schema_);
    btree_.insert(to_key(primary_key), bytes);

    // advance auto-increment counter past the inserted key
    if (primary_key >= next_auto_increment_) {
        next_auto_increment_ = primary_key + 1;
    }

    return primary_key;
}

// ─────────────────────────────────────────────
// SELECT
// ─────────────────────────────────────────────

std::optional<Row> Table::select_by_key(uint32_t primary_key) const {
    auto bytes = btree_.search(to_key(primary_key));
    if (!bytes.has_value()) return std::nullopt;
    return RowSerializer::deserialize(*bytes, schema_);
}

std::vector<ScanResult> Table::scan(const Predicate& predicate) const {
    std::vector<ScanResult> results;
    auto all = btree_.scan_all();

    for (auto& [key, bytes] : all) {
        Row row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(row)) {
            results.push_back({from_key(key), std::move(row)});
        }
    }

    return results;
}

// ─────────────────────────────────────────────
// UPDATE
// ─────────────────────────────────────────────

void Table::update(uint32_t primary_key,
                   const std::vector<std::pair<size_t, Value>>& new_values) {
    // verify key exists and fetch the current row
    auto bytes = btree_.search(to_key(primary_key));
    if (!bytes.has_value()) {
        throw std::runtime_error(
            "Table::update: primary key not found " + std::to_string(primary_key));
    }

    Row row = RowSerializer::deserialize(*bytes, schema_);

    // apply new values
    for (auto& [col_idx, new_val] : new_values) {
        if (col_idx == schema_.primary_key_index) {
            throw std::runtime_error(
                "Table::update: cannot change the primary key column");
        }
        if (col_idx >= schema_.columns.size()) {
            throw std::runtime_error(
                "Table::update: column index out of range " + std::to_string(col_idx));
        }
        row.get(col_idx) = new_val;
    }

    // delete old blob, reinsert new blob
    btree_.remove(to_key(primary_key));
    auto new_bytes = RowSerializer::serialize(row, schema_);
    btree_.insert(to_key(primary_key), new_bytes);
}

// ─────────────────────────────────────────────
// DELETE
// ─────────────────────────────────────────────

void Table::delete_row(uint32_t primary_key) {
    if (!btree_.search(to_key(primary_key)).has_value()) {
        throw std::runtime_error(
            "Table::delete_row: primary key not found " + std::to_string(primary_key));
    }
    btree_.remove(to_key(primary_key));
}

// ─────────────────────────────────────────────
// Scan + DELETE / UPDATE
// ─────────────────────────────────────────────

uint32_t Table::delete_where(const Predicate& predicate) {
    // phase 1: collect all matching primary keys without modifying the tree
    auto all = btree_.scan_all();
    std::vector<uint32_t> to_delete;

    for (auto& [key, bytes] : all) {
        Row row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(row)) {
            to_delete.push_back(from_key(key));
        }
    }

    // phase 2: delete after scan is complete
    for (uint32_t key : to_delete) {
        btree_.remove(to_key(key));
    }

    return static_cast<uint32_t>(to_delete.size());
}

uint32_t Table::update_where(const Predicate& predicate,
                              const std::vector<std::pair<size_t, Value>>& new_values) {
    // phase 1: collect matching keys and their updated rows without touching the tree
    auto all = btree_.scan_all();
    std::vector<std::pair<uint32_t, Row>> to_update;

    for (auto& [key, bytes] : all) {
        Row row = RowSerializer::deserialize(bytes, schema_);
        if (predicate(row)) {
            // apply new values to a copy of the row
            for (auto& [col_idx, new_val] : new_values) {
                if (col_idx == schema_.primary_key_index) {
                    throw std::runtime_error(
                        "Table::update_where: cannot change the primary key column");
                }
                row.get(col_idx) = new_val;
            }
            to_update.emplace_back(from_key(key), std::move(row));
        }
    }

    // phase 2: delete + reinsert after scan is complete
    for (auto& [key, new_row] : to_update) {
        btree_.remove(to_key(key));
        auto new_bytes = RowSerializer::serialize(new_row, schema_);
        btree_.insert(to_key(key), new_bytes);
    }

    return static_cast<uint32_t>(to_update.size());
}