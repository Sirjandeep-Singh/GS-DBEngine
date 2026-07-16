// g++ -std=c++17 tests/test_executor.cpp src/executor/executor.cpp src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/btree/free_list_manager.cpp src/btree/key.cpp src/index/index.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/header/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp -o tests/test_executor && ./tests/test_executor

#include <iostream>
#include <cassert>
#include <filesystem>
#include <algorithm>
#include <tuple>

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

void test_create_table_default_clause() {
    cleanup();
    Env env;

    auto r = exec(env,
        "CREATE TABLE users ("
        "  id     INT PRIMARY KEY AUTO_INCREMENT,"
        "  name   VARCHAR(50) NOT NULL,"
        "  status VARCHAR(10) DEFAULT 'active',"
        "  score  INT DEFAULT 0,"
        "  active BOOLEAN DEFAULT TRUE"
        ");");

    assert(r.success);
    assert(env.cat.table_exists("users"));

    std::cout << "[PASS] CREATE TABLE accepts DEFAULT clauses of every literal type\n";
    cleanup();
}

void test_create_table_default_null_on_not_null_throws() {
    cleanup();
    Env env;

    auto r = exec(env,
        "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(10) NOT NULL DEFAULT NULL);");

    assert(!r.success);
    assert(!r.error_message.empty());

    std::cout << "[PASS] DEFAULT NULL on a NOT NULL column returns error\n";
    cleanup();
}

void test_create_table_default_type_mismatch_throws() {
    cleanup();
    Env env;

    auto r = exec(env, "CREATE TABLE t (id INT PRIMARY KEY, score INT DEFAULT 'oops');");

    assert(!r.success);
    assert(!r.error_message.empty());

    std::cout << "[PASS] DEFAULT with a mismatched literal type returns error\n";
    cleanup();
}

void test_create_table_default_with_auto_increment_throws() {
    cleanup();
    Env env;

    auto r = exec(env, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT DEFAULT 5);");

    assert(!r.success);
    assert(!r.error_message.empty());

    std::cout << "[PASS] Combining DEFAULT with AUTO_INCREMENT returns error\n";
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

void test_insert_omitted_column_uses_default() {
    cleanup();
    Env env;
    exec(env,
        "CREATE TABLE users ("
        "  id     INT PRIMARY KEY AUTO_INCREMENT,"
        "  name   VARCHAR(50) NOT NULL,"
        "  status VARCHAR(10) DEFAULT 'active'"
        ");");

    // status is omitted entirely — should fall back to 'active'
    auto r = exec(env, "INSERT INTO users (name) VALUES ('Alice');");
    assert(r.success);

    auto sel = exec(env, "SELECT status FROM users WHERE name = 'Alice';");
    assert(sel.success);
    assert(sel.rows.size() == 1);
    assert(sel.rows[0][0] == "active");

    std::cout << "[PASS] INSERT with column omitted from named list uses column DEFAULT\n";
    cleanup();
}

void test_insert_omitted_column_no_default_stays_null() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(10), age INT);");

    auto r = exec(env, "INSERT INTO t (id, name) VALUES (1, 'x');");
    assert(r.success);

    auto sel = exec(env, "SELECT age FROM t WHERE id = 1;");
    assert(sel.success);
    assert(sel.rows[0][0] == "NULL");

    std::cout << "[PASS] Omitted column with no DEFAULT still falls back to NULL\n";
    cleanup();
}

void test_insert_explicit_value_overrides_default() {
    cleanup();
    Env env;
    exec(env,
        "CREATE TABLE users (id INT PRIMARY KEY, status VARCHAR(10) DEFAULT 'active');");

    auto r = exec(env, "INSERT INTO users (id, status) VALUES (1, 'banned');");
    assert(r.success);

    auto sel = exec(env, "SELECT status FROM users WHERE id = 1;");
    assert(sel.rows[0][0] == "banned");

    std::cout << "[PASS] Explicit INSERT value overrides column DEFAULT\n";
    cleanup();
}

void test_insert_positional_default_keyword() {
    cleanup();
    Env env;
    exec(env,
        "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL,"
        " score INT DEFAULT 100);");

    auto r = exec(env, "INSERT INTO users VALUES (1, 'Alice', DEFAULT);");
    assert(r.success);

    auto sel = exec(env, "SELECT score FROM users WHERE id = 1;");
    assert(sel.success);
    assert(sel.rows[0][0] == "100");

    std::cout << "[PASS] Positional INSERT VALUES accepts the DEFAULT keyword\n";
    cleanup();
}

void test_insert_named_default_keyword() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, score INT DEFAULT 42);");

    auto r = exec(env, "INSERT INTO users (id, score) VALUES (1, DEFAULT);");
    assert(r.success);

    auto sel = exec(env, "SELECT score FROM users WHERE id = 1;");
    assert(sel.rows[0][0] == "42");

    std::cout << "[PASS] Named-column INSERT VALUES accepts the DEFAULT keyword\n";
    cleanup();
}

void test_insert_default_keyword_no_default_is_null() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, age INT);");

    auto r = exec(env, "INSERT INTO t VALUES (1, DEFAULT);");
    assert(r.success);

    auto sel = exec(env, "SELECT age FROM t WHERE id = 1;");
    assert(sel.rows[0][0] == "NULL");

    std::cout << "[PASS] DEFAULT keyword on a column with no DEFAULT clause yields NULL\n";
    cleanup();
}

void test_insert_default_keyword_on_not_null_without_default_fails() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(10) NOT NULL);");

    auto r = exec(env, "INSERT INTO t VALUES (1, DEFAULT);");
    assert(!r.success);

    std::cout << "[PASS] DEFAULT keyword on a NOT NULL column without a DEFAULT clause fails\n";
    cleanup();
}

void test_insert_default_pk_omitted_still_auto_increments() {
    cleanup();
    Env env;
    exec(env,
        "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, "
        " status VARCHAR(10) DEFAULT 'new');");

    auto r1 = exec(env, "INSERT INTO t (status) VALUES ('given');");
    auto r2 = exec(env, "INSERT INTO t (id) VALUES (DEFAULT);");  // id: no DEFAULT -> NULL -> auto-increment
    assert(r1.success);
    assert(r2.success);

    auto sel = exec(env, "SELECT id, status FROM t ORDER BY id;");
    assert(sel.rows.size() == 2);
    assert(sel.rows[0][1] == "given");
    assert(sel.rows[1][1] == "new");
    assert(sel.rows[0][0] != sel.rows[1][0]);  // distinct, auto-assigned ids

    std::cout << "[PASS] Omitted AUTO_INCREMENT primary key still auto-increments alongside other DEFAULTs\n";
    cleanup();
}

void test_insert_default_value_validated_against_check() {
    cleanup();
    Env env;
    // default (5) satisfies the CHECK, so a normal insert succeeds
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, score INT DEFAULT 5 CHECK (score > 0));");

    auto r = exec(env, "INSERT INTO t (id) VALUES (1);");
    assert(r.success);

    auto sel = exec(env, "SELECT score FROM t WHERE id = 1;");
    assert(sel.rows[0][0] == "5");

    std::cout << "[PASS] Resolved DEFAULT value is validated against CHECK constraints like any other value\n";
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

void test_update_set_default() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, status VARCHAR(10) DEFAULT 'active');");
    exec(env, "INSERT INTO t (id, status) VALUES (1, 'banned');");

    auto r = exec(env, "UPDATE t SET status = DEFAULT WHERE id = 1;");
    assert(r.success);

    auto sel = exec(env, "SELECT status FROM t WHERE id = 1;");
    assert(sel.rows[0][0] == "active");

    std::cout << "[PASS] UPDATE SET col = DEFAULT resolves to the column's schema default\n";
    cleanup();
}

void test_update_set_default_no_default_sets_null() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, age INT);");
    exec(env, "INSERT INTO t VALUES (1, 30);");

    auto r = exec(env, "UPDATE t SET age = DEFAULT WHERE id = 1;");
    assert(r.success);

    auto sel = exec(env, "SELECT age FROM t WHERE id = 1;");
    assert(sel.rows[0][0] == "NULL");

    std::cout << "[PASS] UPDATE SET col = DEFAULT on a column with no DEFAULT clause sets NULL\n";
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

// customers 1..10; orders exist only for customers {1, 2, 3, 5}, with
// amounts spaced out so a correlated range condition (o.amount > X) can
// narrow within a single customer's orders too. `with_index` controls
// whether orders.customer_id gets an index — used to check that a
// correlated subquery returns the SAME rows whether or not G's
// index-seeding path is actually exercised, proving correctness doesn't
// depend on it (only performance does).
static void seed_orders_customers(Env& env, bool with_index) {
    exec(env, "CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(20));");
    exec(env, "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, amount FLOAT);");
    for (int i = 1; i <= 10; i++) {
        exec(env, "INSERT INTO customers VALUES (" + std::to_string(i) + ", 'cust" + std::to_string(i) + "');");
    }
    // orders: (order_id, customer_id, amount)
    std::vector<std::tuple<int,int,float>> orders = {
        {1, 1, 10.0f}, {2, 1, 50.0f},
        {3, 2, 20.0f},
        {4, 3, 5.0f},  {5, 3, 60.0f}, {6, 3, 70.0f},
        {7, 5, 15.0f},
    };
    for (auto& [oid, cid, amt] : orders) {
        exec(env, "INSERT INTO orders VALUES (" + std::to_string(oid) + ", " + std::to_string(cid) +
                  ", " + std::to_string(amt) + ");");
    }
    if (with_index) {
        exec(env, "CREATE INDEX idx_orders_customer ON orders (customer_id);");
    }
}

void test_select_covering_index_returns_correct_projection() {
    cleanup();
    Env env;
    seed_people(env);

    // dept and age are both idx_dept_age columns; id is the primary key.
    // Every column this query touches (WHERE dept, projected dept/age/id)
    // is covered by idx_dept_age + the primary key, so this should be
    // answerable via scan_covering() with no Table::select_by_key() calls.
    auto r = exec(env, "SELECT id, dept, age FROM people WHERE dept = 'eng';");
    assert(r.success);
    assert(r.rows.size() == 10);  // every 5th id, 1..50
    for (auto& row : r.rows) {
        int id = std::stoi(row[0]);
        assert(id % 5 == 0);
        assert(row[1] == "eng");
        int age = std::stoi(row[2]);
        assert(age == 20 + (id % 10));
    }

    std::cout << "[PASS] SELECT covered entirely by an index returns the correct projection\n";
    cleanup();
}

void test_select_covering_index_with_range_and_order_by() {
    cleanup();
    Env env;
    seed_people(env);

    // score is idx_score's only column and also the ORDER BY column, and
    // id (primary key) is the only other projected column — fully
    // coverable by idx_score.
    auto r = exec(env, "SELECT id, score FROM people WHERE score > 45 ORDER BY score ASC;");
    assert(r.success);
    assert(r.rows.size() == 5);  // ids 46..50
    float prev = 0;
    for (auto& row : r.rows) {
        float score = std::stof(row[1]);
        assert(score > prev);
        prev = score;
        assert(std::stoi(row[0]) == static_cast<int>(score));
    }

    std::cout << "[PASS] range-filtered, ORDER BY SELECT covered by an index returns correctly sorted rows\n";
    cleanup();
}

void test_select_not_coverable_still_correct_via_fallback() {
    cleanup();
    Env env;
    seed_people(env);

    // score isn't a column of idx_dept (only dept is), so this can't be
    // covered by that index — must fall back to scan_with_index's
    // select_by_key() path, and still return the right rows.
    auto r = exec(env, "SELECT id, dept, score FROM people WHERE dept = 'eng';");
    assert(r.success);
    assert(r.rows.size() == 10);
    for (auto& row : r.rows) {
        int id = std::stoi(row[0]);
        assert(id % 5 == 0);
        assert(row[1] == "eng");
        assert(std::stof(row[2]) == static_cast<float>(id));
    }

    std::cout << "[PASS] SELECT not coverable by any index still returns correct rows via fallback\n";
    cleanup();
}

void test_correlated_exists_matches_customers_with_orders() {
    for (bool with_index : {false, true}) {
        cleanup();
        Env env;
        seed_orders_customers(env, with_index);

        auto r = exec(env, "SELECT id FROM customers c WHERE EXISTS "
                            "(SELECT 1 FROM orders o WHERE o.customer_id = c.id);");
        assert(r.success);
        assert(r.rows.size() == 4);  // customers 1, 2, 3, 5
        std::vector<int> ids;
        for (auto& row : r.rows) ids.push_back(std::stoi(row[0]));
        std::sort(ids.begin(), ids.end());
        assert((ids == std::vector<int>{1, 2, 3, 5}));

        cleanup();
    }
    std::cout << "[PASS] correlated EXISTS returns the same customers whether or not the "
                 "inner table's correlated column is indexed\n";
}

void test_correlated_not_exists_matches_customers_without_orders() {
    for (bool with_index : {false, true}) {
        cleanup();
        Env env;
        seed_orders_customers(env, with_index);

        auto r = exec(env, "SELECT id FROM customers c WHERE NOT EXISTS "
                            "(SELECT 1 FROM orders o WHERE o.customer_id = c.id);");
        assert(r.success);
        assert(r.rows.size() == 6);  // customers 4, 6, 7, 8, 9, 10
        std::vector<int> ids;
        for (auto& row : r.rows) ids.push_back(std::stoi(row[0]));
        std::sort(ids.begin(), ids.end());
        assert((ids == std::vector<int>{4, 6, 7, 8, 9, 10}));

        cleanup();
    }
    std::cout << "[PASS] correlated NOT EXISTS returns the same customers whether or not the "
                 "inner table's correlated column is indexed\n";
}

void test_correlated_exists_with_additional_range_condition() {
    for (bool with_index : {false, true}) {
        cleanup();
        Env env;
        seed_orders_customers(env, with_index);

        // customer 3 has an order > 50 (60, 70); customer 1 has an order of
        // exactly 50 (not > 50); customer 2's only order is 20; customer 5's
        // only order is 15 — so only customer 3 should match.
        auto r = exec(env, "SELECT id FROM customers c WHERE EXISTS "
                            "(SELECT 1 FROM orders o WHERE o.customer_id = c.id AND o.amount > 50);");
        assert(r.success);
        assert(r.rows.size() == 1);
        assert(r.rows[0][0] == "3");

        cleanup();
    }
    std::cout << "[PASS] correlated EXISTS combined with a range condition on the inner table is correct\n";
}

void test_correlated_in_subquery_matches_customers_with_orders() {
    for (bool with_index : {false, true}) {
        cleanup();
        Env env;
        seed_orders_customers(env, with_index);

        // Non-correlated IN, but exercises the same scan_with_index() path
        // on the inner table (orders.customer_id) that the correlated
        // EXISTS tests exercise for a correlated one.
        auto r = exec(env, "SELECT id FROM customers WHERE id IN "
                            "(SELECT customer_id FROM orders WHERE amount > 10);");
        assert(r.success);
        std::vector<int> ids;
        for (auto& row : r.rows) ids.push_back(std::stoi(row[0]));
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        assert((ids == std::vector<int>{1, 2, 3, 5}));  // orders > 10: cust 1 (50), 2 (20), 3 (60,70), 5 (15)

        cleanup();
    }
    std::cout << "[PASS] IN subquery with a range condition on the inner table returns correct customers\n";
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

void test_select_range_uses_single_column_index() {
    cleanup();
    Env env;
    seed_people(env);

    // score is 1.0..50.0 (score = id), idx_score is a single-column index.
    // score > 40 AND score <= 45 -> ids 41..45.
    auto r = exec(env, "SELECT id FROM people WHERE score > 40 AND score <= 45;");
    assert(r.success);
    assert(r.rows.size() == 5);
    for (auto& row : r.rows) {
        int id = std::stoi(row[0]);
        assert(id >= 41 && id <= 45);
    }

    std::cout << "[PASS] range WHERE (> and <=) uses a single-column index\n";
    cleanup();
}

void test_select_range_scoped_to_composite_prefix() {
    cleanup();
    Env env;
    seed_people(env);

    // idx_dept_age is (dept, age); dept = 'eng' fixes the leftmost column,
    // age >= 20 AND age <= 22 ranges over the next one. eng rows (every
    // 5th id) alternate age 25/20 — only the age=20 ones (ids 10,20,30,
    // 40,50) satisfy the range.
    auto r = exec(env, "SELECT id FROM people WHERE dept = 'eng' AND age >= 20 AND age <= 22;");
    assert(r.success);
    assert(r.rows.size() == 5);
    for (auto& row : r.rows) {
        int id = std::stoi(row[0]);
        assert(id % 10 == 0);  // ids 10,20,30,40,50
    }

    std::cout << "[PASS] composite-index range stays scoped to the leftmost equality prefix\n";
    cleanup();
}

void test_update_with_range_where_uses_index() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "UPDATE people SET age = 99 WHERE score >= 46;");
    assert(r.success);
    assert(r.rows_affected == 5);  // ids 46..50

    auto check = exec(env, "SELECT id FROM people WHERE age = 99;");
    assert(check.rows.size() == 5);

    std::cout << "[PASS] UPDATE with a range WHERE updates exactly the right rows\n";
    cleanup();
}

void test_delete_with_range_where_uses_index() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "DELETE FROM people WHERE score < 6;");
    assert(r.success);
    assert(r.rows_affected == 5);  // ids 1..5

    auto check = exec(env, "SELECT id FROM people;");
    assert(check.rows.size() == 45);

    std::cout << "[PASS] DELETE with a range WHERE deletes exactly the right rows\n";
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

// Skewed on purpose (one outlier: 100) so AVG and MEDIAN land on visibly
// different values — a symmetric fixture could pass even if MEDIAN were
// accidentally implemented as a copy of AVG.
static void seed_skewed_values(Env& env) {
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, grp VARCHAR(10), val INT);");
    exec(env, "INSERT INTO t VALUES (1, 'a', 1);");
    exec(env, "INSERT INTO t VALUES (2, 'a', 2);");
    exec(env, "INSERT INTO t VALUES (3, 'a', 3);");
    exec(env, "INSERT INTO t VALUES (4, 'a', 4);");
    exec(env, "INSERT INTO t VALUES (5, 'a', 100);");
    exec(env, "INSERT INTO t VALUES (6, 'b', 10);");
    exec(env, "INSERT INTO t VALUES (7, 'b', 20);");
    exec(env, "INSERT INTO t VALUES (8, 'b', 30);");
}

void test_select_max_min_avg_median_whole_table() {
    cleanup();
    Env env;
    seed_skewed_values(env);

    auto r = exec(env, "SELECT MAX(val), MIN(val), AVG(val), MEDIAN(val) FROM t WHERE grp = 'a';");
    assert(r.success);
    assert(r.columns[0] == "MAX(val)");
    assert(r.columns[1] == "MIN(val)");
    assert(r.columns[2] == "AVG(val)");
    assert(r.columns[3] == "MEDIAN(val)");
    assert(r.rows[0][0] == "100");
    assert(r.rows[0][1] == "1");
    assert(r.rows[0][2] == "22.000000");  // (1+2+3+4+100)/5
    assert(r.rows[0][3] == "3.000000");   // sorted 1,2,3,4,100 -> middle value

    std::cout << "[PASS] MAX/MIN/AVG/MEDIAN over the whole (WHERE-filtered) table\n";
    cleanup();
}

void test_select_group_by_max_min_avg_median() {
    cleanup();
    Env env;
    seed_skewed_values(env);

    auto r = exec(env,
        "SELECT grp, MAX(val), MIN(val), AVG(val), MEDIAN(val) FROM t GROUP BY grp;");
    assert(r.success);
    assert(r.rows.size() == 2);

    // group 'a': 1, 2, 3, 4, 100
    assert(r.rows[0][0] == "a");
    assert(r.rows[0][1] == "100");
    assert(r.rows[0][2] == "1");
    assert(r.rows[0][3] == "22.000000");
    assert(r.rows[0][4] == "3.000000");

    // group 'b': 10, 20, 30
    assert(r.rows[1][0] == "b");
    assert(r.rows[1][1] == "30");
    assert(r.rows[1][2] == "10");
    assert(r.rows[1][3] == "20.000000");
    assert(r.rows[1][4] == "20.000000");

    std::cout << "[PASS] MAX/MIN/AVG/MEDIAN computed independently per GROUP BY group\n";
    cleanup();
}

void test_select_having_count_filters_groups() {
    cleanup();
    Env env;
    seed_people(env);  // eng: 10 rows, sales: 40 rows

    auto r = exec(env, "SELECT dept, COUNT(*) FROM people GROUP BY dept HAVING COUNT(*) > 20;");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][0] == "sales");
    assert(r.rows[0][1] == "40");

    std::cout << "[PASS] HAVING COUNT(*) > n keeps only groups that satisfy it\n";
    cleanup();
}

void test_select_having_avg_filters_groups() {
    cleanup();
    Env env;
    seed_skewed_values(env);  // grp 'a': avg 22.0, grp 'b': avg 20.0

    auto r = exec(env, "SELECT grp, AVG(val) FROM t GROUP BY grp HAVING AVG(val) < 21;");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][0] == "b");
    assert(r.rows[0][1] == "20.000000");

    std::cout << "[PASS] HAVING AVG(col) < n filters on a computed aggregate, not a stored column\n";
    cleanup();
}

void test_select_having_and_logical() {
    cleanup();
    Env env;
    seed_skewed_values(env);  // grp 'a': count 5, avg 22.0 | grp 'b': count 3, avg 20.0

    auto r = exec(env,
        "SELECT grp, COUNT(*), AVG(val) FROM t GROUP BY grp "
        "HAVING COUNT(*) > 3 AND AVG(val) > 15;");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][0] == "a");  // 'b' fails COUNT(*) > 3 (3 is not > 3)

    std::cout << "[PASS] HAVING <cond> AND <cond> requires both to hold\n";
    cleanup();
}

void test_select_having_on_plain_grouped_column() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "SELECT dept, COUNT(*) FROM people GROUP BY dept HAVING dept = 'eng';");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][0] == "eng");
    assert(r.rows[0][1] == "10");

    std::cout << "[PASS] HAVING on a plain (non-aggregate) grouped column works\n";
    cleanup();
}

void test_select_having_no_matching_groups_returns_empty_success() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "SELECT dept, COUNT(*) FROM people GROUP BY dept HAVING COUNT(*) > 1000;");
    assert(r.success);
    assert(r.rows.empty());

    std::cout << "[PASS] HAVING that matches no groups returns an empty, successful result\n";
    cleanup();
}

void test_select_having_without_group_by_rejected() {
    cleanup();
    Env env;
    seed_people(env);

    auto r = exec(env, "SELECT COUNT(*) FROM people HAVING COUNT(*) > 5;");
    assert(!r.success);

    std::cout << "[PASS] HAVING without GROUP BY is rejected, not silently ignored\n";
    cleanup();
}

void test_select_max_min_skip_nulls() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, val INT);");
    exec(env, "INSERT INTO t VALUES (1, 5);");
    exec(env, "INSERT INTO t VALUES (2, NULL);");
    exec(env, "INSERT INTO t VALUES (3, 9);");

    auto r = exec(env, "SELECT MAX(val), MIN(val) FROM t;");
    assert(r.success);
    assert(r.rows[0][0] == "9");
    assert(r.rows[0][1] == "5");

    std::cout << "[PASS] MAX/MIN skip NULL values\n";
    cleanup();
}

void test_select_max_min_avg_median_all_null_reports_null() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, val INT);");
    exec(env, "INSERT INTO t VALUES (1, NULL);");
    exec(env, "INSERT INTO t VALUES (2, NULL);");

    auto r = exec(env, "SELECT MAX(val), MIN(val), AVG(val), MEDIAN(val) FROM t;");
    assert(r.success);
    assert(r.rows[0][0] == "NULL");
    assert(r.rows[0][1] == "NULL");
    assert(r.rows[0][2] == "NULL");
    assert(r.rows[0][3] == "NULL");

    std::cout << "[PASS] MAX/MIN/AVG/MEDIAN over all-NULL input reports NULL, not an error\n";
    cleanup();
}

void test_select_avg_on_non_numeric_column_fails() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(10));");
    exec(env, "INSERT INTO t VALUES (1, 'x');");

    auto r = exec(env, "SELECT AVG(name) FROM t;");
    assert(!r.success);

    std::cout << "[PASS] AVG on a non-numeric column is rejected, not silently wrong\n";
    cleanup();
}

void test_select_max_on_varchar_column_works() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(10));");
    exec(env, "INSERT INTO t VALUES (1, 'banana');");
    exec(env, "INSERT INTO t VALUES (2, 'apple');");
    exec(env, "INSERT INTO t VALUES (3, 'cherry');");

    auto r = exec(env, "SELECT MAX(name), MIN(name) FROM t;");
    assert(r.success);
    assert(r.rows[0][0] == "cherry");  // lexicographically largest
    assert(r.rows[0][1] == "apple");   // lexicographically smallest

    std::cout << "[PASS] MAX/MIN work on VARCHAR columns (lexicographic ordering)\n";
    cleanup();
}

void test_select_group_by_count() {
    cleanup();
    Env env;
    seed_people(env);  // i % 5 == 0 -> 'eng' (10 rows), else 'sales' (40 rows)

    auto r = exec(env, "SELECT dept, COUNT(*) FROM people GROUP BY dept;");
    assert(r.success);
    assert(r.columns.size() == 2);
    assert(r.columns[0] == "dept");
    assert(r.columns[1] == "COUNT(*)");
    assert(r.rows.size() == 2);  // exactly two groups: eng, sales

    // std::map key ordering sorts group values ascending, so 'eng' < 'sales'
    assert(r.rows[0][0] == "eng");
    assert(r.rows[0][1] == "10");
    assert(r.rows[1][0] == "sales");
    assert(r.rows[1][1] == "40");

    std::cout << "[PASS] SELECT col, COUNT(*) ... GROUP BY col groups correctly\n";
    cleanup();
}

void test_select_group_by_plain_column_no_aggregate() {
    cleanup();
    Env env;
    seed_people(env);

    // GROUP BY with no aggregate column at all still dedups to one row
    // per distinct dept — a plain "SELECT DISTINCT"-style use of GROUP BY.
    auto r = exec(env, "SELECT dept FROM people GROUP BY dept;");
    assert(r.success);
    assert(r.rows.size() == 2);
    assert(r.rows[0][0] == "eng");
    assert(r.rows[1][0] == "sales");

    std::cout << "[PASS] GROUP BY with no aggregate column dedups distinct values\n";
    cleanup();
}

void test_select_group_by_with_where_filters_before_grouping() {
    cleanup();
    Env env;
    seed_people(env);

    // WHERE narrows the rows that get grouped, not the groups themselves —
    // score > 45 only matches ids 46..50, all of which are 'sales'
    // (46,47,48,49 are sales; 50 is eng, since 50 % 5 == 0).
    auto r = exec(env, "SELECT dept, COUNT(*) FROM people WHERE score > 45 GROUP BY dept;");
    assert(r.success);
    assert(r.rows.size() == 2);
    assert(r.rows[0][0] == "eng");
    assert(r.rows[0][1] == "1");
    assert(r.rows[1][0] == "sales");
    assert(r.rows[1][1] == "4");

    std::cout << "[PASS] WHERE filters rows before GROUP BY buckets them\n";
    cleanup();
}

void test_select_group_by_count_column_skips_nulls() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, grp VARCHAR(10), val INT);");
    exec(env, "INSERT INTO t VALUES (1, 'a', 10);");
    exec(env, "INSERT INTO t VALUES (2, 'a', NULL);");
    exec(env, "INSERT INTO t VALUES (3, 'b', 5);");
    exec(env, "INSERT INTO t VALUES (4, 'b', 7);");

    auto r = exec(env, "SELECT grp, COUNT(val) FROM t GROUP BY grp;");
    assert(r.success);
    assert(r.rows.size() == 2);
    assert(r.rows[0][0] == "a");
    assert(r.rows[0][1] == "1");   // one NULL skipped
    assert(r.rows[1][0] == "b");
    assert(r.rows[1][1] == "2");

    std::cout << "[PASS] COUNT(col) under GROUP BY skips NULLs per group\n";
    cleanup();
}

void test_select_group_by_nulls_form_one_group() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE t (id INT PRIMARY KEY, grp VARCHAR(10));");
    exec(env, "INSERT INTO t VALUES (1, NULL);");
    exec(env, "INSERT INTO t VALUES (2, NULL);");
    exec(env, "INSERT INTO t VALUES (3, 'x');");

    auto r = exec(env, "SELECT grp, COUNT(*) FROM t GROUP BY grp;");
    assert(r.success);
    assert(r.rows.size() == 2);  // NULL group + 'x' group, not two separate NULL groups

    bool found_null_group = false;
    for (auto& row : r.rows) {
        if (row[0] == "NULL") {
            found_null_group = true;
            assert(row[1] == "2");
        }
    }
    assert(found_null_group);

    std::cout << "[PASS] GROUP BY treats NULL as a single group, unlike WHERE comparisons\n";
    cleanup();
}

void test_select_group_by_ungrouped_plain_column_rejected() {
    cleanup();
    Env env;
    seed_people(env);

    // 'age' is neither aggregated nor in the GROUP BY list — must be rejected.
    auto r = exec(env, "SELECT dept, age, COUNT(*) FROM people GROUP BY dept;");
    assert(!r.success);
    assert(r.error_message.find("age") != std::string::npos);

    std::cout << "[PASS] GROUP BY rejects a plain SELECT column absent from the GROUP BY list\n";
    cleanup();
}

void test_select_group_by_with_join_rejected() {
    cleanup();
    Env env;
    seed_orders_customers(env, false);

    auto r = exec(env,
        "SELECT customers.name, COUNT(*) FROM customers "
        "INNER JOIN orders ON customers.id = orders.customer_id GROUP BY customers.name;");
    assert(!r.success);

    std::cout << "[PASS] GROUP BY with JOIN is rejected (v1 scope)\n";
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
// DESCRIBE
// ─────────────────────────────────────────────

// Statements here go straight through Executor::execute(Statement) via the
// exec() helper above, never through Database::execute(sql) — so
// CreateTableStmt::source_text (and therefore TableSchema::create_sql) is
// always empty in this harness. That makes this the right place to
// exercise build_create_table_sql's best-effort fallback reconstruction,
// as opposed to the verbatim-text path covered in test_database.cpp.
void test_describe_falls_back_to_reconstruction_without_stored_sql() {
    cleanup();
    Env env;
    exec(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50) NOT NULL);");

    auto r = exec(env, "DESCRIBE users;");
    assert(r.success);
    assert(r.rows.size() == 3);  // 2 columns + trailing "Create Table" row
    assert(r.rows[0][0] == "id");
    assert(r.rows[1][0] == "name");

    const std::string& recreate = r.rows.back()[1];
    assert(recreate.find("CREATE TABLE users") != std::string::npos);
    assert(recreate.find("PRIMARY KEY") != std::string::npos);

    std::cout << "[PASS] DESCRIBE falls back to reconstructing CREATE TABLE when no source text was stored\n";
    cleanup();
}

void test_describe_unknown_table_fails() {
    cleanup();
    Env env;

    auto r = exec(env, "DESCRIBE no_such_table;");
    assert(!r.success);

    std::cout << "[PASS] DESCRIBE on an unknown table fails\n";
    cleanup();
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────

int main() {
    std::cout << "\n=== CREATE TABLE Tests ===\n";
    test_create_table();
    test_create_table_duplicate_throws();
    test_create_table_default_clause();
    test_create_table_default_null_on_not_null_throws();
    test_create_table_default_type_mismatch_throws();
    test_create_table_default_with_auto_increment_throws();

    std::cout << "\n=== INSERT Tests ===\n";
    test_insert_and_rows_affected();
    test_insert_not_null_violation();
    test_insert_omitted_column_uses_default();
    test_insert_omitted_column_no_default_stays_null();
    test_insert_explicit_value_overrides_default();
    test_insert_positional_default_keyword();
    test_insert_named_default_keyword();
    test_insert_default_keyword_no_default_is_null();
    test_insert_default_keyword_on_not_null_without_default_fails();
    test_insert_default_pk_omitted_still_auto_increments();
    test_insert_default_value_validated_against_check();

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
    test_update_set_default();
    test_update_set_default_no_default_sets_null();

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
    test_select_range_uses_single_column_index();
    test_select_range_scoped_to_composite_prefix();
    test_update_with_range_where_uses_index();
    test_delete_with_range_where_uses_index();

    std::cout << "\n=== Covering Index Read Tests ===\n";
    test_select_covering_index_returns_correct_projection();
    test_select_covering_index_with_range_and_order_by();
    test_select_not_coverable_still_correct_via_fallback();

    std::cout << "\n=== Correlated Subquery Index Tests ===\n";
    test_correlated_exists_matches_customers_with_orders();
    test_correlated_not_exists_matches_customers_without_orders();
    test_correlated_exists_with_additional_range_condition();
    test_correlated_in_subquery_matches_customers_with_orders();
    test_select_or_clause_not_incorrectly_narrowed();
    test_select_count_aggregate_uses_index();

    std::cout << "\n=== Aggregate Function Tests (MAX/MIN/AVG/MEDIAN) ===\n";
    test_select_max_min_avg_median_whole_table();
    test_select_max_min_skip_nulls();
    test_select_max_min_avg_median_all_null_reports_null();
    test_select_avg_on_non_numeric_column_fails();
    test_select_max_on_varchar_column_works();

    std::cout << "\n=== GROUP BY Tests ===\n";
    test_select_group_by_count();
    test_select_group_by_plain_column_no_aggregate();
    test_select_group_by_with_where_filters_before_grouping();
    test_select_group_by_count_column_skips_nulls();
    test_select_group_by_nulls_form_one_group();
    test_select_group_by_ungrouped_plain_column_rejected();
    test_select_group_by_with_join_rejected();
    test_select_group_by_max_min_avg_median();

    std::cout << "\n=== HAVING Tests ===\n";
    test_select_having_count_filters_groups();
    test_select_having_avg_filters_groups();
    test_select_having_and_logical();
    test_select_having_on_plain_grouped_column();
    test_select_having_no_matching_groups_returns_empty_success();
    test_select_having_without_group_by_rejected();

    test_update_where_indexed_equality();
    test_delete_where_unique_index_deletes_one_row();
    test_delete_where_nonunique_index_deletes_all_matches();

    std::cout << "\n=== Persistence Tests ===\n";
    test_data_survives_restart();

    std::cout << "\n=== DESCRIBE Tests ===\n";
    test_describe_falls_back_to_reconstruction_without_stored_sql();
    test_describe_unknown_table_fails();

    std::cout << "\nAll executor tests passed.\n";
    return 0;
}