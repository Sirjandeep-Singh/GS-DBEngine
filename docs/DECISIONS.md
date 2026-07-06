# Design Decisions

## Storage
- **Page size**: 4KB fixed
- **File model**: 2 files per database — `.db` + `.wal`
- **Page 0**: DB header (magic string, total pages, schema root page)
- **Page 1**: Catalog (serialized table/index schemas)
- **Concurrency**: Single writer, no MVCC

## B+ Tree
- **Key type**: uint32_t (auto-increment INT primary key)
- **Values**: opaque byte blobs (serialized rows)
- **Leaf storage**: variable-size cells, written back-to-front in page
- **Split**: pre-insertion overflow check (not post)
- **Delete**: full merge-on-underflow (not lazy)
- **Orphaned pages**: not reclaimed (no free list yet)

## WAL
- **Type**: redo-only, SQLite-style
- **Write**: POSIX write() to .wal immediately (no fsync)
- **Commit**: COMMIT frame + fsync(fd) = durability guarantee
- **Abort**: no-op — incomplete records ignored on recovery
- **Recovery**: filter by COMMIT frame, replay only committed txns, then checkpoint
- **Checkpoint**: flush buffer pool → .db, truncate .wal

## Row format
- **Null bitmap**: ceil(n/8) bytes, LSB first, 1=NULL
- **INT**: 4 bytes little-endian signed
- **FLOAT**: 4 bytes IEEE 754
- **BOOLEAN**: 1 byte
- **VARCHAR**: 2-byte length prefix + n bytes
- **NULL columns**: not stored (smaller blob)

## UPDATE strategy
- Delete + reinsert (handles variable-size row growth)

## Parser
- Hand-written recursive descent
- Keywords case-insensitive, identifiers case-sensitive
- AND binds tighter than OR
- PRIMARY KEY implies NOT NULL
- Only honors one key for order by

## Executor
- `QueryResult::rows` is `vector<vector<string>>` — executor converts Row values to strings; CLI is a dumb printer
- JOIN output: merged row, left schema columns first, right schema columns appended
- JOIN column resolution: two-schema overload — qualified refs validated per table, unqualified refs throw on ambiguity if column exists in both tables
- RIGHT JOIN: swap outer/inner sides, run unified LEFT JOIN loop body
- NULLs always sort last in ORDER BY

## Database
- Default data_dir: `~/Documents/GS-DBEngine/` — fixed constant, resolved via `default_data_dir()`
- Custom data_dir is supported at runtime (see "Data Directory / \path command" below) — no longer a stretch goal
- `CREATE DATABASE` does NOT auto-USE; explicit `USE` required
- Non-DB statement with no active database → `"No database selected"`
- `DROP DATABASE` on active database deselects it (`current_db_name_` cleared) before directory removal
- `USE` on already-active database is a no-op (no teardown/rebuild)
- `~Database()` calls `close_database()` — ensures buffer pool flush and clean WAL state on normal shutdown
- `SHOW DATABASES` output sorted alphabetically (`directory_iterator` order is unspecified)
- File layout: `data_dir/<name>/<name>.db` + `data_dir/<name>/<name>.wal`

## Data Directory / \path command
- **CLI surface**: exposed as a REPL meta-command (`\path`, `\path <dir>`, `\path default`), not a SQL statement — changing where databases live isn't a SQL operation (no table touched, nothing SELECT-able), so it's handled in `cli/main.cpp` before SQL tokenizing/buffering even starts, at the same tier as the existing `is_quit()` check
- **`set_data_dir(new_dir)`**: closes any active database first (same flush/checkpoint behavior as normal shutdown — the old storage stack is meaningless once `data_dir` changes), creates `new_dir` if missing, then persists it
- **`reset_data_dir()`**: closes active DB, resets to `default_data_dir()`, and **deletes** the persistence file (rather than writing the default path into it) — so if `default_data_dir()`'s own resolution logic ever changes (e.g. different `$HOME`), a fresh session picks that up instead of reading a stale literal path
- **Persistence file**: `~/Documents/GSDB-config.txt` — a fixed, single-line text file containing the current data_dir path. Deliberately **not** stored inside `data_dir` itself (chicken-and-egg problem: the file needs a stable location independent of wherever `data_dir` currently points)
- **Resolution on startup**: `resolve_data_dir()` reads the persistence file if present/non-empty, else falls back to `default_data_dir()`. This is now the `Database` constructor's default argument (replacing the old hardcoded `default_data_dir()` default), so `Database db;` transparently picks up a prior `\path` change with no changes needed in `main.cpp`
- **Tests / explicit overrides unaffected**: any `Database` constructed with an explicit `data_dir` argument (e.g. tests using a temp directory) bypasses persistence entirely — only the no-argument default path reads the config file
- **Scope**: session-persistent via the config file, but still single global override — no per-invocation flags (e.g. `gsdb --data-dir=...`) and no multiple named profiles; that would be a further stretch goal if needed later

## Build & Packaging
- **Build system**: CMake (>= 3.16), C++17, Linux/POSIX only — enforced with a `NOT UNIX` fatal-error guard (WAL layer needs `fsync()` on a raw fd)
- **Library target**: single static lib `gsdb_lib` containing every layer except `cli/` and `tests/` — simpler than per-test source lists; every test and the CLI link against the same target
- **Include paths**: `target_include_directories` uses `$<BUILD_INTERFACE:...>` / `$<INSTALL_INTERFACE:...>` generator expressions — consumers get the *installed* header path (`include/gsdb`) baked into the exported config, not this machine's local `src/` path
- **CLI distribution**: `install(TARGETS gsdb RUNTIME DESTINATION bin)` — `sudo cmake --install build` puts `gsdb` on `/usr/local/bin`, runnable from anywhere once on PATH
- **Library distribution**: static lib (`.a`) + full header tree installed to `/usr/local/lib` / `/usr/local/include/gsdb` — chosen over the SQLite-style single-file amalgamation, since amalgamation solves a distribution problem (embedding into arbitrary/incompatible build systems) this project doesn't currently have; the project's own multi-file layer structure is preserved 1:1 in the installed headers
- **CMake package config**: `install(EXPORT ...)` + `configure_package_config_file()` generate `GSDBEngineConfig.cmake` / `GSDBEngineTargets.cmake`, enabling `find_package(GSDBEngine)` + `target_link_libraries(... GSDBEngine::gsdb_lib)` for any CMake-based consumer — no manual `-I`/`-L`/`-l` flags needed
  - **Why**: without this, consuming the library means hand-typing include/lib paths on every build (`-I/usr/local/include/gsdb -L/usr/local/lib -lgsdb_lib`), which breaks the moment paths differ across machines and gives no compile-requirement propagation (e.g. C++17) or versioning. `find_package` is the CMake-ecosystem equivalent of `apt install` — it's a lookup mechanism, not a hosted registry: `cmake --install` drops the generated config into a directory (`/usr/local/lib/cmake/GSDBEngine/`) that CMake searches by default, so any later `find_package(GSDBEngine)` call on that same machine resolves automatically
  - Consumers who don't use CMake at all still fall back to manual linking (see below) — this only removes the manual step for CMake-based consumers specifically
  - This is scoped intentionally smaller than a real package registry (vcpkg/Conan) — those require publishing a recipe/manifest to a registry so `vcpkg install gsdb-engine` works with no local build step at all; genuinely useful but out of scope until distribution needs grow beyond "someone clones and builds this repo themselves"
- **Versioning**: `write_basic_package_version_file()` with `SameMajorVersion` compatibility, current version `0.1.0`
- **Non-CMake consumers**: fall back to manual linking (`-I/usr/local/include/gsdb -L/usr/local/lib -lgsdb_lib`) — a `pkg-config` `.pc` file is a stretch goal to remove this manual step too
- **Validation**: a standalone external project (outside this repo, containing only its own `CMakeLists.txt` + `main.cpp`) was built against the installed package to confirm `find_package` resolves headers and linking correctly with zero manual flags — confirmed working