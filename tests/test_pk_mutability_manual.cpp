// Manual sanity check for primary key mutability (UPDATE changing a PK
// column), at the SQL/Executor layer — Table-layer coverage already lives
// in tests/test_table.cpp (test_update_primary_key_moves_row and friends).
// Same pattern as the other test_*_manual.cpp files.
//
// g++ -std=c++17 tests/test_pk_mutability_manual.cpp src/executor/executor.cpp src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/btree/free_list_manager.cpp src/btree/key.cpp src/index/index.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/header/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp -o /tmp/test_pk && /tmp/test_pk

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

static const std::string DB_FILE  = "test_pk.db";
static const std::string WAL_FILE = "test_pk.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

struct Env {
    DiskManager      dm;
    BufferPool       bp;
    WALManager       wal;
    HeaderManager    hm;
    FreeListManager  fl;
    CatalogManager   cat;
    Executor         exec;

    Env()
        : dm(DB_FILE), bp(dm), wal(WAL_FILE, bp), hm(bp, wal),
          fl(bp, wal, hm), cat(bp, wal), exec(cat, bp, wal, fl)
    {
        bool is_new = !fs::exists(DB_FILE) || fs::file_size(DB_FILE) == 0;
        if (is_new) hm.init();
        else        hm.load();
        cat.load(is_new);
    }
};

static Statement parse(const std::string& sql) {
    Tokenizer t(sql);
    Parser    p(t.tokenize());
    return p.parse();
}

static QueryResult run(Env& env, const std::string& sql) {
    return env.exec.execute(parse(sql));
}

int main() {
    // ── basic single-row PK change ──────────────────────────────────────
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));").success);
        assert(run(env, "INSERT INTO users (id, name) VALUES (1, 'Alice');").success);

        auto u = run(env, "UPDATE users SET id = 100 WHERE id = 1;");
        assert(u.success);

        auto old_sel = run(env, "SELECT * FROM users WHERE id = 1;");
        assert(old_sel.success && old_sel.rows.empty());

        auto new_sel = run(env, "SELECT name FROM users WHERE id = 100;");
        assert(new_sel.success && new_sel.rows.size() == 1);
        assert(new_sel.rows[0][0] == "Alice");
        std::cout << "[PASS] UPDATE can change a primary key value and the row moves correctly\n";

        cleanup();
    }

    // ── PK collision is rejected ────────────────────────────────────────
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));").success);
        assert(run(env, "INSERT INTO users (id, name) VALUES (1, 'Alice');").success);
        assert(run(env, "INSERT INTO users (id, name) VALUES (2, 'Bob');").success);

        auto u = run(env, "UPDATE users SET id = 2 WHERE id = 1;");
        assert(!u.success);
        std::cout << "[PASS] UPDATE rejects a primary key change that collides with another row: "
                   << u.error_message << "\n";

        // neither row should have been touched
        auto sel = run(env, "SELECT name FROM users WHERE id = 1;");
        assert(sel.success && sel.rows.size() == 1 && sel.rows[0][0] == "Alice");
        std::cout << "[PASS] rejected PK collision leaves both rows untouched\n";

        cleanup();
    }

    // ── auto-increment resumes correctly after a manual PK change ──────
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50));").success);
        assert(run(env, "INSERT INTO users (name) VALUES ('Alice');").success);  // id = 1

        assert(run(env, "UPDATE users SET id = 500 WHERE name = 'Alice';").success);

        auto ins2 = run(env, "INSERT INTO users (name) VALUES ('Bob');");
        assert(ins2.success);
        auto sel = run(env, "SELECT id FROM users WHERE name = 'Bob';");
        assert(sel.success && sel.rows.size() == 1);
        assert(std::stoi(sel.rows[0][0]) >= 501);
        std::cout << "[PASS] auto-increment resumes past a manually-assigned primary key value\n";

        cleanup();
    }

    // ── multi-row UPDATE colliding on the primary key is rejected ──────
    // (this engine's UPDATE only accepts literal SET values, not
    // per-row expressions — so a single statement can't move several
    // matched rows to DIFFERENT new keys at once; every matched row gets
    // the identical literal. That means a multi-row PK-changing UPDATE
    // can only ever collide with itself, which is exactly what this
    // checks: the final_keys duplicate detection in update_matched.)
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE users (id INT PRIMARY KEY, dept VARCHAR(50));").success);
        assert(run(env, "INSERT INTO users (id, dept) VALUES (1, 'eng');").success);
        assert(run(env, "INSERT INTO users (id, dept) VALUES (2, 'eng');").success);

        auto u = run(env, "UPDATE users SET id = 500 WHERE dept = 'eng';");
        assert(!u.success);
        std::cout << "[PASS] a multi-row UPDATE that would collide two rows onto the same new "
                      "primary key is rejected: " << u.error_message << "\n";

        // neither row should have moved
        auto sel = run(env, "SELECT id FROM users WHERE dept = 'eng' ORDER BY id;");
        assert(sel.success && sel.rows.size() == 2);
        assert(sel.rows[0][0] == "1" && sel.rows[1][0] == "2");
        std::cout << "[PASS] the rejected batch left both rows at their original primary keys\n";

        cleanup();
    }

    // ── same-statement PK swap is rejected with a clear error ──────────
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));").success);
        assert(run(env, "INSERT INTO users (id, name) VALUES (1, 'Alice');").success);
        assert(run(env, "INSERT INTO users (id, name) VALUES (2, 'Bob');").success);

        // there's no CASE expression in this engine's UPDATE, so simulate
        // "swap" the simplest way that still exercises the same code path:
        // two separate UPDATEs targeting keys that currently belong to
        // each other, executed as part of the same logical intent. A
        // *single* multi-row UPDATE statement that would require this
        // reordering is exactly what update_matched's collision check
        // rejects — reproduced directly here by attempting to move id=1
        // onto id=2's still-occupied slot.
        auto u = run(env, "UPDATE users SET id = 2 WHERE id = 1;");
        assert(!u.success);
        std::cout << "[PASS] moving a row onto another still-occupied primary key is rejected "
                      "(matches update_matched's documented swap/chain scope cut): "
                   << u.error_message << "\n";

        cleanup();
    }

    // ── FOREIGN KEY ... ON UPDATE CASCADE now works on a real PK change ─
    // (previously impossible end-to-end: Table::update_matched threw on
    // any PK column change before the FK layer ever got a chance to run;
    // now that PK columns are mutable, the exact same ON UPDATE CASCADE
    // logic from the FK work applies here without any FK-layer changes.)
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE departments (id INT PRIMARY KEY, name VARCHAR(50));").success);
        assert(run(env,
            "CREATE TABLE employees (id INT PRIMARY KEY, department_id INT, "
            "FOREIGN KEY (department_id) REFERENCES departments (id) ON UPDATE CASCADE);").success);

        assert(run(env, "INSERT INTO departments (id, name) VALUES (1, 'Eng');").success);
        assert(run(env, "INSERT INTO employees (id, department_id) VALUES (1, 1);").success);

        auto u = run(env, "UPDATE departments SET id = 99 WHERE id = 1;");
        assert(u.success);

        auto sel = run(env, "SELECT department_id FROM employees WHERE id = 1;");
        assert(sel.success && sel.rows.size() == 1);
        assert(sel.rows[0][0] == "99");
        std::cout << "[PASS] ON UPDATE CASCADE propagates a real primary key change to child rows\n";

        cleanup();
    }

    // ── FOREIGN KEY RESTRICT still blocks a PK change on a referenced row ─
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE departments (id INT PRIMARY KEY, name VARCHAR(50));").success);
        assert(run(env,
            "CREATE TABLE employees (id INT PRIMARY KEY, department_id INT, "
            "FOREIGN KEY (department_id) REFERENCES departments (id));").success);  // default RESTRICT

        assert(run(env, "INSERT INTO departments (id, name) VALUES (1, 'Eng');").success);
        assert(run(env, "INSERT INTO employees (id, department_id) VALUES (1, 1);").success);

        auto u = run(env, "UPDATE departments SET id = 99 WHERE id = 1;");
        assert(!u.success);
        std::cout << "[PASS] ON UPDATE RESTRICT (default) still blocks a primary key change while referenced: "
                   << u.error_message << "\n";

        cleanup();
    }

    // ── PK mutability survives a catalog/data reload ────────────────────
    {
        cleanup();
        {
            Env env;
            assert(run(env, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(50));").success);
            assert(run(env, "INSERT INTO users (id, name) VALUES (1, 'Alice');").success);
            assert(run(env, "UPDATE users SET id = 42 WHERE id = 1;").success);
        }
        {
            Env env;
            auto sel = run(env, "SELECT name FROM users WHERE id = 42;");
            assert(sel.success && sel.rows.size() == 1 && sel.rows[0][0] == "Alice");
            auto gone = run(env, "SELECT * FROM users WHERE id = 1;");
            assert(gone.success && gone.rows.empty());
            std::cout << "[PASS] a moved primary key persists correctly across a database restart\n";
        }
        cleanup();
    }

    std::cout << "\nAll primary key mutability sanity checks passed.\n";
    return 0;
}
