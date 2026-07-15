#include "btree_node.h"

#include <cstring>
#include <stdexcept>
#include <algorithm>

BTreeNode::BTreeNode(Page* page) : page_(page) {}

NodeHeader* BTreeNode::header() {
    return reinterpret_cast<NodeHeader*>(page_->data);
}

const NodeHeader* BTreeNode::header() const {
    return reinterpret_cast<const NodeHeader*>(page_->data);
}

uint8_t* BTreeNode::cell_data_start() {
    return page_->data + NODE_HEADER_SIZE;
}

NodeType BTreeNode::type() const {
    return header()->type;
}

void BTreeNode::set_type(NodeType t) {
    header()->type = t;
}

uint16_t BTreeNode::num_cells() const {
    return header()->num_cells;
}

uint32_t BTreeNode::parent_page() const {
    return header()->parent_page;
}

void BTreeNode::set_parent_page(uint32_t page_id) {
    header()->parent_page = page_id;
}

bool BTreeNode::is_leaf() const {
    return type() == NodeType::LEAF;
}

uint32_t BTreeNode::next_leaf() const {
    return header()->next_leaf;
}

void BTreeNode::set_next_leaf(uint32_t page_id) {
    header()->next_leaf = page_id;
}

uint32_t BTreeNode::rightmost_child() const {
    return header()->rightmost_child;
}

void BTreeNode::set_rightmost_child(uint32_t page_id) {
    header()->rightmost_child = page_id;
}

void BTreeNode::init_node(Page* page, NodeType type) {
    std::memset(page->data, 0, PAGE_SIZE);

    NodeHeader* h = reinterpret_cast<NodeHeader*>(page->data);
    h->type             = type;
    h->num_cells         = 0;
    h->free_space_ptr   = PAGE_SIZE;  // cells grow downward from the end of the page
    h->parent_page      = INVALID_PAGE;
    h->next_leaf        = INVALID_PAGE;
    h->rightmost_child  = INVALID_PAGE;
}

// ─────────────────────────────────────────────
// Leaf node serialization
//
// Layout per leaf entry, written back-to-back starting from the END of the
// page and growing toward the header (free_space_ptr tracks the boundary):
//   [key: KeyCodec-encoded, variable][value_size: 4 bytes][value bytes]
//
// The key portion is self-delimiting (KeyCodec::decode knows exactly how
// many bytes it consumed without an outer length prefix), so entries of
// differing key size — a single INT key next to a multi-column composite
// key, for instance — sit back-to-back with no padding or wrapper needed.
//
// Cell offsets are NOT stored separately — entries are simply concatenated
// in sorted-by-key order, and we scan linearly to find/insert. This keeps
// the implementation simple; a production engine would maintain a sorted
// offset array for binary search, but linear scan within a 4KB page is
// fast enough in practice and far simpler to get correct.
// ─────────────────────────────────────────────

std::vector<LeafEntry> BTreeNode::get_leaf_entries() const {
    std::vector<LeafEntry> entries;
    const uint8_t* ptr = page_->data + header()->free_space_ptr;
    const uint8_t* end = page_->data + PAGE_SIZE;

    while (ptr < end) {
        LeafEntry entry;
        entry.key = KeyCodec::decode(ptr);

        uint32_t value_size;
        std::memcpy(&value_size, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        entry.value.assign(ptr, ptr + value_size);
        ptr += value_size;

        entries.push_back(std::move(entry));
    }

    return entries;
}

void BTreeNode::set_leaf_entries(const std::vector<LeafEntry>& entries) {
    // entries must be passed in sorted-by-key order. Cells are written
    // starting from the end of the page and growing toward the header,
    // but get_leaf_entries() reads forward from free_space_ptr toward the
    // end. To preserve sorted order on read, we must write entries in
    // REVERSE order so the last-written (closest to free_space_ptr) cell
    // is the smallest key.
    uint8_t* write_ptr = page_->data + PAGE_SIZE;

    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const LeafEntry& entry = *it;
        std::vector<uint8_t> key_bytes = KeyCodec::encode(entry.key);
        uint32_t value_size = static_cast<uint32_t>(entry.value.size());
        size_t   entry_size = key_bytes.size() + sizeof(uint32_t) + value_size;

        write_ptr -= entry_size;

        uint8_t* p = write_ptr;
        std::memcpy(p, key_bytes.data(), key_bytes.size());
        p += key_bytes.size();
        std::memcpy(p, &value_size, sizeof(uint32_t));
        p += sizeof(uint32_t);
        std::memcpy(p, entry.value.data(), value_size);
    }

    header()->num_cells       = static_cast<uint16_t>(entries.size());
    header()->free_space_ptr  = static_cast<uint16_t>(write_ptr - page_->data);
}

bool BTreeNode::find_in_leaf(const Key& key, std::vector<uint8_t>& out) const {
    auto entries = get_leaf_entries();
    for (const auto& entry : entries) {
        if (entry.key == key) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────
// Internal node serialization
//
// Layout per internal entry, written back-to-back from the end of the page:
//   [key: KeyCodec-encoded, variable][child_page_id: 4 bytes]
// rightmost_child is stored in the header, not as a cell.
// ─────────────────────────────────────────────

std::vector<InternalEntry> BTreeNode::get_internal_entries() const {
    std::vector<InternalEntry> entries;
    const uint8_t* ptr = page_->data + header()->free_space_ptr;
    const uint8_t* end = page_->data + PAGE_SIZE;

    while (ptr < end) {
        InternalEntry entry;
        entry.key = KeyCodec::decode(ptr);
        std::memcpy(&entry.child_page_id, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        entries.push_back(std::move(entry));
    }

    return entries;
}

void BTreeNode::set_internal_entries(const std::vector<InternalEntry>& entries) {
    // same reverse-write reasoning as set_leaf_entries — see that comment.
    uint8_t* write_ptr = page_->data + PAGE_SIZE;

    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const InternalEntry& entry = *it;
        std::vector<uint8_t> key_bytes = KeyCodec::encode(entry.key);
        write_ptr -= (key_bytes.size() + sizeof(uint32_t));

        uint8_t* p = write_ptr;
        std::memcpy(p, key_bytes.data(), key_bytes.size());
        p += key_bytes.size();
        std::memcpy(p, &entry.child_page_id, sizeof(uint32_t));
    }

    header()->num_cells      = static_cast<uint16_t>(entries.size());
    header()->free_space_ptr = static_cast<uint16_t>(write_ptr - page_->data);
}

uint32_t BTreeNode::find_child_for_key(const Key& key) const {
    auto entries = get_internal_entries();

    // entries are sorted ascending by key (lexicographic tuple order).
    // entries[i].key separates "go left" from "go right":
    //   key < entries[0].key          -> entries[0].child_page_id
    //   entries[i-1].key <= key < entries[i].key -> entries[i].child_page_id
    //   key >= entries.back().key     -> rightmost_child
    for (const auto& entry : entries) {
        if (key < entry.key) {
            return entry.child_page_id;
        }
    }
    return rightmost_child();
}

// ─────────────────────────────────────────────
// Capacity check
// ─────────────────────────────────────────────

uint16_t BTreeNode::free_space_ptr_value() const {
    return header()->free_space_ptr;
}

bool BTreeNode::is_full() const {    // free space remaining = free_space_ptr - end of header
    // if free space is below a safety threshold, consider the page full.
    // This is only a cheap heuristic used to decide whether to proactively
    // split a page right after a write (see BTree::insert_into_parent) —
    // it is NOT what prevents page overflow. With variable-length keys
    // (VARCHAR / composite), a single entry can be far larger than this
    // margin, so every call site that actually writes an entry (leaf
    // insert, internal insert) computes the exact encoded size first and
    // splits BEFORE writing if it wouldn't fit — see the precise
    // available-space checks in btree.cpp. is_full() only decides "is it
    // worth proactively splitting now", not "is it safe to write".
    uint32_t used_by_header = NODE_HEADER_SIZE;
    uint32_t free_space     = header()->free_space_ptr - used_by_header;

    const uint32_t SAFETY_MARGIN = 64;
    return free_space < SAFETY_MARGIN;
}

bool BTreeNode::is_underfull() const {
    // used_space = everything below free_space_ptr that isn't header,
    // i.e. the cells actually written into the page so far.
    uint32_t used_space = PAGE_SIZE - header()->free_space_ptr;
    uint32_t capacity   = PAGE_SIZE - NODE_HEADER_SIZE;

    // classic B+tree invariant: a non-root node should stay at least
    // half full; below that it's a merge/redistribute candidate.
    return used_space < capacity / 2;
}