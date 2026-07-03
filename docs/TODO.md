# TODO

## Immediate
- [ ] Write database.h + database.cpp (top-level orchestrator)
- [ ] Write cli/main.cpp (REPL)
- [ ] Write CMakeLists.txt

## Database tasks
- [ ] CREATE DATABASE — create `~/Documents/GS-DBEngine/<name>/` directory, init .db + .wal
- [ ] DROP DATABASE — delete directory and all contents
- [ ] USE — close current storage stack, open stack for named database
- [ ] SHOW DATABASES — list subdirectories in data_dir
- [ ] Guard: non-DB statement with no active database → "No database selected"
- [ ] execute(string) overload — parse then dispatch to execute(Statement)
- [ ] CREATE DATABASE does NOT auto-USE; explicit USE required
- [ ] Default data_dir: `~/Documents/GS-DBEngine/` — fixed constant, no runtime path change

## Stretch goals
- [ ] Secondary indexes (CREATE INDEX)
- [ ] LIKE operator implementation
- [ ] COUNT(*) aggregate
- [ ] Free page list (reclaim pages after merge/delete) i am thinking track first free page and last free page and write on free pages the next free page
- [ ] Multi-statement transactions (BEGIN / COMMIT / ROLLBACK)
- [ ] MVCC
- [ ] Review Executor
- [ ]Custom Path for Database storage

