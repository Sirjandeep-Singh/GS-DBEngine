#include "wal_manager.h"

#include <stdexcept>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

WALManager::WALManager(const std::string& wal_filename, BufferPool& buffer_pool,
                        uint64_t checkpoint_threshold_bytes)
    : wal_filename_(wal_filename), wal_fd_(-1),
      buffer_pool_(buffer_pool), next_transaction_id_(1),
      checkpoint_threshold_bytes_(checkpoint_threshold_bytes)
{
    wal_fd_ = open(wal_filename.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (wal_fd_ < 0) {
        throw std::runtime_error("WALManager: could not open or create WAL file: " + wal_filename);
    }
}

WALManager::~WALManager() {
    if (wal_fd_ >= 0) {
        fsync(wal_fd_);
        close(wal_fd_);
        wal_fd_ = -1;
    }
}

uint32_t WALManager::begin() {
    active_pages_.clear();
    return next_transaction_id_++;
}

void WALManager::write(uint32_t transaction_id, uint32_t page_id, const Page& page) {
    WalRecordHeader header;
    header.type           = WalRecordType::PAGE_WRITE;
    header.transaction_id = transaction_id;
    header.page_id        = page_id;

    // POSIX write() — lands in OS kernel buffer, not physical disk yet
    // no fsync — if crash here, no COMMIT frame exists, recovery ignores this
    write_bytes(&header, sizeof(WalRecordHeader));
    write_bytes(page.data, PAGE_SIZE);

    // store in memory so commit() can apply to buffer pool
    active_pages_[page_id] = page;
}

void WALManager::commit(uint32_t transaction_id) {
    WalRecordHeader header;
    header.type           = WalRecordType::COMMIT;
    header.transaction_id = transaction_id;
    header.page_id        = INVALID_PAGE;

    write_bytes(&header, sizeof(WalRecordHeader));

    // single fsync — forces all PAGE_WRITE frames + COMMIT frame from
    // OS kernel buffer to physical disk in one shot
    if (fsync(wal_fd_) != 0) {
        throw std::runtime_error("WALManager::commit: fsync failed");
    }

    // apply committed pages to buffer pool, mark dirty for checkpoint to flush
    for (auto& [page_id, page] : active_pages_) {
        Page* bp_page = buffer_pool_.fetch_page(page_id);
        std::memcpy(bp_page->data, page.data, PAGE_SIZE);
        buffer_pool_.unpin_page(page_id, true);
    }

    active_pages_.clear();
}

void WALManager::abort(uint32_t transaction_id) {
    (void)transaction_id;
    // true no-op on disk — incomplete PAGE_WRITE frames in .wal have no
    // COMMIT frame and are ignored by recovery
    active_pages_.clear();
}

void WALManager::recover() {
    if (lseek(wal_fd_, 0, SEEK_SET) < 0) {
        throw std::runtime_error("WALManager::recover: lseek failed");
    }

    std::unordered_map<uint32_t, std::unordered_map<uint32_t, Page>> txn_pages;
    std::vector<uint32_t> committed_txns;

    WalRecordHeader header;

    while (read(wal_fd_, &header, sizeof(WalRecordHeader)) == sizeof(WalRecordHeader)) {
        if (header.type == WalRecordType::PAGE_WRITE) {
            Page page;
            if (read(wal_fd_, page.data, PAGE_SIZE) != static_cast<ssize_t>(PAGE_SIZE)) {
                break;
            }
            txn_pages[header.transaction_id][header.page_id] = page;

        } else if (header.type == WalRecordType::COMMIT) {
            committed_txns.push_back(header.transaction_id);
            if (header.transaction_id >= next_transaction_id_) {
                next_transaction_id_ = header.transaction_id + 1;
            }
        }
    }

    for (uint32_t txn_id : committed_txns) {
        auto it = txn_pages.find(txn_id);
        if (it == txn_pages.end()) continue;
        for (auto& [page_id, page] : it->second) {
            Page* bp_page = buffer_pool_.fetch_page(page_id);
            std::memcpy(bp_page->data, page.data, PAGE_SIZE);
            buffer_pool_.unpin_page(page_id, true);
        }
    }

    checkpoint();  // flush dirty pages to .db and truncate .wal
}

void WALManager::checkpoint() {
    // flush all dirty buffer pool pages to .db file
    buffer_pool_.flush_all();

    // truncate .wal to empty — all committed data is now safely in .db
    if (ftruncate(wal_fd_, 0) != 0) {
        throw std::runtime_error("WALManager::checkpoint: ftruncate failed");
    }

    if (lseek(wal_fd_, 0, SEEK_SET) < 0) {
        throw std::runtime_error("WALManager::checkpoint: lseek after truncate failed");
    }
}

uint64_t WALManager::wal_size() const {
    struct stat st;
    if (fstat(wal_fd_, &st) != 0) {
        throw std::runtime_error("WALManager::wal_size: fstat failed");
    }
    return static_cast<uint64_t>(st.st_size);
}

bool WALManager::should_checkpoint() const {
    return wal_size() >= checkpoint_threshold_bytes_;
}

void WALManager::write_bytes(const void* data, size_t size) {
    ssize_t written = ::write(wal_fd_, data, size);
    if (written != static_cast<ssize_t>(size)) {
        throw std::runtime_error("WALManager::write_bytes: write to WAL failed");
    }
}