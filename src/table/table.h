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
#include "../index/index.h"

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
//   - Keeping every secondary Index passed to it in sync with the primary
//     key tree — every INSERT/UPDATE/DELETE that touches the primary key
//     tree also writes/removes the matching entry in each Index, as one
//     atomic WAL transaction spanning all of them (see the "Index
//     maintenance" section below, and BTree's transaction_id-threaded
//     insert()/remove() overloads for why this has to be one transaction
//     rather than several).
//
// The Table Layer does NOT:
//   - Parse SQL
//   - Handle JOINs
//   - Manage schema persistence (that is CatalogManager's job)
//   - Manage page I/O directly (that is BufferPool's job)
//   - Decide *which* indexes exist on this table, or construct them —
//     that's the caller's job (see the `indexes` constructor parameter)

class Table {
public:
    // schema    : the table's column definitions — owned by CatalogManager,
    //             Table holds a const reference and does not copy it
    // buffer_pool, wal, free_list : passed through to BTree
    // indexes   : every secondary Index on this table, already constructed
    //             by the caller (see Index maintenance notes above) — Table
    //             holds these as non-owning pointers, same as it holds
    //             `schema` by const reference. Empty by default, so
    //             existing callers with no indexes don't need to change.
    //             Every entry's Index::schema().table_name must equal
    //             schema.name (checked in the constructor).
    Table(const TableSchema& schema, BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list,
          std::vector<Index*> indexes = {});

    // returns the current root page_id of the underlying B+ tree.
    // the caller (executor/database class) must persist this via
    // CatalogManager::update_table_root() whenever it changes after an insert.
    uint32_t root_page() const;

    // returns {index_name, current_root_page} for every index passed to
    // this Table, in the same order they were given. Read after any
    // mutating call (insert/update/delete/*_where) — the caller must
    // persist each one via CatalogManager::update_index_root() the same
    // way it already does for root_page() above, since an index's B+
    // tree root can change on any write to it, exactly like the primary
    // key tree's root can.
    std::vector<std::pair<std::string, uint32_t>> index_root_pages() const;

    // ---- INSERT ----

    // inserts a new row into the table.
    // if the primary key column is auto-increment and the value is NULL,
    // the next auto-increment value is assigned automatically. auto-increment
    // only applies to a single-column INT primary key (never a composite one).
    // throws if a non-auto-increment primary key is NULL.
    // throws if the primary key already exists (duplicate).
    // throws if this row's value violates a UNIQUE index on this table.
    // Every check that can throw runs BEFORE any page is written — see
    // "Index maintenance" below — so a thrown exception here means
    // nothing was written at all, to the primary key tree or any index.
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
    // implemented as delete + reinsert to handle variable-size rows correctly
    // — including when new_values changes a primary key column, which now
    // moves the row to its new position in the B+ tree (still one delete +
    // one reinsert, just at two different keys instead of the same one).
    // throws if the primary key does not exist.
    // throws if a primary key column ends up NULL.
    // throws if the resulting primary key already belongs to a DIFFERENT
    // row (only relevant when a primary key column is actually changing —
    // updating a row to its own current key is always fine).
    // throws if the resulting row violates a UNIQUE index on this table —
    // checked, like insert(), before any page is written.
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

    // Same operation as update_where(), but seeded from a caller-supplied
    // list of already-matched rows instead of a fresh btree_.scan_all() —
    // e.g. Executor::scan_with_index()'s index-narrowed candidates, so an
    // indexed UPDATE doesn't pay for a full table scan just to re-find
    // rows it already located. Every other guarantee is identical: phase
    // 1 validates every matched row's UNIQUE indexes against every OTHER
    // row in the same batch (batch_claimed) before phase 2 writes any of
    // them, so a multi-row UPDATE stays atomic on a uniqueness collision
    // regardless of how `matched_rows` was produced.
    //
    // Like update(), a primary key column may be among new_values — each
    // affected row moves to its new key. Also like update(), throws if a
    // resulting primary key already belongs to a DIFFERENT row — including
    // when that different row is itself another row IN this same batch:
    // supporting a same-statement key swap or chain (row A moving into
    // row B's about-to-vacate slot) would require reordering every row's
    // remove before every row's write, widening the crash-recovery window
    // from "one row" to "the whole statement" — not attempted. Split such
    // an UPDATE into separate statements instead.
    //
    // `matched_rows` is trusted as-is — it is the caller's job to ensure
    // every row in it actually satisfies the intended WHERE clause (an
    // index only narrows candidates; Executor re-checks the full
    // predicate before including a row here). Passing rows that don't
    // belong will update rows a caller didn't mean to touch.
    uint32_t update_matched(const std::vector<ScanResult>& matched_rows,
                             const std::vector<std::pair<size_t, Value>>& new_values);

private:
    const TableSchema& schema_;
    BTree              btree_;
    // Needed so Table can open/commit its own WAL transactions that span
    // btree_ plus every index in indexes_ — see the "Index maintenance"
    // section above. BTree already holds its own reference to the same
    // WALManager internally; this is a separate reference at the Table
    // level because index maintenance needs to open ONE transaction that
    // covers btree_ and every Index's tree together, which none of those
    // objects can do on their own.
    WALManager&        wal_;
    // next value for an auto-increment PK column. Only meaningful when the
    // primary key is a single-column INT with auto_increment set — a
    // composite primary key can never be auto-increment (see insert()).
    uint32_t           next_auto_increment_;

    // One entry per Index passed to the constructor: the Index itself,
    // plus its indexed column(s) pre-resolved to column indices into
    // schema_.columns (resolved once here, instead of a string lookup
    // per row on every write).
    struct IndexBinding {
        Index*                 index;
        std::vector<uint32_t>  column_indices;
    };
    std::vector<IndexBinding> indexes_;

    // builds the BTree Key for a row: the tuple of values at
    // schema_.primary_key_indices, in order. Throws if any primary key
    // column holds NULL (a primary key value can never be NULL).
    Key extract_primary_key(const Row& row) const;

    // builds the indexed-value Key for a row, for one IndexBinding: the
    // tuple of values at binding.column_indices, in order. Unlike
    // extract_primary_key, does NOT throw on NULL — an indexed column
    // may legally be NULL (Index::is_indexable is what decides such a
    // row is simply never entered into that index, not an error).
    Key extract_indexed_value(const Row& row, const IndexBinding& binding) const;

    // initializes next_auto_increment_ by scanning the tree for the current max key.
    // no-op (stays at 1) when the primary key isn't a single auto-increment INT column.
    void init_auto_increment();

    // Runs every IndexBinding's Index::check_unique() for `primary_key`'s
    // row against `row`'s current values, throwing on the first
    // violation found. Called before any page is written, for both
    // insert() and update() — see the class-level "Index maintenance"
    // note for why validation has to happen up front rather than via
    // rollback after a write fails partway through.
    void check_all_unique_constraints(const Row& row, const Key& primary_key) const;

    // Writes `bytes` for `primary_key` into btree_, and inserts the
    // matching entry into every index in indexes_ derived from `row` —
    // all under one transaction_id the caller already opened via
    // wal_.begin(). Does not call check_all_unique_constraints() itself;
    // the caller must have already validated everything that can throw,
    // since none of these writes can be rolled back once issued.
    void write_row_and_indexes(uint32_t transaction_id, const Key& primary_key,
                                const Row& row, const std::vector<uint8_t>& bytes);

    // Removes `primary_key` from btree_, and removes the matching entry
    // from every index in indexes_ derived from `old_row` — all under
    // one transaction_id the caller already opened via wal_.begin().
    void remove_row_and_indexes(uint32_t transaction_id, const Key& primary_key, const Row& old_row);
};