# TODO

## Immediate
- [x] Write executor.h + executor.cpp
- [x] Write test_executor.cpp
- [x] Write database.h + database.cpp (top-level orchestrator)
- [x] Write test_database.cpp
- [ ] Write cli/main.cpp (REPL)
- [ ] Write CMakeLists.txt

## Stretch goals
- [ ] Secondary indexes (CREATE INDEX)
- [ ] COUNT(*) aggregate
- [ ] Free page list (reclaim pages after merge/delete) — track first/last free page; write next-free-page pointer into orphaned pages
- [ ] Multi-statement transactions (BEGIN / COMMIT / ROLLBACK)
- [ ] MVCC
- [ ] Review Executor
- [ ] Custom Path for Database storage
