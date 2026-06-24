//Where is it Getting PAGE_SIZE from?
#include "disk_manager.h"

#include <stdexcept>
#include <cstring>

// ─────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────

DiskManager::DiskManager(const std::string& filename)
    : filename_(filename), total_pages_(0)
{
    // Try to open an existing file first (read + write, binary mode).
    file_.open(filename, std::ios::in | std::ios::out | std::ios::binary);

    if (!file_.is_open()) {
        // File does not exist — create it fresh.
        // ios::trunc creates the file if missing.
        file_.open(filename, std::ios::in | std::ios::out
                           | std::ios::binary | std::ios::trunc);

        if (!file_.is_open()) {
            throw std::runtime_error("DiskManager: could not open or create file: " + filename);
        }

        total_pages_ = 0;
    } else {
        // File exists — calculate how many pages it already contains.
        file_.seekg(0, std::ios::end);
        std::streamsize file_size = file_.tellg();

        // file_size should always be a multiple of PAGE_SIZE.
        // If it is not, the file is corrupt.
        if (file_size % PAGE_SIZE != 0) {
            throw std::runtime_error(
                "DiskManager: file size is not a multiple of PAGE_SIZE — file may be corrupt: "
                + filename);
        }

        total_pages_ = static_cast<uint32_t>(file_size / PAGE_SIZE);
    }
}

// ─────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();   // flush any buffered writes
        file_.close();
    }
}

// ─────────────────────────────────────────────
// read_page
// ─────────────────────────────────────────────

void DiskManager::read_page(uint32_t page_id, Page& page) {
    if (page_id >= total_pages_) {
        throw std::out_of_range(
            "DiskManager::read_page: page_id " + std::to_string(page_id)
            + " is out of range (total pages: " + std::to_string(total_pages_) + ")");
    }

    // Calculate byte offset in the file.
    std::streamoff offset = static_cast<std::streamoff>(page_id) * PAGE_SIZE;

    file_.seekg(offset);

    if (file_.fail()) {
        throw std::runtime_error(
            "DiskManager::read_page: seekg failed for page_id " + std::to_string(page_id));
    }

    // Read exactly PAGE_SIZE bytes into page.data.
    file_.read(reinterpret_cast<char*>(page.data), PAGE_SIZE);

    if (file_.fail()) {
        throw std::runtime_error(
            "DiskManager::read_page: read failed for page_id " + std::to_string(page_id));
    }
}

// ─────────────────────────────────────────────
// write_page
// ─────────────────────────────────────────────

void DiskManager::write_page(uint32_t page_id, const Page& page) {
    if (page_id >= total_pages_) {
        throw std::out_of_range(
            "DiskManager::write_page: page_id " + std::to_string(page_id)
            + " is out of range (total pages: " + std::to_string(total_pages_) + ")");
    }

    // Calculate byte offset in the file.
    std::streamoff offset = static_cast<std::streamoff>(page_id) * PAGE_SIZE;

    file_.seekp(offset);

    if (file_.fail()) {
        throw std::runtime_error(
            "DiskManager::write_page: seekp failed for page_id " + std::to_string(page_id));
    }

    // Write all PAGE_SIZE bytes — the entire page slot is overwritten.
    file_.write(reinterpret_cast<const char*>(page.data), PAGE_SIZE);

    if (file_.fail()) {
        throw std::runtime_error(
            "DiskManager::write_page: write failed for page_id " + std::to_string(page_id));
    }

    // Flush immediately so the write reaches the OS.
    // The WAL layer will call fdatasync for true durability guarantees.
    file_.flush();
}

// ─────────────────────────────────────────────
// allocate_page
// ─────────────────────────────────────────────

uint32_t DiskManager::allocate_page() {
    // The new page's id is the current total (pages are 0-indexed).
    uint32_t new_page_id = total_pages_;

    // Seek to the end of the file and write PAGE_SIZE zero bytes.
    // This physically extends the file by one page slot.
    file_.seekp(0, std::ios::end);

    // Write zeroed page — stack allocation, zero-initialized.
    static const uint8_t zero_page[PAGE_SIZE] = {};
    file_.write(reinterpret_cast<const char*>(zero_page), PAGE_SIZE);

    if (file_.fail()) {
        throw std::runtime_error(
            "DiskManager::allocate_page: failed to extend file for new page");
    }

    file_.flush();

    total_pages_++;
    return new_page_id;
}

// ─────────────────────────────────────────────
// total_pages
// ─────────────────────────────────────────────

uint32_t DiskManager::total_pages() const {
    return total_pages_;
}