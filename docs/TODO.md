# TODO

## Stretch goals
- [ ] Secondary indexes (CREATE INDEX)
- [ ] COUNT(*) aggregate
- [ ] Free page list (reclaim pages after merge/delete) — track first/last free page; write next-free-page pointer into orphaned pages
- [ ] Multi-statement transactions (BEGIN / COMMIT / ROLLBACK)
- [ ] MVCC
- [ ] pkg-config (.pc) file — covers plain g++ consumers who don't use CMake
- [ ] Amalgamation build (single .cpp + .h, SQLite-style) — nice-to-have, not required at current scope
- [ ] vcpkg/Conan packaging — only worth it if demonstrating package-manager distribution specifically