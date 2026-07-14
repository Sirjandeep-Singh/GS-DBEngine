// Manual sanity check for FOREIGN KEY constraint support (not wired into
// the CMake test suite — throwaway script, same pattern as
// test_unique_constraint_manual.cpp).
//
// g++ -std=c++17 tests/test_foreign_key_manual.cpp src/executor/executor.cpp src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/btree/free_list_manager.cpp src/btree/key.cpp src/index/index.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/header/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp -o /tmp/test_fk && /tmp/test_fk

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

static const std::string DB_FILE  = "test_fk.db";
static const std::string WAL_FILE = "test_fk.wal";

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
    // ── Column-level REFERENCES shorthand, basic RESTRICT behavior ────
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE customers (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50));").success);
        auto r = run(env,
            "CREATE TABLE orders ("
            "  id          INT PRIMARY KEY AUTO_INCREMENT,"
            "  customer_id INT REFERENCES customers(id),"
            "  amount      INT"
            ");");
        assert(r.success);

        // auto-created non-unique child-side index should exist
        auto idxs = env.cat.get_indexes_for_table("orders");
        assert(idxs.size() == 1);
        assert(!idxs[0].is_unique);
        assert(idxs[0].column_names == std::vector<std::string>{"customer_id"});
        std::cout << "[PASS] column-level REFERENCES creates one non-unique child-side index\n";

        // insert with no matching parent -> rejected
        auto i1 = run(env, "INSERT INTO orders (id, customer_id, amount) VALUES (1, 999, 50);");
        assert(!i1.success);
        std::cout << "[PASS] INSERT with no matching parent row is rejected: " << i1.error_message << "\n";

        // insert with NULL FK -> allowed (MATCH SIMPLE)
        auto i2 = run(env, "INSERT INTO orders (id, customer_id, amount) VALUES (2, NULL, 10);");
        assert(i2.success);
        std::cout << "[PASS] INSERT with NULL FK column is allowed\n";

        // insert parent, then matching child -> allowed
        assert(run(env, "INSERT INTO customers (id, name) VALUES (1, 'Alice');").success);
        auto i3 = run(env, "INSERT INTO orders (id, customer_id, amount) VALUES (3, 1, 75);");
        assert(i3.success);
        std::cout << "[PASS] INSERT with a matching parent row is allowed\n";

        // DELETE parent with a referencing child -> rejected (default RESTRICT)
        auto d1 = run(env, "DELETE FROM customers WHERE id = 1;");
        assert(!d1.success);
        std::cout << "[PASS] DELETE on a referenced parent row is rejected by default (RESTRICT): "
                   << d1.error_message << "\n";

        // remove the child, then the parent delete succeeds
        assert(run(env, "DELETE FROM orders WHERE id = 3;").success);
        auto d2 = run(env, "DELETE FROM customers WHERE id = 1;");
        assert(d2.success);
        std::cout << "[PASS] DELETE on the parent succeeds once no child references it\n";

        // UPDATE parent's referenced column while still referenced -> rejected
        assert(run(env, "INSERT INTO customers (id, name) VALUES (2, 'Bob');").success);
        assert(run(env, "INSERT INTO orders (id, customer_id, amount) VALUES (4, 2, 20);").success);
        auto u1 = run(env, "UPDATE customers SET id = 20 WHERE id = 2;");
        assert(!u1.success);
        std::cout << "[PASS] UPDATE changing a referenced parent key is rejected: " << u1.error_message << "\n";

        cleanup();
    }

    // ── Table-level FOREIGN KEY (...) REFERENCES ... ON DELETE CASCADE ─
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE authors (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50));").success);
        auto r = run(env,
            "CREATE TABLE books ("
            "  id        INT PRIMARY KEY AUTO_INCREMENT,"
            "  author_id INT,"
            "  title     VARCHAR(50),"
            "  FOREIGN KEY (author_id) REFERENCES authors (id) ON DELETE CASCADE"
            ");");
        assert(r.success);

        assert(run(env, "INSERT INTO authors (id, name) VALUES (1, 'Tolkien');").success);
        assert(run(env, "INSERT INTO books (id, author_id, title) VALUES (1, 1, 'The Hobbit');").success);
        assert(run(env, "INSERT INTO books (id, author_id, title) VALUES (2, 1, 'LOTR');").success);

        auto d = run(env, "DELETE FROM authors WHERE id = 1;");
        assert(d.success);
        std::cout << "[PASS] ON DELETE CASCADE deletes the parent row\n";

        auto sel = run(env, "SELECT * FROM books;");
        assert(sel.success);
        assert(sel.rows.empty());
        std::cout << "[PASS] ON DELETE CASCADE also removed every referencing child row\n";

        cleanup();
    }

    // ── FK referencing a UNIQUE (non-PK) column ────────────────────────
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, email VARCHAR(50) UNIQUE);").success);
        auto r = run(env,
            "CREATE TABLE profiles ("
            "  id           INT PRIMARY KEY AUTO_INCREMENT,"
            "  user_email   VARCHAR(50),"
            "  bio          VARCHAR(50),"
            "  FOREIGN KEY (user_email) REFERENCES users (email)"
            ");");
        assert(r.success);

        auto bad = run(env, "INSERT INTO profiles (id, user_email, bio) VALUES (1, 'nobody@x.com', 'hi');");
        assert(!bad.success);
        std::cout << "[PASS] FK referencing a UNIQUE column rejects a non-existent value: " << bad.error_message << "\n";

        assert(run(env, "INSERT INTO users (id, email) VALUES (1, 'a@x.com');").success);
        auto good = run(env, "INSERT INTO profiles (id, user_email, bio) VALUES (1, 'a@x.com', 'hi');");
        assert(good.success);
        std::cout << "[PASS] FK referencing a UNIQUE column accepts an existing value\n";

        cleanup();
    }

    // ── CREATE TABLE validation errors ──────────────────────────────────
    {
        cleanup();
        Env env;
        auto no_parent = run(env,
            "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT REFERENCES customers(id));");
        assert(!no_parent.success);
        std::cout << "[PASS] FOREIGN KEY referencing a nonexistent table is rejected at CREATE TABLE: "
                   << no_parent.error_message << "\n";

        assert(run(env, "CREATE TABLE customers (id INT PRIMARY KEY, age INT, name VARCHAR(50));").success);
        auto bad_ref_col = run(env,
            "CREATE TABLE orders (id INT PRIMARY KEY, customer_age INT REFERENCES customers(age));");
        assert(!bad_ref_col.success);
        std::cout << "[PASS] REFERENCES a non-key/non-unique column is rejected at CREATE TABLE: "
                   << bad_ref_col.error_message << "\n";

        auto self_ref = run(env,
            "CREATE TABLE nodes (id INT PRIMARY KEY, parent_id INT REFERENCES nodes(id));");
        assert(!self_ref.success);
        std::cout << "[PASS] self-referencing FOREIGN KEY is rejected with a clear message: "
                   << self_ref.error_message << "\n";

        cleanup();
    }

    // ── DROP TABLE is blocked while referenced ──────────────────────────
    {
        cleanup();
        Env env;
        assert(run(env, "CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(50));").success);
        assert(run(env, "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT REFERENCES customers(id));").success);

        auto drop = run(env, "DROP TABLE customers;");
        assert(!drop.success);
        std::cout << "[PASS] DROP TABLE on a still-referenced table is rejected: " << drop.error_message << "\n";

        assert(run(env, "DROP TABLE orders;").success);
        auto drop2 = run(env, "DROP TABLE customers;");
        assert(drop2.success);
        std::cout << "[PASS] DROP TABLE succeeds once the referencing table is gone\n";

        cleanup();
    }

    // ── FK metadata survives a catalog reload (persistence round-trip) ─
    {
        cleanup();
        {
            Env env;
            assert(run(env, "CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(50));").success);
            assert(run(env,
                "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT REFERENCES customers(id));"
            ).success);
            assert(run(env, "INSERT INTO customers (id, name) VALUES (1, 'Alice');").success);
            assert(run(env, "INSERT INTO orders (id, customer_id) VALUES (1, 1);").success);
        }
        // Fresh Env re-opens the same DB/WAL files and reloads the catalog
        // from disk — this is what actually exercises serialize()/
        // deserialize() for foreign_keys, not just the in-memory path the
        // tests above already cover.
        {
            Env env;
            auto bad = run(env, "INSERT INTO orders (id, customer_id) VALUES (2, 999);");
            assert(!bad.success);
            std::cout << "[PASS] FOREIGN KEY constraint still enforced after catalog reload: "
                       << bad.error_message << "\n";

            auto blocked_drop = run(env, "DROP TABLE customers;");
            assert(!blocked_drop.success);
            std::cout << "[PASS] DROP TABLE restriction still enforced after catalog reload\n";
        }
        cleanup();
    }

    std::cout << "\nAll FOREIGN KEY sanity checks passed.\n";
    return 0;
}
