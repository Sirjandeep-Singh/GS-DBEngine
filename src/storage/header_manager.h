#pragma once

#include <cstdint>
#include <cstring>
#include "page.h"
#include "disk_manager.h"

// DBHeader

// The raw struct that is serialized into page 0.
// Every field uses fixed-width types to avoid padding/alignment issues
// when writing directly to disk with memcpy.
//
// Layout in page 0 (byte offsets):
//   0  - 15  : magic string ("MYDB_V1\0")
//   16 - 19  : page_size
//   20 - 23  : total_pages
//   24 - 27  : schema_root_page
//   28 - 31  : first_free_page  (0 = no free pages)
//   32 - 35  : version
//   36 - 39  : reserved (padding for future use)
// Total: 40 bytes — fits easily inside a 4096 byte page.

static const char     DB_MAGIC[16]        = "MYDB_V1";
static const uint32_t DB_VERSION          = 1;
static const uint32_t NO_FREE_PAGE        = 0xFFFFFFFF;  // sentinel: free list is empty
static const uint32_t INVALID_PAGE        = 0xFFFFFFFF;  // sentinel: page does not exist yet

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


// HeaderManager

// Owns page 0. Responsible for:
//   - Writing a fresh header when a new database is created (init)
//   - Reading and validating the header when an existing database is opened (load)
//   - Writing the header back to disk whenever fields change (save)
//
// HeaderManager is the only class that interprets page 0.
// All other layers read header fields through the public `header` member.
//
// Usage:
//   HeaderManager hm(disk);
//   hm.init();                          // new database
//   hm.header.schema_root_page = 1;     // set after schema layer allocates its page
//   hm.save();                          // persist the change

class HeaderManager {
public:
    // DiskManager must outlive HeaderManager.
    explicit HeaderManager(DiskManager& disk);

    // Called once when creating a brand new database file.
    // Writes a zeroed + initialized DBHeader to page 0.
    // Throws if page 0 already exists and has a valid magic string.
    void init();

    // Called when opening an existing database file.
    // Reads page 0, validates magic string and page size.
    // Throws if the file is not a valid database.
    void load();

    // Writes the current state of `header` back to page 0.
    // Call this after modifying any field in `header`.
    void save();

    // The header fields — read freely, modify then call save().
    DBHeader header;

private:
    DiskManager& disk_;

    // Serializes `header` into a Page and writes it to disk at page 0.
    void write_header_to_disk();

    // Reads page 0 from disk and deserializes it into `header`.
    void read_header_from_disk();
};