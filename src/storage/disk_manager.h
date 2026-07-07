#pragma once

#include <string>
#include <cstdint>
#include "page.h"

// DiskManager is the lowest layer of the engine.
// It is the only class that touches the .db file directly.
// Every other layer gets data by asking DiskManager for pages.
//
// Responsibilities:
//   - Open / create the database file
//   - Read a page from disk by page_id
//   - Write a page to disk by page_id
//   - Allocate new pages by extending the file
//   - Report total page count
//   - Force durable persistence of pages on demand (sync)
//
// DiskManager knows nothing about what is inside a page.
// It does not know about B+ trees, schemas, or rows.
// It only knows: page_id -> byte offset in file.
//
// Implementation note:
// Uses raw POSIX file descriptors (open/pread/pwrite/fsync) instead of
// std::fstream. This is required because std::fstream does not expose
// a portable file descriptor, so there is no safe way to call fsync()
// on it. Durability at checkpoint time depends on being able to force
// dirty pages out of the OS page cache onto physical disk, which only
// fsync()/fdatasync() can guarantee — write()/pwrite() alone only land
// data in the OS cache, not on disk.

class DiskManager {
public:
    // Opens the file at `filename`.
    // If the file does not exist, it is created as a new empty database.
    explicit DiskManager(const std::string& filename);

    // Flushes pending writes, syncs, and closes the file.
    ~DiskManager();

    // Disable copy — this class owns a raw OS file descriptor.
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Reads PAGE_SIZE bytes from the file at offset (page_id * PAGE_SIZE)
    // into `page.data`. Throws if page_id is out of range.
    void read_page(uint32_t page_id, Page& page);

    // Writes all PAGE_SIZE bytes of `page.data` to the file
    // at offset (page_id * PAGE_SIZE). Overwrites the entire page slot.
    // Throws if page_id is out of range.
    //
    // NOTE ON DURABILITY: this call only guarantees the bytes reach the
    // OS page cache (via pwrite()). It does NOT guarantee the bytes are
    // on physical disk. Callers that need a durability guarantee (e.g.
    // WAL checkpoint before truncating the log) MUST call sync() after
    // all the write_page() calls they depend on.
    void write_page(uint32_t page_id, const Page& page);

    // Extends the file by one page (writes PAGE_SIZE zero bytes at the end).
    // Returns the page_id of the newly allocated page.
    uint32_t allocate_page();

    // Forces all pending writes to this file out of the OS page cache and
    // onto physical disk (fsync). This is the call that provides an actual
    // durability guarantee. Must be called before a WAL checkpoint is
    // allowed to truncate the log, since the log truncation is only safe
    // once the data pages it protects are verifiably on disk.
    void sync();

    // Returns how many pages currently exist in the file.
    uint32_t total_pages() const;

private:
    int             fd_;            // raw OS file descriptor for the .db file
    std::string     filename_;      // stored for error messages
    uint32_t        total_pages_;   // cached page count, kept in sync with file
};