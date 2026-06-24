#pragma once

#include <string>
#include <fstream>
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
//
// DiskManager knows nothing about what is inside a page.
// It does not know about B+ trees, schemas, or rows.
// It only knows: page_id -> byte offset in file.

class DiskManager {
public:
    // Opens the file at `filename`.
    // If the file does not exist, it is created as a new empty database.
    explicit DiskManager(const std::string& filename);

    // Flushes pending writes and closes the file.
    ~DiskManager();

    // Reads PAGE_SIZE bytes from the file at offset (page_id * PAGE_SIZE)
    // into `page.data`. Throws if page_id is out of range.
    void read_page(uint32_t page_id, Page& page);

    // Writes all PAGE_SIZE bytes of `page.data` to the file
    // at offset (page_id * PAGE_SIZE). Overwrites the entire page slot.
    // Throws if page_id is out of range.
    void write_page(uint32_t page_id, const Page& page);

    // Extends the file by one page (writes PAGE_SIZE zero bytes at the end).
    // Returns the page_id of the newly allocated page.
    uint32_t allocate_page();

    // Returns how many pages currently exist in the file.
    uint32_t total_pages() const;

private:
    std::fstream    file_;          // the open file handle
    std::string     filename_;      // stored for error messages
    uint32_t        total_pages_;   // cached page count, kept in sync with file
};