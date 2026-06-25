#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "../storage/page.h"
#include "../storage/buffer_pool.h"

enum class WalRecordType : uint8_t {
    PAGE_WRITE = 1,
    COMMIT     = 2,
};

#pragma pack(push, 1)
struct WalRecordHeader {
    WalRecordType type;
    uint32_t      transaction_id;
    uint32_t      page_id;
};
#pragma pack(pop)

// WALManager implements a redo-only write-ahead log, modeled after SQLite WAL.
// Single writer only — no concurrency support.
//
// Write path:
//   begin()    → get a transaction_id
//   write()    → POSIX write() PAGE_WRITE frame to .wal immediately (no fsync)
//              → also store page in active_pages_ for commit() to apply
//   commit()   → POSIX write() COMMIT frame → fsync(fd) → everything lands on disk
//              → apply active_pages_ to buffer pool (mark dirty)
//              → clear active_pages_
//   abort()    → clear active_pages_, true no-op on disk
//              → incomplete frames sit in WAL harmlessly, ignored by recovery
//
// Recovery:
//   .wal file can contain incomplete transactions (crashed before commit)
//   recover() filters — only replays transactions with a COMMIT frame
//
// Checkpoint:
//   flush_all() writes dirty buffer pool pages to .db, then truncates .wal

class WALManager {
public:
    WALManager(const std::string& wal_filename, BufferPool& buffer_pool);
    ~WALManager();

    uint32_t begin();

    // POSIX write() PAGE_WRITE frame to .wal immediately + store in active_pages_
    // no fsync — incomplete frames on disk are ignored by recovery
    void write(uint32_t transaction_id, uint32_t page_id, const Page& page);

    // POSIX write() COMMIT frame → fsync(fd) → apply active_pages_ to buffer pool
    void commit(uint32_t transaction_id);

    // clears active_pages_ — incomplete frames in .wal ignored by recovery
    void abort(uint32_t transaction_id);

    // scans .wal, replays only transactions with a COMMIT frame into buffer pool
    void recover();

    // flush_all() writes dirty buffer pool pages to .db, then truncates .wal
    void checkpoint();

private:
    std::string  wal_filename_;
    int          wal_fd_;
    BufferPool&  buffer_pool_;
    uint32_t     next_transaction_id_;

    // pages written in the current active transaction: page_id -> page after-image
    std::unordered_map<uint32_t, Page> active_pages_;

    void write_bytes(const void* data, size_t size);
};