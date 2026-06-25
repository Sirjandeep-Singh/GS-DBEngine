#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>

#include "../src/storage/disk_manager.h"
#include "../src/storage/header_manager.h"

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

    std::cout << "\nAll tests passed.\n";
    return 0;
}