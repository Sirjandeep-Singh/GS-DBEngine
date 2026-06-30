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
//   [key: 4 bytes][value_size: 4 bytes][value bytes]
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
        std::memcpy(&entry.key, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

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
        uint32_t value_size = static_cast<uint32_t>(entry.value.size());
        size_t   entry_size = sizeof(uint32_t) + sizeof(uint32_t) + value_size;

        write_ptr -= entry_size;

        uint8_t* p = write_ptr;
        std::memcpy(p, &entry.key, sizeof(uint32_t));
        p += sizeof(uint32_t);
        std::memcpy(p, &value_size, sizeof(uint32_t));
        p += sizeof(uint32_t);
        std::memcpy(p, entry.value.data(), value_size);
    }

    header()->num_cells       = static_cast<uint16_t>(entries.size());
    header()->free_space_ptr  = static_cast<uint16_t>(write_ptr - page_->data);
}

bool BTreeNode::find_in_leaf(uint32_t key, std::vector<uint8_t>& out) const {
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
//   [key: 4 bytes][child_page_id: 4 bytes]
// rightmost_child is stored in the header, not as a cell.
// ─────────────────────────────────────────────

std::vector<InternalEntry> BTreeNode::get_internal_entries() const {
    std::vector<InternalEntry> entries;
    const uint8_t* ptr = page_->data + header()->free_space_ptr;
    const uint8_t* end = page_->data + PAGE_SIZE;

    while (ptr < end) {
        InternalEntry entry;
        std::memcpy(&entry.key, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        std::memcpy(&entry.child_page_id, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        entries.push_back(entry);
    }

    return entries;
}

void BTreeNode::set_internal_entries(const std::vector<InternalEntry>& entries) {
    // same reverse-write reasoning as set_leaf_entries — see that comment.
    uint8_t* write_ptr = page_->data + PAGE_SIZE;

    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const InternalEntry& entry = *it;
        write_ptr -= (sizeof(uint32_t) + sizeof(uint32_t));

        uint8_t* p = write_ptr;
        std::memcpy(p, &entry.key, sizeof(uint32_t));
        p += sizeof(uint32_t);
        std::memcpy(p, &entry.child_page_id, sizeof(uint32_t));
    }

    header()->num_cells      = static_cast<uint16_t>(entries.size());
    header()->free_space_ptr = static_cast<uint16_t>(write_ptr - page_->data);
}

uint32_t BTreeNode::find_child_for_key(uint32_t key) const {
    auto entries = get_internal_entries();

    // entries are sorted ascending by key.
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
    // threshold accounts for the worst case next insertion: a max-size
    // entry should not be allowed to overflow the page.
    uint32_t used_by_header = NODE_HEADER_SIZE;
    uint32_t free_space     = header()->free_space_ptr - used_by_header;

    // conservative threshold — leave room for at least one more reasonably
    // sized entry before declaring full. Actual overflow is also checked
    // by the caller before writing.
    const uint32_t SAFETY_MARGIN = 64;
    return free_space < SAFETY_MARGIN;
}