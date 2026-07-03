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

## Layers Remaining

| Layer | Files | Status |
|-------|-------|--------|
| Executor | executor/executor.h/cpp | ⬜ Not started |
| Database | database.h/cpp | ⬜ Not started |
| CLI | cli/main.cpp | ⬜ Not started |
| Build system | CMakeLists.txt | ⬜ Not started |

## Known Issues / Gaps
- No free page list — orphaned pages after B+ tree merges are not reclaimed
- No secondary index execution (schema supports it, executor not written yet)
