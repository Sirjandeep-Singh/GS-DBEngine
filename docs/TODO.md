# TODO

## Immediate
- [x] Write executor.h + executor.cpp
- [x] Write test_executor.cpp
- [x] Write database.h + database.cpp (top-level orchestrator)
- [x] Write test_database.cpp
- [x] Write cli/main.cpp (REPL)
- [x] Write CMakeLists.txt
- [x] Add install() rules — CLI binary (`sudo cmake --install build` → `gsdb` on PATH)
- [x] Add install() rules — static lib + headers (`/usr/local/lib`, `/usr/local/include/gsdb`)
- [x] Add CMake package config (GSDBEngineConfig.cmake) — enables `find_package(GSDBEngine)`
- [x] Validate `find_package` + `target_link_libraries` end-to-end via standalone consumer test project (zero manual -I/-L/-l flags, confirmed working)
- [ ] Write README.md (build/install instructions for CLI + library use, prerequisites, quick-start snippet)

## Stretch goals
- [ ] Secondary indexes (CREATE INDEX)
- [ ] COUNT(*) aggregate
- [ ] Free page list (reclaim pages after merge/delete) — track first/last free page; write next-free-page pointer into orphaned pages
- [ ] Multi-statement transactions (BEGIN / COMMIT / ROLLBACK)
- [ ] MVCC
- [ ] Review Executor
- [ ] Custom Path for Database storage
- [ ] pkg-config (.pc) file — covers plain g++ consumers who don't use CMake
- [ ] Amalgamation build (single .cpp + .h, SQLite-style) — nice-to-have, not required at current scope
- [ ] vcpkg/Conan packaging — only worth it if demonstrating package-manager distribution specifically