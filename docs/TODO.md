# TODO

## Stretch goals
- [ ] pkg-config (.pc) file — covers plain g++ consumers who don't use CMake
- [ ] Amalgamation build (single .cpp + .h, SQLite-style) — nice-to-have, not required at current scope
- [ ] vcpkg/Conan packaging — only worth it if demonstrating package-manager distribution specifically
- [ ] Quantitative comparison
- [ ] Review ALL
- [ ] inquire if database calls checkpoint after a while automatically to regulate log size.
- [ ] GROUP BY, HAVING, AGGREGATE FUNCTION
        1. Type of MEAN/MEDIAN results — your Value variant has no double. I'd return float, consistent with existing FLOAT handling, but truncation/precision is worth a comment in the code.
        2. JOIN + GROUP BY — I'd explicitly restrict Step 1–3 to non-JOIN selects at first (mirroring how aggregates already reject JOINs today) and treat JOIN+GROUP BY as a future step, same pattern your roadmap already uses for scoping.
        3. ORDER BY on aggregate/group results — your current sort_by_column sorts by index into the table's Row via schema. Grouped/aggregate output rows don't map 1:1 to schema columns anymore, so ORDER BY after GROUP BY needs to sort on the projected result, not the raw row — a small but real deviation from the existing sort helper.
        4. NULL semantics in grouping — decide once, document it, move on (standard SQL: NULLs form their own group).
- [ ] Multi-statement transactions (BEGIN / COMMIT / ROLLBACK) - not important YET
- [ ] Query Planner
- [ ] MVCC
- [ ] add a timer for queries.
