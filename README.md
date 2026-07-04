# GS-DBEngine

A relational database engine, built from scratch in C++17 — no SQLite, no libpq, no existing storage engine underneath. Every layer, from raw page I/O up through a hand-written SQL parser and executor, is implemented in this repository.

---

## What does this do?

GS-DBEngine takes SQL strings and executes them against your own data, using an engine built entirely in-house across 13 architectural layers:

```
Database (top-level orchestrator)
    └── Executor (AST walker)
        └── Parser (tokenizer → AST → parser)
            └── Table Layer
                ├── RowSerializer
                └── BTree
                    └── BTreeNode
                        └── BufferPool
                            └── WALManager
                                └── HeaderManager / CatalogManager
                                    └── DiskManager
```

Concretely, it supports:

- **Standard SQL**: `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`, `CREATE DATABASE`, `DROP TABLE`, `DROP DATABASE`, `USE`, `SHOW DATABASES`
- **JOINs**: `INNER`, `LEFT`, `RIGHT` (nested-loop), with qualified/unqualified column resolution
- **WHERE filtering**: `AND` / `OR` / `NOT`, comparisons, `IS NULL` / `IS NOT NULL`, `LIKE` with `%` and `_`
- **ORDER BY** and **LIMIT**
- **Auto-increment primary keys**
- **ACID durability** via a redo-only, SQLite-style write-ahead log (WAL) with crash recovery
- **B+ tree indexing** with page splits, merges, and range scans
- **A real on-disk storage format** — fixed 4KB pages, a binary row format with a NULL bitmap, and a persistent catalog — not an in-memory toy

It ships two ways: as a **CLI tool** (`gsdb`) you can run interactively or pipe SQL scripts into, and as a **static library** (`gsdb_lib`) you can link into your own C++ programs.

## Why should I care?

**If you're evaluating it as a portfolio/capstone piece:** this isn't a wrapper around an existing database or a toy in-memory key-value store. It's a genuine bottom-up implementation of the same architecture real databases use — write-ahead logging for crash safety, a buffer pool with LRU eviction sitting between the query layer and disk, a B+ tree with real split/merge/redistribution logic, and a hand-written recursive-descent SQL parser. Every design decision (page size, WAL protocol, row serialization format, JOIN semantics) is documented in [`DECISIONS.md`](DECISIONS.md) — so nothing here is a black box.

**If you want to actually use it:** it's a small, dependency-free (no external libraries — just the C++ standard library and POSIX file APIs), single-writer embedded database. Good fit for small tools, learning how database internals work by reading real (not textbook-simplified) code, or as a base to extend — the codebase is intentionally modular, with each of the 13 layers only depending on the layer directly beneath it.

**Known limitations, upfront:** single writer (no concurrent transactions, no MVCC), no query optimizer, no secondary index execution yet (schema support exists, execution doesn't), and Linux/POSIX only (the WAL layer needs `fsync()` on a raw file descriptor). See [`STATUS.md`](STATUS.md) for the full current state and [`TODO.md`](TODO.md) for what's planned.

---

## How do I use it?

### As a CLI tool

Once installed (see below), `gsdb` drops you into an interactive REPL:

```bash
$ gsdb
GS-DBEngine v0.1
Type SQL statements terminated with ';'.
Type 'exit' or 'quit' to leave.

gsdb> CREATE DATABASE shop;
OK

gsdb> USE shop;
OK

gsdb> CREATE TABLE products (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50), price FLOAT);
OK

gsdb> INSERT INTO products (name, price) VALUES ('Keyboard', 49.99);
OK (1 row(s) affected)

gsdb> SELECT * FROM products WHERE price > 20;
+----+----------+-------+
| id | name     | price |
+----+----------+-------+
|  1 | Keyboard | 49.99 |
+----+----------+-------+
1 row(s)

gsdb> exit
Bye.
```

You can also pipe a `.sql` file straight in, no interactive prompt needed:

```bash
gsdb < setup.sql
```

Multi-line statements work too — the REPL waits for a trailing `;` before executing, so you can spread a long `CREATE TABLE` across several lines.

### As a library

Link `gsdb_lib` into your own C++ program and drive it entirely through `Database::execute()`:

```cpp
#include "database.h"
#include <iostream>

int main() {
    Database db;  // defaults to ~/Documents/GS-DBEngine/ as the data directory

    db.execute("CREATE DATABASE app;");
    db.execute("USE app;");
    db.execute("CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50));");
    db.execute("INSERT INTO users (name) VALUES ('Sirjandeep');");

    QueryResult result = db.execute("SELECT * FROM users;");
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << "\n";
        return 1;
    }

    for (const auto& row : result.rows) {
        for (const auto& cell : row) std::cout << cell << " ";
        std::cout << "\n";
    }
}
```

`QueryResult` gives you `success`, `error_message`, `columns`, `rows` (as `vector<vector<string>>`), and `rows_affected` — everything the CLI's own table-formatter uses, so you have the same information available programmatically.

---

## How do I install it?

### Prerequisites

- **Linux** (or WSL on Windows) — the WAL layer uses POSIX `fsync()` on a raw file descriptor, so this won't build on native Windows
- **CMake ≥ 3.16**
- **A C++17 compiler** (g++ or clang++)

Install the toolchain on Debian/Ubuntu (including WSL) if you don't have it already:

```bash
sudo apt update
sudo apt install cmake build-essential
```

### Build

```bash
git clone https://github.com/<your-username>/GS-DBEngine.git
cd GS-DBEngine
cmake -B build
cmake --build build -j
```

This builds the `gsdb` CLI, the `gsdb_lib` static library, and every layer's test executable (`test_storage`, `test_btree`, `test_parser`, etc.).

Run the test suite:

```bash
cd build && ctest --output-on-failure
```

### Install — CLI

```bash
sudo cmake --install build
```

This puts `gsdb` on `/usr/local/bin`. If that's on your `PATH` (it is by default on most Linux distros), you can now run it from anywhere:

```bash
gsdb
```

### Install — as a library, for use in other CMake projects

The same `sudo cmake --install build` also installs the static library, headers, and a CMake package config, so any other CMake-based project can pull in GS-DBEngine with **no manual `-I`/`-L`/`-l` flags**. In the consumer project's own `CMakeLists.txt`:

```cmake
find_package(GSDBEngine REQUIRED)
target_link_libraries(myapp PRIVATE GSDBEngine::gsdb_lib)
```

And in their source: `#include "database.h"`, exactly as shown in the usage example above.

### Install — as a library, for non-CMake (plain g++) projects

If you're not using CMake in your own project, link manually against the installed headers and library:

```bash
g++ myapp.cpp -I/usr/local/include/gsdb -L/usr/local/lib -lgsdb_lib -o myapp
```

---

## Project layout

```
GS-DBEngine/
├── src/            — all 13 engine layers (storage, wal, catalog, btree, row, table, parser, executor, database)
├── cli/            — the gsdb REPL (main.cpp)
├── tests/          — one test file per layer
├── cmake/          — CMake package config template (for find_package support)
├── CMakeLists.txt
├── DECISIONS.md    — design decisions and rationale
├── STATUS.md       — current build/feature status
├── TODO.md         — planned work
└── Architecture.md — the original 13-layer build plan
```

See [`DECISIONS.md`](DECISIONS.md) for the full reasoning behind every architectural choice — storage format, WAL protocol, JOIN semantics, and the build/packaging setup.