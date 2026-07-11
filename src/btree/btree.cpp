#include "btree.h"

#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace {
// stringifies a Key for error messages, e.g. "(1, "abc")" for a composite key
std::string key_to_string(const Key& key) {
    std::string s = "(";
    for (size_t i = 0; i < key.size(); i++) {
        if (i > 0) s += ", ";
        s += value_to_string(key[i]);
    }
    s += ")";
    return s;
}
}

BTree::BTree(BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list, uint32_t root_page)
    : buffer_pool_(buffer_pool), wal_(wal), free_list_(free_list), root_page_(root_page)
{
    if (root_page_ == INVALID_PAGE) {
        create_empty_root();
    }
}

uint32_t BTree::root_page() const {
    return root_page_;
}

void BTree::create_empty_root() {
    uint32_t transaction_id = wal_.begin();

    uint32_t page_id;
    Page* page = allocate_page(transaction_id, page_id);
    BTreeNode::init_node(page, NodeType::LEAF);
    buffer_pool_.unpin_page(page_id, true);
    write_page_through_wal(transaction_id, page_id, page);

    wal_.commit(transaction_id);

    root_page_ = page_id;
}

Page* BTree::allocate_page(uint32_t transaction_id, uint32_t& page_id_out) {
    return free_list_.allocate_page(transaction_id, page_id_out);
}

void BTree::write_page_through_wal(uint32_t transaction_id, uint32_t page_id, Page* page) {
    wal_.write(transaction_id, page_id, *page);
}

uint16_t BTree::min_cells_for(NodeType type) const {
    (void)type;
    return 1;
}

uint32_t BTree::leaf_entry_size(const Key& key, const std::vector<uint8_t>& value) {
    return static_cast<uint32_t>(KeyCodec::encode(key).size()) + sizeof(uint32_t) + static_cast<uint32_t>(value.size());
}

uint32_t BTree::internal_entry_size(const Key& key) {
    return static_cast<uint32_t>(KeyCodec::encode(key).size()) + sizeof(uint32_t);
}

uint32_t BTree::find_leaf_page(const Key& key) const {
    std::vector<uint32_t> path;
    return find_leaf_page_with_path(key, path);
}

uint32_t BTree::find_leaf_page_with_path(const Key& key, std::vector<uint32_t>& path) const {
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

std::optional<std::vector<uint8_t>> BTree::search(const Key& key) const {
    uint32_t leaf_page_id = find_leaf_page(key);

    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    std::vector<uint8_t> value;
    bool found = node.find_in_leaf(key, value);
    buffer_pool_.unpin_page(leaf_page_id, false);

    if (found) return value;
    return std::nullopt;
}

void BTree::insert(const Key& key, const std::vector<uint8_t>& value) {
    uint32_t transaction_id = wal_.begin();
    insert(transaction_id, key, value);
    wal_.commit(transaction_id);
}

void BTree::insert(uint32_t transaction_id, const Key& key, const std::vector<uint8_t>& value) {
    uint32_t leaf_page_id = find_leaf_page(key);

    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    std::vector<uint8_t> existing;
    if (node.find_in_leaf(key, existing)) {
        buffer_pool_.unpin_page(leaf_page_id, false);
        throw std::runtime_error("BTree::insert: duplicate key " + key_to_string(key));
    }

    uint32_t entry_size  = leaf_entry_size(key, value);
    uint32_t used_space  = PAGE_SIZE - node.free_space_ptr_value();
    uint32_t available   = PAGE_SIZE - NODE_HEADER_SIZE - used_space;
    bool would_overflow  = entry_size > available;

    buffer_pool_.unpin_page(leaf_page_id, false);

    if (would_overflow) {
        split_leaf(transaction_id, leaf_page_id);
        leaf_page_id = find_leaf_page(key);
    }

    insert_into_leaf(transaction_id, leaf_page_id, key, value);
}

void BTree::insert_into_leaf(uint32_t transaction_id, uint32_t leaf_page_id, const Key& key, const std::vector<uint8_t>& value) {
    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    auto entries = node.get_leaf_entries();
    entries.push_back({key, value});
    std::sort(entries.begin(), entries.end(),
        [](const LeafEntry& a, const LeafEntry& b) { return a.key < b.key; });

    node.set_leaf_entries(entries);
    buffer_pool_.unpin_page(leaf_page_id, true);

    write_page_through_wal(transaction_id, leaf_page_id, page);
}

void BTree::split_leaf(uint32_t transaction_id, uint32_t leaf_page_id) {
    Page* left_page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode left_node(left_page);

    auto entries = left_node.get_leaf_entries();
    size_t mid = entries.size() / 2;

    std::vector<LeafEntry> left_entries(entries.begin(), entries.begin() + mid);
    std::vector<LeafEntry> right_entries(entries.begin() + mid, entries.end());

    uint32_t old_next_leaf = left_node.next_leaf();
    uint32_t old_parent     = left_node.parent_page();

    uint32_t right_page_id;
    Page* right_page = allocate_page(transaction_id, right_page_id);
    BTreeNode::init_node(right_page, NodeType::LEAF);
    BTreeNode right_node(right_page);

    right_node.set_leaf_entries(right_entries);
    right_node.set_next_leaf(old_next_leaf);
    right_node.set_parent_page(old_parent);

    left_node.set_leaf_entries(left_entries);
    left_node.set_next_leaf(right_page_id);

    Key split_key = right_entries.front().key;

    buffer_pool_.unpin_page(leaf_page_id, true);
    buffer_pool_.unpin_page(right_page_id, true);

    write_page_through_wal(transaction_id, leaf_page_id, left_page);
    write_page_through_wal(transaction_id, right_page_id, right_page);

    insert_into_parent(transaction_id, leaf_page_id, split_key, right_page_id);
}

void BTree::split_internal(uint32_t transaction_id, uint32_t internal_page_id) {
    Page* left_page = buffer_pool_.fetch_page(internal_page_id);
    BTreeNode left_node(left_page);

    auto entries = left_node.get_internal_entries();
    uint32_t old_rightmost = left_node.rightmost_child();
    uint32_t old_parent    = left_node.parent_page();

    size_t mid = entries.size() / 2;

    Key      promoted_key          = entries[mid].key;
    uint32_t promoted_left_child   = entries[mid].child_page_id;

    std::vector<InternalEntry> left_entries(entries.begin(), entries.begin() + mid);
    std::vector<InternalEntry> right_entries(entries.begin() + mid + 1, entries.end());

    uint32_t right_page_id;
    Page* right_page = allocate_page(transaction_id, right_page_id);
    BTreeNode::init_node(right_page, NodeType::INTERNAL);
    BTreeNode right_node(right_page);

    right_node.set_internal_entries(right_entries);
    right_node.set_rightmost_child(old_rightmost);
    right_node.set_parent_page(old_parent);

    left_node.set_internal_entries(left_entries);
    left_node.set_rightmost_child(promoted_left_child);

    for (auto& entry : right_entries) {
        Page* child_page = buffer_pool_.fetch_page(entry.child_page_id);
        BTreeNode child_node(child_page);
        child_node.set_parent_page(right_page_id);
        buffer_pool_.unpin_page(entry.child_page_id, true);
        write_page_through_wal(transaction_id, entry.child_page_id, child_page);
    }
    {
        Page* child_page = buffer_pool_.fetch_page(old_rightmost);
        BTreeNode child_node(child_page);
        child_node.set_parent_page(right_page_id);
        buffer_pool_.unpin_page(old_rightmost, true);
        write_page_through_wal(transaction_id, old_rightmost, child_page);
    }

    buffer_pool_.unpin_page(internal_page_id, true);
    buffer_pool_.unpin_page(right_page_id, true);

    write_page_through_wal(transaction_id, internal_page_id, left_page);
    write_page_through_wal(transaction_id, right_page_id, right_page);

    insert_into_parent(transaction_id, internal_page_id, promoted_key, right_page_id);
}

void BTree::insert_into_parent(uint32_t transaction_id, uint32_t left_page_id, const Key& split_key, uint32_t right_page_id) {
    Page* left_page = buffer_pool_.fetch_page(left_page_id);
    BTreeNode left_node(left_page);
    uint32_t parent_page_id = left_node.parent_page();
    buffer_pool_.unpin_page(left_page_id, false);

    if (parent_page_id == INVALID_PAGE) {
        uint32_t new_root_id;
        Page* new_root_page = allocate_page(transaction_id, new_root_id);
        BTreeNode::init_node(new_root_page, NodeType::INTERNAL);
        BTreeNode new_root(new_root_page);

        std::vector<InternalEntry> entries = { {split_key, left_page_id} };
        new_root.set_internal_entries(entries);
        new_root.set_rightmost_child(right_page_id);

        buffer_pool_.unpin_page(new_root_id, true);
        write_page_through_wal(transaction_id, new_root_id, new_root_page);

        Page* lp = buffer_pool_.fetch_page(left_page_id);
        BTreeNode(lp).set_parent_page(new_root_id);
        buffer_pool_.unpin_page(left_page_id, true);
        write_page_through_wal(transaction_id, left_page_id, lp);

        Page* rp = buffer_pool_.fetch_page(right_page_id);
        BTreeNode(rp).set_parent_page(new_root_id);
        buffer_pool_.unpin_page(right_page_id, true);
        write_page_through_wal(transaction_id, right_page_id, rp);

        root_page_ = new_root_id;
        return;
    }

    // With fixed-size (uint32_t) keys, a page holding entries pushed past
    // is_full()'s 64-byte margin could still absorb one more small entry,
    // so the old code checked is_full() only AFTER writing and split
    // reactively. With variable-length keys (VARCHAR / composite) a single
    // new entry can be far larger than that margin, so we now compute the
    // exact size up front and split BEFORE writing if it wouldn't fit —
    // the margin-based is_full() check afterward remains as a secondary,
    // cheap proactive split for the common case.
    Page* precheck_page = buffer_pool_.fetch_page(parent_page_id);
    BTreeNode precheck_node(precheck_page);
    uint32_t new_entry_size = internal_entry_size(split_key);
    uint32_t used_space     = PAGE_SIZE - precheck_node.free_space_ptr_value();
    uint32_t available      = PAGE_SIZE - NODE_HEADER_SIZE - used_space;
    bool would_overflow     = new_entry_size > available;
    buffer_pool_.unpin_page(parent_page_id, false);

    if (would_overflow) {
        split_internal(transaction_id, parent_page_id);
        // the split changed which page split_key's entry belongs in —
        // re-resolve the parent the same way find_leaf_page would for a
        // leaf: walk from left_page_id's (now possibly updated) parent.
        Page* lp = buffer_pool_.fetch_page(left_page_id);
        parent_page_id = BTreeNode(lp).parent_page();
        buffer_pool_.unpin_page(left_page_id, false);
    }

    Page* parent_page = buffer_pool_.fetch_page(parent_page_id);
    BTreeNode parent_node(parent_page);

    auto entries = parent_node.get_internal_entries();
    entries.push_back({split_key, left_page_id});
    std::sort(entries.begin(), entries.end(),
        [](const InternalEntry& a, const InternalEntry& b) { return a.key < b.key; });

    auto it = std::find_if(entries.begin(), entries.end(),
        [&split_key](const InternalEntry& e) { return e.key == split_key; });
    size_t idx = std::distance(entries.begin(), it);

    if (idx + 1 < entries.size()) {
        entries[idx + 1].child_page_id = right_page_id;
    } else {
        parent_node.set_rightmost_child(right_page_id);
    }

    parent_node.set_internal_entries(entries);
    buffer_pool_.unpin_page(parent_page_id, true);
    write_page_through_wal(transaction_id, parent_page_id, parent_page);

    Page* rp2 = buffer_pool_.fetch_page(right_page_id);
    BTreeNode(rp2).set_parent_page(parent_page_id);
    buffer_pool_.unpin_page(right_page_id, true);
    write_page_through_wal(transaction_id, right_page_id, rp2);

    Page* check_page = buffer_pool_.fetch_page(parent_page_id);
    bool parent_full = BTreeNode(check_page).is_full();
    buffer_pool_.unpin_page(parent_page_id, false);

    if (parent_full) {
        split_internal(transaction_id, parent_page_id);
    }
}

std::vector<std::pair<Key, std::vector<uint8_t>>> BTree::scan_all() const {
    std::vector<std::pair<Key, std::vector<uint8_t>>> result;

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

std::vector<std::pair<Key, std::vector<uint8_t>>> BTree::range_scan(const Key& start, const Key& end) const {
    std::vector<std::pair<Key, std::vector<uint8_t>>> result;

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

namespace {
// true if `key` begins with every element of `prefix`, in order.
// A key shorter than prefix can never match. Equal-length is a match
// exactly when the keys are equal.
bool key_has_prefix(const Key& key, const Key& prefix) {
    if (key.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (!(key[i] == prefix[i])) return false;
    }
    return true;
}
}

std::vector<std::pair<Key, std::vector<uint8_t>>> BTree::prefix_scan(const Key& prefix) const {
    std::vector<std::pair<Key, std::vector<uint8_t>>> result;

    uint32_t current = find_leaf_page(prefix);

    bool done = false;
    while (current != INVALID_PAGE && !done) {
        Page* page = buffer_pool_.fetch_page(current);
        BTreeNode node(page);
        auto entries = node.get_leaf_entries();
        uint32_t next = node.next_leaf();
        buffer_pool_.unpin_page(current, false);

        for (auto& e : entries) {
            // entries sorted ascending; anything strictly less than
            // `prefix` sorts before the matching range and hasn't been
            // reached yet (find_leaf_page can land slightly early, same
            // as range_scan does for its start bound).
            if (e.key < prefix) continue;
            if (!key_has_prefix(e.key, prefix)) { done = true; break; }
            result.emplace_back(e.key, e.value);
        }

        current = next;
    }

    return result;
}

void BTree::remove(const Key& key) {
    uint32_t transaction_id = wal_.begin();
    remove(transaction_id, key);
    wal_.commit(transaction_id);
}

void BTree::remove(uint32_t transaction_id, const Key& key) {
    std::vector<uint32_t> path;
    uint32_t leaf_page_id = find_leaf_page_with_path(key, path);

    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);
    std::vector<uint8_t> existing;
    bool found = node.find_in_leaf(key, existing);
    buffer_pool_.unpin_page(leaf_page_id, false);

    if (!found) {
        throw std::runtime_error("BTree::remove: key not found " + key_to_string(key));
    }

    remove_from_leaf(transaction_id, leaf_page_id, key);

    if (leaf_page_id != root_page_) {
        Page* check_page = buffer_pool_.fetch_page(leaf_page_id);
        uint16_t cells = BTreeNode(check_page).num_cells();
        buffer_pool_.unpin_page(leaf_page_id, false);

        if (cells < min_cells_for(NodeType::LEAF)) {
            handle_underflow(transaction_id, leaf_page_id, path);
        }
    }
}

void BTree::remove_from_leaf(uint32_t transaction_id, uint32_t leaf_page_id, const Key& key) {
    Page* page = buffer_pool_.fetch_page(leaf_page_id);
    BTreeNode node(page);

    auto entries = node.get_leaf_entries();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&key](const LeafEntry& e) { return e.key == key; }), entries.end());

    node.set_leaf_entries(entries);
    buffer_pool_.unpin_page(leaf_page_id, true);
    write_page_through_wal(transaction_id, leaf_page_id, page);
}

void BTree::remove_from_internal(uint32_t transaction_id, uint32_t internal_page_id, const Key& key) {
    Page* page = buffer_pool_.fetch_page(internal_page_id);
    BTreeNode node(page);

    auto entries = node.get_internal_entries();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&key](const InternalEntry& e) { return e.key == key; }), entries.end());

    node.set_internal_entries(entries);
    buffer_pool_.unpin_page(internal_page_id, true);
    write_page_through_wal(transaction_id, internal_page_id, page);
}

BTree::SiblingInfo BTree::find_siblings(uint32_t page_id, uint32_t parent_page_id) const {
    SiblingInfo info;

    Page* parent_page = buffer_pool_.fetch_page(parent_page_id);
    BTreeNode parent_node(parent_page);
    auto entries = parent_node.get_internal_entries();
    uint32_t rightmost = parent_node.rightmost_child();
    buffer_pool_.unpin_page(parent_page_id, false);

    std::vector<uint32_t> children;
    for (auto& e : entries) children.push_back(e.child_page_id);
    children.push_back(rightmost);

    std::vector<Key> keys;
    for (auto& e : entries) keys.push_back(e.key);

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

void BTree::handle_underflow(uint32_t transaction_id, uint32_t page_id, const std::vector<uint32_t>& path) {
    if (page_id == root_page_) {
        Page* page = buffer_pool_.fetch_page(page_id);
        BTreeNode node(page);
        bool is_internal_empty = !node.is_leaf() && node.num_cells() == 0;
        uint32_t only_child = is_internal_empty ? node.rightmost_child() : INVALID_PAGE;
        buffer_pool_.unpin_page(page_id, false);

        if (is_internal_empty && only_child != INVALID_PAGE) {
            Page* child_page = buffer_pool_.fetch_page(only_child);
            BTreeNode(child_page).set_parent_page(INVALID_PAGE);
            buffer_pool_.unpin_page(only_child, true);
            write_page_through_wal(transaction_id, only_child, child_page);

            root_page_ = only_child;

            free_list_.free_page(transaction_id, page_id);
        }
        return;
    }

    if (path.empty()) {
        return;
    }

    uint32_t parent_page_id = path.back();
    std::vector<uint32_t> parent_path(path.begin(), path.end() - 1);

    bool redistributed = try_redistribute(transaction_id, page_id, parent_page_id);
    if (redistributed) {
        return;
    }

    merge_with_sibling(transaction_id, page_id, parent_page_id);

    if (parent_page_id != root_page_) {
        Page* parent_page = buffer_pool_.fetch_page(parent_page_id);
        uint16_t parent_cells = BTreeNode(parent_page).num_cells();
        buffer_pool_.unpin_page(parent_page_id, false);

        if (parent_cells < min_cells_for(NodeType::INTERNAL)) {
            handle_underflow(transaction_id, parent_page_id, parent_path);
        }
    } else {
        handle_underflow(transaction_id, parent_page_id, parent_path);
    }
}

bool BTree::try_redistribute(uint32_t transaction_id, uint32_t page_id, uint32_t parent_page_id) {
    SiblingInfo siblings = find_siblings(page_id, parent_page_id);

    Page* page = buffer_pool_.fetch_page(page_id);
    bool is_leaf = BTreeNode(page).is_leaf();
    buffer_pool_.unpin_page(page_id, false);

    if (siblings.left_sibling != INVALID_PAGE) {
        Page* left_page = buffer_pool_.fetch_page(siblings.left_sibling);
        uint16_t left_cells = BTreeNode(left_page).num_cells();
        buffer_pool_.unpin_page(siblings.left_sibling, false);

        if (left_cells > min_cells_for(is_leaf ? NodeType::LEAF : NodeType::INTERNAL)) {
            if (is_leaf) {
                Page* lp = buffer_pool_.fetch_page(siblings.left_sibling);
                BTreeNode left_node(lp);
                auto left_entries = left_node.get_leaf_entries();
                LeafEntry borrowed = left_entries.back();
                left_entries.pop_back();
                left_node.set_leaf_entries(left_entries);
                buffer_pool_.unpin_page(siblings.left_sibling, true);
                write_page_through_wal(transaction_id, siblings.left_sibling, lp);

                Page* cp = buffer_pool_.fetch_page(page_id);
                BTreeNode cur_node(cp);
                auto cur_entries = cur_node.get_leaf_entries();
                cur_entries.insert(cur_entries.begin(), borrowed);
                cur_node.set_leaf_entries(cur_entries);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(transaction_id, page_id, cp);

                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                for (auto& e : p_entries) {
                    if (e.key == siblings.left_key) { e.key = borrowed.key; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(transaction_id, parent_page_id, pp);

            } else {
                Page* lp = buffer_pool_.fetch_page(siblings.left_sibling);
                BTreeNode left_node(lp);
                auto left_entries = left_node.get_internal_entries();
                uint32_t left_rightmost = left_node.rightmost_child();

                Page* cp = buffer_pool_.fetch_page(page_id);
                BTreeNode cur_node(cp);
                auto cur_entries = cur_node.get_internal_entries();

                cur_entries.insert(cur_entries.begin(), {siblings.left_key, left_rightmost});

                InternalEntry promoted = left_entries.back();
                left_entries.pop_back();
                left_node.set_rightmost_child(promoted.child_page_id);

                left_node.set_internal_entries(left_entries);
                cur_node.set_internal_entries(cur_entries);

                buffer_pool_.unpin_page(siblings.left_sibling, true);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(transaction_id, siblings.left_sibling, lp);
                write_page_through_wal(transaction_id, page_id, cp);

                Page* moved_child = buffer_pool_.fetch_page(left_rightmost);
                BTreeNode(moved_child).set_parent_page(page_id);
                buffer_pool_.unpin_page(left_rightmost, true);
                write_page_through_wal(transaction_id, left_rightmost, moved_child);

                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                for (auto& e : p_entries) {
                    if (e.key == siblings.left_key) { e.key = promoted.key; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(transaction_id, parent_page_id, pp);
            }

            return true;
        }
    }

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
                write_page_through_wal(transaction_id, siblings.right_sibling, rp);

                Page* cp = buffer_pool_.fetch_page(page_id);
                BTreeNode cur_node(cp);
                auto cur_entries = cur_node.get_leaf_entries();
                cur_entries.push_back(borrowed);
                cur_node.set_leaf_entries(cur_entries);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(transaction_id, page_id, cp);

                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                Key new_right_first = right_entries.empty() ? borrowed.key : right_entries.front().key;
                for (auto& e : p_entries) {
                    if (e.key == siblings.right_key) { e.key = new_right_first; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(transaction_id, parent_page_id, pp);

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

                cur_entries.push_back({siblings.right_key, cur_rightmost});
                cur_node.set_rightmost_child(promoted.child_page_id);

                cur_node.set_internal_entries(cur_entries);
                right_node.set_internal_entries(right_entries);

                buffer_pool_.unpin_page(siblings.right_sibling, true);
                buffer_pool_.unpin_page(page_id, true);
                write_page_through_wal(transaction_id, siblings.right_sibling, rp);
                write_page_through_wal(transaction_id, page_id, cp);

                Page* moved_child = buffer_pool_.fetch_page(promoted.child_page_id);
                BTreeNode(moved_child).set_parent_page(page_id);
                buffer_pool_.unpin_page(promoted.child_page_id, true);
                write_page_through_wal(transaction_id, promoted.child_page_id, moved_child);

                Page* pp = buffer_pool_.fetch_page(parent_page_id);
                BTreeNode parent_node(pp);
                auto p_entries = parent_node.get_internal_entries();
                for (auto& e : p_entries) {
                    if (e.key == siblings.right_key) { e.key = promoted.key; break; }
                }
                parent_node.set_internal_entries(p_entries);
                buffer_pool_.unpin_page(parent_page_id, true);
                write_page_through_wal(transaction_id, parent_page_id, pp);
            }

            return true;
        }
    }

    return false;
}

void BTree::merge_with_sibling(uint32_t transaction_id, uint32_t page_id, uint32_t parent_page_id) {
    SiblingInfo siblings = find_siblings(page_id, parent_page_id);

    uint32_t merge_into;
    uint32_t merge_from;
    Key      separator_key;
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

        into_entries.push_back({separator_key, into_rightmost});
        into_entries.insert(into_entries.end(), from_entries.begin(), from_entries.end());

        into_node.set_internal_entries(into_entries);
        into_node.set_rightmost_child(from_rightmost);

        for (auto& e : from_entries) {
            Page* child_page = buffer_pool_.fetch_page(e.child_page_id);
            BTreeNode(child_page).set_parent_page(merge_into);
            buffer_pool_.unpin_page(e.child_page_id, true);
            write_page_through_wal(transaction_id, e.child_page_id, child_page);
        }
        Page* rightmost_child_page = buffer_pool_.fetch_page(from_rightmost);
        BTreeNode(rightmost_child_page).set_parent_page(merge_into);
        buffer_pool_.unpin_page(from_rightmost, true);
        write_page_through_wal(transaction_id, from_rightmost, rightmost_child_page);
    }

    buffer_pool_.unpin_page(merge_into, true);
    buffer_pool_.unpin_page(merge_from, false);

    write_page_through_wal(transaction_id, merge_into, into_page);

    (void)current_is_left;

    // Remove the separator key from the parent — it no longer separates
    // anything. Also fix up parent.rightmost_child if it was pointing at
    // merge_from: rightmost_child is a header field, not an entry in
    // entries[], so remove_from_internal() alone can never touch it. If
    // merge_from was the parent's rightmost child (either because page_id
    // itself was rightmost and merged left, or because the right sibling
    // absorbed here was itself the rightmost), leaving it unfixed produces
    // a stale pointer to a page we're about to hand back to the free list.
    {
        Page* parent_page_fix = buffer_pool_.fetch_page(parent_page_id);
        BTreeNode parent_node_fix(parent_page_fix);

        auto p_entries = parent_node_fix.get_internal_entries();

        // merge_into always sits immediately to the left of merge_from in
        // the child ordering, but entries[i].child_page_id stores the
        // child to the LEFT of entries[i].key. That means the entry
        // matching separator_key is the one holding merge_into (which
        // stays valid), while merge_from — if it wasn't the rightmost
        // child — is the child_page_id of a *different*, later entry.
        // That entry must be retargeted to merge_into before we erase the
        // now-obsolete separator entry, or it's left dangling on a page
        // we're about to hand back to the free list.
        for (auto& e : p_entries) {
            if (e.child_page_id == merge_from) {
                e.child_page_id = merge_into;
                break;
            }
        }

        p_entries.erase(std::remove_if(p_entries.begin(), p_entries.end(),
            [&separator_key](const InternalEntry& e) { return e.key == separator_key; }),
            p_entries.end());
        parent_node_fix.set_internal_entries(p_entries);

        if (parent_node_fix.rightmost_child() == merge_from) {
            parent_node_fix.set_rightmost_child(merge_into);
        }

        buffer_pool_.unpin_page(parent_page_id, true);
        write_page_through_wal(transaction_id, parent_page_id, parent_page_fix);
    }

    // merge_from is now unreachable from the tree. Instead of leaving it
    // permanently orphaned (the old behavior), free it back to the free
    // list under this same transaction — FreeListManager logs the header's
    // first_free_page update as part of transaction_id, so the page
    // becoming free and the rest of this merge become durable together,
    // atomically, at the single wal_.commit() in remove().
    free_list_.free_page(transaction_id, merge_from);
}