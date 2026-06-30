// BTreeNode tests
//g++ -std=c++17 tests/test_btree_node.cpp src/btree/btree_node.cpp -o tests/test_btree_node && ./tests/test_btree_node

#include <iostream>
#include <cassert>
#include <cstring>

#include "../src/storage/page.h"
#include "../src/btree/btree_node.h"

// ─────────────────────────────────────────────
// BTreeNode Tests
// ─────────────────────────────────────────────

void test_init_leaf_node() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    assert(node.type()      == NodeType::LEAF);
    assert(node.is_leaf()   == true);
    assert(node.num_cells() == 0);

    std::cout << "[PASS] init_node creates an empty leaf node\n";
}

void test_init_internal_node() {
    Page page;
    BTreeNode::init_node(&page, NodeType::INTERNAL);
    BTreeNode node(&page);

    assert(node.type()      == NodeType::INTERNAL);
    assert(node.is_leaf()   == false);
    assert(node.num_cells() == 0);

    std::cout << "[PASS] init_node creates an empty internal node\n";
}

void test_leaf_set_and_get_entries() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    std::vector<LeafEntry> entries;
    entries.push_back({10, {1, 2, 3}});
    entries.push_back({20, {4, 5, 6, 7}});
    entries.push_back({30, {8}});

    node.set_leaf_entries(entries);

    assert(node.num_cells() == 3);

    auto result = node.get_leaf_entries();
    assert(result.size() == 3);
    assert(result[0].key == 10);
    assert(result[0].value == std::vector<uint8_t>({1, 2, 3}));
    assert(result[1].key == 20);
    assert(result[1].value == std::vector<uint8_t>({4, 5, 6, 7}));
    assert(result[2].key == 30);
    assert(result[2].value == std::vector<uint8_t>({8}));

    std::cout << "[PASS] leaf set_leaf_entries / get_leaf_entries round trip correctly\n";
}

void test_leaf_find_in_leaf_found() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    std::vector<LeafEntry> entries;
    entries.push_back({5,  {0xAA}});
    entries.push_back({15, {0xBB, 0xCC}});
    node.set_leaf_entries(entries);

    std::vector<uint8_t> out;
    bool found = node.find_in_leaf(15, out);

    assert(found == true);
    assert(out == std::vector<uint8_t>({0xBB, 0xCC}));

    std::cout << "[PASS] find_in_leaf finds an existing key\n";
}

void test_leaf_find_in_leaf_not_found() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    std::vector<LeafEntry> entries;
    entries.push_back({5, {0xAA}});
    node.set_leaf_entries(entries);

    std::vector<uint8_t> out;
    bool found = node.find_in_leaf(999, out);

    assert(found == false);

    std::cout << "[PASS] find_in_leaf returns false for missing key\n";
}

void test_leaf_next_leaf_pointer() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    assert(node.next_leaf() == INVALID_PAGE);

    node.set_next_leaf(42);
    assert(node.next_leaf() == 42);

    std::cout << "[PASS] next_leaf get/set works correctly\n";
}

void test_internal_set_and_get_entries() {
    Page page;
    BTreeNode::init_node(&page, NodeType::INTERNAL);
    BTreeNode node(&page);

    std::vector<InternalEntry> entries;
    entries.push_back({10, 100});
    entries.push_back({20, 200});
    entries.push_back({30, 300});

    node.set_internal_entries(entries);
    node.set_rightmost_child(400);

    assert(node.num_cells() == 3);

    auto result = node.get_internal_entries();
    assert(result.size() == 3);
    assert(result[0].key == 10 && result[0].child_page_id == 100);
    assert(result[1].key == 20 && result[1].child_page_id == 200);
    assert(result[2].key == 30 && result[2].child_page_id == 300);
    assert(node.rightmost_child() == 400);

    std::cout << "[PASS] internal set_internal_entries / get_internal_entries round trip correctly\n";
}

void test_internal_find_child_for_key() {
    Page page;
    BTreeNode::init_node(&page, NodeType::INTERNAL);
    BTreeNode node(&page);

    // keys: 10, 20, 30 separate children 100, 200, 300, 400 (rightmost)
    // key < 10        -> child 100
    // 10 <= key < 20   -> child 200
    // 20 <= key < 30   -> child 300
    // key >= 30        -> child 400 (rightmost)
    std::vector<InternalEntry> entries;
    entries.push_back({10, 100});
    entries.push_back({20, 200});
    entries.push_back({30, 300});
    node.set_internal_entries(entries);
    node.set_rightmost_child(400);

    assert(node.find_child_for_key(5)  == 100);
    assert(node.find_child_for_key(10) == 200);
    assert(node.find_child_for_key(15) == 200);
    assert(node.find_child_for_key(20) == 300);
    assert(node.find_child_for_key(25) == 300);
    assert(node.find_child_for_key(30) == 400);
    assert(node.find_child_for_key(999) == 400);

    std::cout << "[PASS] find_child_for_key returns correct child for all ranges\n";
}

void test_parent_page_get_set() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    assert(node.parent_page() == INVALID_PAGE);

    node.set_parent_page(7);
    assert(node.parent_page() == 7);

    std::cout << "[PASS] parent_page get/set works correctly\n";
}

void test_is_full_detects_capacity() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    assert(node.is_full() == false);

    // insert entries with reasonably sized values until the page fills up
    std::vector<LeafEntry> entries;
    std::vector<uint8_t> value(50, 0xFF);  // 50 byte value per entry

    bool became_full = false;
    for (uint32_t i = 0; i < 200; i++) {
        entries.push_back({i, value});
        node.set_leaf_entries(entries);
        if (node.is_full()) {
            became_full = true;
            break;
        }
    }

    assert(became_full == true);

    std::cout << "[PASS] is_full correctly detects when a page can no longer fit more entries\n";
}

int main() {
    std::cout << "\n=== BTreeNode Tests ===\n";
    test_init_leaf_node();
    test_init_internal_node();
    test_leaf_set_and_get_entries();
    test_leaf_find_in_leaf_found();
    test_leaf_find_in_leaf_not_found();
    test_leaf_next_leaf_pointer();
    test_internal_set_and_get_entries();
    test_internal_find_child_for_key();
    test_parent_page_get_set();
    test_is_full_detects_capacity();

    std::cout << "\nAll BTreeNode tests passed.\n";
    return 0;
}