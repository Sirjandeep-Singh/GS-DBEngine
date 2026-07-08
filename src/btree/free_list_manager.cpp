#include "free_list_manager.h"
#include <cstring>

FreeListManager::FreeListManager(BufferPool& buffer_pool, WALManager& wal, HeaderManager& header_manager)
    : buffer_pool_(buffer_pool), wal_(wal), header_manager_(header_manager)
{}

uint32_t FreeListManager::read_next_pointer(const Page& page) {
    uint32_t next;
    std::memcpy(&next, page.data, sizeof(uint32_t));
    return next;
}

void FreeListManager::write_next_pointer(Page& page, uint32_t next) {
    std::memcpy(page.data, &next, sizeof(uint32_t));
}

void FreeListManager::header_to_page(const HeaderManager& header_manager, Page& out) {
    std::memset(out.data, 0, PAGE_SIZE);
    std::memcpy(out.data, &header_manager.header, sizeof(header_manager.header));
}

Page* FreeListManager::allocate_page(uint32_t transaction_id, uint32_t& page_id_out) {
    uint32_t head = header_manager_.header.first_free_page;

    if (head == NO_FREE_PAGE) {
        // Free list empty — fall back to a genuinely new page. No header
        // mutation needed since we're not touching the free list.
        return buffer_pool_.new_page(page_id_out);
    }

    // Reclaim the head of the free list.
    Page* free_page = buffer_pool_.fetch_page(head);
    uint32_t next = read_next_pointer(*free_page);

    // Update the header's free-list head and log that write under the
    // caller's transaction. Not durable until the caller commits.
    header_manager_.header.first_free_page = next;

    Page header_page;
    header_to_page(header_manager_, header_page);
    wal_.write(transaction_id, HEADER_PAGE_ID, header_page);

    page_id_out = head;

    // free_page stays pinned (fetch_page incremented pin_count) — caller
    // now owns it, exactly as if it had come from buffer_pool_.new_page().
    // Caller is responsible for writing real content, wal_.write()-ing
    // that content, and unpin_page() when done.
    return free_page;
}

void FreeListManager::free_page(uint32_t transaction_id, uint32_t page_id) {
    // Fetch it ourselves rather than requiring the caller to hold it
    // pinned — keeps the pin/unpin bookkeeping self-contained to this call.
    Page* page = buffer_pool_.fetch_page(page_id);

    uint32_t old_head = header_manager_.header.first_free_page;
    write_next_pointer(*page, old_head);

    // Log the page's new "free record" content under the caller's
    // transaction — same transaction the caller is already using for
    // whatever operation (e.g. B+Tree merge) triggered this free.
    wal_.write(transaction_id, page_id, *page);

    // Update and log the header, threading page_id onto the head of the
    // list. This and the write above only become durable together, at
    // the caller's wal_.commit(transaction_id) — never independently.
    header_manager_.header.first_free_page = page_id;

    Page header_page;
    header_to_page(header_manager_, header_page);
    wal_.write(transaction_id, HEADER_PAGE_ID, header_page);

    buffer_pool_.unpin_page(page_id, true);
}