//g++ -std=c++17 tests/test_table_indexes.cpp src/row/serializer.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/header/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/btree/free_list_manager.cpp src/table/table.cpp src/index/index.cpp -o tests/test_table_indexes && ./tests/test_table_indexes
#include <iostream>
#include <cassert>
#include <filesystem>
#include <algorithm>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/header/header_manager.h"
#include "../src/btree/free_list_manager.h"
#include "../src/catalog/schema.h"
#include "../src/row/row.h"
#include "../src/table/table.h"
#include "../src/index/index.h"

namespace fs = std::filesystem;

static const std::string DB_FILE  = "test_table_indexes.db";
static const std::string WAL_FILE = "test_table_indexes.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

// users schema: id INT PK auto_increment, name VARCHAR(50), email VARCHAR(50), country VARCHAR(20)
TableSchema make_users_schema(uint32_t root_page = INVALID_PAGE) {
    TableSchema schema;
    schema.name                = "users";
    schema.root_page           = root_page;
    schema.primary_key_indices = {0};

    Column id;
    id.name = "id"; id.type = ColumnType::INT;
    id.max_length = 0; id.is_nullable = false;
    id.is_primary_key = true; id.auto_increment = true;

    Column name;
    name.name = "name"; name.type = ColumnType::VARCHAR;
    name.max_length = 50; name.is_nullable = false;
    name.is_primary_key = false; name.auto_increment = false;

    Column email;
    email.name = "email"; email.type = ColumnType::VARCHAR;
    email.max_length = 50; email.is_nullable = true;
    email.is_primary_key = false; email.auto_increment = false;

    Column country;
    country.name = "country"; country.type = ColumnType::VARCHAR;
    country.max_length = 20; country.is_nullable = false;
    country.is_primary_key = false; country.auto_increment = false;

    schema.columns = {id, name, email, country};
    return schema;
}

struct Env {
    DiskManager      dm;
    BufferPool       bp;
    WALManager       wal;
    HeaderManager    hm;
    FreeListManager  fl;

    Env() : dm(DB_FILE), bp(dm), wal(WAL_FILE, bp), hm(bp, wal), fl(bp, wal, hm) {
        hm.init();
        uint32_t placeholder_id;
        bp.new_page(placeholder_id);  // page 1 — catalog placeholder, unused here
    }
};

Row make_user(const std::string& name, const std::string& email, const std::string& country) {
    Row row;
    row.values = {Value(std::monostate{}), name, email, country};
    return row;
}

Row make_user_null_email(const std::string& name, const std::string& country) {
    Row row;
    row.values = {Value(std::monostate{}), name, Value(std::monostate{}), country};
    return row;
}

Key pk(int32_t id) { return Key{ Value(id) }; }

bool contains_pk(const std::vector<Key>& keys, int32_t id) {
    return std::find(keys.begin(), keys.end(), pk(id)) != keys.end();
}

// ─────────────────────────────────────────────
// INSERT keeps a non-unique index in sync
// ─────────────────────────────────────────────

void test_insert_populates_non_unique_index() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_country";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"country"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = false;

    Index country_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&country_idx});

    table.insert(make_user("Alice", "a@x.com", "IN"));
    table.insert(make_user("Bob",   "b@x.com", "IN"));
    table.insert(make_user("Carol", "c@x.com", "US"));

    auto in_users = country_idx.find(Key{Value(std::string("IN"))});
    assert(in_users.size() == 2);
    assert(contains_pk(in_users, 1));
    assert(contains_pk(in_users, 2));

    auto us_users = country_idx.find(Key{Value(std::string("US"))});
    assert(us_users.size() == 1);
    assert(contains_pk(us_users, 3));

    std::cout << "[PASS] insert() populates a non-unique index for every row, duplicates included\n";
    cleanup();
}

// ─────────────────────────────────────────────
// INSERT enforces a unique index and writes nothing on violation
// ─────────────────────────────────────────────

void test_insert_rejects_unique_index_violation_and_writes_nothing() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_email";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"email"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = true;

    Index email_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&email_idx});

    table.insert(make_user("Alice", "a@x.com", "IN"));

    bool threw = false;
    try {
        table.insert(make_user("Bob", "a@x.com", "US"));  // duplicate email
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // the rejected insert must not have written the row itself either —
    // not just the index. pk 2 should not exist in the table at all.
    assert(!table.select_by_key(pk(2)).has_value());

    // and the index must still show exactly one entry for that email.
    auto matches = email_idx.find(Key{Value(std::string("a@x.com"))});
    assert(matches.size() == 1);
    assert(matches[0] == pk(1));

    std::cout << "[PASS] insert() rejects a unique index violation and writes neither the row nor the index entry\n";
    cleanup();
}

// ─────────────────────────────────────────────
// NULL indexed column is skipped, even under a unique index
// ─────────────────────────────────────────────

void test_insert_skips_null_indexed_column_under_unique_index() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_email";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"email"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = true;

    Index email_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&email_idx});

    // two rows with NULL email must both insert fine, even under a
    // unique index — NULL is never subject to uniqueness.
    table.insert(make_user_null_email("Alice", "IN"));
    table.insert(make_user_null_email("Bob", "US"));

    assert(table.select_by_key(pk(1)).has_value());
    assert(table.select_by_key(pk(2)).has_value());

    std::cout << "[PASS] insert() allows multiple NULLs in an indexed column even under a unique index\n";
    cleanup();
}

// ─────────────────────────────────────────────
// UPDATE moves the index entry: old value gone, new value present
// ─────────────────────────────────────────────

void test_update_moves_index_entry() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_country";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"country"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = false;

    Index country_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&country_idx});

    Key id1 = table.insert(make_user("Alice", "a@x.com", "IN"));

    size_t country_col = static_cast<size_t>(schema.column_index("country"));
    table.update(id1, {{country_col, Value(std::string("US"))}});

    auto in_users = country_idx.find(Key{Value(std::string("IN"))});
    assert(in_users.empty());  // old value's entry must be gone

    auto us_users = country_idx.find(Key{Value(std::string("US"))});
    assert(us_users.size() == 1);
    assert(us_users[0] == id1);

    std::cout << "[PASS] update() removes the old index entry and writes the new one\n";
    cleanup();
}

void test_update_rejects_unique_violation_and_leaves_old_entry_intact() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_email";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"email"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = true;

    Index email_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&email_idx});

    Key id1 = table.insert(make_user("Alice", "a@x.com", "IN"));
    Key id2 = table.insert(make_user("Bob",   "b@x.com", "US"));

    size_t email_col = static_cast<size_t>(schema.column_index("email"));

    bool threw = false;
    try {
        table.update(id2, {{email_col, Value(std::string("a@x.com"))}});  // collides with Alice
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // Bob's row must be untouched, and both index entries must still
    // point at their original rows.
    auto bob_row = table.select_by_key(id2);
    assert(bob_row.has_value());
    assert(get_string(bob_row->get(2)) == "b@x.com");

    auto a_matches = email_idx.find(Key{Value(std::string("a@x.com"))});
    assert(a_matches.size() == 1 && a_matches[0] == id1);
    auto b_matches = email_idx.find(Key{Value(std::string("b@x.com"))});
    assert(b_matches.size() == 1 && b_matches[0] == id2);

    std::cout << "[PASS] update() rejects a unique violation and leaves both rows/index entries untouched\n";
    cleanup();
}

// a row updated to its OWN current value must not trip its own unique index
void test_update_to_same_value_does_not_self_violate_unique_index() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_email";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"email"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = true;

    Index email_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&email_idx});

    Key id1 = table.insert(make_user("Alice", "a@x.com", "IN"));
    size_t name_col = static_cast<size_t>(schema.column_index("name"));

    // updating an unrelated column must not trip the unique index just
    // because the (unchanged) email now gets re-validated against itself.
    table.update(id1, {{name_col, Value(std::string("Alicia"))}});

    auto matches = email_idx.find(Key{Value(std::string("a@x.com"))});
    assert(matches.size() == 1);
    assert(matches[0] == id1);

    std::cout << "[PASS] update() of an unrelated column doesn't self-violate an unchanged unique index value\n";
    cleanup();
}

// ─────────────────────────────────────────────
// DELETE removes the index entry
// ─────────────────────────────────────────────

void test_delete_removes_index_entry() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_country";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"country"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = false;

    Index country_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&country_idx});

    Key id1 = table.insert(make_user("Alice", "a@x.com", "IN"));
    Key id2 = table.insert(make_user("Bob",   "b@x.com", "IN"));

    table.delete_row(id1);

    auto in_users = country_idx.find(Key{Value(std::string("IN"))});
    assert(in_users.size() == 1);
    assert(in_users[0] == id2);

    std::cout << "[PASS] delete_row() removes exactly the deleted row's index entry\n";
    cleanup();
}

// ─────────────────────────────────────────────
// delete_where / update_where maintain the index across every affected row
// ─────────────────────────────────────────────

void test_delete_where_maintains_index_across_multiple_rows() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_country";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"country"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = false;

    Index country_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&country_idx});

    table.insert(make_user("Alice", "a@x.com", "IN"));
    table.insert(make_user("Bob",   "b@x.com", "IN"));
    table.insert(make_user("Carol", "c@x.com", "US"));

    size_t country_col = static_cast<size_t>(schema.column_index("country"));
    uint32_t deleted = table.delete_where([&](const Row& r) {
        return get_string(r.get(country_col)) == "IN";
    });
    assert(deleted == 2);

    assert(country_idx.find(Key{Value(std::string("IN"))}).empty());
    auto us_users = country_idx.find(Key{Value(std::string("US"))});
    assert(us_users.size() == 1);

    std::cout << "[PASS] delete_where() removes index entries for every deleted row\n";
    cleanup();
}

void test_update_where_maintains_index_and_validates_uniqueness_upfront() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_email";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"email"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = true;

    Index email_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&email_idx});

    table.insert(make_user("Alice", "a@x.com", "IN"));
    table.insert(make_user("Bob",   "b@x.com", "IN"));

    size_t email_col   = static_cast<size_t>(schema.column_index("email"));
    size_t country_col = static_cast<size_t>(schema.column_index("country"));

    // both matching rows would collide on the SAME new email — must
    // throw, and must not leave a partially-applied batch behind.
    bool threw = false;
    try {
        table.update_where(
            [&](const Row& r) { return get_string(r.get(country_col)) == "IN"; },
            {{email_col, Value(std::string("shared@x.com"))}});
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // neither row should have been changed — validation runs for every
    // matched row before phase 2 writes anything.
    auto alice = table.select_by_key(pk(1));
    auto bob   = table.select_by_key(pk(2));
    assert(get_string(alice->get(email_col)) == "a@x.com");
    assert(get_string(bob->get(email_col))   == "b@x.com");
    assert(email_idx.find(Key{Value(std::string("shared@x.com"))}).empty());

    std::cout << "[PASS] update_where() validates uniqueness across the whole batch before writing any row\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Multiple indexes on the same table stay independently in sync
// ─────────────────────────────────────────────

void test_multiple_indexes_stay_independently_in_sync() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema email_schema;
    email_schema.name         = "idx_email";
    email_schema.table_name   = "users";
    email_schema.column_names = {"email"};
    email_schema.root_page    = INVALID_PAGE;
    email_schema.is_unique    = true;

    IndexSchema country_schema;
    country_schema.name         = "idx_country";
    country_schema.table_name   = "users";
    country_schema.column_names = {"country"};
    country_schema.root_page    = INVALID_PAGE;
    country_schema.is_unique    = false;

    Index email_idx(email_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Index country_idx(country_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&email_idx, &country_idx});

    Key id1 = table.insert(make_user("Alice", "a@x.com", "IN"));
    table.insert(make_user("Bob", "b@x.com", "IN"));

    assert(email_idx.find(Key{Value(std::string("a@x.com"))}).size() == 1);
    assert(country_idx.find(Key{Value(std::string("IN"))}).size() == 2);

    table.delete_row(id1);

    assert(email_idx.find(Key{Value(std::string("a@x.com"))}).empty());
    assert(country_idx.find(Key{Value(std::string("IN"))}).size() == 1);

    std::cout << "[PASS] two indexes on the same table are both kept in sync independently\n";
    cleanup();
}

// ─────────────────────────────────────────────
// index_root_pages() reports every wired index
// ─────────────────────────────────────────────

void test_index_root_pages_reports_every_index() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema email_schema;
    email_schema.name         = "idx_email";
    email_schema.table_name   = "users";
    email_schema.column_names = {"email"};
    email_schema.root_page    = INVALID_PAGE;
    email_schema.is_unique    = true;

    Index email_idx(email_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);
    Table table(schema, env.bp, env.wal, env.fl, {&email_idx});

    table.insert(make_user("Alice", "a@x.com", "IN"));

    auto roots = table.index_root_pages();
    assert(roots.size() == 1);
    assert(roots[0].first == "idx_email");
    assert(roots[0].second == email_idx.root_page());
    assert(roots[0].second != INVALID_PAGE);

    std::cout << "[PASS] index_root_pages() reports the current root page for every wired index\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Constructor validation
// ─────────────────────────────────────────────

void test_constructor_rejects_index_for_wrong_table() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_email";
    idx_schema.table_name   = "orders";  // wrong table
    idx_schema.column_names = {"email"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = false;

    Index bad_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);

    bool threw = false;
    try {
        Table table(schema, env.bp, env.wal, env.fl, {&bad_idx});
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] Table constructor rejects an index whose table_name doesn't match\n";
    cleanup();
}

void test_constructor_rejects_index_for_unknown_column() {
    cleanup();
    Env env;
    TableSchema schema = make_users_schema();

    IndexSchema idx_schema;
    idx_schema.name         = "idx_nope";
    idx_schema.table_name   = "users";
    idx_schema.column_names = {"does_not_exist"};
    idx_schema.root_page    = INVALID_PAGE;
    idx_schema.is_unique    = false;

    Index bad_idx(idx_schema, /*pk_arity=*/1, env.bp, env.wal, env.fl);

    bool threw = false;
    try {
        Table table(schema, env.bp, env.wal, env.fl, {&bad_idx});
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] Table constructor rejects an index that references an unknown column\n";
    cleanup();
}

int main() {
    std::cout << "\n=== Table + Index Tests ===\n";
    test_insert_populates_non_unique_index();
    test_insert_rejects_unique_index_violation_and_writes_nothing();
    test_insert_skips_null_indexed_column_under_unique_index();
    test_update_moves_index_entry();
    test_update_rejects_unique_violation_and_leaves_old_entry_intact();
    test_update_to_same_value_does_not_self_violate_unique_index();
    test_delete_removes_index_entry();
    test_delete_where_maintains_index_across_multiple_rows();
    test_update_where_maintains_index_and_validates_uniqueness_upfront();
    test_multiple_indexes_stay_independently_in_sync();
    test_index_root_pages_reports_every_index();
    test_constructor_rejects_index_for_wrong_table();
    test_constructor_rejects_index_for_unknown_column();
    std::cout << "\nAll Table + Index tests passed.\n";
    return 0;
}
