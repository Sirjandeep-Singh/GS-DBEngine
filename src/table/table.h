#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

#include "../catalog/schema.h"
#include "../row/row.h"
#include "../btree/btree.h"
#include "../btree/free_list_manager.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"

// A ScanResult is a matched row plus its primary key.
// The primary key is included so UPDATE and DELETE can locate the row for modification.
// Key is a generic vector<Value> — a 1-element key for a scalar (INT,
// VARCHAR, ...) primary key, or an N-element key for a composite
// table-level PRIMARY KEY (a, b, ...).
struct ScanResult {
    Key primary_key;
    Row row;
};

// A predicate is a function that takes a Row and returns true if it matches.
// Used for WHERE clause filtering during scans.
using Predicate = std::function<bool(const Row&)>;

// Table wraps a single B+ tree and provides row-level CRUD operations.
// It is the only layer that combines schema knowledge (column types, nullable,
// auto-increment) with B+ tree storage to produce meaningful row operations.
//
// One Table object corresponds to one table in the database.
// The Table Layer is responsible for:
//   - Serializing Row objects into byte blobs for BTree storage
//   - Deserializing byte blobs from BTree back into Row objects
//   - Auto-increment: tracking and assigning the next primary key
//   - Filtering: applying predicates to rows during scans
//
// The Table Layer does NOT:
//   - Parse SQL
//   - Handle JOINs
//   - Manage schema persistence (that is CatalogManager's job)
//   - Manage page I/O directly (that is BufferPool's job)

class Table {
public:
    // schema    : the table's column definitions — owned by CatalogManager,
    //             Table holds a const reference and does not copy it
    // buffer_pool, wal, free_list : passed through to BTree
    Table(const TableSchema& schema, BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list);

    // returns the current root page_id of the underlying B+ tree.
    // the caller (executor/database class) must persist this via
    // CatalogManager::update_table_root() whenever it changes after an insert.
    uint32_t root_page() const;

    // ---- INSERT ----

    // inserts a new row into the table.
    // if the primary key column is auto-increment and the value is NULL,
    // the next auto-increment value is assigned automatically. auto-increment
    // only applies to a single-column INT primary key (never a composite one).
    // throws if a non-auto-increment primary key is NULL.
    // throws if the primary key already exists (duplicate).
    // returns the primary key that was inserted (useful for auto-increment).
    Key insert(Row row);

    // ---- SELECT ----

    // returns the row with the given primary key, or std::nullopt if not found.
    // `key` must have exactly schema.primary_key_indices.size() elements.
    std::optional<Row> select_by_key(const Key& key) const;

    // scans all rows in the table, returning those that match the predicate.
    // pass a predicate that always returns true to return all rows.
    // rows are returned in ascending primary key order (leaf linked list order).
    std::vector<ScanResult> scan(const Predicate& predicate) const;

    // ---- UPDATE ----

    // updates the row with the given primary key by applying `new_values`.
    // new_values is a map of column_index -> new Value.
    // implemented as delete + reinsert to handle variable-size rows correctly.
    // throws if the primary key does not exist.
    // throws if new_values attempts to change a primary key column.
    void update(const Key& key, const std::vector<std::pair<size_t, Value>>& new_values);

    // ---- DELETE ----

    // deletes the row with the given primary key.
    // throws if the primary key does not exist.
    void delete_row(const Key& key);

    // ---- SCAN with UPDATE/DELETE ----

    // scans all rows matching the predicate and deletes them.
    // returns the number of rows deleted.
    uint32_t delete_where(const Predicate& predicate);

    // scans all rows matching the predicate and updates them.
    // returns the number of rows updated.
    uint32_t update_where(const Predicate& predicate,
                          const std::vector<std::pair<size_t, Value>>& new_values);

private:
    const TableSchema& schema_;
    BTree              btree_;
    // next value for an auto-increment PK column. Only meaningful when the
    // primary key is a single-column INT with auto_increment set — a
    // composite primary key can never be auto-increment (see insert()).
    uint32_t           next_auto_increment_;

    // builds the BTree Key for a row: the tuple of values at
    // schema_.primary_key_indices, in order. Throws if any primary key
    // column holds NULL (a primary key value can never be NULL).
    Key extract_primary_key(const Row& row) const;

    // initializes next_auto_increment_ by scanning the tree for the current max key.
    // no-op (stays at 1) when the primary key isn't a single auto-increment INT column.
    void init_auto_increment();
};