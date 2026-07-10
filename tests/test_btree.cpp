//# BTree + FreeListManager tests (same layer — tested together)
//g++ -std=c++17 tests/test_btree.cpp src/storage/disk_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp src/header/header_manager.cpp src/btree/free_list_manager.cpp src/btree/key.cpp src/btree/btree_node.cpp src/btree/btree.cpp -o tests/test_btree && ./tests/test_btree
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <algorithm>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/header/header_manager.h"
#include "../src/btree/free_list_manager.h"
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

// BTree keys are now Key (vector<Value>) rather than a bare uint32_t —
// these two helpers keep the existing uint32_t-based test bodies below
// mostly unchanged, converting at the BTree call boundary. K() builds a
// scalar 1-element INT key; KV() extracts it back out.
Key K(uint32_t n) {
    return Key{ Value(static_cast<int32_t>(n)) };
}

uint32_t KV(const Key& key) {
    return static_cast<uint32_t>(get_int(key.at(0)));
}

// NOTE ON SETUP CHANGE: every test below now bootstraps a full
// DiskManager -> BufferPool -> WALManager -> HeaderManager ->
// FreeListManager chain before constructing a BTree, instead of the old
// `dm.allocate_page()` (which just reserved page 0 as a blank, un-typed
// slot). BTree now allocates every page — including its own root — via
// FreeListManager, and FreeListManager reads/writes a real DBHeader on
// page 0 (specifically `first_free_page`). Without hm.init(), page 0
// would just be zeroed garbage, not a valid header, and FreeListManager
// would be reading uninitialized memory for the free-list head.

// ─────────────────────────────────────────────
// Basic insert / search
// ─────────────────────────────────────────────

void test_insert_and_search_single_key() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);
    tree.insert(K(1), make_value(1));

    auto result = tree.search(K(1));
    assert(result.has_value());
    assert(result.value() == make_value(1));

    std::cout << "[PASS] insert and search a single key\n";
    cleanup();
}

void test_search_missing_key_returns_nullopt() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);
    tree.insert(K(1), make_value(1));

    auto result = tree.search(K(999));
    assert(!result.has_value());

    std::cout << "[PASS] search returns nullopt for missing key\n";
    cleanup();
}

void test_insert_duplicate_key_throws() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);
    tree.insert(K(5), make_value(5));

    bool threw = false;
    try {
        tree.insert(K(5), make_value(999));
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
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    const uint32_t N = 1000;
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(K(i), make_value(i));
    }

    for (uint32_t i = 0; i < N; i++) {
        auto result = tree.search(K(i));
        assert(result.has_value());
        assert(result.value() == make_value(i));
    }

    std::cout << "[PASS] insert 1000 keys, all searchable correctly\n";
    cleanup();
}

void test_insert_out_of_order_keys() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    std::vector<uint32_t> keys = {500, 10, 999, 1, 250, 750, 0, 333};
    for (uint32_t k : keys) {
        tree.insert(K(k), make_value(k));
    }

    for (uint32_t k : keys) {
        auto result = tree.search(K(k));
        assert(result.has_value());
        assert(result.value() == make_value(k));
    }

    std::cout << "[PASS] insert out-of-order keys, all searchable correctly\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Composite keys / non-int keys
// ─────────────────────────────────────────────

void test_insert_and_search_varchar_keys() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    std::vector<std::string> names = {"banana", "apple", "cherry", "date", "fig"};
    for (auto& name : names) {
        Key key = { Value(name) };
        tree.insert(key, std::vector<uint8_t>(name.begin(), name.end()));
    }

    for (auto& name : names) {
        Key key = { Value(name) };
        auto result = tree.search(key);
        assert(result.has_value());
        assert((*result == std::vector<uint8_t>(name.begin(), name.end())));
    }

    // scan_all must come back in lexicographic (dictionary) string order,
    // not insertion order
    auto all = tree.scan_all();
    assert(all.size() == names.size());
    std::vector<std::string> sorted_names = names;
    std::sort(sorted_names.begin(), sorted_names.end());
    for (size_t i = 0; i < all.size(); i++) {
        assert(all[i].first == Key{ Value(sorted_names[i]) });
    }

    std::cout << "[PASS] VARCHAR keys insert/search correctly and sort lexicographically\n";
    cleanup();
}

void test_insert_and_search_composite_keys() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    // composite (order_id, product_id) keys, inserted out of sorted order
    std::vector<std::pair<int32_t, int32_t>> pairs = {
        {2, 3}, {1, 15}, {1, 5}, {2, 1}, {1, 20}, {3, 1}
    };
    for (auto& [a, b] : pairs) {
        Key key = { Value(a), Value(b) };
        tree.insert(key, make_value(static_cast<uint32_t>(a) * 1000 + static_cast<uint32_t>(b)));
    }

    for (auto& [a, b] : pairs) {
        Key key = { Value(a), Value(b) };
        auto result = tree.search(key);
        assert(result.has_value());
        assert(result.value() == make_value(static_cast<uint32_t>(a) * 1000 + static_cast<uint32_t>(b)));
    }

    // a key that shares the first column with existing entries but not the
    // second must be treated as distinct — no accidental collision
    Key not_present = { Value(int32_t(1)), Value(int32_t(6)) };
    assert(!tree.search(not_present).has_value());

    // scan_all must be in lexicographic tuple order: (1,5) < (1,15) < (1,20)
    // < (2,1) < (2,3) < (3,1) — first column dominates, second breaks ties
    auto all = tree.scan_all();
    assert(all.size() == pairs.size());
    std::vector<Key> expected_order = {
        {Value(int32_t(1)), Value(int32_t(5))},
        {Value(int32_t(1)), Value(int32_t(15))},
        {Value(int32_t(1)), Value(int32_t(20))},
        {Value(int32_t(2)), Value(int32_t(1))},
        {Value(int32_t(2)), Value(int32_t(3))},
        {Value(int32_t(3)), Value(int32_t(1))},
    };
    for (size_t i = 0; i < all.size(); i++) {
        assert(all[i].first == expected_order[i]);
    }

    std::cout << "[PASS] composite keys insert/search correctly and sort lexicographically\n";
    cleanup();
}

void test_composite_key_duplicate_throws() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    Key key = { Value(int32_t(1)), Value(int32_t(1)) };
    tree.insert(key, make_value(1));

    bool threw = false;
    try {
        tree.insert(key, make_value(2));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] duplicate composite key throws\n";
    cleanup();
}

void test_composite_keys_survive_splits_and_removal() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    // enough composite keys, with large-ish values, to force real splits
    // and later merges — not just a single leaf
    std::vector<std::vector<uint8_t>> values;
    const int32_t ORDERS = 20;
    const int32_t ITEMS_PER_ORDER = 10;
    std::vector<uint8_t> big_value(60, 0x77);

    for (int32_t order = 0; order < ORDERS; order++) {
        for (int32_t item = 0; item < ITEMS_PER_ORDER; item++) {
            Key key = { Value(order), Value(item) };
            tree.insert(key, big_value);
        }
    }

    for (int32_t order = 0; order < ORDERS; order++) {
        for (int32_t item = 0; item < ITEMS_PER_ORDER; item++) {
            Key key = { Value(order), Value(item) };
            auto result = tree.search(key);
            assert(result.has_value());
            assert(result.value() == big_value);
        }
    }

    // remove every other item within each order, verify the rest survive —
    // exercises underflow/redistribution/merge with composite keys
    for (int32_t order = 0; order < ORDERS; order++) {
        for (int32_t item = 0; item < ITEMS_PER_ORDER; item += 2) {
            Key key = { Value(order), Value(item) };
            tree.remove(key);
        }
    }

    for (int32_t order = 0; order < ORDERS; order++) {
        for (int32_t item = 0; item < ITEMS_PER_ORDER; item++) {
            Key key = { Value(order), Value(item) };
            auto result = tree.search(key);
            if (item % 2 == 0) {
                assert(!result.has_value());
            } else {
                assert(result.has_value());
                assert(result.value() == big_value);
            }
        }
    }

    std::cout << "[PASS] composite keys remain correct across splits, merges, and removal\n";
    cleanup();
}

void test_range_scan_with_composite_keys_scoped_to_first_column() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    // three orders, five items each — range_scan bounded to order 2 only,
    // using (2, INT_MIN)-ish and (2, INT_MAX)-ish bounds via explicit item bounds
    for (int32_t order = 1; order <= 3; order++) {
        for (int32_t item = 0; item < 5; item++) {
            Key key = { Value(order), Value(item) };
            tree.insert(key, make_value(static_cast<uint32_t>(order * 10 + item)));
        }
    }

    Key start = { Value(int32_t(2)), Value(int32_t(0)) };
    Key end   = { Value(int32_t(2)), Value(int32_t(4)) };
    auto result = tree.range_scan(start, end);

    assert(result.size() == 5);
    for (auto& [key, value] : result) {
        assert(get_int(key[0]) == 2);
    }

    std::cout << "[PASS] range_scan correctly scopes to a composite key range within one first-column group\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Splitting (forces multiple levels)
// ─────────────────────────────────────────────

void test_insert_forces_leaf_split_and_tree_still_correct() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    // insert enough large-value entries to force several leaf splits
    const uint32_t N = 500;
    std::vector<uint8_t> big_value(100, 0xAB);

    for (uint32_t i = 0; i < N; i++) {
        tree.insert(K(i), big_value);
    }

    for (uint32_t i = 0; i < N; i++) {
        auto result = tree.search(K(i));
        assert(result.has_value());
        assert(result.value() == big_value);
    }

    std::cout << "[PASS] tree remains correct after many leaf splits\n";
    cleanup();
}

void test_root_page_changes_after_split() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);
    uint32_t initial_root = tree.root_page();

    std::vector<uint8_t> big_value(200, 0xCD);
    for (uint32_t i = 0; i < 100; i++) {
        tree.insert(K(i), big_value);
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
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    std::vector<uint32_t> keys = {50, 10, 90, 30, 70, 20, 80, 40, 60, 0};
    for (uint32_t k : keys) {
        tree.insert(K(k), make_value(k));
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
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    for (uint32_t i = 0; i < 100; i++) {
        tree.insert(K(i), make_value(i));
    }

    auto result = tree.range_scan(K(20), K(30));
    assert(result.size() == 11);  // 20 through 30 inclusive
    assert(result.front().first == K(20));
    assert(result.back().first  == K(30));

    for (auto& [key, value] : result) {
        assert(value == make_value(KV(key)));
    }

    std::cout << "[PASS] range_scan returns correct subset of keys\n";
    cleanup();
}

void test_range_scan_across_leaf_boundaries() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    std::vector<uint8_t> big_value(100, 0xEF);
    for (uint32_t i = 0; i < 500; i++) {
        tree.insert(K(i), big_value);
    }

    // range that almost certainly spans multiple leaf pages
    auto result = tree.range_scan(K(100), K(400));
    assert(result.size() == 301);
    assert(result.front().first == K(100));
    assert(result.back().first  == K(400));

    for (size_t i = 1; i < result.size(); i++) {
        assert(KV(result[i].first) == KV(result[i-1].first) + 1);  // contiguous
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
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);
    tree.insert(K(1), make_value(1));

    assert(tree.search(K(1)).has_value());

    tree.remove(K(1));

    assert(!tree.search(K(1)).has_value());

    std::cout << "[PASS] remove deletes a single key correctly\n";
    cleanup();
}

void test_remove_nonexistent_key_throws() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);
    tree.insert(K(1), make_value(1));

    bool threw = false;
    try {
        tree.remove(K(999));
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
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);
    for (uint32_t i = 0; i < 20; i++) {
        tree.insert(K(i), make_value(i));
    }

    tree.remove(K(5));
    tree.remove(K(10));
    tree.remove(K(15));

    assert(!tree.search(K(5)).has_value());
    assert(!tree.search(K(10)).has_value());
    assert(!tree.search(K(15)).has_value());

    // remaining keys still present
    for (uint32_t i = 0; i < 20; i++) {
        if (i == 5 || i == 10 || i == 15) continue;
        assert(tree.search(K(i)).has_value());
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
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    // insert enough large-value entries to force multiple leaf splits,
    // creating a multi-level tree with several leaves
    std::vector<uint8_t> big_value(150, 0x11);
    const uint32_t N = 300;
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(K(i), big_value);
    }

    // delete most of the keys — should trigger redistribution and merging
    // across many nodes as pages empty out
    for (uint32_t i = 0; i < N; i += 2) {
        tree.remove(K(i));
    }

    // verify remaining keys are still findable
    for (uint32_t i = 0; i < N; i++) {
        auto result = tree.search(K(i));
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
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    const uint32_t N = 200;
    std::vector<uint8_t> value(80, 0x99);
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(K(i), value);
    }

    for (uint32_t i = 0; i < N; i++) {
        tree.remove(K(i));
    }

    for (uint32_t i = 0; i < N; i++) {
        assert(!tree.search(K(i)).has_value());
    }

    auto all = tree.scan_all();
    assert(all.empty());

    std::cout << "[PASS] removing all keys empties the tree completely\n";
    cleanup();
}

void test_remove_then_reinsert_works_correctly() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    for (uint32_t i = 0; i < 50; i++) {
        tree.insert(K(i), make_value(i));
    }

    for (uint32_t i = 0; i < 25; i++) {
        tree.remove(K(i));
    }

    // reinsert some of the deleted keys with new values
    for (uint32_t i = 0; i < 25; i++) {
        tree.insert(K(i), make_value(i + 1000));
    }

    for (uint32_t i = 0; i < 25; i++) {
        auto result = tree.search(K(i));
        assert(result.has_value());
        assert(result.value() == make_value(i + 1000));
    }
    for (uint32_t i = 25; i < 50; i++) {
        auto result = tree.search(K(i));
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
        BufferPool  bp(dm);
        WALManager  wal(WAL_FILE, bp);
        HeaderManager hm(bp, wal);
        hm.init();
        FreeListManager free_list(bp, wal, hm);

        BTree tree(bp, wal, free_list, INVALID_PAGE);
        for (uint32_t i = 0; i < 100; i++) {
            tree.insert(K(i), make_value(i));
        }
        root = tree.root_page();
    }

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();
    HeaderManager hm2(bp2, wal2);
    hm2.load();
    FreeListManager free_list2(bp2, wal2, hm2);

    BTree tree2(bp2, wal2, free_list2, root);

    for (uint32_t i = 0; i < 100; i++) {
        auto result = tree2.search(K(i));
        assert(result.has_value());
        assert(result.value() == make_value(i));
    }

    std::cout << "[PASS] tree persists correctly across reopen using saved root_page\n";
    cleanup();
}

// ─────────────────────────────────────────────
// FreeListManager — direct unit tests
//
// BTree and FreeListManager sit at the same layer (both depend on
// BufferPool + WALManager; BTree additionally depends on FreeListManager
// for allocation), so their tests live together in this file. These
// tests exercise FreeListManager directly, without going through BTree,
// to isolate push/pop/reuse behavior from B+Tree split/merge logic.
// ─────────────────────────────────────────────

void test_freelist_allocate_from_empty_list_gets_fresh_page() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    assert(hm.header.first_free_page == NO_FREE_PAGE);

    uint32_t txn = wal.begin();
    uint32_t page_id;
    free_list.allocate_page(txn, page_id);
    wal.commit(txn);

    // free list was empty, so this must have fallen back to a genuinely
    // new page — page 0 is the header, so the first allocated page is 1
    assert(page_id == 1);
    // and the free-list head must be untouched, since no pop happened
    assert(hm.header.first_free_page == NO_FREE_PAGE);

    std::cout << "[PASS] allocate_page falls back to a fresh page when free list is empty\n";
    cleanup();
}

void test_freelist_free_then_allocate_reuses_same_page() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    uint32_t txn1 = wal.begin();
    uint32_t page_id_a;
    free_list.allocate_page(txn1, page_id_a);
    wal.commit(txn1);

    uint32_t txn2 = wal.begin();
    free_list.free_page(txn2, page_id_a);
    // header is updated in-memory immediately, even before commit,
    // since FreeListManager mutates header_manager_.header directly
    assert(hm.header.first_free_page == page_id_a);
    wal.commit(txn2);

    uint32_t txn3 = wal.begin();
    uint32_t page_id_b;
    free_list.allocate_page(txn3, page_id_b);
    wal.commit(txn3);

    // the freed page must be handed back out again, not a new one
    assert(page_id_b == page_id_a);
    // and the list must be empty again afterward
    assert(hm.header.first_free_page == NO_FREE_PAGE);
    // total_pages must not have grown — no new page was allocated
    assert(dm.total_pages() == 2);  // page 0 (header) + the one reused page

    std::cout << "[PASS] freeing then allocating reuses the exact same page, no growth\n";
    cleanup();
}

void test_freelist_lifo_order() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    // allocate three distinct pages
    uint32_t page_a, page_b, page_c;
    { uint32_t t = wal.begin(); free_list.allocate_page(t, page_a); wal.commit(t); }
    { uint32_t t = wal.begin(); free_list.allocate_page(t, page_b); wal.commit(t); }
    { uint32_t t = wal.begin(); free_list.allocate_page(t, page_c); wal.commit(t); }

    // free them in order a, b, c — list is a stack, so c ends up on top
    { uint32_t t = wal.begin(); free_list.free_page(t, page_a); wal.commit(t); }
    { uint32_t t = wal.begin(); free_list.free_page(t, page_b); wal.commit(t); }
    { uint32_t t = wal.begin(); free_list.free_page(t, page_c); wal.commit(t); }

    // popping should return them in reverse order: c, then b, then a
    uint32_t popped1, popped2, popped3;
    { uint32_t t = wal.begin(); free_list.allocate_page(t, popped1); wal.commit(t); }
    { uint32_t t = wal.begin(); free_list.allocate_page(t, popped2); wal.commit(t); }
    { uint32_t t = wal.begin(); free_list.allocate_page(t, popped3); wal.commit(t); }

    assert(popped1 == page_c);
    assert(popped2 == page_b);
    assert(popped3 == page_a);

    std::cout << "[PASS] free list behaves as a LIFO stack (last freed, first reused)\n";
    cleanup();
}

void test_freelist_persists_across_reopen() {
    cleanup();
    uint32_t freed_page_id;
    {
        DiskManager dm(DB_FILE);
        BufferPool  bp(dm);
        WALManager  wal(WAL_FILE, bp);
        HeaderManager hm(bp, wal);
        hm.init();
        FreeListManager free_list(bp, wal, hm);

        uint32_t t1 = wal.begin();
        free_list.allocate_page(t1, freed_page_id);
        wal.commit(t1);

        uint32_t t2 = wal.begin();
        free_list.free_page(t2, freed_page_id);
        wal.commit(t2);
    }  // scope exit — BufferPool destructor flushes+syncs

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();
    HeaderManager hm2(bp2, wal2);
    hm2.load();
    FreeListManager free_list2(bp2, wal2, hm2);

    // the free list's head must have survived the reopen
    assert(hm2.header.first_free_page == freed_page_id);

    uint32_t t3 = wal2.begin();
    uint32_t reused_page_id;
    free_list2.allocate_page(t3, reused_page_id);
    wal2.commit(t3);

    assert(reused_page_id == freed_page_id);

    std::cout << "[PASS] free list state persists correctly across reopen\n";
    cleanup();
}

// ─────────────────────────────────────────────
// FreeListManager + BTree — integration
// ─────────────────────────────────────────────

void test_btree_reuses_freed_pages_after_merge() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();
    FreeListManager free_list(bp, wal, hm);

    BTree tree(bp, wal, free_list, INVALID_PAGE);

    std::vector<uint8_t> big_value(150, 0x33);
    const uint32_t N = 300;

    // Phase 1: insert enough to force several splits.
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(K(i), big_value);
    }
    uint32_t pages_after_inserts = dm.total_pages();

    // Phase 2: remove everything — should trigger cascading merges,
    // freeing pages back to the free list rather than leaking them.
    for (uint32_t i = 0; i < N; i++) {
        tree.remove(K(i));
    }
    uint32_t pages_after_removal = dm.total_pages();

    // the file never shrinks (pages are reused, not truncated), so this
    // should be unchanged — this line mainly documents that expectation
    assert(pages_after_removal == pages_after_inserts);

    // Phase 3: reinsert the same volume of data. If freed pages are
    // actually being reused (not leaked), this must NOT need to grow
    // the file back up by another full N-worth of pages — it should
    // reuse what's already sitting on the free list from phase 2.
    for (uint32_t i = 0; i < N; i++) {
        tree.insert(K(i), big_value);
    }
    uint32_t pages_after_reinsert = dm.total_pages();

    assert(pages_after_reinsert <= pages_after_removal);

    // and correctness still holds after all that churn
    for (uint32_t i = 0; i < N; i++) {
        auto result = tree.search(K(i));
        assert(result.has_value());
        assert(result.value() == big_value);
    }

    std::cout << "[PASS] BTree reuses freed pages via FreeListManager instead of leaking them\n";
    cleanup();
}

int main() {
    std::cout << "\n=== BTree Basic Insert/Search Tests ===\n";
    test_insert_and_search_single_key();
    test_search_missing_key_returns_nullopt();
    test_insert_duplicate_key_throws();
    test_insert_many_keys_and_search_each();
    test_insert_out_of_order_keys();

    std::cout << "\n=== BTree Composite / Non-Int Key Tests ===\n";
    test_insert_and_search_varchar_keys();
    test_insert_and_search_composite_keys();
    test_composite_key_duplicate_throws();
    test_composite_keys_survive_splits_and_removal();
    test_range_scan_with_composite_keys_scoped_to_first_column();

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

    std::cout << "\n=== FreeListManager Tests ===\n";
    test_freelist_allocate_from_empty_list_gets_fresh_page();
    test_freelist_free_then_allocate_reuses_same_page();
    test_freelist_lifo_order();
    test_freelist_persists_across_reopen();

    std::cout << "\n=== FreeListManager + BTree Integration ===\n";
    test_btree_reuses_freed_pages_after_merge();

    std::cout << "\nAll BTree + FreeListManager tests passed.\n";
    return 0;
}