// Manual sanity check for UNIQUE constraint support (not wired into the
// CMake test suite — throwaway script to validate the feature end to end).
//
// g++ -std=c++17 tests/test_unique_constraint_manual.cpp src/executor/executor.cpp src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/btree/free_list_manager.cpp src/btree/key.cpp src/index/index.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/header/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp -o /tmp/test_unique && /tmp/test_unique

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

static const std::string DB_FILE  = "test_unique.db";
static const std::string WAL_FILE = "test_unique.wal";

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
        hm.init();
        cat.load(true);
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
    // ── Column-level UNIQUE ──────────────────────────────────────────
    {
        cleanup();
        Env env;
        auto r = run(env,
            "CREATE TABLE users ("
            "  id    INT PRIMARY KEY AUTO_INCREMENT,"
            "  email VARCHAR(50) UNIQUE,"
            "  name  VARCHAR(50)"
            ");");
        assert(r.success);

        // one auto-named unique index should exist for 'email'
        auto idxs = env.cat.get_indexes_for_table("users");
        assert(idxs.size() == 1);
        assert(idxs[0].is_unique);
        assert(idxs[0].column_names == std::vector<std::string>{"email"});
        std::cout << "[PASS] column-level UNIQUE creates one unique index\n";

        auto i1 = run(env, "INSERT INTO users (id, email, name) VALUES (1, 'a@x.com', 'Alice');");
        assert(i1.success);
        auto i2 = run(env, "INSERT INTO users (id, email, name) VALUES (2, 'a@x.com', 'Bob');");
        assert(!i2.success);
        std::cout << "[PASS] column-level UNIQUE rejects duplicate email: " << i2.error_message << "\n";

        auto i3 = run(env, "INSERT INTO users (id, email, name) VALUES (3, 'b@x.com', 'Carol');");
        assert(i3.success);
        std::cout << "[PASS] column-level UNIQUE allows distinct email\n";

        // NULL is never subject to uniqueness — two NULL emails are fine
        auto i4 = run(env, "INSERT INTO users (id, email, name) VALUES (4, NULL, 'Dave');");
        auto i5 = run(env, "INSERT INTO users (id, email, name) VALUES (5, NULL, 'Eve');");
        assert(i4.success && i5.success);
        std::cout << "[PASS] column-level UNIQUE allows multiple NULLs\n";
        cleanup();
    }

    // ── Table-level composite UNIQUE ────────────────────────────────
    {
        cleanup();
        Env env;
        auto r = run(env,
            "CREATE TABLE enrollments ("
            "  id         INT PRIMARY KEY AUTO_INCREMENT,"
            "  student_id INT,"
            "  course_id  INT,"
            "  UNIQUE (student_id, course_id)"
            ");");
        assert(r.success);

        auto idxs = env.cat.get_indexes_for_table("enrollments");
        assert(idxs.size() == 1);
        assert(idxs[0].is_unique);
        assert((idxs[0].column_names == std::vector<std::string>{"student_id", "course_id"}));
        std::cout << "[PASS] table-level UNIQUE (...) creates one composite unique index\n";

        auto i1 = run(env, "INSERT INTO enrollments (id, student_id, course_id) VALUES (1, 10, 100);");
        assert(i1.success);
        auto i2 = run(env, "INSERT INTO enrollments (id, student_id, course_id) VALUES (2, 10, 100);");
        assert(!i2.success);
        std::cout << "[PASS] composite UNIQUE rejects duplicate (student_id, course_id) pair: " << i2.error_message << "\n";

        auto i3 = run(env, "INSERT INTO enrollments (id, student_id, course_id) VALUES (3, 10, 200);");
        assert(i3.success);
        std::cout << "[PASS] composite UNIQUE allows same student with a different course\n";
        cleanup();
    }

    // ── UNIQUE on the primary key column is deduped (no extra index) ──
    {
        cleanup();
        Env env;
        auto r = run(env, "CREATE TABLE t (id INT PRIMARY KEY UNIQUE, val INT);");
        assert(r.success);
        auto idxs = env.cat.get_indexes_for_table("t");
        assert(idxs.empty());
        std::cout << "[PASS] UNIQUE on the primary key column doesn't create a redundant index\n";
        cleanup();
    }

    // ── UPDATE also enforces the constraint (generic Table/Index path) ─
    {
        cleanup();
        Env env;
        run(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, email VARCHAR(50) UNIQUE);");
        run(env, "INSERT INTO users (id, email) VALUES (1, 'a@x.com');");
        run(env, "INSERT INTO users (id, email) VALUES (2, 'b@x.com');");
        auto u = run(env, "UPDATE users SET email = 'a@x.com' WHERE id = 2;");
        assert(!u.success);
        std::cout << "[PASS] UPDATE is also blocked by the column-level UNIQUE constraint: " << u.error_message << "\n";
        cleanup();
    }

    // ── DROP TABLE also drops the table's indexes from the catalog ────
    {
        cleanup();
        Env env;
        run(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, email VARCHAR(50) UNIQUE);");
        assert(env.cat.get_indexes_for_table("users").size() == 1);

        auto d = run(env, "DROP TABLE users;");
        assert(d.success);
        assert(env.cat.get_indexes_for_table("users").empty());
        assert(!env.cat.table_exists("users"));
        std::cout << "[PASS] DROP TABLE removes every index belonging to the table from the catalog\n";

        // recreating a table with the same name must not inherit the old
        // (dropped) table's indexes
        run(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50));");
        assert(env.cat.get_indexes_for_table("users").empty());
        std::cout << "[PASS] a table recreated with the same name starts with no leftover indexes\n";
        cleanup();
    }

    std::cout << "\nAll UNIQUE constraint sanity checks passed.\n";
    return 0;
}
