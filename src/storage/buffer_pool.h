#pragma once
#include <cstdint>
#include <unordered_map>
#include <list>
#include <stdexcept>
#include "page.h"
#include "disk_manager.h"
static const uint32_t BUFFER_POOL_SIZE = 64;  // max pages in memory at once
static const uint32_t INVALID_SLOT     = 0xFFFFFFFF;
// metadata for one slot in the buffer pool
struct FrameMeta {
uint32_t page_id   = INVALID_PAGE;  // which page is in this slot
bool     is_dirty  = false;          // has this page been modified
uint32_t pin_count = 0;              // how many callers are using this page
};
// BufferPool sits between all upper layers and DiskManager.
// Upper layers never call DiskManager directly — they always go through BufferPool.
//
// Responsibilities:
//   - Cache up to BUFFER_POOL_SIZE pages in memory
//   - Serve page requests from cache when possible (cache hit)
//   - Load pages from disk on cache miss
//   - Evict least recently used unpinned page when cache is full
//   - Track dirty pages and flush them to disk before eviction or on demand
//   - Pin pages that are actively in use so they are not evicted
class BufferPool {
public:
explicit BufferPool(DiskManager& disk);
~BufferPool();
    // Returns a pointer to the requested page in the buffer pool.
    // Loads from disk if not already cached.
    // Increments pin count — caller must call unpin_page() when done.
    // Throws if all pages are pinned and no slot is available for eviction.
Page* fetch_page(uint32_t page_id);
    // Signals that the caller is done using this page.
    // is_dirty = true marks the page as modified — it will be written to disk
    // before eviction or on flush. Must be called after every fetch_page().
void unpin_page(uint32_t page_id, bool is_dirty);
    // Allocates a new page on disk, loads it into the buffer pool,
    // and returns a pointer to it. Pin count starts at 1.
    // Caller must call unpin_page() when done.
    // Returns the new page_id via the out parameter.
Page* new_page(uint32_t& page_id_out);
    // Writes a specific dirty page to disk immediately.
    // Page stays in buffer pool after flush.
void flush_page(uint32_t page_id);
    // Writes all dirty pages to disk.
    // Called during checkpoint and on shutdown.
void flush_all();
    // Passthrough to DiskManager::total_pages(). Added so callers that
    // only hold a BufferPool& (e.g. HeaderManager, to check whether the
    // file is empty before deciding init() vs load()) don't need a
    // separate DiskManager reference just for this one check.
uint32_t total_pages() const { return disk_.total_pages(); }
private:
    DiskManager&  disk_;
    Page      frames_[BUFFER_POOL_SIZE];       // the actual page data slots
    FrameMeta meta_[BUFFER_POOL_SIZE];         // metadata for each slot
std::unordered_map<uint32_t, uint32_t> page_table_;  // page_id -> slot index
std::list<uint32_t>                    lru_list_;     // front = MRU, back = LRU [works on slots/indexes]
std::unordered_map<uint32_t, std::list<uint32_t>::iterator> lru_map_;  // slot -> iterator in lru_list_
    // finds a free slot or evicts the LRU unpinned page
    // returns the slot index, throws if no slot is available
uint32_t evict_slot();
    // writes the page in `slot` to disk if dirty, then clears the slot metadata
void evict_frame(uint32_t slot);
    // moves a slot to the front of the LRU list (most recently used)
void touch(uint32_t slot);
};