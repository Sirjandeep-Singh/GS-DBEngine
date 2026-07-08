# TODO

## Stretch goals
- [ ] Secondary indexes (CREATE INDEX)
- [ ] COUNT(*) aggregate
- [ ] Multi-statement transactions (BEGIN / COMMIT / ROLLBACK)
- [ ] MVCC
- [ ] pkg-config (.pc) file — covers plain g++ consumers who don't use CMake
- [ ] Amalgamation build (single .cpp + .h, SQLite-style) — nice-to-have, not required at current scope
- [ ] vcpkg/Conan packaging — only worth it if demonstrating package-manager distribution specifically
- [ ] Quantitative comparison
- [ ] Review Free List Manager, btree, header manager
- [ ] inquire if database calls checkpoint after a while automatically to regulate log size.