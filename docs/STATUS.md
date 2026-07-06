# Project Status

## Layers Complete

| Layer | Files | Tests | Status |
|-------|-------|-------|--------|
| DiskManager | storage/disk_manager.h/cpp | test_storage.cpp | ✅ |
| HeaderManager | storage/header_manager.h/cpp | test_storage.cpp | ✅ |
| BufferPool | storage/buffer_pool.h/cpp | test_storage.cpp | ✅ |
| WALManager | wal/wal_manager.h/cpp | test_wal.cpp | ✅ |
| Schema | catalog/schema.h | test_catalog.cpp | ✅ |
| CatalogManager | catalog/catalog_manager.h/cpp | test_catalog.cpp | ✅ |
| BTreeNode | btree/btree_node.h/cpp | test_btree_node.cpp | ✅ |
| BTree | btree/btree.h/cpp | test_btree.cpp | ✅ |
| Row | row/row.h | test_row.cpp | ✅ |
| RowSerializer | row/serializer.h/cpp | test_row.cpp | ✅ |
| Table | table/table.h/cpp | test_table.cpp | ✅ |
| Tokenizer | parser/tokenizer.h/cpp | test_tokenizer.cpp | ✅ |
| Parser + AST | parser/parser.h/cpp + ast.h | test_parser.cpp | ✅ |
| Executor | executor/executor.h/cpp | test_executor.cpp | ✅ |
| Database | database.h/cpp | test_database.cpp | ✅ |
| CLI | cli/main.cpp | manual REPL testing | ✅ |
| Build system | CMakeLists.txt | `cmake -B build && cmake --build build` | ✅ |

## Packaging / Distribution

| Item | Mechanism | Status |
|------|-----------|--------|
| CLI install | `install(TARGETS gsdb RUNTIME DESTINATION bin)` → `sudo cmake --install build` puts `gsdb` on PATH | ✅ |
| Library + headers install | `install(TARGETS gsdb_lib ...)` + `install(DIRECTORY src/ ...)` → `/usr/local/lib`, `/usr/local/include/gsdb` | ✅ |
| CMake package config | `GSDBEngineConfig.cmake` + `GSDBEngineTargets.cmake` → consumers use `find_package(GSDBEngine)` | ✅ |
| End-to-end consumer validation | Standalone external test project (`find_package` + `target_link_libraries`, zero manual `-I`/`-L`/`-l`) — built, linked, and ran real SQL successfully | ✅ |
| pkg-config (.pc) support | For plain-g++ consumers without CMake | ⬜ Not started (stretch) |
| README.md | End-user build/install/usage instructions | ⬜ Not started |

## Known Issues / Gaps
- No free page list — orphaned pages after B+ tree merges are not reclaimed
- No secondary index execution (schema supports it, execution not yet wired up)
