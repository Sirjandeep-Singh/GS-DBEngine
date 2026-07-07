#include "disk_manager.h"

#include <stdexcept>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// Constructor

DiskManager::DiskManager(const std::string& filename)
    : fd_(-1), filename_(filename), total_pages_(0)
{
    // O_CREAT: create the file if it doesn't exist yet.
    // O_RDWR: we need both read_page and write_page.
    // No O_TRUNC — an existing database file must be preserved.
    fd_ = open(filename.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("DiskManager: could not open or create file: " + filename);
    }

    // Figure out how many pages already exist by checking the file size.
    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("DiskManager: fstat failed for file: " + filename);
    }

    off_t file_size = st.st_size;

    // file_size should always be a multiple of PAGE_SIZE.
    // If it is not, the file is corrupt.
    if (file_size % PAGE_SIZE != 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error(
            "DiskManager: file size is not a multiple of PAGE_SIZE — file may be corrupt: "
            + filename);
    }

    total_pages_ = static_cast<uint32_t>(file_size / PAGE_SIZE);
}

// Destructor

DiskManager::~DiskManager() {
    if (fd_ >= 0) {
        // Best-effort durability on close. Ignore the return value here —
        // a destructor cannot usefully report failure, and callers relying
        // on durability guarantees should already be calling sync()
        // explicitly at their checkpoint boundaries.
        fsync(fd_);
        close(fd_);
        fd_ = -1;
    }
}

// read_page

void DiskManager::read_page(uint32_t page_id, Page& page) {
    if (page_id >= total_pages_) {
        throw std::out_of_range(
            "DiskManager::read_page: page_id " + std::to_string(page_id)
            + " is out of range (total pages: " + std::to_string(total_pages_) + ")");
    }

    off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;

    // pread() takes an explicit offset — no separate seek step, and no
    // shared file-position state to worry about if this class is ever
    // used from multiple threads.
    ssize_t bytes_read = pread(fd_, page.data, PAGE_SIZE, offset);

    if (bytes_read != static_cast<ssize_t>(PAGE_SIZE)) {
        throw std::runtime_error(
            "DiskManager::read_page: read failed for page_id " + std::to_string(page_id));
    }
}

// write_page

void DiskManager::write_page(uint32_t page_id, const Page& page) {
    if (page_id >= total_pages_) {
        throw std::out_of_range(
            "DiskManager::write_page: page_id " + std::to_string(page_id)
            + " is out of range (total pages: " + std::to_string(total_pages_) + ")");
    }

    off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;

    // pwrite() writes all PAGE_SIZE bytes at the given offset, overwriting
    // the entire page slot.
    //
    // IMPORTANT: this only guarantees the bytes reach the OS page cache.
    // It does NOT fsync. A page written here can still be lost on power
    // loss until sync() is called. Callers that need a durability
    // guarantee (e.g. WAL checkpoint before truncating the log) MUST
    // call sync() after all the write_page() calls they depend on.
    ssize_t bytes_written = pwrite(fd_, page.data, PAGE_SIZE, offset);

    if (bytes_written != static_cast<ssize_t>(PAGE_SIZE)) {
        throw std::runtime_error(
            "DiskManager::write_page: write failed for page_id " + std::to_string(page_id));
    }
}

// allocate_page

uint32_t DiskManager::allocate_page() {
    uint32_t new_page_id = total_pages_;
    off_t offset = static_cast<off_t>(new_page_id) * PAGE_SIZE;

    // Write a zeroed page at the offset immediately past the current end
    // of file. pwrite() past the current EOF extends the file, filling
    // the gap (if any) with zero bytes automatically — writing our own
    // zero_page here keeps the new page's content explicit and correct
    // regardless of that OS behavior.
    static const uint8_t zero_page[PAGE_SIZE] = {};
    ssize_t bytes_written = pwrite(fd_, zero_page, PAGE_SIZE, offset);

    if (bytes_written != static_cast<ssize_t>(PAGE_SIZE)) {
        throw std::runtime_error(
            "DiskManager::allocate_page: failed to extend file for new page");
    }

    total_pages_++;
    return new_page_id;
}

// sync

void DiskManager::sync() {
    // Forces all writes made via pwrite() above out of the OS page cache
    // and onto physical disk. This is the call that turns write_page()'s
    // "cache only" guarantee into an actual durability guarantee.
    //
    // This MUST be called, and MUST succeed, before a WAL checkpoint is
    // allowed to truncate the log. Truncating the log first and syncing
    // the .db file second reintroduces the exact bug this method exists
    // to close: a crash between those two steps would leave no WAL
    // records to recover pages that were never actually persisted.
    if (fsync(fd_) != 0) {
        throw std::runtime_error(
            "DiskManager::sync: fsync failed for file: " + filename_);
    }
}

// total_pages

uint32_t DiskManager::total_pages() const {
    return total_pages_;
}