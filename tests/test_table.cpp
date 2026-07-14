//g++ -std=c++17 tests/test_table.cpp src/row/serializer.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/header/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/btree/free_list_manager.cpp src/table/table.cpp -o tests/test_table && ./tests/test_table
#include <iostream>
#include <cassert>
#include <filesystem>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/header/header_manager.h"
#include "../src/btree/free_list_manager.h"
#include "../src/catalog/schema.h"
#include "../src/row/row.h"
#include "../src/table/table.h"

namespace fs = std::filesystem;

static const std::string DB_FILE  = "test_table.db";
static const std::string WAL_FILE = "test_table.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

// helper — users schema: id INT PK auto_increment, name VARCHAR(50), age INT nullable, active BOOLEAN
TableSchema make_users_schema(uint32_t root_page = INVALID_PAGE) {
    TableSchema schema;
    schema.name              = "users";
    schema.root_page         = root_page;
    schema.primary_key_indices = {0};

    Column id;
    id.name = "id"; id.type = ColumnType::INT;
    id.max_length = 0; id.is_nullable = false;
    id.is_primary_key = true; id.auto_increment = true;

    Column name;
    name.name = "name"; name.type = ColumnType::VARCHAR;
    name.max_length = 50; name.is_nullable = false;
    name.is_primary_key = false; name.auto_increment = false;

    Column age;
    age.name = "age"; age.type = ColumnType::INT;
    age.max_length = 0; age.is_nullable = true;
    age.is_primary_key = false; age.auto_increment = false;

    Column active;
    active.name = "active"; active.type = ColumnType::BOOLEAN;
    active.max_length = 0; active.is_nullable = false;
    active.is_primary_key = false; active.auto_increment = false;

    schema.columns = {id, name, age, active};
    return schema;
}

// helper — makes a setup with DiskManager, BufferPool, WALManager, HeaderManager, FreeListManager
struct Env {
    DiskManager      dm;
    BufferPool       bp;
    WALManager       wal;
    HeaderManager    hm;
    FreeListManager  fl;

    // reopen=false (default): fresh database — hm.init() allocates page 0.
    // reopen=true: reopening an existing DB_FILE — recovers the WAL then
    // loads the existing header instead of re-initializing it.
    explicit Env(bool reopen = false) : dm(DB_FILE), bp(dm), wal(WAL_FILE, bp), hm(bp, wal), fl(bp, wal, hm) {
        if (reopen) {
            wal.recover();
            hm.load();
        } else {
            hm.init();          // allocates page 0 — header (also sets first_free_page = NO_FREE_PAGE, required by FreeListManager)
            uint32_t placeholder_id;
            bp.new_page(placeholder_id);   // page 1 — catalog (not used here but keeps page numbering consistent)
        }
    }
};

// helper — make a user row
Row make_user(int32_t id, const std::string& name, int32_t age, bool active) {
    Row row;
    row.values = {id, name, age, active};
    return row;
}

Row make_user_null_id(const std::string& name, int32_t age, bool active) {
    Row row;
    row.values = {std::monostate{}, name, age, active};
    return row;
}

// helper — builds a single-column primary Key from an int, for the
// users schema's INT primary key (id).
Key pk(int32_t id) {
    return Key{Value(id)};
}

// ─────────────────────────────────────────────
// INSERT Tests
// ─────────────────────────────────────────────

void test_insert_explicit_key() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    Row row = make_user(1, "Alice", 25, true);
    Key key = table.insert(row);
    assert(key == pk(1));

    std::cout << "[PASS] insert with explicit primary key returns correct key\n";
    cleanup();
}

void test_insert_auto_increment() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    Row r1 = make_user_null_id("Alice", 25, true);
    Row r2 = make_user_null_id("Bob", 30, false);
    Row r3 = make_user_null_id("Carol", 22, true);

    Key k1 = table.insert(r1);
    Key k2 = table.insert(r2);
    Key k3 = table.insert(r3);

    assert(k1 == pk(1));
    assert(k2 == pk(2));
    assert(k3 == pk(3));

    std::cout << "[PASS] auto-increment assigns sequential keys\n";
    cleanup();
}

void test_insert_duplicate_key_throws() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    Row r1 = make_user(5, "Alice", 25, true);
    table.insert(r1);

    Row r2 = make_user(5, "Bob", 30, false);
    bool threw = false;
    try {
        table.insert(r2);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] insert throws on duplicate primary key\n";
    cleanup();
}

// ─────────────────────────────────────────────
// SELECT Tests
// ─────────────────────────────────────────────

void test_select_by_key_found() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    Row row = make_user(1, "Alice", 25, true);
    table.insert(row);

    auto result = table.select_by_key(pk(1));
    assert(result.has_value());
    assert(get_string(result->get(1)) == "Alice");
    assert(get_int(result->get(2))    == 25);
    assert(get_bool(result->get(3))   == true);

    std::cout << "[PASS] select_by_key returns correct row\n";
    cleanup();
}

void test_select_by_key_not_found() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    auto result = table.select_by_key(pk(999));
    assert(!result.has_value());

    std::cout << "[PASS] select_by_key returns nullopt for missing key\n";
    cleanup();
}

void test_scan_all_rows() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    Row r1 = make_user(1, "Alice", 25, true);
    Row r2 = make_user(2, "Bob",   30, false);
    Row r3 = make_user(3, "Carol", 22, true);
    table.insert(r1);
    table.insert(r2);
    table.insert(r3);

    auto results = table.scan([](const Row&) { return true; });
    assert(results.size() == 3);
    assert(results[0].primary_key == pk(1));
    assert(results[1].primary_key == pk(2));
    assert(results[2].primary_key == pk(3));

    std::cout << "[PASS] scan returns all rows in primary key order\n";
    cleanup();
}

void test_scan_with_predicate() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));
    table.insert(make_user(2, "Bob",   30, false));
    table.insert(make_user(3, "Carol", 22, true));
    table.insert(make_user(4, "Dave",  40, true));

    // filter: active == true
    auto results = table.scan([](const Row& row) {
        return get_bool(row.get(3)) == true;
    });

    assert(results.size() == 3);
    assert(get_string(results[0].row.get(1)) == "Alice");
    assert(get_string(results[1].row.get(1)) == "Carol");
    assert(get_string(results[2].row.get(1)) == "Dave");

    std::cout << "[PASS] scan with predicate returns only matching rows\n";
    cleanup();
}

void test_scan_with_null_column() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    Row r1 = make_user(1, "Alice", 25, true);
    Row r2;
    r2.values = {int32_t(2), std::string("Bob"), std::monostate{}, bool(false)};
    table.insert(r1);
    table.insert(r2);

    auto result = table.select_by_key(pk(2));
    assert(result.has_value());
    assert(is_null(result->get(2)));

    std::cout << "[PASS] scan correctly handles rows with NULL columns\n";
    cleanup();
}

// ─────────────────────────────────────────────
// UPDATE Tests
// ─────────────────────────────────────────────

void test_update_single_column() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));

    // update age to 26
    table.update(pk(1), {{2, Value(int32_t(26))}});

    auto result = table.select_by_key(pk(1));
    assert(result.has_value());
    assert(get_int(result->get(2)) == 26);
    assert(get_string(result->get(1)) == "Alice");  // unchanged

    std::cout << "[PASS] update changes only the specified column\n";
    cleanup();
}

void test_update_multiple_columns() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));

    table.update(pk(1), {
        {1, Value(std::string("Alicia"))},
        {3, Value(bool(false))}
    });

    auto result = table.select_by_key(pk(1));
    assert(result.has_value());
    assert(get_string(result->get(1)) == "Alicia");
    assert(get_bool(result->get(3))   == false);
    assert(get_int(result->get(2))    == 25);  // unchanged

    std::cout << "[PASS] update changes multiple columns correctly\n";
    cleanup();
}

void test_update_nonexistent_key_throws() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    bool threw = false;
    try {
        table.update(pk(999), {{2, Value(int32_t(30))}});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] update throws on nonexistent primary key\n";
    cleanup();
}

void test_update_primary_key_moves_row() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));

    table.update(pk(1), {{0, Value(int32_t(99))}});  // col 0 = primary key

    // the old key is gone
    assert(!table.select_by_key(pk(1)).has_value());

    // the row now lives at the new key, with every other column intact
    auto moved = table.select_by_key(pk(99));
    assert(moved.has_value());
    assert(get_string(moved->get(1)) == "Alice");
    assert(get_int(moved->get(2))    == 25);
    assert(get_bool(moved->get(3))   == true);

    std::cout << "[PASS] update moves a row to its new primary key\n";
    cleanup();
}

void test_update_primary_key_collision_throws() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));
    table.insert(make_user(2, "Bob",   30, false));

    bool threw = false;
    try {
        table.update(pk(1), {{0, Value(int32_t(2))}});  // 2 already belongs to Bob
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    // neither row should have moved — the throw happens before any write
    assert(table.select_by_key(pk(1)).has_value());
    auto bob = table.select_by_key(pk(2));
    assert(bob.has_value());
    assert(get_string(bob->get(1)) == "Bob");  // still Bob, not overwritten by Alice

    std::cout << "[PASS] update throws when the new primary key already belongs to a different row\n";
    cleanup();
}

void test_update_primary_key_to_same_value_is_a_noop_collision_check() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));

    // "changing" the primary key to its own current value must not be
    // rejected as a collision with itself
    table.update(pk(1), {{0, Value(int32_t(1))}, {2, Value(int32_t(26))}});

    auto result = table.select_by_key(pk(1));
    assert(result.has_value());
    assert(get_int(result->get(2)) == 26);

    std::cout << "[PASS] update to the primary key's own current value is not treated as a collision\n";
    cleanup();
}

// ─────────────────────────────────────────────
// DELETE Tests
// ─────────────────────────────────────────────

void test_delete_row() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));
    assert(table.select_by_key(pk(1)).has_value());

    table.delete_row(pk(1));
    assert(!table.select_by_key(pk(1)).has_value());

    std::cout << "[PASS] delete_row removes the row correctly\n";
    cleanup();
}

void test_delete_nonexistent_key_throws() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    bool threw = false;
    try {
        table.delete_row(pk(999));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] delete_row throws on nonexistent primary key\n";
    cleanup();
}

void test_delete_where() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));
    table.insert(make_user(2, "Bob",   30, false));
    table.insert(make_user(3, "Carol", 22, true));
    table.insert(make_user(4, "Dave",  40, false));

    // delete inactive users
    uint32_t count = table.delete_where([](const Row& row) {
        return get_bool(row.get(3)) == false;
    });

    assert(count == 2);
    assert(!table.select_by_key(pk(2)).has_value());
    assert(!table.select_by_key(pk(4)).has_value());
    assert(table.select_by_key(pk(1)).has_value());
    assert(table.select_by_key(pk(3)).has_value());

    std::cout << "[PASS] delete_where deletes only matching rows\n";
    cleanup();
}

void test_update_where() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    table.insert(make_user(1, "Alice", 25, true));
    table.insert(make_user(2, "Bob",   30, false));
    table.insert(make_user(3, "Carol", 22, true));

    // set active=false for users older than 24
    uint32_t count = table.update_where(
        [](const Row& row) { return get_int(row.get(2)) > 24; },
        {{3, Value(bool(false))}}
    );

    assert(count == 2);

    auto alice = table.select_by_key(pk(1));
    auto bob   = table.select_by_key(pk(2));
    auto carol = table.select_by_key(pk(3));

    assert(get_bool(alice->get(3)) == false);  // age 25, updated
    assert(get_bool(bob->get(3))   == false);  // age 30, updated
    assert(get_bool(carol->get(3)) == true);   // age 22, unchanged

    std::cout << "[PASS] update_where updates only matching rows\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Auto-increment persistence
// ─────────────────────────────────────────────

void test_auto_increment_continues_after_reopen() {
    cleanup();
    uint32_t root;
    {
        Env env;
        TableSchema schema = make_users_schema();
        Table table(schema, env.bp, env.wal, env.fl);

        table.insert(make_user_null_id("Alice", 25, true));   // key=1
        table.insert(make_user_null_id("Bob",   30, false));  // key=2
        root = table.root_page();
    }

    Env env2(/*reopen=*/true);
    TableSchema schema2 = make_users_schema(root);
    Table table2(schema2, env2.bp, env2.wal, env2.fl);

    // should continue from 3, not restart at 1
    Row r = make_user_null_id("Carol", 22, true);
    Key key = table2.insert(r);
    assert(key == pk(3));

    std::cout << "[PASS] auto-increment resumes correctly after reopen\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Large table
// ─────────────────────────────────────────────

void test_insert_and_scan_many_rows() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();
    Table table(schema, env.bp, env.wal, env.fl);

    const uint32_t N = 500;
    for (uint32_t i = 1; i <= N; i++) {
        Row r = make_user(i, "user_" + std::to_string(i), i % 100, i % 2 == 0);
        table.insert(r);
    }

    auto all = table.scan([](const Row&) { return true; });
    assert(all.size() == N);

    for (uint32_t i = 1; i <= N; i++) {
        auto result = table.select_by_key(pk(static_cast<int32_t>(i)));
        assert(result.has_value());
        assert(get_string(result->get(1)) == "user_" + std::to_string(i));
    }

    std::cout << "[PASS] insert and scan 500 rows correctly\n";
    cleanup();
}

int main() {
    std::cout << "\n=== INSERT Tests ===\n";
    test_insert_explicit_key();
    test_insert_auto_increment();
    test_insert_duplicate_key_throws();

    std::cout << "\n=== SELECT Tests ===\n";
    test_select_by_key_found();
    test_select_by_key_not_found();
    test_scan_all_rows();
    test_scan_with_predicate();
    test_scan_with_null_column();

    std::cout << "\n=== UPDATE Tests ===\n";
    test_update_single_column();
    test_update_multiple_columns();
    test_update_nonexistent_key_throws();
    test_update_primary_key_moves_row();
    test_update_primary_key_collision_throws();
    test_update_primary_key_to_same_value_is_a_noop_collision_check();

    std::cout << "\n=== DELETE Tests ===\n";
    test_delete_row();
    test_delete_nonexistent_key_throws();
    test_delete_where();
    test_update_where();

    std::cout << "\n=== Persistence Tests ===\n";
    test_auto_increment_continues_after_reopen();

    std::cout << "\n=== Large Table Tests ===\n";
    test_insert_and_scan_many_rows();

    std::cout << "\nAll tests passed.\n";
    return 0;
}