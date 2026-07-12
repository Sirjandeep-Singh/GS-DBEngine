#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include "../catalog/schema.h"
#include "../btree/btree.h"
#include "../btree/key.h"
#include "../btree/free_list_manager.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"

// Index wraps a single B+ tree that implements one secondary index —
// the analogue of Table for a secondary index instead of the primary key.
//
// STORAGE KEY DESIGN — this is the whole trick that lets a secondary
// index hold duplicate values without any change to BTree itself:
//
//     raw BTree key = { indexed_value[0], indexed_value[1], ...,
//                        primary_key[0],   primary_key[1],   ... }
//
// i.e. the indexed column(s)' value(s), followed by the ENTIRE primary
// key of the row that has that value. Because the primary key is
// always unique, this composite is always unique at the raw-bytes
// level even when a thousand rows share the same indexed value — so
// BTree::insert() (which throws on a truly duplicate key) never throws
// for a legitimate repeated index value. Key is already a generic
// tuple (see key.h), so this composite key is nothing new to BTree —
// it's the same mechanism a composite PRIMARY KEY already uses.
//
// The leaf VALUE is unused (stored as an empty byte vector) — the
// primary key is already inside the key itself, so a lookup is:
// prefix_scan(indexed_value) -> strip the indexed_value prefix off
// each result key -> what's left is the row's primary key -> the
// caller (Table/executor) uses that to fetch the actual row via
// Table::select_by_key(). This makes the index non-covering (one
// extra lookup hop to get the row), but keeps Index decoupled from
// RowSerializer/TableSchema entirely — same separation of concerns
// BTree already keeps from Table.
//
// UNIQUENESS: enforced here, one layer above BTree, not inside BTree.
// For a UNIQUE index, "duplicate" means "same indexed_value under a
// DIFFERENT primary key" — checked via a prefix_scan on just the
// indexed_value portion, BEFORE the (indexed_value, primary_key) key
// is written. See insert_entry() for why this check must happen
// before any write, not as a rollback after.
//
// NULLs: a Value that is NULL is never indexed (see is_indexable) —
// standard SQL practice; NULL is never subject to uniqueness and
// "WHERE indexed_col = NULL" isn't how NULL comparison works anyway.
// For a composite index, ANY NULL element in indexed_value skips the
// whole entry, same as a composite PRIMARY KEY can never contain NULL.
class Index {
public:
    // schema   : this index's IndexSchema (name, table, column(s),
    //            uniqueness, root_page) — owned by CatalogManager,
    //            Index holds a const reference and does not copy it.
    // pk_arity : number of columns in the indexed table's primary key —
    //            used only to validate primary_key.size() on every call,
    //            since Index has no TableSchema of its own to check
    //            against (mirrors how BTree has no TableSchema either).
    Index(const IndexSchema& schema, size_t pk_arity,
          BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list);

    // returns the current root page_id of the underlying B+ tree — the
    // caller must persist this via CatalogManager::update_index_root()
    // whenever it changes after insert_entry()/remove_entry().
    uint32_t root_page() const;

    // this index's schema — lets a caller (e.g. Table) resolve
    // column_names against its own TableSchema without Index needing to
    // know about TableSchema/Row at all.
    const IndexSchema& schema() const { return schema_; }

    // Throws if schema.is_unique and indexed_value already exists under
    // a primary key other than primary_key. A no-op (never throws) if
    // schema.is_unique is false, or if indexed_value contains a NULL.
    // Read-only — does not touch the tree.
    //
    // Exposed publicly (not just used internally by insert_entry) so a
    // caller coordinating a write across several trees under one shared
    // transaction — see BTree's transaction_id overload of insert() —
    // can validate every constraint on every tree BEFORE issuing any
    // write, since none of those writes can be rolled back once made.
    // insert_entry() also calls this itself, so Index stays safe to use
    // correctly even by a caller that doesn't pre-validate.
    void check_unique(const Key& indexed_value, const Key& primary_key) const;

    // Indexes one row. `indexed_value` is the tuple of values pulled
    // from the row's indexed column(s), in schema.column_names order;
    // `primary_key` is that row's full table primary key.
    //
    // Throws if schema.is_unique and indexed_value already exists under
    // a different primary key. That check runs — and, if it's going to
    // throw, throws — BEFORE any page is written, since neither this
    // class nor BTree can roll back an in-place page mutation once made
    // (see the transaction_id overload of BTree::insert for why).
    //
    // No-op if indexed_value contains a NULL (see is_indexable).
    void insert_entry(const Key& indexed_value, const Key& primary_key);

    // Same as above, but rides inside a transaction the caller already
    // opened via wal_.begin(), instead of opening/committing its own —
    // for coordinating this write atomically with the row's primary-key
    // tree write and/or other indexes' writes. Caller commits once, after
    // every tree touched under transaction_id is done. The rollback
    // caveat on BTree's transaction_id overloads applies here too: the
    // unique check below still runs before this issues any write, but
    // that only protects THIS call — validate everything across every
    // tree in the transaction before writing any of them.
    void insert_entry(uint32_t transaction_id, const Key& indexed_value, const Key& primary_key);

    // Removes the entry for exactly this (indexed_value, primary_key)
    // pair. Called on row delete, and as the "remove old value" half of
    // an update (delete old entry, insert new entry).
    // No-op if indexed_value contains a NULL (nothing was ever indexed).
    void remove_entry(const Key& indexed_value, const Key& primary_key);

    // Same as above, rides inside a caller-owned transaction_id.
    void remove_entry(uint32_t transaction_id, const Key& indexed_value, const Key& primary_key);

    // Returns every primary key currently indexed under indexed_value,
    // in ascending primary-key order. Empty if indexed_value contains a
    // NULL, or if nothing is indexed under it.
    //
    // indexed_value may be a LEFTMOST PREFIX of this index's columns —
    // for a composite index on (last_name, first_name), find({"Smith"})
    // returns every row with that last name regardless of first name,
    // same leftmost-prefix rule real engines apply to composite indexes.
    // Throws if indexed_value has MORE elements than the index has
    // columns.
    std::vector<Key> find(const Key& indexed_value) const;

    // Returns every primary key indexed under `prefix` (a leftmost
    // prefix fixed by equality, possibly empty — same rule as find())
    // whose value in the very next column falls within [lo, hi] (each
    // bound optional/independent inclusivity), in ascending order —
    // real engines' rule that a range condition can only be the last
    // condition in a leftmost prefix, e.g. an index on (dept, age):
    // `dept = 'eng' AND age > 25` is prefix = {"eng"}, lo = 25;
    // `age > 25` alone on a single-column age index is prefix = {}.
    //
    // At least one of lo/hi must be set — this is the range-lookup
    // counterpart of find(), not a substitute for it. Throws if
    // prefix.size() >= this index's column count (no column left to
    // range over) or if neither lo nor hi is set. Empty (never throws)
    // if prefix or a bound value contains NULL — NULL is never indexed
    // and never satisfies a comparison, same convention find()/
    // is_indexable use.
    std::vector<Key> find_range(const Key&                  prefix,
                                 const std::optional<Value>& lo, bool lo_inclusive,
                                 const std::optional<Value>& hi, bool hi_inclusive) const;

    // Covering-read variants of find()/find_range(): same lookup, but
    // return the FULL indexed-column tuple alongside each primary key
    // instead of discarding it. A caller whose SELECT list, ORDER BY, and
    // WHERE columns all lie within this index's columns ∪ the table's
    // primary key can build result rows directly from these pairs — no
    // Table::select_by_key() fetch needed at all (item H, "covering index
    // reads"). Same preconditions/semantics as find()/find_range()
    // respectively; see those for details.
    std::vector<std::pair<Key, Key>> find_with_values(const Key& indexed_value) const;
    std::vector<std::pair<Key, Key>> find_range_with_values(
        const Key&                  prefix,
        const std::optional<Value>& lo, bool lo_inclusive,
        const std::optional<Value>& hi, bool hi_inclusive) const;

    // true if a value tuple is eligible to be indexed at all — false if
    // any element is NULL. Used consistently by insert_entry, the
    // uniqueness check, and find(), so NULL handling can't drift between
    // write and read paths.
    static bool is_indexable(const Key& indexed_value);

private:
    const IndexSchema& schema_;
    size_t              pk_arity_;
    BTree               btree_;

    // {indexed_value..., primary_key...} — the raw BTree key for one entry.
    Key make_entry_key(const Key& indexed_value, const Key& primary_key) const;
};
