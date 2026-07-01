1. DiskManager
   - raw file I/O
   - read_page / write_page by page_id
   - allocate_page (extend file)
   Test: write a page, read it back, verify bytes match

2. HeaderManager
   - owns page 1
   - encode/decode DBHeader struct
   - magic string validation on open
   Test: create new db, reopen it, verify header fields intact

3. Buffer Pool
   - in-memory page cache
   - hashmap: page_id → slot
   - LRU eviction
   - dirty page tracking
   Test: load pages, evict LRU, verify dirty pages flush to disk

4. WAL
   - write changes to .wal before applying to .db
   - crash recovery on startup (replay WAL)
   - Working: Evert Write is written to .wal file but only transactions with commit block are redone.
   Test: simulate crash mid-transaction, restart, verify data integrity

5. Schema / Catalog
   - Table and Column structs
   - stores table definitions, column types, index locations
   - backed by a system table in the .db file
   - loaded fully into memory on startup
   Test: create table, reopen db, verify schema survives restart

6. Row Serializer
   - serialize row struct → raw bytes
   - deserialize raw bytes → row struct
   - handles INT, VARCHAR, FLOAT, BOOLEAN, NULL
   Test: serialize a row, deserialize it, verify values match

7. B+ Tree
   - internal nodes: keys + child page numbers
   - leaf nodes: keys + serialized row data
   - leaf linked list for range scans
   - page splits when a node is full
   - pages merge when underflow occurs
   - currently allows sparse nodes. Underflow value may be hard-coded too low.
   Test: insert 1000 rows, search each one, verify correctness

8. Table Layer
   - INSERT / SELECT / UPDATE / DELETE
   - calls B+ tree for lookups
   - calls Row Serializer for encoding/decoding
   - handles auto increment
   Test: insert rows, select them back, update, delete

9. SQL Parser
   - tokenizer (SQL string → tokens)
   - recursive descent parser (tokens → AST)
   - supports SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, CREATE DATABASE, USE, SHOW DATABASES
   Test: parse SQL strings, print AST, verify structure

10. Executor
    - walks the AST
    - calls Table Layer
    - handles WHERE filtering
    - handles JOIN (nested loop)
    - handles ORDER BY, LIMIT
    Test: end to end — run SQL string, get correct rows back

11. Database (top level)
    - owns and initializes all layers
    - bootstraps on startup (load header, replay WAL, load schema)
    - entry point: execute(sql_string)
    Test: full integration — create db, create table, insert, select, restart, verify

12. Shipping
    - As a CLI Wrapped Around it
    - As an External Library

    NOTES:
    - Header Manager must create header before catalog Manager - 3