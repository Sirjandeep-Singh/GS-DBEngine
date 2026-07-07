#include "buffer_pool.h"
#include <cstring>

BufferPool::BufferPool(DiskManager& disk)
    : disk_(disk)
{
    for (uint32_t i = 0; i < BUFFER_POOL_SIZE; i++) {
        std::memset(frames_[i].data, 0, PAGE_SIZE);
        meta_[i] = FrameMeta();
        lru_list_.push_back(i);
        lru_map_[i] = std::prev(lru_list_.end());
    }
}

BufferPool::~BufferPool() {
    flush_all();
}

Page* BufferPool::fetch_page(uint32_t page_id) {
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        uint32_t slot = it->second;
        meta_[slot].pin_count++;
        touch(slot);
        return &frames_[slot];
    }
    uint32_t slot = evict_slot();
    disk_.read_page(page_id, frames_[slot]);
    meta_[slot].page_id   = page_id;
    meta_[slot].is_dirty  = false;
    meta_[slot].pin_count = 1;
    page_table_[page_id] = slot;
    touch(slot);
    return &frames_[slot];
}

void BufferPool::unpin_page(uint32_t page_id, bool is_dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;
    uint32_t slot = it->second;
    if (meta_[slot].pin_count == 0) return;
    meta_[slot].pin_count--;
    if (is_dirty) {
        meta_[slot].is_dirty = true;
    }
}

Page* BufferPool::new_page(uint32_t& page_id_out) {
    uint32_t page_id = disk_.allocate_page();
    uint32_t slot    = evict_slot();
    // evict_slot zeroes out evicted page hence we dont need to do so here
    meta_[slot].page_id   = page_id;
    meta_[slot].is_dirty  = false;
    meta_[slot].pin_count = 1;
    page_table_[page_id] = slot;
    touch(slot);
    page_id_out = page_id;
    return &frames_[slot];
}

void BufferPool::flush_page(uint32_t page_id) {
    // NOTE: intentionally does not call disk_.sync(). This is used during
    // normal-path eviction, while the page's WAL record still exists (the
    // WAL is only truncated at checkpoint). If a crash happens after this
    // write but before the next checkpoint, WALManager::recover() will
    // replay this page from the log — so an fsync here buys no additional
    // durability, it would only add disk-sync latency to every cache-miss
    // eviction. Only flush_all() (the checkpoint path) needs to sync.
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;
    uint32_t slot = it->second;
    if (meta_[slot].is_dirty) {
        disk_.write_page(page_id, frames_[slot]);
        meta_[slot].is_dirty = false;
    }
}

void BufferPool::flush_all() {
    for (auto& [page_id, slot] : page_table_) {
        if (meta_[slot].is_dirty) {
            disk_.write_page(page_id, frames_[slot]);
            meta_[slot].is_dirty = false;
        }
    }

    // This is the checkpoint durability boundary: flush_all() is called
    // from WALManager::checkpoint() immediately before the WAL is
    // truncated. write_page() above only guarantees pages reached the OS
    // page cache, not physical disk. Without this sync(), a crash between
    // checkpoint()'s flush_all() and its ftruncate() could lose pages that
    // the (now-truncated) WAL no longer has any record of. Calling
    // disk_.sync() here forces every page written above out to physical
    // disk before this function returns, so it is safe for the caller to
    // truncate the log immediately afterward.
    disk_.sync();
}

uint32_t BufferPool::evict_slot() {
    // walk LRU list from back (least recently used) to find unpinned slot
    for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
        uint32_t slot = *it;
        if (meta_[slot].pin_count > 0) continue;  // pinned, skip
        evict_frame(slot);
        return slot;
    }
    throw std::runtime_error("BufferPool: all pages are pinned, no slot available for eviction");
}

void BufferPool::evict_frame(uint32_t slot) {
    uint32_t old_page_id = meta_[slot].page_id;
    if (old_page_id != INVALID_PAGE) {
        if (meta_[slot].is_dirty) {
            disk_.write_page(old_page_id, frames_[slot]);
        }
        page_table_.erase(old_page_id);
    }
    meta_[slot] = FrameMeta();
    std::memset(frames_[slot].data, 0, PAGE_SIZE);
}

void BufferPool::touch(uint32_t slot) {
    lru_list_.erase(lru_map_[slot]);
    lru_list_.push_front(slot);
    lru_map_[slot] = lru_list_.begin();
}