//# Index tests
//g++ -std=c++17 tests/test_index.cpp src/storage/disk_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp src/header/header_manager.cpp src/btree/free_list_manager.cpp src/btree/key.cpp src/btree/btree_node.cpp src/btree/btree.cpp src/index/index.cpp -o tests/test_index && ./tests/test_index
#include <iostream>
#include <cassert>
#include <filesystem>
#include <algorithm>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/header/header_manager.h"
#include "../src/btree/free_list_manager.h"
#include "../src/index/index.h"

namespace fs = std::filesystem;

static const std::string DB_FILE  = "test_index.db";
static const std::string WAL_FILE = "test_index.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

Key K(int32_t n) { return Key{ Value(n) }; }
Key S(const std::string& s) { return Key{ Value(s) }; }
Value VS(const std::string& s) { return Value(s); }

// bundles the full stack an Index needs, same shape as BTree's test setup.
struct Rig {
    DiskManager      dm;
    BufferPool       bp;
    WALManager       wal;
    HeaderManager    hm;
    FreeListManager  fl;

    Rig() : dm(DB_FILE), bp(dm), wal(WAL_FILE, bp), hm(bp, wal), fl(bp, wal, hm) {
        hm.init();
    }
};

bool contains_key(const std::vector<Key>& keys, const Key& k) {
    return std::find(keys.begin(), keys.end(), k) != keys.end();
}

// ─────────────────────────────────────────────
// Non-unique index — the core "duplicates allowed" behavior
// ─────────────────────────────────────────────

void test_non_unique_index_allows_duplicate_values() {
    cleanup();
    Rig rig;

    IndexSchema schema;
    schema.name         = "idx_country";
    schema.table_name   = "users";
    schema.column_names = {"country"};
    schema.root_page    = INVALID_PAGE;
    schema.is_unique    = false;

    Index idx(schema, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    // three different rows (pk 1, 2, 3) all have country = "IN" — this
    // must NOT throw, even though the raw indexed value repeats.
    idx.insert_entry(S("IN"), K(1));
    idx.insert_entry(S("IN"), K(2));
    idx.insert_entry(S("IN"), K(3));
    idx.insert_entry(S("US"), K(4));

    auto in_rows = idx.find(S("IN"));
    assert(in_rows.size() == 3);
    assert(contains_key(in_rows, K(1)));
    assert(contains_key(in_rows, K(2)));
    assert(contains_key(in_rows, K(3)));

    auto us_rows = idx.find(S("US"));
    assert(us_rows.size() == 1);
    assert(contains_key(us_rows, K(4)));

    auto missing = idx.find(S("FR"));
    assert(missing.empty());

    std::cout << "[PASS] non-unique index allows duplicate indexed values\n";
    cleanup();
}

void test_non_unique_index_survives_many_duplicates_across_splits() {
    cleanup();
    Rig rig;

    IndexSchema schema;
    schema.name         = "idx_status";
    schema.table_name   = "orders";
    schema.column_names = {"status"};
    schema.root_page    = INVALID_PAGE;
    schema.is_unique    = false;

    Index idx(schema, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    // enough entries under ONE repeated value to force leaf splits in the
    // underlying tree, proving prefix_scan still finds every one of them
    // afterward — not just within a single leaf.
    const int N = 200;
    for (int i = 0; i < N; i++) {
        idx.insert_entry(S("pending"), K(i));
    }

    auto rows = idx.find(S("pending"));
    assert(rows.size() == static_cast<size_t>(N));
    for (int i = 0; i < N; i++) {
        assert(contains_key(rows, K(i)));
    }

    std::cout << "[PASS] non-unique index holds many duplicates correctly across leaf splits\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Unique index
// ─────────────────────────────────────────────

void test_unique_index_rejects_duplicate_value() {
    cleanup();
    Rig rig;

    IndexSchema schema;
    schema.name         = "idx_email";
    schema.table_name   = "users";
    schema.column_names = {"email"};
    schema.root_page    = INVALID_PAGE;
    schema.is_unique    = true;

    Index idx(schema, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    idx.insert_entry(S("a@example.com"), K(1));

    bool threw = false;
    try {
        idx.insert_entry(S("a@example.com"), K(2));  // different row, same email
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // the failed insert must not have written anything for pk 2.
    auto rows = idx.find(S("a@example.com"));
    assert(rows.size() == 1);
    assert(rows[0] == K(1));

    std::cout << "[PASS] unique index rejects a second row with the same value\n";
    cleanup();
}

void test_unique_index_allows_reinsert_of_same_pk() {
    cleanup();
    Rig rig;

    IndexSchema schema;
    schema.name         = "idx_email";
    schema.table_name   = "users";
    schema.column_names = {"email"};
    schema.root_page    = INVALID_PAGE;
    schema.is_unique    = true;

    Index idx(schema, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    idx.insert_entry(S("a@example.com"), K(1));
    idx.remove_entry(S("a@example.com"), K(1));
    // after removing, the same (value, pk) pair may be inserted again —
    // this is the shape an UPDATE (delete old entry, insert new entry
    // with the same pk) relies on.
    idx.insert_entry(S("a@example.com"), K(1));

    auto rows = idx.find(S("a@example.com"));
    assert(rows.size() == 1);

    std::cout << "[PASS] unique index allows delete+reinsert of the same primary key\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Composite index
// ─────────────────────────────────────────────

void test_composite_index_prefix_lookup() {
    cleanup();
    Rig rig;

    IndexSchema schema;
    schema.name         = "idx_last_first";
    schema.table_name   = "users";
    schema.column_names = {"last_name", "first_name"};
    schema.root_page    = INVALID_PAGE;
    schema.is_unique    = false;

    Index idx(schema, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    idx.insert_entry(Key{VS("Smith"), VS("Alice")}, K(1));
    idx.insert_entry(Key{VS("Smith"), VS("Bob")},   K(2));
    idx.insert_entry(Key{VS("Jones"), VS("Carol")}, K(3));

    auto smiths = idx.find(Key{VS("Smith")});
    assert(smiths.size() == 2);
    assert(contains_key(smiths, K(1)));
    assert(contains_key(smiths, K(2)));

    auto exact = idx.find(Key{VS("Smith"), VS("Alice")});
    assert(exact.size() == 1);
    assert(exact[0] == K(1));

    std::cout << "[PASS] composite index supports both full-key and column-prefix lookups\n";
    cleanup();
}

// ─────────────────────────────────────────────
// NULL handling
// ─────────────────────────────────────────────

void test_null_indexed_value_is_never_indexed() {
    cleanup();
    Rig rig;

    IndexSchema schema;
    schema.name         = "idx_nickname";
    schema.table_name   = "users";
    schema.column_names = {"nickname"};
    schema.root_page    = INVALID_PAGE;
    schema.is_unique    = true;  // even unique — NULL must still be exempt

    Index idx(schema, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    Key null_val{ Value(std::monostate{}) };
    idx.insert_entry(null_val, K(1));
    idx.insert_entry(null_val, K(2));  // two NULLs — must NOT throw even though unique

    assert(idx.find(null_val).empty());  // nothing was actually indexed

    std::cout << "[PASS] NULL indexed values are silently skipped, even under a unique index\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Remove
// ─────────────────────────────────────────────

void test_remove_entry_removes_exactly_one_row() {
    cleanup();
    Rig rig;

    IndexSchema schema;
    schema.name         = "idx_country";
    schema.table_name   = "users";
    schema.column_names = {"country"};
    schema.root_page    = INVALID_PAGE;
    schema.is_unique    = false;

    Index idx(schema, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    idx.insert_entry(S("IN"), K(1));
    idx.insert_entry(S("IN"), K(2));
    idx.remove_entry(S("IN"), K(1));

    auto rows = idx.find(S("IN"));
    assert(rows.size() == 1);
    assert(rows[0] == K(2));

    std::cout << "[PASS] remove_entry removes only the targeted (value, pk) pair\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Transaction-threaded overload (Option B) — two indexes, one transaction
// ─────────────────────────────────────────────

void test_transaction_threaded_insert_spans_multiple_indexes() {
    cleanup();
    Rig rig;

    IndexSchema schema_a;
    schema_a.name         = "idx_a";
    schema_a.table_name   = "t";
    schema_a.column_names = {"a"};
    schema_a.root_page    = INVALID_PAGE;
    schema_a.is_unique    = false;

    IndexSchema schema_b;
    schema_b.name         = "idx_b";
    schema_b.table_name   = "t";
    schema_b.column_names = {"b"};
    schema_b.root_page    = INVALID_PAGE;
    schema_b.is_unique    = true;

    Index idx_a(schema_a, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);
    Index idx_b(schema_b, /*pk_arity=*/1, rig.bp, rig.wal, rig.fl);

    // simulates what Table::insert() will do once wired up: one shared
    // transaction, writes into both index trees, single commit.
    uint32_t txn = rig.wal.begin();
    idx_a.insert_entry(txn, K(100), K(1));
    idx_b.insert_entry(txn, K(200), K(1));
    rig.wal.commit(txn);

    assert(idx_a.find(K(100)).size() == 1);
    assert(idx_b.find(K(200)).size() == 1);

    std::cout << "[PASS] transaction_id-threaded insert_entry writes multiple indexes under one commit\n";
    cleanup();
}

int main() {
    std::cout << "\n=== Index Tests ===\n";
    test_non_unique_index_allows_duplicate_values();
    test_non_unique_index_survives_many_duplicates_across_splits();
    test_unique_index_rejects_duplicate_value();
    test_unique_index_allows_reinsert_of_same_pk();
    test_composite_index_prefix_lookup();
    test_null_indexed_value_is_never_indexed();
    test_remove_entry_removes_exactly_one_row();
    test_transaction_threaded_insert_spans_multiple_indexes();
    std::cout << "\nAll Index tests passed.\n";
    return 0;
}
