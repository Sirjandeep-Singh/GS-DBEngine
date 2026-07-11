// g++ -std=c++17 tests/test_executor.cpp src/executor/executor.cpp src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/btree/free_list_manager.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/header/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp -o tests/test_executor && ./tests/test_executor

#include <iostream>
#include <cassert>
#include <filesystem>

#include "../src/storage/disk_manager.h"
#include "../src/header/header_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/btree/free_list_manager.h"
#include "../src/catalog/catalog_manager.h"
#include "../src/parser/parser.h"
#include "../src/parser/tokenizer.h"
#include "../src/executor/executor.h"

namespace fs = std::filesystem;

static const std::string DB_FILE  = "test_executor.db";
static const std::string WAL_FILE = "test_executor.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

// Full stack environment — mirrors the Database class bootstrap sequence
// for a brand-new database (from AGENT.md Layer 9).
struct Env {
    DiskManager      dm;
    BufferPool       bp;
    WALManager       wal;
    HeaderManager    hm;
    FreeListManager  fl;
    CatalogManager   cat;
    Executor         exec;

    Env()
        : dm(DB_FILE)
        , bp(dm)
        , wal(WAL_FILE, bp)
        , hm(bp, wal)
        , fl(bp, wal, hm)
        , cat(bp, wal)
        , exec(cat, bp, wal, fl)
    {
        hm.init();             // allocates page 0
        cat.load(true);        // allocates page 1
    }
};

// Helper: parse a SQL string and return the Statement.
static Statement parse(const std::string& sql) {
    Tokenizer t(sql);
    Parser    p(t.tokenize());
    return p.parse();
}

// Helper: execute a SQL string and return the QueryResult.
static QueryResult exec(Env& env, const std::string& sql) {
    return env.exec.execute(parse(sql));
}

// ─────────────────────────────────────────────
// CREATE TABLE Tests
// ─────────────────────────────────────────────

void test_create_table() {
    cleanup();
    Env env;

    auto r = exec(env,
        "CREATE TABLE users ("
        "  id   INT PRIMARY KEY AUTO_INCREMENT,"
        "  name VARCHAR(50) NOT NULL,"
        "  age  INT"
        ");");

    assert(r.success);
    assert(r.rows_affected == 0);
    assert(env.cat.table_exists("users"));

    std::cout << "[PASS] CREATE TABLE creates table in catalog\n";
    cleanup();
}

void test_create_table_duplicate_throws() {
    cleanup();
    Env env;

    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));");
    auto r = exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));");

    assert(!r.success);
    assert(!r.error_message.empty());

    std::cout << "[PASS] CREATE TABLE on duplicate table returns error\n";
    cleanup();
}

// ─────────────────────────────────────────────
// INSERT Tests
// ─────────────────────────────────────────────

void test_insert_and_rows_affected() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");

    auto r = exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    assert(r.success);
    assert(r.rows_affected == 1);

    std::cout << "[PASS] INSERT returns rows_affected = 1\n";
    cleanup();
}

void test_insert_not_null_violation() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");

    // name column is NOT NULL — inserting NULL should fail
    auto r = exec(env, "INSERT INTO users VALUES (NULL, NULL, 25);");
    assert(!r.success);

    std::cout << "[PASS] INSERT with NULL into NOT NULL column returns error\n";
    cleanup();
}

// ─────────────────────────────────────────────
// SELECT Tests
// ─────────────────────────────────────────────

void test_select_star() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");

    auto r = exec(env, "SELECT * FROM users;");
    assert(r.success);
    assert(r.rows.size() == 3);
    assert(r.columns.size() == 3);
    assert(r.columns[0] == "id");
    assert(r.columns[1] == "name");
    assert(r.columns[2] == "age");

    // rows are in primary key order (B+ tree scan order)
    assert(r.rows[0][1] == "Alice");
    assert(r.rows[1][1] == "Bob");
    assert(r.rows[2][1] == "Carol");

    std::cout << "[PASS] SELECT * returns all rows with correct column headers\n";
    cleanup();
}

void test_select_named_columns() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");

    auto r = exec(env, "SELECT name, age FROM users;");
    assert(r.success);
    assert(r.columns.size() == 2);
    assert(r.columns[0] == "name");
    assert(r.columns[1] == "age");
    assert(r.rows.size() == 1);
    assert(r.rows[0][0] == "Alice");
    assert(r.rows[0][1] == "25");

    std::cout << "[PASS] SELECT named columns projects correctly\n";
    cleanup();
}

void test_select_where_equals() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");

    auto r = exec(env, "SELECT * FROM users WHERE name = 'Bob';");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][1] == "Bob");

    std::cout << "[PASS] SELECT WHERE = filters to matching rows\n";
    cleanup();
}

void test_select_where_greater_than() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");

    auto r = exec(env, "SELECT * FROM users WHERE age > 24;");
    assert(r.success);
    assert(r.rows.size() == 2);
    assert(r.rows[0][1] == "Alice");
    assert(r.rows[1][1] == "Bob");

    std::cout << "[PASS] SELECT WHERE > filters correctly\n";
    cleanup();
}

void test_select_where_and() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");

    auto r = exec(env, "SELECT * FROM users WHERE age > 22 AND age < 30;");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][1] == "Alice");

    std::cout << "[PASS] SELECT WHERE AND filters correctly\n";
    cleanup();
}

void test_select_where_is_null() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob', NULL);");

    auto r = exec(env, "SELECT * FROM users WHERE age IS NULL;");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][1] == "Bob");

    std::cout << "[PASS] SELECT WHERE IS NULL filters correctly\n";
    cleanup();
}

void test_select_no_matching_rows() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");

    auto r = exec(env, "SELECT * FROM users WHERE age > 100;");
    assert(r.success);
    assert(r.rows.empty());

    std::cout << "[PASS] SELECT with no matching rows returns empty result\n";
    cleanup();
}

void test_select_order_by_asc() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");

    auto r = exec(env, "SELECT * FROM users ORDER BY age ASC;");
    assert(r.success);
    assert(r.rows.size() == 3);
    assert(r.rows[0][1] == "Carol");
    assert(r.rows[1][1] == "Alice");
    assert(r.rows[2][1] == "Bob");

    std::cout << "[PASS] SELECT ORDER BY ASC sorts correctly\n";
    cleanup();
}

void test_select_order_by_desc() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");

    auto r = exec(env, "SELECT * FROM users ORDER BY age DESC;");
    assert(r.success);
    assert(r.rows.size() == 3);
    assert(r.rows[0][1] == "Bob");
    assert(r.rows[1][1] == "Alice");
    assert(r.rows[2][1] == "Carol");

    std::cout << "[PASS] SELECT ORDER BY DESC sorts correctly\n";
    cleanup();
}

void test_select_limit() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");

    auto r = exec(env, "SELECT * FROM users LIMIT 2;");
    assert(r.success);
    assert(r.rows.size() == 2);

    std::cout << "[PASS] SELECT LIMIT truncates result correctly\n";
    cleanup();
}

void test_select_from_nonexistent_table() {
    cleanup();
    Env env;

    auto r = exec(env, "SELECT * FROM ghost;");
    assert(!r.success);
    assert(!r.error_message.empty());

    std::cout << "[PASS] SELECT from nonexistent table returns error\n";
    cleanup();
}

// ─────────────────────────────────────────────
// UPDATE Tests
// ─────────────────────────────────────────────

void test_update_where() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");

    auto r = exec(env, "UPDATE users SET age = 99 WHERE name = 'Bob';");
    assert(r.success);
    assert(r.rows_affected == 1);

    auto sel = exec(env, "SELECT * FROM users WHERE name = 'Bob';");
    assert(sel.rows[0][2] == "99");

    std::cout << "[PASS] UPDATE WHERE modifies correct rows\n";
    cleanup();
}

void test_update_all_rows() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");

    auto r = exec(env, "UPDATE users SET age = 0;");
    assert(r.success);
    assert(r.rows_affected == 2);

    std::cout << "[PASS] UPDATE without WHERE modifies all rows\n";
    cleanup();
}

// ─────────────────────────────────────────────
// DELETE Tests
// ─────────────────────────────────────────────

void test_delete_where() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Carol', 22);");

    auto r = exec(env, "DELETE FROM users WHERE age > 24;");
    assert(r.success);
    assert(r.rows_affected == 2);

    auto sel = exec(env, "SELECT * FROM users;");
    assert(sel.rows.size() == 1);
    assert(sel.rows[0][1] == "Carol");

    std::cout << "[PASS] DELETE WHERE removes correct rows\n";
    cleanup();
}

void test_delete_all_rows() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");

    auto r = exec(env, "DELETE FROM users;");
    assert(r.success);
    assert(r.rows_affected == 2);

    auto sel = exec(env, "SELECT * FROM users;");
    assert(sel.rows.empty());

    std::cout << "[PASS] DELETE without WHERE removes all rows\n";
    cleanup();
}

// ─────────────────────────────────────────────
// DROP TABLE Tests
// ─────────────────────────────────────────────

void test_drop_table() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));");
    assert(env.cat.table_exists("users"));

    auto r = exec(env, "DROP TABLE users;");
    assert(r.success);
    assert(!env.cat.table_exists("users"));

    std::cout << "[PASS] DROP TABLE removes table from catalog\n";
    cleanup();
}

void test_drop_nonexistent_table() {
    cleanup();
    Env env;

    auto r = exec(env, "DROP TABLE ghost;");
    assert(!r.success);

    std::cout << "[PASS] DROP TABLE on nonexistent table returns error\n";
    cleanup();
}

// ─────────────────────────────────────────────
// SHOW Tests
// ─────────────────────────────────────────────

void test_show_tables() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));");
    exec(env, "CREATE TABLE orders (id INT PRIMARY KEY, total INT);");

    auto r = exec(env, "SHOW TABLES;");
    assert(r.success);
    assert(r.rows.size() == 2);

    std::cout << "[PASS] SHOW TABLES lists all tables\n";
    cleanup();
}

// ─────────────────────────────────────────────
// INNER JOIN Tests
// ─────────────────────────────────────────────

void test_inner_join() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL);");
    exec(env, "CREATE TABLE orders (id INT PRIMARY KEY AUTO_INCREMENT, user_id INT NOT NULL, total INT);");

    exec(env, "INSERT INTO users VALUES (NULL, 'Alice');");
    exec(env, "INSERT INTO users VALUES (NULL, 'Bob');");
    exec(env, "INSERT INTO orders VALUES (NULL, 1, 100);");
    exec(env, "INSERT INTO orders VALUES (NULL, 1, 200);");
    exec(env, "INSERT INTO orders VALUES (NULL, 2, 50);");

    auto r = exec(env,
        "SELECT * FROM orders "
        "INNER JOIN users ON orders.user_id = users.id;");

    assert(r.success);
    assert(r.rows.size() == 3);

    std::cout << "[PASS] INNER JOIN returns correct matched rows\n";
    cleanup();
}

void test_inner_join_no_match() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL);");
    exec(env, "CREATE TABLE orders (id INT PRIMARY KEY, user_id INT NOT NULL, total INT);");

    exec(env, "INSERT INTO users VALUES (1, 'Alice');");
    exec(env, "INSERT INTO orders VALUES (1, 99, 100);");  // user_id 99 doesn't exist

    auto r = exec(env,
        "SELECT * FROM orders "
        "INNER JOIN users ON orders.user_id = users.id;");

    assert(r.success);
    assert(r.rows.empty());

    std::cout << "[PASS] INNER JOIN with no matches returns empty result\n";
    cleanup();
}

void test_left_join_preserves_unmatched() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL);");
    exec(env, "CREATE TABLE orders (id INT PRIMARY KEY, user_id INT NOT NULL, total INT);");

    exec(env, "INSERT INTO users VALUES (1, 'Alice');");
    exec(env, "INSERT INTO users VALUES (2, 'Bob');");
    exec(env, "INSERT INTO orders VALUES (1, 1, 100);");
    // Bob has no orders

    auto r = exec(env,
        "SELECT * FROM users "
        "LEFT JOIN orders ON users.id = orders.user_id;");

    assert(r.success);
    // Alice with order + Bob with NULL order row
    assert(r.rows.size() == 2);

    std::cout << "[PASS] LEFT JOIN includes unmatched left rows with NULL right values\n";
    cleanup();
}

// ─────────────────────────────────────────────
// CREATE INDEX Tests
// ─────────────────────────────────────────────
//
// Covers CREATE INDEX statement handling (recognition, backfill, error
// cases) and confirms Executor's open_indexes()/persist_index_roots()
// wiring keeps a live index in sync across INSERT/UPDATE/DELETE driven
// through real SQL — not just the raw Table+Index API already covered
// directly in test_table_indexes.cpp.

void test_create_index_backfills_existing_rows() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50), country VARCHAR(20));");
    exec(env, "INSERT INTO users VALUES (1, 'Amrit', 'IN');");
    exec(env, "INSERT INTO users VALUES (2, 'Priya', 'IN');");
    exec(env, "INSERT INTO users VALUES (3, 'Sam', 'US');");
    exec(env, "INSERT INTO users VALUES (4, 'NoCountry', NULL);");  // NULL indexed value — must be skipped

    auto r = exec(env, "CREATE INDEX idx_country ON users (country);");
    assert(r.success);

    const TableSchema& tschema = env.cat.get_table("users");
    Index idx(env.cat.get_index("idx_country"), tschema.primary_key_indices.size(), env.bp, env.wal, env.fl);
    assert(idx.find(Key{Value(std::string("IN"))}).size() == 2);
    assert(idx.find(Key{Value(std::string("US"))}).size() == 1);

    std::cout << "[PASS] CREATE INDEX backfills entries for pre-existing rows\n";
    cleanup();
}

void test_create_index_duplicate_name_rejected() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, country VARCHAR(20));");
    exec(env, "CREATE INDEX idx_country ON users (country);");

    auto r = exec(env, "CREATE INDEX idx_country ON users (id);");
    assert(!r.success);

    std::cout << "[PASS] CREATE INDEX with a name already in use is rejected\n";
    cleanup();
}

void test_create_index_unknown_column_rejected() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, country VARCHAR(20));");

    auto r = exec(env, "CREATE INDEX idx_bad ON users (does_not_exist);");
    assert(!r.success);

    std::cout << "[PASS] CREATE INDEX on an unknown column is rejected\n";
    cleanup();
}

void test_create_index_unknown_table_rejected() {
    cleanup();
    Env env;

    auto r = exec(env, "CREATE INDEX idx_bad ON no_such_table (id);");
    assert(!r.success);

    std::cout << "[PASS] CREATE INDEX on an unknown table is rejected\n";
    cleanup();
}

void test_create_unique_index_rejects_existing_duplicates() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, country VARCHAR(20));");
    exec(env, "INSERT INTO users VALUES (1, 'IN');");
    exec(env, "INSERT INTO users VALUES (2, 'IN');");  // duplicate country

    auto r = exec(env, "CREATE UNIQUE INDEX idx_country_unique ON users (country);");
    assert(!r.success);

    bool leaked_partial_entry = true;
    try {
        env.cat.get_index("idx_country_unique");
    } catch (const std::exception&) {
        leaked_partial_entry = false;  // expected: no partial catalog entry survives the rejection
    }
    assert(!leaked_partial_entry);

    std::cout << "[PASS] CREATE UNIQUE INDEX rejected on pre-existing duplicates, no partial catalog entry\n";
    cleanup();
}

void test_index_stays_live_after_creation() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, country VARCHAR(20));");
    exec(env, "INSERT INTO users VALUES (1, 'IN');");
    exec(env, "INSERT INTO users VALUES (2, 'IN');");
    exec(env, "INSERT INTO users VALUES (3, 'US');");
    exec(env, "CREATE INDEX idx_country ON users (country);");

    // Post-creation writes must keep flowing through the index — this is
    // exactly what open_indexes()/persist_index_roots() exist to guarantee.
    exec(env, "INSERT INTO users VALUES (5, 'IN');");
    exec(env, "UPDATE users SET country = 'US' WHERE id = 2;");
    exec(env, "DELETE FROM users WHERE id = 3;");
    // started IN={1,2}, US={3}; +5(IN); 2 moves IN->US; 3 deleted
    // expected: IN={1,5}, US={2}

    const TableSchema& tschema = env.cat.get_table("users");
    Index idx(env.cat.get_index("idx_country"), tschema.primary_key_indices.size(), env.bp, env.wal, env.fl);
    assert(idx.find(Key{Value(std::string("IN"))}).size() == 2);
    assert(idx.find(Key{Value(std::string("US"))}).size() == 1);

    std::cout << "[PASS] index stays in sync across INSERT/UPDATE/DELETE after CREATE INDEX\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Index-Assisted Query Tests
// ─────────────────────────────────────────────
//
// Covers execute_select / execute_select_aggregate / execute_update /
// execute_delete picking a usable secondary index for an equality WHERE
// clause (find_index_prefix / scan_with_index) instead of a full
// Table::scan(), and falling back correctly whenever no index fits.

static void seed_people(Env& env) {
    exec(env, "CREATE TABLE people (id INT PRIMARY KEY, dept VARCHAR(20), age INT, score FLOAT);");
    for (int i = 1; i <= 50; i++) {
        std::string dept = (i % 5 == 0) ? "eng" : "sales";
        std::string q = "INSERT INTO people VALUES (" + std::to_string(i) + ", '" + dept +
                         "', " + std::to_string(20 + (i % 10)) + ", " + std::to_string(i) + ".0);";
        exec(env, q);
    }
    exec(env, "CREATE INDEX idx_dept ON people (dept);");
    exec(env, "CREATE INDEX idx_dept_age ON people (dept, age);");
    exec(env, "CREATE UNIQUE INDEX idx_id_dup ON people (id);");
    exec(env, "CREATE INDEX idx_score ON people (score);");
}

void test_select_equality_uses_single_column_index() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "SELECT id FROM people WHERE dept = 'eng';");
    assert(r.success);
    assert(r.rows.size() == 10);  // every 5th row, 1..50

    std::cout << "[PASS] equality WHERE on a single-column index returns correct rows\n";
    cleanup();
}

void test_select_composite_index_leftmost_prefix() {
    cleanup();
    Env env;
    seed_people(env);

    // Only the first column of the composite (dept, age) index is given —
    // must still work as a leftmost-prefix lookup.
    auto r = exec(env, "SELECT id FROM people WHERE dept = 'sales';");
    assert(r.success);
    assert(r.rows.size() == 40);

    std::cout << "[PASS] composite index used via leftmost prefix (first column only)\n";
    cleanup();
}

void test_select_composite_index_full_match() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "SELECT id FROM people WHERE dept = 'eng' AND age = 25;");
    assert(r.success);
    int expected = 0;
    for (int i = 5; i <= 50; i += 5) {
        if (20 + (i % 10) == 25) expected++;
    }
    assert(static_cast<int>(r.rows.size()) == expected);

    std::cout << "[PASS] composite index full two-column equality match returns correct rows\n";
    cleanup();
}

void test_select_index_candidates_still_filtered_by_full_predicate() {
    cleanup();
    Env env;
    seed_people(env);

    // 'score > 40' isn't part of any index equality — index only narrows on
    // dept, every candidate must still be checked against the full WHERE.
    auto r = exec(env, "SELECT id FROM people WHERE dept = 'eng' AND score > 40;");
    assert(r.success);
    int expected = 0;
    for (int i = 5; i <= 50; i += 5) if (i > 40) expected++;
    assert(static_cast<int>(r.rows.size()) == expected);

    std::cout << "[PASS] index-narrowed candidates are still filtered by the full predicate\n";
    cleanup();
}

void test_select_int_literal_against_float_column_is_coerced() {
    cleanup();
    Env env;
    seed_people(env);

    // score is FLOAT and stores 7.0f; the literal '7' parses as int32_t —
    // without coercion, a raw index-key lookup would silently miss it.
    auto r = exec(env, "SELECT id FROM people WHERE score = 7;");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][0] == "7");

    std::cout << "[PASS] int literal against a FLOAT indexed column is coerced and matched\n";
    cleanup();
}

void test_select_or_clause_not_incorrectly_narrowed() {
    cleanup();
    Env env;
    seed_people(env);

    // A top-level OR must not be narrowed down to just the 'dept' side —
    // both branches have to be considered.
    auto r = exec(env, "SELECT id FROM people WHERE dept = 'eng' OR score > 45;");
    assert(r.success);
    int expected = 0;
    for (int i = 1; i <= 50; i++) {
        bool eng = (i % 5 == 0);
        bool hi  = (i > 45);
        if (eng || hi) expected++;
    }
    assert(static_cast<int>(r.rows.size()) == expected);

    std::cout << "[PASS] OR at the top level is not incorrectly narrowed by an index\n";
    cleanup();
}

void test_select_count_aggregate_uses_index() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "SELECT COUNT(*) FROM people WHERE dept = 'eng';");
    assert(r.success);
    assert(r.rows[0][0] == "10");

    std::cout << "[PASS] COUNT(*) aggregate uses an index-assisted scan correctly\n";
    cleanup();
}

void test_update_where_indexed_equality() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "UPDATE people SET age = 99 WHERE dept = 'eng' AND age = 25;");
    assert(r.success);

    auto check = exec(env, "SELECT id FROM people WHERE age = 99;");
    assert(check.success);
    assert(check.rows.size() == r.rows_affected);

    std::cout << "[PASS] UPDATE with an indexable WHERE updates exactly the right rows ("
              << r.rows_affected << ")\n";
    cleanup();
}

void test_delete_where_unique_index_deletes_one_row() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "DELETE FROM people WHERE id = 3;");
    assert(r.success);
    assert(r.rows_affected == 1);

    auto check = exec(env, "SELECT id FROM people WHERE id = 3;");
    assert(check.success);
    assert(check.rows.empty());

    std::cout << "[PASS] DELETE via a unique index deletes exactly the targeted row\n";
    cleanup();
}

void test_delete_where_nonunique_index_deletes_all_matches() {
    cleanup();
    Env env;
    seed_people(env);

    auto before = exec(env, "SELECT id FROM people WHERE dept = 'eng';");
    size_t n = before.rows.size();

    auto r = exec(env, "DELETE FROM people WHERE dept = 'eng';");
    assert(r.success);
    assert(r.rows_affected == n);

    auto after = exec(env, "SELECT id FROM people WHERE dept = 'eng';");
    assert(after.success);
    assert(after.rows.empty());

    std::cout << "[PASS] DELETE via a non-unique index deletes every matching row (" << n << ")\n";
    cleanup();
}

// ─────────────────────────────────────────────
// Persistence Test
// ─────────────────────────────────────────────

void test_data_survives_restart() {
    cleanup();

    uint32_t root_page;
    {
        Env env;
        exec(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
        exec(env, "INSERT INTO users VALUES (NULL, 'Alice', 25);");
        exec(env, "INSERT INTO users VALUES (NULL, 'Bob',   30);");
        root_page = env.cat.get_table("users").root_page;
    }
    // Env destructor flushes buffer pool to disk

    // Reopen — mirrors existing database bootstrap sequence from AGENT.md
    {
        DiskManager     dm(DB_FILE);
        BufferPool      bp(dm);
        WALManager      wal(WAL_FILE, bp);
        wal.recover();
        HeaderManager   hm(bp, wal);
        hm.load();
        FreeListManager fl(bp, wal, hm);
        CatalogManager  cat(bp, wal);
        cat.load(false);
        Executor exec(cat, bp, wal, fl);

        auto r = exec.execute(parse("SELECT * FROM users;"));
        assert(r.success);
        assert(r.rows.size() == 2);
        assert(r.rows[0][1] == "Alice");
        assert(r.rows[1][1] == "Bob");
    }

    std::cout << "[PASS] Data persists across database restart\n";
    cleanup();
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────

int main() {
    std::cout << "\n=== CREATE TABLE Tests ===\n";
    test_create_table();
    test_create_table_duplicate_throws();

    std::cout << "\n=== INSERT Tests ===\n";
    test_insert_and_rows_affected();
    test_insert_not_null_violation();

    std::cout << "\n=== SELECT Tests ===\n";
    test_select_star();
    test_select_named_columns();
    test_select_where_equals();
    test_select_where_greater_than();
    test_select_where_and();
    test_select_where_is_null();
    test_select_no_matching_rows();
    test_select_order_by_asc();
    test_select_order_by_desc();
    test_select_limit();
    test_select_from_nonexistent_table();

    std::cout << "\n=== UPDATE Tests ===\n";
    test_update_where();
    test_update_all_rows();

    std::cout << "\n=== DELETE Tests ===\n";
    test_delete_where();
    test_delete_all_rows();

    std::cout << "\n=== DROP TABLE Tests ===\n";
    test_drop_table();
    test_drop_nonexistent_table();

    std::cout << "\n=== SHOW Tests ===\n";
    test_show_tables();

    std::cout << "\n=== JOIN Tests ===\n";
    test_inner_join();
    test_inner_join_no_match();
    test_left_join_preserves_unmatched();

    std::cout << "\n=== CREATE INDEX Tests ===\n";
    test_create_index_backfills_existing_rows();
    test_create_index_duplicate_name_rejected();
    test_create_index_unknown_column_rejected();
    test_create_index_unknown_table_rejected();
    test_create_unique_index_rejects_existing_duplicates();
    test_index_stays_live_after_creation();

    std::cout << "\n=== Index-Assisted Query Tests ===\n";
    test_select_equality_uses_single_column_index();
    test_select_composite_index_leftmost_prefix();
    test_select_composite_index_full_match();
    test_select_index_candidates_still_filtered_by_full_predicate();
    test_select_int_literal_against_float_column_is_coerced();
    test_select_or_clause_not_incorrectly_narrowed();
    test_select_count_aggregate_uses_index();
    test_update_where_indexed_equality();
    test_delete_where_unique_index_deletes_one_row();
    test_delete_where_nonunique_index_deletes_all_matches();

    std::cout << "\n=== Persistence Tests ===\n";
    test_data_survives_restart();

    std::cout << "\nAll executor tests passed.\n";
    return 0;
}