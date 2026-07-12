// g++ -std=c++17 tests/test_database.cpp src/database.cpp src/executor/executor.cpp \
//     src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp \
//     src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp \
//     src/btree/free_list_manager.cpp src/btree/key.cpp src/index/index.cpp \
//     src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp \
//     src/header/header_manager.cpp src/storage/buffer_pool.cpp \
//     src/wal/wal_manager.cpp -o tests/test_database && ./tests/test_database

#include <iostream>
#include <cassert>
#include <filesystem>

#include "../src/database.h"

namespace fs = std::filesystem;

// Use a dedicated temp directory so tests never touch ~/Documents/GS-DBEngine/
static const fs::path TEST_DIR = fs::temp_directory_path() / "gs_db_test";

void cleanup() {
    if (fs::exists(TEST_DIR)) fs::remove_all(TEST_DIR);
}

// Construct a fresh Database pointed at TEST_DIR
static Database make_db() { return Database(TEST_DIR); }

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

void test_construction_creates_data_dir() {
    cleanup();
    Database db = make_db();
    assert(fs::exists(TEST_DIR));
    assert(db.current_database() == "");
    std::cout << "[PASS] constructor creates data_dir and starts with no active database\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// CREATE DATABASE
// ─────────────────────────────────────────────────────────────────────────────

void test_create_database() {
    cleanup();
    Database db = make_db();

    auto r = db.execute("CREATE DATABASE mydb;");
    assert(r.success);
    assert(fs::exists(TEST_DIR / "mydb"));

    std::cout << "[PASS] CREATE DATABASE creates the database directory\n";
    cleanup();
}

void test_create_database_does_not_auto_use() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE mydb;");
    assert(db.current_database() == "");   // must still be empty

    std::cout << "[PASS] CREATE DATABASE does NOT auto-USE\n";
    cleanup();
}

void test_create_database_duplicate_fails() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE mydb;");
    auto r = db.execute("CREATE DATABASE mydb;");
    assert(!r.success);
    assert(!r.error_message.empty());

    std::cout << "[PASS] CREATE DATABASE on duplicate returns error\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// USE
// ─────────────────────────────────────────────────────────────────────────────

void test_use_database() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE mydb;");
    auto r = db.execute("USE mydb;");
    assert(r.success);
    assert(db.current_database() == "mydb");

    std::cout << "[PASS] USE switches active database\n";
    cleanup();
}

void test_use_nonexistent_fails() {
    cleanup();
    Database db = make_db();

    auto r = db.execute("USE ghost;");
    assert(!r.success);
    assert(db.current_database() == "");  // unchanged

    std::cout << "[PASS] USE nonexistent database returns error, leaves state unchanged\n";
    cleanup();
}

void test_use_switches_between_databases() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE db1;");
    db.execute("CREATE DATABASE db2;");

    db.execute("USE db1;");
    assert(db.current_database() == "db1");

    db.execute("USE db2;");
    assert(db.current_database() == "db2");

    std::cout << "[PASS] USE correctly switches between two databases\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// No database selected guard
// ─────────────────────────────────────────────────────────────────────────────

void test_no_database_selected_guard() {
    cleanup();
    Database db = make_db();

    auto r1 = db.execute("SELECT * FROM users;");
    assert(!r1.success);
    assert(!r1.error_message.empty());

    auto r2 = db.execute("CREATE TABLE t (id INT PRIMARY KEY);");
    assert(!r2.success);

    auto r3 = db.execute("INSERT INTO t VALUES (1);");
    assert(!r3.success);

    std::cout << "[PASS] non-DB statements without active database return error\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// Table operations through Database
// ─────────────────────────────────────────────────────────────────────────────

void test_create_table_and_insert() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE mydb;");
    db.execute("USE mydb;");

    auto r = db.execute("CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    assert(r.success);

    auto ins = db.execute("INSERT INTO users VALUES (NULL, 'Alice', 25);");
    assert(ins.success);
    assert(ins.rows_affected == 1);

    std::cout << "[PASS] CREATE TABLE and INSERT work after USE\n";
    cleanup();
}

void test_select_after_insert() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE mydb;");
    db.execute("USE mydb;");
    db.execute("CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL, age INT);");
    db.execute("INSERT INTO users VALUES (NULL, 'Alice', 25);");
    db.execute("INSERT INTO users VALUES (NULL, 'Bob', 30);");

    auto r = db.execute("SELECT * FROM users;");
    assert(r.success);
    assert(r.rows.size() == 2);
    assert(r.rows[0][1] == "Alice");
    assert(r.rows[1][1] == "Bob");

    std::cout << "[PASS] SELECT returns correct rows after INSERT\n";
    cleanup();
}

void test_database_isolation() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE db1;");
    db.execute("CREATE DATABASE db2;");

    db.execute("USE db1;");
    db.execute("CREATE TABLE items (id INT PRIMARY KEY, val VARCHAR(10));");
    db.execute("INSERT INTO items VALUES (1, 'hello');");

    // Switch to db2 — items table must not exist there
    db.execute("USE db2;");
    auto r = db.execute("SELECT * FROM items;");
    assert(!r.success);  // table doesn't exist in db2

    // Switch back — data in db1 still intact
    db.execute("USE db1;");
    auto r2 = db.execute("SELECT * FROM items;");
    assert(r2.success);
    assert(r2.rows.size() == 1);
    assert(r2.rows[0][1] == "hello");

    std::cout << "[PASS] databases are isolated — data in one not visible in another\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// SHOW DATABASES
// ─────────────────────────────────────────────────────────────────────────────

void test_show_databases() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE alpha;");
    db.execute("CREATE DATABASE beta;");

    auto r = db.execute("SHOW DATABASES;");
    assert(r.success);
    assert(r.rows.size() == 2);

    std::cout << "[PASS] SHOW DATABASES lists all created databases\n";
    cleanup();
}

void test_show_databases_empty() {
    cleanup();
    Database db = make_db();

    auto r = db.execute("SHOW DATABASES;");
    assert(r.success);
    assert(r.rows.empty());

    std::cout << "[PASS] SHOW DATABASES returns empty list when no databases exist\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// DROP DATABASE
// ─────────────────────────────────────────────────────────────────────────────

void test_drop_database() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE mydb;");
    assert(fs::exists(TEST_DIR / "mydb"));

    auto r = db.execute("DROP DATABASE mydb;");
    assert(r.success);
    assert(!fs::exists(TEST_DIR / "mydb"));

    std::cout << "[PASS] DROP DATABASE removes the directory\n";
    cleanup();
}

void test_drop_nonexistent_database_fails() {
    cleanup();
    Database db = make_db();

    auto r = db.execute("DROP DATABASE ghost;");
    assert(!r.success);

    std::cout << "[PASS] DROP DATABASE on nonexistent returns error\n";
    cleanup();
}

void test_drop_active_database_deselects() {
    cleanup();
    Database db = make_db();

    db.execute("CREATE DATABASE mydb;");
    db.execute("USE mydb;");
    assert(db.current_database() == "mydb");

    auto r = db.execute("DROP DATABASE mydb;");
    assert(r.success);
    assert(db.current_database() == "");   // no longer active

    std::cout << "[PASS] DROP DATABASE on active database deselects it\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────────

void test_data_persists_across_restart() {
    cleanup();

    {
        Database db = make_db();
        db.execute("CREATE DATABASE mydb;");
        db.execute("USE mydb;");
        db.execute("CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50) NOT NULL);");
        db.execute("INSERT INTO users VALUES (NULL, 'Alice');");
        db.execute("INSERT INTO users VALUES (NULL, 'Bob');");
    }
    // Database object destroyed — stack flushed

    {
        Database db = make_db();   // reopen same data_dir
        db.execute("USE mydb;");
        auto r = db.execute("SELECT * FROM users;");
        assert(r.success);
        assert(r.rows.size() == 2);
        assert(r.rows[0][1] == "Alice");
        assert(r.rows[1][1] == "Bob");
    }

    std::cout << "[PASS] data persists across Database object destruction and recreation\n";
    cleanup();
}

void test_databases_list_persists_across_restart() {
    cleanup();

    {
        Database db = make_db();
        db.execute("CREATE DATABASE alpha;");
        db.execute("CREATE DATABASE beta;");
    }

    {
        Database db = make_db();
        auto r = db.execute("SHOW DATABASES;");
        assert(r.success);
        assert(r.rows.size() == 2);
    }

    std::cout << "[PASS] SHOW DATABASES reflects databases created before restart\n";
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== Construction Tests ===\n";
    test_construction_creates_data_dir();

    std::cout << "\n=== CREATE DATABASE Tests ===\n";
    test_create_database();
    test_create_database_does_not_auto_use();
    test_create_database_duplicate_fails();

    std::cout << "\n=== USE Tests ===\n";
    test_use_database();
    test_use_nonexistent_fails();
    test_use_switches_between_databases();

    std::cout << "\n=== Guard Tests ===\n";
    test_no_database_selected_guard();

    std::cout << "\n=== Table Operation Tests ===\n";
    test_create_table_and_insert();
    test_select_after_insert();
    test_database_isolation();

    std::cout << "\n=== SHOW DATABASES Tests ===\n";
    test_show_databases();
    test_show_databases_empty();

    std::cout << "\n=== DROP DATABASE Tests ===\n";
    test_drop_database();
    test_drop_nonexistent_database_fails();
    test_drop_active_database_deselects();

    std::cout << "\n=== Persistence Tests ===\n";
    test_data_persists_across_restart();
    test_databases_list_persists_across_restart();

    std::cout << "\nAll database tests passed.\n";
    return 0;
}