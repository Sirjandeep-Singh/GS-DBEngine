#include "header_manager.h"
#include <stdexcept>
#include <cstring>

HeaderManager::HeaderManager(BufferPool& buffer_pool, WALManager& wal)
    : buffer_pool_(buffer_pool), wal_(wal)
{
    std::memset(&header, 0, sizeof(DBHeader));
}

void HeaderManager::init() {
    if (buffer_pool_.total_pages() > 0) {
        throw std::runtime_error("HeaderManager::init: file already has pages, use load() instead");
    }

    uint32_t page_id_out;
    Page* page = buffer_pool_.new_page(page_id_out);  // allocates page 0

    std::memcpy(header.magic, DB_MAGIC, sizeof(header.magic));
    header.page_size        = PAGE_SIZE;
    header.total_pages      = 1;
    header.schema_root_page = INVALID_PAGE;
    header.first_free_page  = NO_FREE_PAGE;
    header.version          = DB_VERSION;
    header.reserved         = 0;

    std::memset(page->data, 0, PAGE_SIZE);
    std::memcpy(page->data, &header, sizeof(DBHeader));
    buffer_pool_.unpin_page(page_id_out, true);

    // This is the one write in the whole system with no WAL protection —
    // there is no prior page 0 to fall back on and no log entry that
    // could ever replay it (WAL doesn't exist meaningfully before this
    // point). flush_all() writes the dirty page 0 out and then calls
    // disk_.sync(), giving this bootstrap write an actual durability
    // guarantee before anything else (including the first WAL
    // transaction) is allowed to proceed.
    buffer_pool_.flush_all();
}

void HeaderManager::load() {
    if (buffer_pool_.total_pages() == 0) {
        throw std::runtime_error("HeaderManager::load: file is empty, use init() instead");
    }

    read_header_from_buffer_pool();

    if (std::memcmp(header.magic, DB_MAGIC, sizeof(header.magic)) != 0) {
        throw std::runtime_error("HeaderManager::load: invalid magic string — not a valid database file");
    }
    if (header.page_size != PAGE_SIZE) {
        throw std::runtime_error(
            "HeaderManager::load: page size mismatch — file uses "
            + std::to_string(header.page_size)
            + " bytes but engine expects "
            + std::to_string(PAGE_SIZE));
    }
    if (header.version != DB_VERSION) {
        throw std::runtime_error(
            "HeaderManager::load: unsupported file version "
            + std::to_string(header.version));
    }
}

void HeaderManager::save() {
    // Wrapped in its own single-write WAL transaction rather than
    // writing the buffer pool directly. Reasoning: a direct buffer-pool
    // write only lands in an in-memory frame — if the process crashes
    // (or power is lost) before the next checkpoint, that change is
    // silently gone, with no WAL record to recover it from, unlike
    // every other write in the system which goes through wal.write() +
    // wal.commit() before being applied. Routing save() through WAL
    // closes that gap: wal_.commit() below fsyncs the WAL (so the
    // change survives a crash even if it's still sitting in the buffer
    // pool's RAM-only frame) and applies the page to the buffer pool
    // for us, so there's no separate direct write needed here at all.
    Page page;
    std::memset(page.data, 0, PAGE_SIZE);
    std::memcpy(page.data, &header, sizeof(DBHeader));

    uint32_t txn_id = wal_.begin();
    wal_.write(txn_id, HEADER_PAGE_ID, page);
    wal_.commit(txn_id);
}

void HeaderManager::read_header_from_buffer_pool() {
    Page* page = buffer_pool_.fetch_page(HEADER_PAGE_ID);
    std::memcpy(&header, page->data, sizeof(DBHeader));
    buffer_pool_.unpin_page(HEADER_PAGE_ID, false);
}