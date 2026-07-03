# TODO

## Immediate
- [ ] Write executor.h + executor.cpp
- [ ] Write test_executor.cpp
- [ ] Write database.h + database.cpp (top-level orchestrator)
- [ ] Write cli/main.cpp (REPL)
- [ ] Write CMakeLists.txt

## Executor tasks
- [ ] SELECT — scan/filter, JOIN (nested loop), ORDER BY, LIMIT
- [ ] INSERT — parse values, map to schema columns, call Table::insert
- [ ] UPDATE — find rows via predicate, call Table::update_where
- [ ] DELETE — find rows via predicate, call Table::delete_where
- [ ] CREATE TABLE — validate schema, call CatalogManager::create_table + allocate B+ tree root
- [ ] DROP TABLE — call CatalogManager::drop_table
- [ ] CREATE DATABASE / DROP DATABASE / USE / SHOW

## Stretch goals
- [ ] Secondary indexes (CREATE INDEX)
- [ ] LIKE operator implementation
- [ ] COUNT(*) aggregate
- [ ] Free page list (reclaim pages after merge/delete)
- [ ] Multi-statement transactions (BEGIN / COMMIT / ROLLBACK)
- [ ] MVCC
