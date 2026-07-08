#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <optional>
#include "btree_node.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"
#include "./free_list_manager.h"

// BTree implements a disk-backed B+ tree, one tree per table or index.
// All page access goes through BufferPool — BTree never touches DiskManager.
// All page modifications go through WALManager for durability.
// All page allocation/reclamation goes through FreeListManager instead of
// BufferPool::new_page() directly, so freed pages get reused instead of
// leaking forever (see merge_with_sibling()).
//
// Keys are uint32_t (matches auto-increment INT primary keys).
// Values are opaque byte blobs (serialized row data) — BTree does not
// interpret the value, that is the Table Layer's responsibility.
//
// Deletion supports underflow handling via redistribution (borrowing from
// a sibling) and merging, with merges cascading upward through parents
// when necessary — full B+ tree delete semantics.
//
// TRANSACTION MODEL — CHANGED:
// Previously, write_page_through_wal() called wal_.begin()/wal_.commit()
// on every individual page write. That meant a single logical operation
// touching multiple pages (e.g. a leaf split writing the left leaf, the
// new right leaf, and the parent) was actually committed as several
// independent transactions. That breaks atomicity across the operation:
// a crash between those commits could leave the tree with a child split
// written but the parent never updated to point at it, and there was no
// single transaction a free-list header update could ever attach to.
//
// Now, every public mutating entry point (insert, remove) opens exactly
// one transaction via wal_.begin(), threads that transaction_id through
// every private helper it calls (including nested splits/merges/
// redistribution and any FreeListManager allocate/free calls), and
// commits exactly once via wal_.commit() when the whole operation is
// done. This is what makes it possible for a free-list pop or push to
// be atomic with the rest of the operation that triggered it — they're
// now genuinely part of the same transaction, not just adjacent calls.

class BTree {
public:
    // root_page: page_id of an existing tree's root, or INVALID_PAGE to create a new tree
    BTree(BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list, uint32_t root_page);

    // returns the current root page_id — the Table/Catalog layer must persist
    // this value (e.g. via CatalogManager::update_table_root) whenever it changes
    uint32_t root_page() const;

    // inserts a key-value pair. Throws if the key already exists.
    // may cause node splits, which can change the root page.
    // Runs as a single WAL transaction covering every page touched.
    void insert(uint32_t key, const std::vector<uint8_t>& value);

    // searches for a key. Returns the value if found, std::nullopt otherwise.
    // Read-only — does not open a transaction.
    std::optional<std::vector<uint8_t>> search(uint32_t key) const;

    // removes a key. Throws if the key does not exist.
    // handles underflow via redistribution or merging, cascading upward
    // through parent nodes as needed.
    // Runs as a single WAL transaction covering every page touched,
    // including any pages freed back to the free list via merges.
    void remove(uint32_t key);

    // returns all key-value pairs with start <= key <= end, in ascending order.
    // uses the leaf linked list for efficient range scanning.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> range_scan(uint32_t start, uint32_t end) const;

    // returns all key-value pairs in the tree, in ascending order.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> scan_all() const;

private:
    BufferPool&      buffer_pool_;
    WALManager&       wal_;
    FreeListManager&  free_list_;
    uint32_t          root_page_;

    // minimum number of cells a non-root node must hold before underflowing.
    // computed at runtime based on max cells per page.
    uint16_t min_cells_for(NodeType type) const;

    // ---- creation / traversal ----

    // creates a brand new empty leaf node and makes it the root.
    // Runs as its own single-page transaction (called only from the
    // constructor, before any caller-supplied transaction could exist).
    void create_empty_root();

    // descends from root to the leaf page that should contain `key`
    uint32_t find_leaf_page(uint32_t key) const;

    // descends from root to the leaf page, recording the path of ancestor
    // page_ids visited along the way (root first, leaf last excluded)
    uint32_t find_leaf_page_with_path(uint32_t key, std::vector<uint32_t>& path) const;

    // ---- insertion ----

    // inserts into a leaf page that has room; does not handle splitting
    void insert_into_leaf(uint32_t transaction_id, uint32_t leaf_page_id, uint32_t key, const std::vector<uint8_t>& value);

    // splits a full leaf page into two, propagates the split key to the parent
    void split_leaf(uint32_t transaction_id, uint32_t leaf_page_id);

    // splits a full internal page into two, propagates the split key to the parent
    void split_internal(uint32_t transaction_id, uint32_t internal_page_id);

    // inserts a new separator key + child pointer into a parent internal node
    // after a child split. Creates a new root if the page being split was the root.
    void insert_into_parent(uint32_t transaction_id, uint32_t left_page_id, uint32_t split_key, uint32_t right_page_id);

    // ---- deletion / underflow handling ----

    // removes a key from a leaf page's entries
    void remove_from_leaf(uint32_t transaction_id, uint32_t leaf_page_id, uint32_t key);

    // removes a key + its associated child pointer from an internal node
    // (used when a child has been merged away)
    void remove_from_internal(uint32_t transaction_id, uint32_t internal_page_id, uint32_t key);

    // checks if `page_id` is below minimum occupancy and fixes it via
    // redistribution or merge, recursing upward through `path` if a merge
    // causes the parent to underflow as well
    void handle_underflow(uint32_t transaction_id, uint32_t page_id, const std::vector<uint32_t>& path);

    // attempts to borrow one entry from a sibling of `page_id` through their
    // shared parent. Returns true if redistribution succeeded.
    bool try_redistribute(uint32_t transaction_id, uint32_t page_id, uint32_t parent_page_id);

    // merges `page_id` with one of its siblings, removing the separator key
    // from `parent_page_id`, and frees the absorbed page back to the free
    // list (via FreeListManager, under the same transaction) instead of
    // leaving it permanently orphaned. The path is passed so the parent's
    // own underflow (if any) can be handled afterward by the caller.
    void merge_with_sibling(uint32_t transaction_id, uint32_t page_id, uint32_t parent_page_id);

    // given a child page_id and its parent, finds the left and right sibling
    // page_ids (INVALID_PAGE if no such sibling exists) and the separator
    // key index in the parent that sits between them
    struct SiblingInfo {
        uint32_t left_sibling  = INVALID_PAGE;
        uint32_t right_sibling = INVALID_PAGE;
        uint32_t left_key      = INVALID_PAGE;  // separator key before this child
        uint32_t right_key     = INVALID_PAGE;  // separator key after this child
    };
    SiblingInfo find_siblings(uint32_t page_id, uint32_t parent_page_id) const;

    // ---- shared helpers ----

    // logs a modified page's content into an already-open transaction.
    // Does NOT begin or commit — the caller (insert()/remove()) owns the
    // transaction's lifetime. This is the change from the old version,
    // which opened and committed its own transaction on every call.
    void write_page_through_wal(uint32_t transaction_id, uint32_t page_id, Page* page);

    // allocates a page via FreeListManager (reclaimed from the free list
    // if available, otherwise a fresh page from BufferPool), under the
    // given transaction — any resulting header update is logged as part
    // of that same transaction.
    Page* allocate_page(uint32_t transaction_id, uint32_t& page_id_out);
};