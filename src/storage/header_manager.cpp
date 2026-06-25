#include "header_manager.h"

#include <stdexcept>
#include <cstring>

HeaderManager::HeaderManager(DiskManager& disk)
    : disk_(disk)
{
    std::memset(&header, 0, sizeof(DBHeader));
}

void HeaderManager::init() {
    if (disk_.total_pages() > 0) {
        throw std::runtime_error("HeaderManager::init: file already has pages, use load() instead");
    }

    disk_.allocate_page();  // allocates page 0

    std::memcpy(header.magic, DB_MAGIC, sizeof(header.magic));
    header.page_size        = PAGE_SIZE;
    header.total_pages      = 1;
    header.schema_root_page = INVALID_PAGE;
    header.first_free_page  = NO_FREE_PAGE;
    header.version          = DB_VERSION;
    header.reserved         = 0;

    write_header_to_disk();
}

void HeaderManager::load() {
    if (disk_.total_pages() == 0) {
        throw std::runtime_error("HeaderManager::load: file is empty, use init() instead");
    }

    read_header_from_disk();

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
    write_header_to_disk();
}

void HeaderManager::write_header_to_disk() {
    Page page;
    std::memset(page.data, 0, PAGE_SIZE);
    std::memcpy(page.data, &header, sizeof(DBHeader));
    disk_.write_page(HEADER_PAGE_ID, page);
}

void HeaderManager::read_header_from_disk() {
    Page page;
    disk_.read_page(HEADER_PAGE_ID, page);
    std::memcpy(&header, page.data, sizeof(DBHeader));
}