#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <optional>
#include "btree_node.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"

// BTree implements a disk-backed B+ tree, one tree per table or index.
// All page access goes through BufferPool — BTree never touches DiskManager.
// All page modifications go through WALManager for durability.
//
// Keys are uint32_t (matches auto-increment INT primary keys).
// Values are opaque byte blobs (serialized row data) — BTree does not
// interpret the value, that is the Table Layer's responsibility.
//
// Deletion supports underflow handling via redistribution (borrowing from
// a sibling) and merging, with merges cascading upward through parents
// when necessary — full B+ tree delete semantics.

class BTree {
public:
    // root_page: page_id of an existing tree's root, or INVALID_PAGE to create a new tree
    BTree(BufferPool& buffer_pool, WALManager& wal, uint32_t root_page);

    // returns the current root page_id — the Table/Catalog layer must persist
    // this value (e.g. via CatalogManager::update_table_root) whenever it changes
    uint32_t root_page() const;

    // inserts a key-value pair. Throws if the key already exists.
    // may cause node splits, which can change the root page.
    void insert(uint32_t key, const std::vector<uint8_t>& value);

    // searches for a key. Returns the value if found, std::nullopt otherwise.
    std::optional<std::vector<uint8_t>> search(uint32_t key) const;

    // removes a key. Throws if the key does not exist.
    // handles underflow via redistribution or merging, cascading upward
    // through parent nodes as needed.
    void remove(uint32_t key);

    // returns all key-value pairs with start <= key <= end, in ascending order.
    // uses the leaf linked list for efficient range scanning.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> range_scan(uint32_t start, uint32_t end) const;

    // returns all key-value pairs in the tree, in ascending order.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> scan_all() const;

private:
    BufferPool& buffer_pool_;
    WALManager& wal_;
    uint32_t    root_page_;

    // minimum number of cells a non-root node must hold before underflowing.
    // computed at runtime based on max cells per page.
    uint16_t min_cells_for(NodeType type) const;

    // ---- creation / traversal ----

    // creates a brand new empty leaf node and makes it the root
    void create_empty_root();

    // descends from root to the leaf page that should contain `key`
    uint32_t find_leaf_page(uint32_t key) const;

    // descends from root to the leaf page, recording the path of ancestor
    // page_ids visited along the way (root first, leaf last excluded)
    uint32_t find_leaf_page_with_path(uint32_t key, std::vector<uint32_t>& path) const;

    // ---- insertion ----

    // inserts into a leaf page that has room; does not handle splitting
    void insert_into_leaf(uint32_t leaf_page_id, uint32_t key, const std::vector<uint8_t>& value);

    // splits a full leaf page into two, propagates the split key to the parent
    void split_leaf(uint32_t leaf_page_id);

    // splits a full internal page into two, propagates the split key to the parent
    void split_internal(uint32_t internal_page_id);

    // inserts a new separator key + child pointer into a parent internal node
    // after a child split. Creates a new root if the page being split was the root.
    void insert_into_parent(uint32_t left_page_id, uint32_t split_key, uint32_t right_page_id);

    // ---- deletion / underflow handling ----

    // removes a key from a leaf page's entries
    void remove_from_leaf(uint32_t leaf_page_id, uint32_t key);

    // removes a key + its associated child pointer from an internal node
    // (used when a child has been merged away)
    void remove_from_internal(uint32_t internal_page_id, uint32_t key);

    // checks if `page_id` is below minimum occupancy and fixes it via
    // redistribution or merge, recursing upward through `path` if a merge
    // causes the parent to underflow as well
    void handle_underflow(uint32_t page_id, const std::vector<uint32_t>& path);

    // attempts to borrow one entry from a sibling of `page_id` through their
    // shared parent. Returns true if redistribution succeeded.
    bool try_redistribute(uint32_t page_id, uint32_t parent_page_id);

    // merges `page_id` with one of its siblings, removing the separator key
    // from `parent_page_id`. The path is passed so the parent's own underflow
    // (if any) can be handled afterward by the caller.
    void merge_with_sibling(uint32_t page_id, uint32_t parent_page_id);

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

    // writes a modified page through WAL (begin -> write -> commit)
    void write_page_through_wal(uint32_t page_id, Page* page);

    // allocates a brand new page via buffer pool, returns its page_id and pointer
    Page* allocate_page(uint32_t& page_id_out);
};