#pragma once

#include <cstdint>

// Every unit of data in the database is stored in fixed-size pages.
// The page size is 4KB — every read and write to disk is exactly this many bytes.
// Nothing above this layer ever reads partial pages.

static const uint32_t PAGE_SIZE = 4096;

// Page 0 is always the database header.
// All other pages are B+ tree nodes, schema table entries, or free pages.
static const uint32_t HEADER_PAGE_ID = 0;

struct Page {
    uint8_t data[PAGE_SIZE];  // raw bytes — interpreted by layers above
};