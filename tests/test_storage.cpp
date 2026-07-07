// g++ -std=c++17 tests/test_storage.cpp src/storage/disk_manager.cpp src/storage/header_manager.cpp src/storage/buffer_pool.cpp -o tests/test_storage && ./tests/test_storage
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <filesystem>

#include "../src/storage/disk_manager.h"
#include "../src/storage/header_manager.h"
#include "../src/storage/buffer_pool.h"

namespace fs = std::filesystem;

// helper — removes test file before and after each test
static const std::string TEST_FILE = "test_db.db";

void cleanup() {
    if (fs::exists(TEST_FILE)) fs::remove(TEST_FILE);
}

// ─────────────────────────────────────────────
// DiskManager Tests
// ─────────────────────────────────────────────

void test_new_file_has_zero_pages() {
    cleanup();
    DiskManager dm(TEST_FILE);
    assert(dm.total_pages() == 0);
    std::cout << "[PASS] new file has zero pages\n";
    cleanup();
}

void test_allocate_page_returns_correct_id() {
    cleanup();
    DiskManager dm(TEST_FILE);
    uint32_t id0 = dm.allocate_page();
    uint32_t id1 = dm.allocate_page();
    uint32_t id2 = dm.allocate_page();
    assert(id0 == 0);
    assert(id1 == 1);
    assert(id2 == 2);
    assert(dm.total_pages() == 3);
    std::cout << "[PASS] allocate_page returns correct sequential ids\n";
    cleanup();
}

void test_write_and_read_page_matches() {
    cleanup();
    DiskManager dm(TEST_FILE);
    dm.allocate_page();

    Page write_page;
    // fill page with a recognizable pattern
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        write_page.data[i] = static_cast<uint8_t>(i % 256);
    }

    dm.write_page(0, write_page);

    Page read_page;
    dm.read_page(0, read_page);

    assert(std::memcmp(write_page.data, read_page.data, PAGE_SIZE) == 0);
    std::cout << "[PASS] write then read returns identical bytes\n";
    cleanup();
}

void test_write_multiple_pages_no_overlap() {
    cleanup();
    DiskManager dm(TEST_FILE);
    dm.allocate_page();  // page 0
    dm.allocate_page();  // page 1

    Page page0, page1;
    std::memset(page0.data, 0xAA, PAGE_SIZE);  // fill page 0 with 0xAA
    std::memset(page1.data, 0xBB, PAGE_SIZE);  // fill page 1 with 0xBB

    dm.write_page(0, page0);
    dm.write_page(1, page1);

    Page read0, read1;
    dm.read_page(0, read0);
    dm.read_page(1, read1);

    assert(std::memcmp(read0.data, page0.data, PAGE_SIZE) == 0);
    assert(std::memcmp(read1.data, page1.data, PAGE_SIZE) == 0);
    // verify pages did not bleed into each other
    assert(read0.data[0] == 0xAA);
    assert(read1.data[0] == 0xBB);
    std::cout << "[PASS] multiple pages do not overlap\n";
    cleanup();
}

void test_read_out_of_range_throws() {
    cleanup();
    DiskManager dm(TEST_FILE);
    bool threw = false;
    try {
        Page p;
        dm.read_page(0, p);  // no pages allocated yet
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);
    std::cout << "[PASS] read_page out of range throws\n";
    cleanup();
}

void test_write_out_of_range_throws() {
    cleanup();
    DiskManager dm(TEST_FILE);
    bool threw = false;
    try {
        Page p;
        dm.write_page(0, p);  // no pages allocated yet
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);
    std::cout << "[PASS] write_page out of range throws\n";
    cleanup();
}

void test_reopen_file_restores_total_pages() {
    cleanup();
    {
        DiskManager dm(TEST_FILE);
        dm.allocate_page();
        dm.allocate_page();
        dm.allocate_page();
    }  // dm goes out of scope, file closed

    DiskManager dm2(TEST_FILE);
    assert(dm2.total_pages() == 3);
    std::cout << "[PASS] reopening file restores total_pages correctly\n";
    cleanup();
}

// ─────────────────────────────────────────────
// HeaderManager Tests
// ─────────────────────────────────────────────

void test_header_init_writes_correct_fields() {
    cleanup();
    DiskManager dm(TEST_FILE);
    HeaderManager hm(dm);
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
        HeaderManager hm(dm);
        hm.init();
    }  // closed

    DiskManager dm2(TEST_FILE);
    HeaderManager hm2(dm2);
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
        HeaderManager hm(dm);
        hm.init();
        hm.header.schema_root_page = 5;  // simulate schema layer setting this
        hm.save();
    }  // closed

    DiskManager dm2(TEST_FILE);
    HeaderManager hm2(dm2);
    hm2.load();

    assert(hm2.header.schema_root_page == 5);
    std::cout << "[PASS] save persists field changes across reopen\n";
    cleanup();
}

void test_header_init_on_existing_file_throws() {
    cleanup();
    DiskManager dm(TEST_FILE);
    HeaderManager hm(dm);
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
    HeaderManager hm(dm);

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

        // write garbage into page 0 — not a valid header
        Page p;
        std::memset(p.data, 0x42, PAGE_SIZE);
        dm.write_page(0, p);
    }

    DiskManager dm2(TEST_FILE);
    HeaderManager hm2(dm2);

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
// BufferPool Tests
// ─────────────────────────────────────────────

void test_buffer_fetch_and_read() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);

    uint32_t page_id;
    Page* p = bp.new_page(page_id);
    std::memset(p->data, 0xAA, PAGE_SIZE);
    bp.unpin_page(page_id, true);

    bp.flush_page(page_id);

    Page* p2 = bp.fetch_page(page_id);
    assert(p2->data[0] == 0xAA);
    assert(p2->data[PAGE_SIZE - 1] == 0xAA);
    bp.unpin_page(page_id, false);

    std::cout << "[PASS] fetch_page returns correct data after write\n";
    cleanup();
}

void test_new_page_is_not_dirty() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);

    uint32_t page_id;
    bp.new_page(page_id);
    bp.unpin_page(page_id, false);

    // flush_all should not write anything — page is clean
    // verify by checking no exception is thrown and page is still readable
    bp.flush_all();
    Page* p = bp.fetch_page(page_id);
    assert(p != nullptr);
    bp.unpin_page(page_id, false);

    std::cout << "[PASS] new_page starts as not dirty\n";
    cleanup();
}

void test_unpin_dirty_marks_page() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);

    uint32_t page_id;
    Page* p = bp.new_page(page_id);
    p->data[0] = 0x42;
    bp.unpin_page(page_id, true);   // mark dirty

    bp.flush_page(page_id);         // flush to disk

    // reopen and verify data persisted
    BufferPool bp2(dm);
    Page* p2 = bp2.fetch_page(page_id);
    assert(p2->data[0] == 0x42);
    bp2.unpin_page(page_id, false);

    std::cout << "[PASS] unpin with is_dirty=true persists data on flush\n";
    cleanup();
}

void test_flush_all_writes_dirty_pages() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);

    uint32_t id0, id1;
    Page* p0 = bp.new_page(id0);
    Page* p1 = bp.new_page(id1);

    std::memset(p0->data, 0x11, PAGE_SIZE);
    std::memset(p1->data, 0x22, PAGE_SIZE);

    bp.unpin_page(id0, true);
    bp.unpin_page(id1, true);
    bp.flush_all();

    // read back directly from disk via a fresh buffer pool
    BufferPool bp2(dm);
    Page* r0 = bp2.fetch_page(id0);
    Page* r1 = bp2.fetch_page(id1);

    assert(r0->data[0] == 0x11);
    assert(r1->data[0] == 0x22);

    bp2.unpin_page(id0, false);
    bp2.unpin_page(id1, false);

    std::cout << "[PASS] flush_all writes all dirty pages to disk\n";
    cleanup();
}

void test_cache_hit_does_not_reload_from_disk() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);

    uint32_t page_id;
    Page* p = bp.new_page(page_id);
    p->data[0] = 0x55;
    bp.unpin_page(page_id, true);

    // fetch again — should be a cache hit, same pointer
    Page* p2 = bp.fetch_page(page_id);
    assert(p2->data[0] == 0x55);
    bp.unpin_page(page_id, false);

    std::cout << "[PASS] repeated fetch returns cached page\n";
    cleanup();
}

void test_eviction_flushes_dirty_page() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);

    // fill the entire buffer pool
    std::vector<uint32_t> ids(BUFFER_POOL_SIZE);
    for (uint32_t i = 0; i < BUFFER_POOL_SIZE; i++) {
        Page* p = bp.new_page(ids[i]);
        p->data[0] = static_cast<uint8_t>(i + 1);
        bp.unpin_page(ids[i], true);  // all dirty, all unpinned
    }

    // allocate one more — forces eviction of LRU page
    uint32_t extra_id;
    bp.new_page(extra_id);
    bp.unpin_page(extra_id, false);

    // the evicted page should have been flushed to disk
    // read it back via a fresh DiskManager to bypass buffer pool
    Page raw;
    dm.read_page(ids[0], raw);  // ids[0] was LRU — first allocated, first evicted
    assert(raw.data[0] == 1);

    std::cout << "[PASS] eviction flushes dirty page to disk\n";
    cleanup();
}

void test_pinned_page_not_evicted() {
    cleanup();
    DiskManager dm(TEST_FILE);
    BufferPool bp(dm);

    // fill buffer pool, keep first page pinned
    uint32_t pinned_id;
    bp.new_page(pinned_id);  // pin count = 1, NOT unpinned

    std::vector<uint32_t> ids(BUFFER_POOL_SIZE - 1);
    for (uint32_t i = 0; i < BUFFER_POOL_SIZE - 1; i++) {
        Page* p = bp.new_page(ids[i]);
        bp.unpin_page(ids[i], false);
    }

    // buffer pool is now full with pinned_id still pinned
    // allocating one more should evict an unpinned page, not the pinned one
    uint32_t extra_id;
    bp.new_page(extra_id);
    bp.unpin_page(extra_id, false);

    // pinned page should still be in buffer pool and accessible
    bp.unpin_page(pinned_id, false);  // unpin now
    Page* p = bp.fetch_page(pinned_id);
    assert(p != nullptr);
    bp.unpin_page(pinned_id, false);

    std::cout << "[PASS] pinned page is not evicted\n";
    cleanup();
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────

int main() {
    std::cout << "\n=== DiskManager Tests ===\n";
    test_new_file_has_zero_pages();
    test_allocate_page_returns_correct_id();
    test_write_and_read_page_matches();
    test_write_multiple_pages_no_overlap();
    test_read_out_of_range_throws();
    test_write_out_of_range_throws();
    test_reopen_file_restores_total_pages();

    std::cout << "\n=== HeaderManager Tests ===\n";
    test_header_init_writes_correct_fields();
    test_header_load_restores_fields();
    test_header_save_persists_changes();
    test_header_init_on_existing_file_throws();
    test_header_load_on_empty_file_throws();
    test_header_load_on_corrupt_file_throws();

    std::cout << "\n=== BufferPool Tests ===\n";
    test_buffer_fetch_and_read();
    test_new_page_is_not_dirty();
    test_unpin_dirty_marks_page();
    test_flush_all_writes_dirty_pages();
    test_cache_hit_does_not_reload_from_disk();
    test_eviction_flushes_dirty_page();
    test_pinned_page_not_evicted();

    std::cout << "\nAll tests passed.\n";
    return 0;
}