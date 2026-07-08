// g++ -std=c++17 tests/test_header_manager.cpp src/storage/disk_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp src/header/header_manager.cpp -o tests/test_header_manager && ./tests/test_header_manager
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/header/header_manager.h"

namespace fs = std::filesystem;

// helper — removes test files before and after each test
static const std::string TEST_FILE = "test_db.db";
static const std::string WAL_FILE  = "test_db.wal";

void cleanup() {
    if (fs::exists(TEST_FILE)) fs::remove(TEST_FILE);
    if (fs::exists(WAL_FILE))  fs::remove(WAL_FILE);
}

// ─────────────────────────────────────────────
// HeaderManager Tests
//
// NOTE ON THE UPDATED CONSTRUCTOR: HeaderManager now takes
// (BufferPool&, WALManager&) instead of (DiskManager&) — page 0 is read
// through the buffer pool like any other page, and save() writes go
// through their own single-write WAL transaction rather than straight
// to disk. Every test below builds the full DiskManager -> BufferPool
// -> WALManager -> HeaderManager chain to match.
// ─────────────────────────────────────────────

void test_header_init_writes_correct_fields() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);
    WALManager wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();

    assert(std::memcmp(hm.header.magic, DB_MAGIC, sizeof(hm.header.magic)) == 0);
    assert(hm.header.page_size        == PAGE_SIZE);
    assert(hm.header.total_pages      == 1);
    assert(hm.header.schema_root_page == INVALID_PAGE);
    assert(hm.header.first_free_page  == NO_FREE_PAGE);
    assert(hm.header.version          == DB_VERSION);
    std::cout << "[PASS] init writes correct header fields\n";
    cleanup();
}

void test_header_load_restores_fields() {
    cleanup();
    {
        DiskManager dm(TEST_FILE);
        BufferPool bp(dm);
        WALManager wal(WAL_FILE, bp);
        HeaderManager hm(bp, wal);
        hm.init();
    }  // closed — BufferPool's destructor flushes+syncs page 0 to disk

    DiskManager dm2(TEST_FILE);
    BufferPool bp2(dm2);
    WALManager wal2(WAL_FILE, bp2);
    HeaderManager hm2(bp2, wal2);
    hm2.load();

    assert(std::memcmp(hm2.header.magic, DB_MAGIC, sizeof(hm2.header.magic)) == 0);
    assert(hm2.header.page_size   == PAGE_SIZE);
    assert(hm2.header.version     == DB_VERSION);
    std::cout << "[PASS] load restores header fields from disk\n";
    cleanup();
}

void test_header_save_persists_changes() {
    cleanup();
    {
        DiskManager dm(TEST_FILE);
        BufferPool bp(dm);
        WALManager wal(WAL_FILE, bp);
        HeaderManager hm(bp, wal);
        hm.init();
        hm.header.schema_root_page = 5;  // simulate schema layer setting this
        hm.save();  // now durable immediately via its own WAL transaction
    }  // closed — BufferPool's destructor also flushes+syncs on the way out

    DiskManager dm2(TEST_FILE);
    BufferPool bp2(dm2);
    WALManager wal2(WAL_FILE, bp2);
    HeaderManager hm2(bp2, wal2);
    hm2.load();

    assert(hm2.header.schema_root_page == 5);
    std::cout << "[PASS] save persists field changes across reopen\n";
    cleanup();
}

void test_header_init_on_existing_file_throws() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);
    WALManager wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);
    hm.init();

    bool threw = false;
    try {
        hm.init();  // calling init again should throw
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "[PASS] init on existing file throws\n";
    cleanup();
}

void test_header_load_on_empty_file_throws() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);
    WALManager wal(WAL_FILE, bp);
    HeaderManager hm(bp, wal);

    bool threw = false;
    try {
        hm.load();  // no pages allocated yet
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "[PASS] load on empty file throws\n";
    cleanup();
}

void test_header_load_on_corrupt_file_throws() {
    cleanup();
    {
        DiskManager dm(TEST_FILE);
        dm.allocate_page();

        // write garbage into page 0 — not a valid header.
        // Written directly through DiskManager (bypassing BufferPool/WAL
        // entirely) to simulate a corrupt file on disk — DiskManager's
        // destructor fsyncs on scope exit, so this is durably on disk
        // before we reopen below.
        Page p;
        std::memset(p.data, 0x42, PAGE_SIZE);
        dm.write_page(0, p);
    }

    DiskManager dm2(TEST_FILE);
    BufferPool bp2(dm2);
    WALManager wal2(WAL_FILE, bp2);
    HeaderManager hm2(bp2, wal2);

    bool threw = false;
    try {
        hm2.load();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    std::cout << "[PASS] load on corrupt file throws\n";
    cleanup();
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────

int main() {
    std::cout << "\n=== HeaderManager Tests ===\n";
    test_header_init_writes_correct_fields();
    test_header_load_restores_fields();
    test_header_save_persists_changes();
    test_header_init_on_existing_file_throws();
    test_header_load_on_empty_file_throws();
    test_header_load_on_corrupt_file_throws();

    std::cout << "\nAll tests passed.\n";
    return 0;
}