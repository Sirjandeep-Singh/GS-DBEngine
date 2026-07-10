#pragma once

#include <cstdint>
#include <vector>
#include "../storage/page.h"
#include "key.h"

// A B+ tree node is just an interpretation of a Page's raw bytes.
// Two node types share the same page layout convention:
//
// Page layout (both types):
//   [NodeHeader]            ← fixed size header at the start of the page
//   [cell offset array]     ← grows downward from after the header
//   [free space]
//   [cells]                 ← grow upward from the end of the page
//
// Keys are variable-length (see key.h / KeyCodec) — a single-column INT
// or VARCHAR key and a multi-column composite key are both just a Key
// (vector<Value>) encoded to a self-delimiting byte blob, so no separate
// "wide key" code path is needed here.
//
// Internal node cell:  [key (KeyCodec-encoded, variable)] [child_page_id (4 bytes)]
// Leaf node cell:       [key (KeyCodec-encoded, variable)] [value_size (4 bytes)] [value bytes]

enum class NodeType : uint8_t {
    INTERNAL = 1,
    LEAF     = 2,
};

#pragma pack(push, 1)
struct NodeHeader {
    NodeType type;            // INTERNAL or LEAF
    uint16_t num_cells;       // how many keys/entries are on this page
    uint16_t free_space_ptr;  // byte offset where free space begins (cells grow down from end)
    uint32_t parent_page;     // page_id of parent, INVALID_PAGE if root
    uint32_t next_leaf;       // for LEAF nodes only — page_id of next leaf, INVALID_PAGE if last
    uint32_t rightmost_child; // for INTERNAL nodes only — page_id of the rightmost child
};
#pragma pack(pop)

static const uint32_t NODE_HEADER_SIZE = sizeof(NodeHeader);

// An in-memory, deserialized view of an internal node's entries.
// key[i] separates child[i] (keys < key[i]) from child[i+1] (keys >= key[i]).
// child[num_keys] is stored separately as rightmost_child in the header.
struct InternalEntry {
    Key      key;
    uint32_t child_page_id;
};

// An in-memory, deserialized view of a leaf node's entries.
struct LeafEntry {
    Key                   key;
    std::vector<uint8_t> value;  // serialized row bytes
};

// BTreeNode wraps a raw Page and provides typed access to its contents.
// It does not own the Page — the caller (via BufferPool) owns the memory.
// All reads/writes go through this class so the byte layout is never
// touched directly by BTree logic.

class BTreeNode {
public:
    explicit BTreeNode(Page* page);

    NodeType type() const;
    void     set_type(NodeType type);

    uint16_t num_cells() const;
    uint32_t parent_page() const;
    void     set_parent_page(uint32_t page_id);

    bool is_leaf() const;
    bool is_full() const;   // true if no more entries can fit

    // returns the raw free_space_ptr value — used by BTree to compute
    // exact available space before writing, to prevent overflow
    uint16_t free_space_ptr_value() const;

    // ---- Leaf node operations ----
    uint32_t next_leaf() const;
    void     set_next_leaf(uint32_t page_id);

    std::vector<LeafEntry> get_leaf_entries() const;
    void                   set_leaf_entries(const std::vector<LeafEntry>& entries);

    // returns true and fills `out` if key is found in this leaf
    bool find_in_leaf(const Key& key, std::vector<uint8_t>& out) const;

    // ---- Internal node operations ----
    uint32_t rightmost_child() const;
    void     set_rightmost_child(uint32_t page_id);

    std::vector<InternalEntry> get_internal_entries() const;
    void                       set_internal_entries(const std::vector<InternalEntry>& entries);

    // given a search key, returns which child page to descend into
    uint32_t find_child_for_key(const Key& key) const;

    // initializes a blank page as an empty node of the given type
    static void init_node(Page* page, NodeType type);

private:
    Page* page_;

    NodeHeader* header();
    const NodeHeader* header() const;

    uint8_t* cell_data_start();  // pointer to byte right after the header
};