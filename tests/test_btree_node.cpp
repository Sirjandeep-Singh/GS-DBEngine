// BTreeNode tests
//g++ -std=c++17 tests/test_btree_node.cpp src/btree/btree_node.cpp src/btree/key.cpp -o tests/test_btree_node && ./tests/test_btree_node

#include <iostream>
#include <cassert>
#include <cstring>
#include <algorithm>

#include "../src/storage/page.h"
#include "../src/btree/btree_node.h"

// small helper: build a single-element Key from an int32_t, since keys are
// now Key (vector<Value>) rather than a bare uint32_t — a scalar key is
// just a 1-element Key.
static Key K(int32_t v) { return Key{ Value(v) }; }

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
    entries.push_back({K(10), {1, 2, 3}});
    entries.push_back({K(20), {4, 5, 6, 7}});
    entries.push_back({K(30), {8}});

    node.set_leaf_entries(entries);

    assert(node.num_cells() == 3);

    auto result = node.get_leaf_entries();
    assert(result.size() == 3);
    assert(result[0].key == K(10));
    assert(result[0].value == std::vector<uint8_t>({1, 2, 3}));
    assert(result[1].key == K(20));
    assert(result[1].value == std::vector<uint8_t>({4, 5, 6, 7}));
    assert(result[2].key == K(30));
    assert(result[2].value == std::vector<uint8_t>({8}));

    std::cout << "[PASS] leaf set_leaf_entries / get_leaf_entries round trip correctly\n";
}

void test_leaf_set_and_get_composite_and_string_entries() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    // a mix of a composite (2-column) key and a scalar VARCHAR key on the
    // same page, to check variable-length keys don't corrupt each other.
    Key composite = { Value(int32_t(1)), Value(int32_t(15)) };
    Key varchar_key = { Value(std::string("hello")) };

    std::vector<LeafEntry> entries;
    entries.push_back({composite,   {0xAA}});
    entries.push_back({varchar_key, {0xBB, 0xCC}});
    // sorted order matters for get_leaf_entries to read back ascending —
    // composite (1,15) < ("hello",) is irrelevant here since these are
    // different key shapes stored in the same page purely to test byte
    // layout does not clobber across variable-length entries; sort so the
    // write order matches what BTree::insert_into_leaf would produce.
    std::sort(entries.begin(), entries.end(),
        [](const LeafEntry& a, const LeafEntry& b) { return a.key < b.key; });

    node.set_leaf_entries(entries);
    assert(node.num_cells() == 2);

    auto result = node.get_leaf_entries();
    assert(result.size() == 2);

    std::vector<uint8_t> out;
    assert(node.find_in_leaf(composite, out) == true);
    assert(out == std::vector<uint8_t>({0xAA}));
    assert(node.find_in_leaf(varchar_key, out) == true);
    assert(out == std::vector<uint8_t>({0xBB, 0xCC}));

    std::cout << "[PASS] leaf entries round trip correctly with composite and VARCHAR keys\n";
}

void test_leaf_find_in_leaf_found() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    std::vector<LeafEntry> entries;
    entries.push_back({K(5),  {0xAA}});
    entries.push_back({K(15), {0xBB, 0xCC}});
    node.set_leaf_entries(entries);

    std::vector<uint8_t> out;
    bool found = node.find_in_leaf(K(15), out);

    assert(found == true);
    assert(out == std::vector<uint8_t>({0xBB, 0xCC}));

    std::cout << "[PASS] find_in_leaf finds an existing key\n";
}

void test_leaf_find_in_leaf_not_found() {
    Page page;
    BTreeNode::init_node(&page, NodeType::LEAF);
    BTreeNode node(&page);

    std::vector<LeafEntry> entries;
    entries.push_back({K(5), {0xAA}});
    node.set_leaf_entries(entries);

    std::vector<uint8_t> out;
    bool found = node.find_in_leaf(K(999), out);

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
    entries.push_back({K(10), 100});
    entries.push_back({K(20), 200});
    entries.push_back({K(30), 300});

    node.set_internal_entries(entries);
    node.set_rightmost_child(400);

    assert(node.num_cells() == 3);

    auto result = node.get_internal_entries();
    assert(result.size() == 3);
    assert(result[0].key == K(10) && result[0].child_page_id == 100);
    assert(result[1].key == K(20) && result[1].child_page_id == 200);
    assert(result[2].key == K(30) && result[2].child_page_id == 300);
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
    entries.push_back({K(10), 100});
    entries.push_back({K(20), 200});
    entries.push_back({K(30), 300});
    node.set_internal_entries(entries);
    node.set_rightmost_child(400);

    assert(node.find_child_for_key(K(5))  == 100);
    assert(node.find_child_for_key(K(10)) == 200);
    assert(node.find_child_for_key(K(15)) == 200);
    assert(node.find_child_for_key(K(20)) == 300);
    assert(node.find_child_for_key(K(25)) == 300);
    assert(node.find_child_for_key(K(30)) == 400);
    assert(node.find_child_for_key(K(999)) == 400);

    std::cout << "[PASS] find_child_for_key returns correct child for all ranges\n";
}

void test_internal_find_child_for_composite_key() {
    Page page;
    BTreeNode::init_node(&page, NodeType::INTERNAL);
    BTreeNode node(&page);

    // separators (1,10) and (2,5) — lexicographic tuple order
    Key sep1 = { Value(int32_t(1)), Value(int32_t(10)) };
    Key sep2 = { Value(int32_t(2)), Value(int32_t(5)) };

    std::vector<InternalEntry> entries;
    entries.push_back({sep1, 100});
    entries.push_back({sep2, 200});
    node.set_internal_entries(entries);
    node.set_rightmost_child(300);

    // (1,5) < (1,10) -> child 100
    assert(node.find_child_for_key({Value(int32_t(1)), Value(int32_t(5))}) == 100);
    // (1,10) <= (1,20) < (2,5) -> child 200
    assert(node.find_child_for_key({Value(int32_t(1)), Value(int32_t(20))}) == 200);
    // (2,5) <= (2,99) -> rightmost child 300
    assert(node.find_child_for_key({Value(int32_t(2)), Value(int32_t(99))}) == 300);

    std::cout << "[PASS] find_child_for_key respects lexicographic tuple order for composite keys\n";
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
    for (int32_t i = 0; i < 200; i++) {
        entries.push_back({K(i), value});
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
    test_leaf_set_and_get_composite_and_string_entries();
    test_leaf_find_in_leaf_found();
    test_leaf_find_in_leaf_not_found();
    test_leaf_next_leaf_pointer();
    test_internal_set_and_get_entries();
    test_internal_find_child_for_key();
    test_internal_find_child_for_composite_key();
    test_parent_page_get_set();
    test_is_full_detects_capacity();

    std::cout << "\nAll BTreeNode tests passed.\n";
    return 0;
}
