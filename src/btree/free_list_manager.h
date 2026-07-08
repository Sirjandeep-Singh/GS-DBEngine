#pragma once

#include <cstdint>
#include "../storage/page.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"
#include "../header/header_manager.h"

// HEADER_PAGE_ID is defined in header_manager.h (the header page always
// lives at page_id 0, per HeaderManager::init()).

// FreeListManager sits at the same layer as BTree — a peer, not a
// sub-component of BufferPool or HeaderManager. It depends on
// BufferPool and WALManager, exactly like BTree does.
//
// It owns the *logic* of the free list (push/pop semantics, threading
// "next" pointers through reclaimed pages), not separate storage — the
// list head lives in the header page's first_free_page field, and the
// list itself is threaded through the free pages' own bytes.
//
// CRITICAL INVARIANT: every mutation this class makes (header update,
// reclaimed-page content) is issued via wal_.write() under a
// caller-supplied transaction_id. It never writes directly to disk and
// never calls anything durability-related (fsync/checkpoint) itself.
// Atomicity comes entirely from riding inside whatever transaction the
// caller (typically BTree) is already running — the header update and
// the page reuse become durable together, at the caller's wal.commit(),
// or not at all if the transaction aborts/crashes first.
//
// KNOWN OPEN SEAM: this class assumes the header page participates in
// the normal BufferPool/WAL path (fetched via buffer_pool_, mutated,
// logged via wal_). If HeaderManager still also maintains an in-memory
// `header` struct that it writes directly to DiskManager (its current
// pattern per the test file), that in-memory struct will go stale after
// WALManager::recover() replays a header page write — recovery updates
// the buffer pool's copy of page 0, not HeaderManager's cached struct.
// HeaderManager should be changed to read first_free_page (and any
// other header fields) directly from the buffer-pool-owned header page
// on demand, rather than caching a separate copy, or explicitly reload
// its cache immediately after recover() completes. This class does not
// solve that on its own — flagging it rather than papering over it.

class FreeListManager {
public:
    FreeListManager(BufferPool& buffer_pool, WALManager& wal, HeaderManager& header_manager);

    // Returns a page to use for a new B+Tree node (or any other caller).
    // If the free list has an entry, pops it and returns that page,
    // pinned (pin_count == 1) — same contract as BufferPool::new_page().
    // If the free list is empty, falls back to buffer_pool_.new_page().
    //
    // If a page was reclaimed, this issues a wal_.write() for the
    // updated header page under transaction_id. That write is NOT
    // durable on its own — it becomes durable only when the caller
    // later calls wal_.commit(transaction_id), same as any other write
    // in that transaction.
    //
    // Caller must still: write the page's real content, wal_.write()
    // that content under transaction_id, and unpin_page() when done —
    // exactly as if it had come from buffer_pool_.new_page() directly.
    Page* allocate_page(uint32_t transaction_id, uint32_t& page_id_out);

    // Pushes page_id onto the head of the free list. Call this once the
    // caller is done with page_id's old content (e.g. after a B+Tree
    // merge empties a node) — this call overwrites that page's content
    // with a free-list "next pointer" record, so nothing in the old
    // content should still be needed.
    //
    // Fetches and unpins page_id itself — caller does not need to have
    // it pinned beforehand and should not hold a pointer to its old
    // content across this call.
    //
    // Issues wal_.write() for both the page's new content and the
    // updated header, under transaction_id. Not durable until the
    // caller's wal_.commit(transaction_id).
    void free_page(uint32_t transaction_id, uint32_t page_id);

private:
    BufferPool&    buffer_pool_;
    WALManager&    wal_;
    HeaderManager& header_manager_;

    // Free pages store their "next" pointer as the first 4 bytes of the
    // page. The rest of the page's bytes are unused (left zeroed) until
    // the page is reallocated and overwritten with real content.
    static uint32_t read_next_pointer(const Page& page);
    static void     write_next_pointer(Page& page, uint32_t next);

    // Serializes header_manager_.header into a Page for wal_.write().
    // See KNOWN OPEN SEAM above re: keeping this struct in sync with
    // what recovery actually applies to the buffer-pool-owned page 0.
    static void header_to_page(const HeaderManager& header_manager, Page& out);
};