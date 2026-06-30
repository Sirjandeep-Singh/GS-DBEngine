//# BTree tests
//g++ -std=c++17 tests/test_btree.cpp src/storage/disk_manager.cpp src/storage/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp src/btree/btree_node.cpp src/btree/btree.cpp -o tests/test_btree && ./tests/test_btree
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <algorithm>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/btree/btree.h"

namespace fs = std::filesystem;

static const std::string DB_FILE  = "test_btree.db";
static const std::string WAL_FILE = "test_btree.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

std::vector<uint8_t> make_value(uint32_t n) {
    // simple deterministic value derived from n, for verification
    std::string s = "value_" + std::to_string(n);
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ─────────────────────────────────────────────
// Basic insert / search
// ─────────────────────────────────────────────

void test_insert_and_search_single_key() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();  // page 0 reserved
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);
    tree.insert(1, make_value(1));

    auto result = tree.search(1);
    assert(result.has_value());
    assert(result.value() == make_value(1));

    std::cout << "[PASS] insert and search a single key\n";
    cleanup();
}

void test_search_missing_key_returns_nullopt() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);
    tree.insert(1, make_value(1));

    auto result = tree.search(999);
    assert(!result.has_value());

    std::cout << "[PASS] search returns nullopt for missing key\n";
    cleanup();
}

void test_insert_duplicate_key_throws() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);
    tree.insert(5, make_value(5));

    bool threw = false;
    try {
        tree.insert(5, make_value(999));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] insert throws on duplicate key\n";
    cleanup();
}

void test_insert_many_keys_and_search_each() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    const uint32_t N = 1000;
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(i, make_value(i));
    }

    for (uint32_t i = 0; i < N; i++) {
        auto result = tree.search(i);
        assert(result.has_value());
        assert(result.value() == make_value(i));
    }

    std::cout << "[PASS] insert 1000 keys, all searchable correctly\n";
    cleanup();
}

void test_insert_out_of_order_keys() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    std::vector<uint32_t> keys = {500, 10, 999, 1, 250, 750, 0, 333};
    for (uint32_t k : keys) {
        tree.insert(k, make_value(k));
    }

    for (uint32_t k : keys) {
        auto result = tree.search(k);
        assert(result.has_value());
        assert(result.value() == make_value(k));
    }

    std::cout << "[PASS] insert out-of-order keys, all searchable correctly\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Splitting (forces multiple levels)
// ─────────────────────────────────────────────

void test_insert_forces_leaf_split_and_tree_still_correct() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    // insert enough large-value entries to force several leaf splits
    const uint32_t N = 500;
    std::vector<uint8_t> big_value(100, 0xAB);

    for (uint32_t i = 0; i < N; i++) {
        tree.insert(i, big_value);
    }

    for (uint32_t i = 0; i < N; i++) {
        auto result = tree.search(i);
        assert(result.has_value());
        assert(result.value() == big_value);
    }

    std::cout << "[PASS] tree remains correct after many leaf splits\n";
    cleanup();
}

void test_root_page_changes_after_split() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);
    uint32_t initial_root = tree.root_page();

    std::vector<uint8_t> big_value(200, 0xCD);
    for (uint32_t i = 0; i < 100; i++) {
        tree.insert(i, big_value);
    }

    // after enough splits, root should have changed from the original leaf
    assert(tree.root_page() != initial_root);

    std::cout << "[PASS] root_page changes after enough splits occur\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Range scan / scan all
// ─────────────────────────────────────────────

void test_scan_all_returns_sorted_order() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    std::vector<uint32_t> keys = {50, 10, 90, 30, 70, 20, 80, 40, 60, 0};
    for (uint32_t k : keys) {
        tree.insert(k, make_value(k));
    }

    auto all = tree.scan_all();
    assert(all.size() == keys.size());

    for (size_t i = 1; i < all.size(); i++) {
        assert(all[i-1].first < all[i].first);  // strictly ascending
    }

    std::cout << "[PASS] scan_all returns entries in ascending sorted order\n";
    cleanup();
}

void test_range_scan_returns_correct_subset() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    for (uint32_t i = 0; i < 100; i++) {
        tree.insert(i, make_value(i));
    }

    auto result = tree.range_scan(20, 30);
    assert(result.size() == 11);  // 20 through 30 inclusive
    assert(result.front().first == 20);
    assert(result.back().first  == 30);

    for (auto& [key, value] : result) {
        assert(value == make_value(key));
    }

    std::cout << "[PASS] range_scan returns correct subset of keys\n";
    cleanup();
}

void test_range_scan_across_leaf_boundaries() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    std::vector<uint8_t> big_value(100, 0xEF);
    for (uint32_t i = 0; i < 500; i++) {
        tree.insert(i, big_value);
    }

    // range that almost certainly spans multiple leaf pages
    auto result = tree.range_scan(100, 400);
    assert(result.size() == 301);
    assert(result.front().first == 100);
    assert(result.back().first  == 400);

    for (size_t i = 1; i < result.size(); i++) {
        assert(result[i].first == result[i-1].first + 1);  // contiguous
    }

    std::cout << "[PASS] range_scan works correctly across leaf page boundaries\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Deletion — basic
// ─────────────────────────────────────────────

void test_remove_single_key() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);
    tree.insert(1, make_value(1));

    assert(tree.search(1).has_value());

    tree.remove(1);

    assert(!tree.search(1).has_value());

    std::cout << "[PASS] remove deletes a single key correctly\n";
    cleanup();
}

void test_remove_nonexistent_key_throws() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);
    tree.insert(1, make_value(1));

    bool threw = false;
    try {
        tree.remove(999);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] remove throws on nonexistent key\n";
    cleanup();
}

void test_remove_from_multiple_keys() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);
    for (uint32_t i = 0; i < 20; i++) {
        tree.insert(i, make_value(i));
    }

    tree.remove(5);
    tree.remove(10);
    tree.remove(15);

    assert(!tree.search(5).has_value());
    assert(!tree.search(10).has_value());
    assert(!tree.search(15).has_value());

    // remaining keys still present
    for (uint32_t i = 0; i < 20; i++) {
        if (i == 5 || i == 10 || i == 15) continue;
        assert(tree.search(i).has_value());
    }

    std::cout << "[PASS] remove correctly deletes selected keys, leaves others intact\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Deletion — underflow, redistribution, merging
// ─────────────────────────────────────────────

void test_remove_causes_merge_tree_stays_correct() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    // insert enough large-value entries to force multiple leaf splits,
    // creating a multi-level tree with several leaves
    std::vector<uint8_t> big_value(150, 0x11);
    const uint32_t N = 300;
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(i, big_value);
    }

    // delete most of the keys — should trigger redistribution and merging
    // across many nodes as pages empty out
    for (uint32_t i = 0; i < N; i += 2) {
        tree.remove(i);
    }

    // verify remaining keys are still findable
    for (uint32_t i = 0; i < N; i++) {
        auto result = tree.search(i);
        if (i % 2 == 0) {
            assert(!result.has_value());  // deleted
        } else {
            assert(result.has_value());   // should still exist
            assert(result.value() == big_value);
        }
    }

    std::cout << "[PASS] mass deletion triggers merges/redistribution, tree remains correct\n";
    cleanup();
}

void test_remove_all_keys_empties_tree() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    const uint32_t N = 200;
    std::vector<uint8_t> value(80, 0x99);
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(i, value);
    }

    for (uint32_t i = 0; i < N; i++) {
        tree.remove(i);
    }

    for (uint32_t i = 0; i < N; i++) {
        assert(!tree.search(i).has_value());
    }

    auto all = tree.scan_all();
    assert(all.empty());

    std::cout << "[PASS] removing all keys empties the tree completely\n";
    cleanup();
}

void test_remove_then_reinsert_works_correctly() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    BTree tree(bp, wal, INVALID_PAGE);

    for (uint32_t i = 0; i < 50; i++) {
        tree.insert(i, make_value(i));
    }

    for (uint32_t i = 0; i < 25; i++) {
        tree.remove(i);
    }

    // reinsert some of the deleted keys with new values
    for (uint32_t i = 0; i < 25; i++) {
        tree.insert(i, make_value(i + 1000));
    }

    for (uint32_t i = 0; i < 25; i++) {
        auto result = tree.search(i);
        assert(result.has_value());
        assert(result.value() == make_value(i + 1000));
    }
    for (uint32_t i = 25; i < 50; i++) {
        auto result = tree.search(i);
        assert(result.has_value());
        assert(result.value() == make_value(i));
    }

    std::cout << "[PASS] remove then reinsert works correctly, no stale state\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Persistence across reopen
// ─────────────────────────────────────────────

void test_tree_persists_across_reopen() {
    cleanup();
    uint32_t root;
    {
        DiskManager dm(DB_FILE);
        dm.allocate_page();
        BufferPool  bp(dm);
        WALManager  wal(WAL_FILE, bp);

        BTree tree(bp, wal, INVALID_PAGE);
        for (uint32_t i = 0; i < 100; i++) {
            tree.insert(i, make_value(i));
        }
        root = tree.root_page();
    }

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();

    BTree tree2(bp2, wal2, root);

    for (uint32_t i = 0; i < 100; i++) {
        auto result = tree2.search(i);
        assert(result.has_value());
        assert(result.value() == make_value(i));
    }

    std::cout << "[PASS] tree persists correctly across reopen using saved root_page\n";
    cleanup();
}

int main() {
    std::cout << "\n=== BTree Basic Insert/Search Tests ===\n";
    test_insert_and_search_single_key();
    test_search_missing_key_returns_nullopt();
    test_insert_duplicate_key_throws();
    test_insert_many_keys_and_search_each();
    test_insert_out_of_order_keys();

    std::cout << "\n=== BTree Splitting Tests ===\n";
    test_insert_forces_leaf_split_and_tree_still_correct();
    test_root_page_changes_after_split();

    std::cout << "\n=== BTree Range Scan Tests ===\n";
    test_scan_all_returns_sorted_order();
    test_range_scan_returns_correct_subset();
    test_range_scan_across_leaf_boundaries();

    std::cout << "\n=== BTree Basic Deletion Tests ===\n";
    test_remove_single_key();
    test_remove_nonexistent_key_throws();
    test_remove_from_multiple_keys();

    std::cout << "\n=== BTree Underflow / Merge Tests ===\n";
    test_remove_causes_merge_tree_stays_correct();
    test_remove_all_keys_empties_tree();
    test_remove_then_reinsert_works_correctly();

    std::cout << "\n=== BTree Persistence Tests ===\n";
    test_tree_persists_across_reopen();

    std::cout << "\nAll BTree tests passed.\n";
    return 0;
}