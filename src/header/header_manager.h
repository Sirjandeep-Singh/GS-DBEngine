#pragma once
#include <cstdint>
#include <cstring>
#include "../storage/page.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"

// DBHeader
// The raw struct that is serialized into page 0.
// Every field uses fixed-width types to avoid padding/alignment issues
// when writing directly to disk with memcpy.
//
// Layout in page 0 (byte offsets):
//   0  - 15  : magic string ("MYDB_V2\0")
//   16 - 19  : page_size
//   20 - 23  : total_pages
//   24 - 27  : schema_root_page
//   28 - 31  : first_free_page  (0 = no free pages)
//   32 - 35  : version
//   36 - 39  : reserved (padding for future use)
// Total: 40 bytes — fits easily inside a 4096 byte page.
static const char     DB_MAGIC[16]        = "MYDB_V2";
static const uint32_t DB_VERSION          = 2;
static const uint32_t NO_FREE_PAGE        = 0xFFFFFFFF;  // sentinel: free list is empty

#pragma pack(push, 1)   // disable struct padding — we need exact byte layout on disk
struct DBHeader {
    char     magic[16];           // must match DB_MAGIC on open
    uint32_t page_size;           // must match PAGE_SIZE on open
    uint32_t total_pages;         // how many pages exist in the file
    uint32_t schema_root_page;    // page_id of the schema table B+ tree root
    uint32_t first_free_page;     // page_id of first free page (NO_FREE_PAGE if none)
    uint32_t version;             // file format version
    uint32_t reserved;            // unused, reserved for future fields
};
#pragma pack(pop)

// Sanity check — header must fit inside a single page.
static_assert(sizeof(DBHeader) <= PAGE_SIZE,
    "DBHeader is too large to fit in a single page");

// HEADER_PAGE_ID is already defined in page.h — reused here, not redeclared.

// HeaderManager
// Owns the *interpretation* of page 0 — it does not own a separate,
// independently-durable copy of it. Responsible for:
//   - Bootstrapping page 0 when a new database is created (init)
//   - Reading and validating the header when an existing database is
//     opened (load)
//   - Writing the header back to page 0 whenever fields change, with a
//     durability guarantee (save)
//
// CHANGED FROM THE DISKMANAGER-DIRECT VERSION:
// HeaderManager now goes through BufferPool for reads of page 0, and
// through WALManager for writes, instead of calling DiskManager
// directly. Reads are cache reads like any other page. Writes are now
// wrapped in their own single-write WAL transaction rather than being
// written to the buffer pool directly.
//
// WHY save() GOES THROUGH WAL, NOT JUST THE BUFFER POOL: an earlier
// version of save() wrote straight to the buffer pool's cached page 0
// (marked dirty). That only guarantees the change exists in the buffer
// pool's in-memory frame — if the process crashes (or power is lost)
// before the next checkpoint evicts or flushes that frame, the change
// is silently lost, with no WAL record to recover it from, unlike
// every other write in the system. Wrapping save() in its own
// begin()/write()/commit() closes that gap: commit()'s fsync makes the
// change crash-durable immediately, and commit() already applies the
// page into the buffer pool for us — so save() no longer touches the
// buffer pool directly at all.
//
// init() is the one exception that still writes directly and syncs
// immediately — see its comment below — since it runs before WAL has
// anything meaningful to log against.
//
// Usage (unchanged from the caller's perspective, aside from the
// constructor now also taking a WALManager&):
//   HeaderManager hm(buffer_pool, wal_manager);
//   hm.init();                          // new database
//   hm.header.schema_root_page = 1;     // set after schema layer allocates its page
//   hm.save();                          // now durable immediately, via WAL
class HeaderManager {
public:
    // BufferPool and WALManager must both outlive HeaderManager.
    HeaderManager(BufferPool& buffer_pool, WALManager& wal);

    // Called once when creating a brand new database file.
    // Bootstraps page 0 with an initialized DBHeader.
    // Throws if page 0 already exists (i.e. the file already has pages).
    //
    // This still writes directly and calls disk sync immediately,
    // rather than going through the normal buffer-pool/WAL path: at
    // this point there is no prior page 0 to cache, WAL has nothing
    // meaningful to log yet, and there is nothing else that will ever
    // protect this specific write. It must be durable on its own
    // before anything else (including the first real WAL transaction)
    // proceeds.
    void init();

    // Called when opening an existing database file.
    // Reads page 0 (via the buffer pool), validates magic string and
    // page size. Throws if the file is not a valid database.
    void load();

    // Writes the current state of `header` back to page 0, wrapped in
    // its own single-write WAL transaction — durable immediately upon
    // return (see the class comment above). Call this after modifying
    // any field in `header`, UNLESS that modification needs to be
    // atomic with some other write already in progress (e.g. a
    // free-list pop happening inside a larger BTree transaction) — in
    // that case, don't call save(); log the header change via that
    // transaction's own wal_.write() instead, the way FreeListManager
    // does. Calling save() mid-transaction would fsync and apply the
    // header change immediately, ahead of and independent from the
    // rest of that transaction, breaking the atomicity you're trying
    // to preserve.
    void save();

    // The header fields — read freely, modify then call save() (or, for
    // transactional updates, log the change via WAL directly instead).
    DBHeader header;

private:
    BufferPool& buffer_pool_;
    WALManager& wal_;

    // Reads page 0 via the buffer pool and deserializes it into `header`.
    void read_header_from_buffer_pool();
};