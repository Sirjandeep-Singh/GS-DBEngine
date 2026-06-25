// WAL tests
// g++ -std=c++17 tests/test_wal.cpp src/storage/disk_manager.cpp src/storage/header_manager
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"

namespace fs = std::filesystem;

static const std::string DB_FILE  = "test_wal.db";
static const std::string WAL_FILE = "test_wal.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

void test_wal_begin_returns_incrementing_ids() {
    cleanup();
    DiskManager dm(DB_FILE);
    BufferPool  bp(dm);
    WALManager  wal(WAL_FILE, bp);

    uint32_t id1 = wal.begin();
    uint32_t id2 = wal.begin();
    uint32_t id3 = wal.begin();

    assert(id1 == 1);
    assert(id2 == 2);
    assert(id3 == 3);

    std::cout << "[PASS] begin() returns incrementing transaction ids\n";
    cleanup();
}

void test_wal_commit_persists_page() {
    cleanup();
    {
        DiskManager dm(DB_FILE);
        dm.allocate_page();
        BufferPool bp(dm);
        WALManager wal(WAL_FILE, bp);

        Page p;
        std::memset(p.data, 0xAB, PAGE_SIZE);

        uint32_t txn = wal.begin();
        wal.write(txn, 0, p);
        wal.commit(txn);
    }

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();

    Page* recovered = bp2.fetch_page(0);
    assert(recovered->data[0]           == 0xAB);
    assert(recovered->data[PAGE_SIZE-1] == 0xAB);
    bp2.unpin_page(0, false);

    std::cout << "[PASS] committed transaction recovered correctly\n";
    cleanup();
}

void test_wal_abort_not_recovered() {
    cleanup();
    {
        DiskManager dm(DB_FILE);
        dm.allocate_page();
        BufferPool bp(dm);
        WALManager wal(WAL_FILE, bp);

        Page p;
        std::memset(p.data, 0xCD, PAGE_SIZE);

        uint32_t txn = wal.begin();
        wal.write(txn, 0, p);
        wal.abort(txn);
    }

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();

    Page* p = bp2.fetch_page(0);
    assert(p->data[0] == 0x00);
    bp2.unpin_page(0, false);

    std::cout << "[PASS] aborted transaction not recovered\n";
    cleanup();
}

void test_wal_only_committed_transactions_recovered() {
    cleanup();
    {
        DiskManager dm(DB_FILE);
        dm.allocate_page();
        dm.allocate_page();
        BufferPool bp(dm);
        WALManager wal(WAL_FILE, bp);

        Page p1;
        std::memset(p1.data, 0x11, PAGE_SIZE);
        uint32_t txn1 = wal.begin();
        wal.write(txn1, 0, p1);
        wal.commit(txn1);

        Page p2;
        std::memset(p2.data, 0x22, PAGE_SIZE);
        uint32_t txn2 = wal.begin();
        wal.write(txn2, 1, p2);
        // no commit — simulates crash
    }

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();

    Page* r0 = bp2.fetch_page(0);
    Page* r1 = bp2.fetch_page(1);

    assert(r0->data[0] == 0x11);
    assert(r1->data[0] == 0x00);

    bp2.unpin_page(0, false);
    bp2.unpin_page(1, false);

    std::cout << "[PASS] only committed transactions recovered, incomplete ignored\n";
    cleanup();
}

void test_wal_checkpoint_truncates_wal() {
    cleanup();
    DiskManager dm(DB_FILE);
    dm.allocate_page();
    BufferPool bp(dm);
    WALManager wal(WAL_FILE, bp);

    Page p;
    std::memset(p.data, 0x55, PAGE_SIZE);
    uint32_t txn = wal.begin();
    wal.write(txn, 0, p);
    wal.commit(txn);

    assert(fs::file_size(WAL_FILE) > 0);
    wal.checkpoint();
    assert(fs::file_size(WAL_FILE) == 0);

    std::cout << "[PASS] checkpoint truncates WAL file to empty\n";
    cleanup();
}

void test_wal_recover_truncates_wal() {
    cleanup();
    {
        DiskManager dm(DB_FILE);
        dm.allocate_page();
        BufferPool bp(dm);
        WALManager wal(WAL_FILE, bp);

        Page p;
        std::memset(p.data, 0x66, PAGE_SIZE);
        uint32_t txn = wal.begin();
        wal.write(txn, 0, p);
        wal.commit(txn);
    }

    assert(fs::file_size(WAL_FILE) > 0);

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();

    assert(fs::file_size(WAL_FILE) == 0);

    std::cout << "[PASS] recover truncates WAL via checkpoint at end\n";
    cleanup();
}

void test_wal_transaction_id_resumes_after_recovery() {
    cleanup();
    {
        DiskManager dm(DB_FILE);
        dm.allocate_page();
        BufferPool bp(dm);
        WALManager wal(WAL_FILE, bp);

        uint32_t txn = wal.begin();
        Page p;
        std::memset(p.data, 0x77, PAGE_SIZE);
        wal.write(txn, 0, p);
        wal.commit(txn);
    }

    DiskManager dm2(DB_FILE);
    BufferPool  bp2(dm2);
    WALManager  wal2(WAL_FILE, bp2);
    wal2.recover();

    uint32_t new_txn = wal2.begin();
    assert(new_txn > 1);

    std::cout << "[PASS] transaction ids resume correctly after recovery\n";
    cleanup();
}

int main() {
    std::cout << "\n=== WALManager Tests ===\n";
    test_wal_begin_returns_incrementing_ids();
    test_wal_commit_persists_page();
    test_wal_abort_not_recovered();
    test_wal_only_committed_transactions_recovered();
    test_wal_checkpoint_truncates_wal();
    test_wal_recover_truncates_wal();
    test_wal_transaction_id_resumes_after_recovery();

    std::cout << "\nAll tests passed.\n";
    return 0;
}