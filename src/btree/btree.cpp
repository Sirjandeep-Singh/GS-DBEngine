#include "btree.h"

#include <stdexcept>
#include <algorithm>
#include <cstring>

BTree::BTree(BufferPool& buffer_pool, WALManager& wal, uint32_t root_page)
    : buffer_pool_(buffer_pool), wal_(wal), root_page_(root_page)
{
    if (root_page_ == INVALID_PAGE) {
        create_empty_root();
    }
}

uint32_t BTree::root_page() const {
    return root_page_;
}

void BTree::create_empty_root() {
    uint32_t page_id;
    Page* page = allocate_page(page_id);
    BTreeNode::init_node(page, NodeType::LEAF);
    buffer_pool_.unpin_page(page_id, true);
    write_page_through_wal(page_id, page);
    root_page_ = page_id;
}

Page* BTree::allocate_page(uint32_t& page_id_out) {
    return buffer_pool_.new_page(page_id_out);
}

void BTree::write_page_through_wal(uint32_t page_id, Page* page) {
    uint32_t txn = wal_.begin();
    wal_.write(txn, page_id, *page);
    wal_.commit(txn);
}

uint16_t BTree::min_cells_for(NodeType type) const {
    // simple fixed minimum — at least 1 entry required to remain non-empty,
    // underflow triggers when a non-root node drops below this.
    // For a stricter B+ tree this would be ceil(max/2), but since our pages
    // hold variable-size entries we use a conservative fixed minimum.
    (void)type;
    return 1;
}

// ─────────────────────────────────────────────
// Traversal
// ─────────────────────────────────────────────

uint32_t BTree::find_leaf_page(uint32_t key) const {
    std::vector<uint32_t> path;  // unused here, but reuse same logic
    return find_leaf_page_with_path(key, path);
}

uint32_t BTree::find_leaf_page_with_path(uint32_t key, std::vector<uint32_t>& path) const {
    uint32_t current = root_page_;

    while (true) {
        Page* page = buffer_pool_.fetch_page(current);
        BTreeNode node(page);

        if (node.is_leaf()) {
            buffer_pool_.unpin_page(current, false);
            return current;
        }

        uint32_t child = node.find_child_for_key(key);
        buffer_pool_.unpin_page(current, false);

        path.push_back(current);
        current = child;
    }
}

// ─────────────────────────────────────────────
// Search
// ─────────────────────────────────────────────

std::optional<std::vector<uint8_t>> BTree::search(uint32_t key) const {
    uint32_t leaf_page_id = find_leaf_page(key);

    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    std::vector<uint8_t> value;
    bool found = node.find_in_leaf(key, value);
    buffer_pool_.unpin_page(leaf_page_id, false);

    if (found) return value;
    return std::nullopt;
}

// ─────────────────────────────────────────────
// Insert
// ─────────────────────────────────────────────

void BTree::insert(uint32_t key, const std::vector<uint8_t>& value) {
    uint32_t leaf_page_id = find_leaf_page(key);

    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    std::vector<uint8_t> existing;
    if (node.find_in_leaf(key, existing)) {
        buffer_pool_.unpin_page(leaf_page_id, false);
        throw std::runtime_error("BTree::insert: duplicate key " + std::to_string(key));
    }

    // compute whether this new entry would overflow the page BEFORE writing
    // it — set_leaf_entries does not bounds-check, so we must verify here.
    uint32_t entry_size  = sizeof(uint32_t) + sizeof(uint32_t) + static_cast<uint32_t>(value.size());
    uint32_t used_space  = PAGE_SIZE - node.free_space_ptr_value();
    uint32_t available   = PAGE_SIZE - NODE_HEADER_SIZE - used_space;
    bool would_overflow  = entry_size > available;

    buffer_pool_.unpin_page(leaf_page_id, false);

    if (would_overflow) {
        // split first so the leaf has room, then re-find the correct leaf
        // for this key (the split may have moved which leaf it belongs to)
        split_leaf(leaf_page_id);
        leaf_page_id = find_leaf_page(key);
    }

    insert_into_leaf(leaf_page_id, key, value);
}

void BTree::insert_into_leaf(uint32_t leaf_page_id, uint32_t key, const std::vector<uint8_t>& value) {
    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    auto entries = node.get_leaf_entries();
    entries.push_back({key, value});
    std::sort(entries.begin(), entries.end(),
        [](const LeafEntry& a, const LeafEntry& b) { return a.key < b.key; });

    node.set_leaf_entries(entries);
    buffer_pool_.unpin_page(leaf_page_id, true);

    write_page_through_wal(leaf_page_id, page);
}

void BTree::split_leaf(uint32_t leaf_page_id) {
    Page* left_page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode left_node(left_page);

    auto entries = left_node.get_leaf_entries();
    size_t mid = entries.size() / 2;

    std::vector<LeafEntry> left_entries(entries.begin(), entries.begin() + mid);
    std::vector<LeafEntry> right_entries(entries.begin() + mid, entries.end());

    uint32_t old_next_leaf = left_node.next_leaf();
    uint32_t old_parent     = left_node.parent_page();

    // allocate the new right-hand leaf
    uint32_t right_page_id;
    Page* right_page = allocate_page(right_page_id);
    BTreeNode::init_node(right_page, NodeType::LEAF);
    BTreeNode right_node(right_page);

    right_node.set_leaf_entries(right_entries);
    right_node.set_next_leaf(old_next_leaf);
    right_node.set_parent_page(old_parent);

    // rewrite the left leaf with only its half
    left_node.set_leaf_entries(left_entries);
    left_node.set_next_leaf(right_page_id);

    uint32_t split_key = right_entries.front().key;

    buffer_pool_.unpin_page(leaf_page_id, true);
    buffer_pool_.unpin_page(right_page_id, true);

    write_page_through_wal(leaf_page_id, left_page);
    write_page_through_wal(right_page_id, right_page);

    insert_into_parent(leaf_page_id, split_key, right_page_id);
}

void BTree::split_internal(uint32_t internal_page_id) {
    Page* left_page = buffer_pool_.fetch_page(internal_page_id);
    BTreeNode left_node(left_page);

    auto entries = left_node.get_internal_entries();
    uint32_t old_rightmost = left_node.rightmost_child();
    uint32_t old_parent    = left_node.parent_page();

    size_t mid = entries.size() / 2;

    // middle entry's key is promoted to the parent and does NOT appear in
    // either child — only its child pointer matters as the boundary
    uint32_t promoted_key          = entries[mid].key;
    uint32_t promoted_left_child   = entries[mid].child_page_id;

    std::vector<InternalEntry> left_entries(entries.begin(), entries.begin() + mid);
    std::vector<InternalEntry> right_entries(entries.begin() + mid + 1, entries.end());

    // allocate new right-hand internal node
    uint32_t right_page_id;
    Page* right_page = allocate_page(right_page_id);
    BTreeNode::init_node(right_page, NodeType::INTERNAL);
    BTreeNode right_node(right_page);

    right_node.set_internal_entries(right_entries);
    right_node.set_rightmost_child(old_rightmost);
    right_node.set_parent_page(old_parent);

    // left node keeps its first half; its rightmost_child becomes the
    // promoted entry's child (the boundary between left and right)
    left_node.set_internal_entries(left_entries);
    left_node.set_rightmost_child(promoted_left_child);

    // fix parent_page pointers of all children that moved to the right node
    for (auto& entry : right_entries) {
        Page* child_page = buffer_pool_.fetch_page(entry.child_page_id);
        BTreeNode child_node(child_page);
        child_node.set_parent_page(right_page_id);
        buffer_pool_.unpin_page(entry.child_page_id, true);
        write_page_through_wal(entry.child_page_id, child_page);
    }
    {
        Page* child_page = buffer_pool_.fetch_page(old_rightmost);
        BTreeNode child_node(child_page);
        child_node.set_parent_page(right_page_id);
        buffer_pool_.unpin_page(old_rightmost, true);
        write_page_through_wal(old_rightmost, child_page);
    }

    buffer_pool_.unpin_page(internal_page_id, true);
    buffer_pool_.unpin_page(right_page_id, true);

    write_page_through_wal(internal_page_id, left_page);
    write_page_through_wal(right_page_id, right_page);

    insert_into_parent(internal_page_id, promoted_key, right_page_id);
}

void BTree::insert_into_parent(uint32_t left_page_id, uint32_t split_key, uint32_t right_page_id) {
    Page* left_page = buffer_pool_.fetch_page(left_page_id);
    BTreeNode left_node(left_page);
    uint32_t parent_page_id = left_node.parent_page();
    buffer_pool_.unpin_page(left_page_id, false);

    if (parent_page_id == INVALID_PAGE) {
        // left_page_id was the root — create a brand new root
        uint32_t new_root_id;
        Page* new_root_page = allocate_page(new_root_id);
        BTreeNode::init_node(new_root_page, NodeType::INTERNAL);
        BTreeNode new_root(new_root_page);

        std::vector<InternalEntry> entries = { {split_key, left_page_id} };
        new_root.set_internal_entries(entries);
        new_root.set_rightmost_child(right_page_id);

        buffer_pool_.unpin_page(new_root_id, true);
        write_page_through_wal(new_root_id, new_root_page);

        // update parent pointers of both children to point at the new root
        Page* lp = buffer_pool_.fetch_page(left_page_id);
        BTreeNode(lp).set_parent_page(new_root_id);
        buffer_pool_.unpin_page(left_page_id, true);
        write_page_through_wal(left_page_id, lp);

        Page* rp = buffer_pool_.fetch_page(right_page_id);
        BTreeNode(rp).set_parent_page(new_root_id);
        buffer_pool_.unpin_page(right_page_id, true);
        write_page_through_wal(right_page_id, rp);

        root_page_ = new_root_id;
        return;
    }

    // parent exists — insert split_key/right_page_id into it
    Page* parent_page = buffer_pool_.fetch_page(parent_page_id);
    BTreeNode parent_node(parent_page);

    auto entries = parent_node.get_internal_entries();
    entries.push_back({split_key, left_page_id});
    std::sort(entries.begin(), entries.end(),
        [](const InternalEntry& a, const InternalEntry& b) { return a.key < b.key; });

    // the new right_page_id becomes the child immediately after split_key.
    // since entries store (key, child) where child is the LEFT side of key,
    // we need right_page_id to be the child of whichever entry comes after
    // split_key, or the new rightmost_child if split_key is now the largest.
    auto it = std::find_if(entries.begin(), entries.end(),
        [split_key](const InternalEntry& e) { return e.key == split_key; });
    size_t idx = std::distance(entries.begin(), it);

    if (idx + 1 < entries.size()) {
        entries[idx + 1].child_page_id = right_page_id;
    } else {
        parent_node.set_rightmost_child(right_page_id);
    }

    parent_node.set_internal_entries(entries);
    buffer_pool_.unpin_page(parent_page_id, true);
    write_page_through_wal(parent_page_id, parent_page);

    // re-check parent fullness
    Page* check_page = buffer_pool_.fetch_page(parent_page_id);
    bool parent_full = BTreeNode(check_page).is_full();
    buffer_pool_.unpin_page(parent_page_id, false);

    if (parent_full) {
        split_internal(parent_page_id);
    }
}

// ─────────────────────────────────────────────
// Scan operations
// ─────────────────────────────────────────────

std::vector<std::pair<uint32_t, std::vector<uint8_t>>> BTree::scan_all() const {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> result;

    // descend to the leftmost leaf
    uint32_t current = root_page_;
    while (true) {
        Page* page = buffer_pool_.fetch_page(current);
        BTreeNode node(page);
        if (node.is_leaf()) {
            buffer_pool_.unpin_page(current, false);
            break;
        }
        auto entries = node.get_internal_entries();
        uint32_t leftmost_child = entries.empty() ? node.rightmost_child() : entries.front().child_page_id;
        buffer_pool_.unpin_page(current, false);
        current = leftmost_child;
    }

    // walk the leaf linked list
    while (current != INVALID_PAGE) {
        Page* page = buffer_pool_.fetch_page(current);
        BTreeNode node(page);
        auto entries = node.get_leaf_entries();
        uint32_t next = node.next_leaf();
        buffer_pool_.unpin_page(current, false);

        for (auto& e : entries) {
            result.emplace_back(e.key, e.value);
        }
        current = next;
    }

    return result;
}

std::vector<std::pair<uint32_t, std::vector<uint8_t>>> BTree::range_scan(uint32_t start, uint32_t end) const {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> result;

    uint32_t current = find_leaf_page(start);

    bool done = false;
    while (current != INVALID_PAGE && !done) {
        Page* page = buffer_pool_.fetch_page(current);
        BTreeNode node(page);
        auto entries = node.get_leaf_entries();
        uint32_t next = node.next_leaf();
        buffer_pool_.unpin_page(current, false);

        for (auto& e : entries) {
            if (e.key < start) continue;
            if (e.key > end) { done = true; break; }
            result.emplace_back(e.key, e.value);
        }

        current = next;
    }

    return result;
}

// ─────────────────────────────────────────────
// Deletion
// ─────────────────────────────────────────────

void BTree::remove(uint32_t key) {
    std::vector<uint32_t> path;
    uint32_t leaf_page_id = find_leaf_page_with_path(key, path);

    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);
    std::vector<uint8_t> existing;
    bool found = node.find_in_leaf(key, existing);
    buffer_pool_.unpin_page(leaf_page_id, false);

    if (!found) {
        throw std::runtime_error("BTree::remove: key not found " + std::to_string(key));
    }

    remove_from_leaf(leaf_page_id, key);

    // check for underflow — root leaf is exempt (it can be small/empty)
    if (leaf_page_id != root_page_) {
        Page* check_page = buffer_pool_.fetch_page(leaf_page_id);
        uint16_t cells = BTreeNode(check_page).num_cells();
        buffer_pool_.unpin_page(leaf_page_id, false);

        if (cells < min_cells_for(NodeType::LEAF)) {
            handle_underflow(leaf_page_id, path);
        }
    }
}

void BTree::remove_from_leaf(uint32_t leaf_page_id, uint32_t key) {
    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    auto entries = node.get_leaf_entries();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [key](const LeafEntry& e) { return e.key == key; }), entries.end());

    node.set_leaf_entries(entries);
    buffer_pool_.unpin_page(leaf_page_id, true);
    write_page_through_wal(leaf_page_id, page);
}

void BTree::remove_from_internal(uint32_t internal_page_id, uint32_t key) {
    Page* page = buffer_pool_.fetch_page(internal_page_id);
    BTreeNode node(page);

    auto entries = node.get_internal_entries();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [key](const InternalEntry& e) { return e.key == key; }), entries.end());

    node.set_internal_entries(entries);
    buffer_pool_.unpin_page(internal_page_id, true);
    write_page_through_wal(internal_page_id, page);
}

BTree::SiblingInfo BTree::find_siblings(uint32_t page_id, uint32_t parent_page_id) const {
    SiblingInfo info;

    Page* parent_page = buffer_pool_.fetch_page(parent_page_id);
    BTreeNode parent_node(parent_page);
    auto entries = parent_node.get_internal_entries();
    uint32_t rightmost = parent_node.rightmost_child();
    buffer_pool_.unpin_page(parent_page_id, false);

    // build the ordered list of children: entries[0].child, entries[1].child, ..., rightmost
    std::vector<uint32_t> children;
    for (auto& e : entries) children.push_back(e.child_page_id);
    children.push_back(rightmost);

    // separator keys sit BETWEEN children: keys[i] separates children[i] and children[i+1]
    std::vector<uint32_t> keys;
    for (auto& e : entries) keys.push_back(e.key);

    // find index of page_id among children
    int idx = -1;
    for (size_t i = 0; i < children.size(); i++) {
        if (children[i] == page_id) { idx = static_cast<int>(i); break; }
    }
    if (idx == -1) {
        throw std::runtime_error("BTree::find_siblings: page not found among parent's children");
    }

    if (idx > 0) {
        info.left_sibling = children[idx - 1];
        info.left_key     = keys[idx - 1];
    }
    if (idx < static_cast<int>(children.size()) - 1) {
        info.right_sibling = children[idx + 1];
        info.right_key      = keys[idx];
    }

    return info;
}

void BTree::handle_underflow(uint32_t page_id, const std::vector<uint32_t>& path) {
    if (page_id == root_page_) {
        // root underflow: if it's an internal node with zero entries left,
        // collapse it — its single rightmost_child becomes the new root
        Page* page = buffer_pool_.fetch_page(page_id);
        BTreeNode node(page);
        bool is_internal_empty = !node.is_leaf() && node.num_cells() == 0;
        uint32_t only_child = is_internal_empty ? node.rightmost_child() : INVALID_PAGE;
        buffer_pool_.unpin_page(page_id, false);

        if (is_internal_empty && only_child != INVALID_PAGE) {
            Page* child_page = buffer_pool_.fetch_page(only_child);
            BTreeNode(child_page).set_parent_page(INVALID_PAGE);
            buffer_pool_.unpin_page(only_child, true);
            write_page_through_wal(only_child, child_page);

            root_page_ = only_child;
        }
        return;  // root is allowed to be sparse otherwise
    }

    if (path.empty()) {
        // page_id has no recorded parent path but is not root — should not happen
        return;
    }

    uint32_t parent_page_id = path.back();
    std::vector<uint32_t> parent_path(path.begin(), path.end() - 1);

    bool redistributed = try_redistribute(page_id, parent_page_id);
    if (redistributed) {
        return;
    }

    // redistribution not possible — merge with a sibling
    merge_with_sibling(page_id, parent_page_id);

    // after merge, parent lost an entry — check if parent itself underflowed
    if (parent_page_id != root_page_) {
        Page* parent_page = buffer_pool_.fetch_page(parent_page_id);
        uint16_t parent_cells = BTreeNode(parent_page).num_cells();
        buffer_pool_.unpin_page(parent_page_id, false);

        if (parent_cells < min_cells_for(NodeType::INTERNAL)) {
            handle_underflow(parent_page_id, parent_path);
        }
    } else {
        // parent is root — check for root collapse
        handle_underflow(parent_page_id, parent_path);
    }
}

bool BTree::try_redistribute(uint32_t page_id, uint32_t parent_page_id) {
    SiblingInfo siblings = find_siblings(page_id, parent_page_id);

    Page* page = buffer_pool_.fetch_page(page_id);
    bool is_leaf = BTreeNode(page).is_leaf();
    buffer_pool_.unpin_page(page_id, false);

    // try borrowing from left sibling first
    if (siblings.left_sibling != INVALID_PAGE) {
        Page* left_page = buffer_pool_.fetch_page(siblings.left_sibling);
        uint16_t left_cells = BTreeNode(left_page).num_cells();
        buffer_pool_.unpin_page(siblings.left_sibling, false);

        if (left_cells > min_cells_for(is_leaf ? NodeType::LEAF : NodeType::INTERNAL)) {
            // borrow the LAST entry from left sibling, move it to the front of page_id
            if (is_leaf) {
                Page* lp = buffer_pool_.fetch_page(siblings.left_sibling);
                BTreeNode left_node(lp);
                auto left_entries = left_node.get_leaf_entries();
                LeafEntry borrowed = left_entries.back();
                left_entries.pop_back();
                left_node.set_leaf_entries(left_entries);
                buffer_pool_.unpin_page(siblings.left_sibling, true);
                write_page_through_wal(siblings.left_sibling, lp);

                Page* cp = buffer_pool_.fetch_page(page_id);
                BTreeNode cur_node(cp);
                auto cur_entries = cur_node.get_leaf_entries();
                cur_entries.insert(cur_entries.begin(), borrowed);
                cur_node.set_leaf_entries(cur_entries);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(page_id, cp);

                // update parent separator key to the new first key of page_id
                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                for (auto& e : p_entries) {
                    if (e.key == siblings.left_key) { e.key = borrowed.key; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(parent_page_id, pp);

            } else {
                // internal redistribution: rotate through the parent separator key
                Page* lp = buffer_pool_.fetch_page(siblings.left_sibling);
                BTreeNode left_node(lp);
                auto left_entries = left_node.get_internal_entries();
                uint32_t left_rightmost = left_node.rightmost_child();

                Page* cp = buffer_pool_.fetch_page(page_id);
                BTreeNode cur_node(cp);
                auto cur_entries = cur_node.get_internal_entries();

                // the separator key in parent moves down to become cur's new first key,
                // and left's rightmost_child becomes cur's new first child
                cur_entries.insert(cur_entries.begin(), {siblings.left_key, left_rightmost});

                // left's last entry's key moves up to parent; its child becomes left's new rightmost
                InternalEntry promoted = left_entries.back();
                left_entries.pop_back();
                left_node.set_rightmost_child(promoted.child_page_id);

                left_node.set_internal_entries(left_entries);
                cur_node.set_internal_entries(cur_entries);

                buffer_pool_.unpin_page(siblings.left_sibling, true);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(siblings.left_sibling, lp);
                write_page_through_wal(page_id, cp);

                // fix parent pointer of the moved child (left_rightmost moved to cur)
                Page* moved_child = buffer_pool_.fetch_page(left_rightmost);
                BTreeNode(moved_child).set_parent_page(page_id);
                buffer_pool_.unpin_page(left_rightmost, true);
                write_page_through_wal(left_rightmost, moved_child);

                // update parent separator key
                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                for (auto& e : p_entries) {
                    if (e.key == siblings.left_key) { e.key = promoted.key; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(parent_page_id, pp);
            }

            return true;
        }
    }

    // try borrowing from right sibling
    if (siblings.right_sibling != INVALID_PAGE) {
        Page* right_page = buffer_pool_.fetch_page(siblings.right_sibling);
        uint16_t right_cells = BTreeNode(right_page).num_cells();
        buffer_pool_.unpin_page(siblings.right_sibling, false);

        if (right_cells > min_cells_for(is_leaf ? NodeType::LEAF : NodeType::INTERNAL)) {
            if (is_leaf) {
                Page* rp = buffer_pool_.fetch_page(siblings.right_sibling);
                BTreeNode right_node(rp);
                auto right_entries = right_node.get_leaf_entries();
                LeafEntry borrowed = right_entries.front();
                right_entries.erase(right_entries.begin());
                right_node.set_leaf_entries(right_entries);
                buffer_pool_.unpin_page(siblings.right_sibling, true);
                write_page_through_wal(siblings.right_sibling, rp);

                Page* cp = buffer_pool_.fetch_page(page_id);
                BTreeNode cur_node(cp);
                auto cur_entries = cur_node.get_leaf_entries();
                cur_entries.push_back(borrowed);
                cur_node.set_leaf_entries(cur_entries);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(page_id, cp);

                // update parent separator key to the new first key of right sibling
                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                uint32_t new_right_first = right_entries.empty() ? borrowed.key : right_entries.front().key;
                for (auto& e : p_entries) {
                    if (e.key == siblings.right_key) { e.key = new_right_first; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(parent_page_id, pp);

            } else {
                Page* rp = buffer_pool_.fetch_page(siblings.right_sibling);
                BTreeNode right_node(rp);
                auto right_entries = right_node.get_internal_entries();
                InternalEntry promoted = right_entries.front();
                right_entries.erase(right_entries.begin());

                Page* cp = buffer_pool_.fetch_page(page_id);
                BTreeNode cur_node(cp);
                auto cur_entries = cur_node.get_internal_entries();
                uint32_t cur_rightmost = cur_node.rightmost_child();

                // parent separator key moves down to become cur's new last entry,
                // pointing at cur's OLD rightmost_child; cur's new rightmost becomes
                // the borrowed entry's child
                cur_entries.push_back({siblings.right_key, cur_rightmost});
                cur_node.set_rightmost_child(promoted.child_page_id);

                cur_node.set_internal_entries(cur_entries);
                right_node.set_internal_entries(right_entries);

                buffer_pool_.unpin_page(siblings.right_sibling, true);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(siblings.right_sibling, rp);
                write_page_through_wal(page_id, cp);

                // fix parent pointer of the moved child
                Page* moved_child = buffer_pool_.fetch_page(promoted.child_page_id);
                BTreeNode(moved_child).set_parent_page(page_id);
                buffer_pool_.unpin_page(promoted.child_page_id, true);
                write_page_through_wal(promoted.child_page_id, moved_child);

                // update parent separator key
                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                for (auto& e : p_entries) {
                    if (e.key == siblings.right_key) { e.key = promoted.key; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(parent_page_id, pp);
            }

            return true;
        }
    }

    return false;  // no sibling had enough to lend
}

void BTree::merge_with_sibling(uint32_t page_id, uint32_t parent_page_id) {
    SiblingInfo siblings = find_siblings(page_id, parent_page_id);

    // prefer merging with left sibling if it exists, otherwise right
    uint32_t merge_into;   // page that survives
    uint32_t merge_from;   // page that gets absorbed and discarded
    uint32_t separator_key;
    bool current_is_left;

    if (siblings.left_sibling != INVALID_PAGE) {
        merge_into     = siblings.left_sibling;
        merge_from     = page_id;
        separator_key  = siblings.left_key;
        current_is_left = false;
    } else {
        merge_into     = page_id;
        merge_from     = siblings.right_sibling;
        separator_key  = siblings.right_key;
        current_is_left = true;
    }

    Page* into_page = buffer_pool_.fetch_page(merge_into);
    BTreeNode into_node(into_page);
    bool is_leaf = into_node.is_leaf();

    Page* from_page = buffer_pool_.fetch_page(merge_from);
    BTreeNode from_node(from_page);

    if (is_leaf) {
        auto into_entries = into_node.get_leaf_entries();
        auto from_entries = from_node.get_leaf_entries();

        into_entries.insert(into_entries.end(), from_entries.begin(), from_entries.end());
        into_node.set_leaf_entries(into_entries);
        into_node.set_next_leaf(from_node.next_leaf());

    } else {
        auto into_entries = into_node.get_internal_entries();
        auto from_entries = from_node.get_internal_entries();
        uint32_t into_rightmost = into_node.rightmost_child();
        uint32_t from_rightmost = from_node.rightmost_child();

        // the separator key from parent comes down between the two halves,
        // pointing at into's old rightmost_child
        into_entries.push_back({separator_key, into_rightmost});
        into_entries.insert(into_entries.end(), from_entries.begin(), from_entries.end());

        into_node.set_internal_entries(into_entries);
        into_node.set_rightmost_child(from_rightmost);

        // fix parent_page pointer of all children that moved from `from` into `into`
        for (auto& e : from_entries) {
            Page* child_page = buffer_pool_.fetch_page(e.child_page_id);
            BTreeNode(child_page).set_parent_page(merge_into);
            buffer_pool_.unpin_page(e.child_page_id, true);
            write_page_through_wal(e.child_page_id, child_page);
        }
        Page* rightmost_child_page = buffer_pool_.fetch_page(from_rightmost);
        BTreeNode(rightmost_child_page).set_parent_page(merge_into);
        buffer_pool_.unpin_page(from_rightmost, true);
        write_page_through_wal(from_rightmost, rightmost_child_page);
    }

    buffer_pool_.unpin_page(merge_into, true);
    buffer_pool_.unpin_page(merge_from, false);

    write_page_through_wal(merge_into, into_page);

    (void)current_is_left;

    // remove the separator key from the parent — it no longer separates anything
    remove_from_internal(parent_page_id, separator_key);

    // NOTE: the merge_from page is now orphaned (unreachable) but remains
    // allocated on disk. A free-list to reclaim it is a future improvement;
    // for this engine's scope, orphaned pages are an acceptable simplification.
}