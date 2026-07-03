# Design Decisions

## Storage
- **Page size**: 4KB fixed
- **File model**: 2 files per database — `.db` + `.wal`
- **Page 0**: DB header (magic string, total pages, schema root page)
- **Page 1**: Catalog (serialized table/index schemas)
- **Concurrency**: Single writer, no MVCC

## B+ Tree
- **Key type**: uint32_t (auto-increment INT primary key)
- **Values**: opaque byte blobs (serialized rows)
- **Leaf storage**: variable-size cells, written back-to-front in page
- **Split**: pre-insertion overflow check (not post)
- **Delete**: full merge-on-underflow (not lazy)
- **Orphaned pages**: not reclaimed (no free list yet)

## WAL
- **Type**: redo-only, SQLite-style
- **Write**: POSIX write() to .wal immediately (no fsync)
- **Commit**: COMMIT frame + fsync(fd) = durability guarantee
- **Abort**: no-op — incomplete records ignored on recovery
- **Recovery**: filter by COMMIT frame, replay only committed txns, then checkpoint
- **Checkpoint**: flush buffer pool → .db, truncate .wal

## Row format
- **Null bitmap**: ceil(n/8) bytes, LSB first, 1=NULL
- **INT**: 4 bytes little-endian signed
- **FLOAT**: 4 bytes IEEE 754
- **BOOLEAN**: 1 byte
- **VARCHAR**: 2-byte length prefix + n bytes
- **NULL columns**: not stored (smaller blob)

## UPDATE strategy
- Delete + reinsert (handles variable-size row growth)

## Parser
- Hand-written recursive descent
- Keywords case-insensitive, identifiers case-sensitive
- AND binds tighter than OR
- PRIMARY KEY implies NOT NULL
- Only honors one key for order by

## Executor
- `QueryResult::rows` is `vector<vector<string>>` — executor converts Row values to strings; CLI is a dumb printer
- JOIN output: merged row, left schema columns first, right schema columns appended
- JOIN column resolution: two-schema overload — qualified refs validated per table, unqualified refs throw on ambiguity if column exists in both tables
- RIGHT JOIN: swap outer/inner sides, run unified LEFT JOIN loop body
- NULLs always sort last in ORDER BY
