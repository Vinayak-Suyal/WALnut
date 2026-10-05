// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/buffer/buffer_pool.cpp
//
// Thread-safety: a single std::mutex (mutex_) protects all shared state.
// The lock is held during I/O calls to FileManager to avoid TOCTOU races.
// A future improvement is to use a readers-writer lock and release it during
// the actual disk read, but for this milestone simplicity + correctness wins.
// =============================================================================

#include "walnut/buffer/buffer_pool.hpp"
#include <stdexcept>
#include <cassert>
#include <string>

namespace walnut {

// ---------------------------------------------------------------------------
BufferPool::BufferPool(std::size_t frame_count)
    : frames_(frame_count), frame_count_(frame_count), clock_hand_(0) {}

// ---------------------------------------------------------------------------
// Internal: find frame index for a page_id (no lock – caller holds mutex_).
// ---------------------------------------------------------------------------
std::size_t BufferPool::findFrame(uint32_t page_id) const {
    auto it = page_table_.find(page_id);
    return (it != page_table_.end()) ? it->second : SIZE_MAX;
}

// ---------------------------------------------------------------------------
// Clock replacement algorithm (caller holds mutex_).
//
// Scans frames starting at clock_hand_ until it finds a frame with
//   - pin_count == 0   (not currently in use)
//   - reference_bit == false
// Frames with reference_bit == true get their bit cleared and are skipped.
// Frames with pin_count > 0 are never selected.
//
// If all frames are pinned, throws std::runtime_error.
// ---------------------------------------------------------------------------
std::size_t BufferPool::selectVictim() {
    // Two full sweeps: first pass clears reference bits, second selects.
    for (std::size_t sweeps = 0; sweeps < 2 * frame_count_; ++sweeps) {
        Frame& f = frames_[clock_hand_];

        if (!f.isValid()) {
            // Empty frame – grab it immediately.
            std::size_t idx = clock_hand_;
            clock_hand_ = (clock_hand_ + 1) % frame_count_;
            return idx;
        }

        if (f.pinCount() == 0) {
            if (f.referenceBit()) {
                // Give this page a second chance.
                f.clearReferenceBit();
            } else {
                // This page is the victim.
                std::size_t idx = clock_hand_;
                clock_hand_ = (clock_hand_ + 1) % frame_count_;
                return idx;
            }
        }
        // Move hand forward.
        clock_hand_ = (clock_hand_ + 1) % frame_count_;
    }

    throw std::runtime_error(
        "BufferPool::selectVictim – all frames are pinned; pool exhausted");
}

// ---------------------------------------------------------------------------
// Evict the frame at frame_index (caller holds mutex_).
// ---------------------------------------------------------------------------
void BufferPool::evictFrame(std::size_t frame_index, FileManager& file_manager) {
    Frame& f = frames_[frame_index];
    if (!f.isValid()) return;

    ++stats_.evictions;

    if (f.isDirty()) {
        // Check WAL hook (integration point for Kanak).
        bool should_flush = true;
        if (pre_flush_hook_) {
            should_flush = pre_flush_hook_(f.pageId());
        }
        if (should_flush) {
            file_manager.writePage(f.page());
            ++stats_.dirty_evictions;
        }
    }

    // Remove from page table.
    page_table_.erase(f.pageId());
    f.reset();
}

// ---------------------------------------------------------------------------
Page& BufferPool::fetchPage(uint32_t page_id, FileManager& file_manager) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t idx = findFrame(page_id);
    if (idx != SIZE_MAX) {
        // Cache hit.
        ++stats_.hits;
        frames_[idx].pin();
        frames_[idx].setReferenceBit(true);
        return frames_[idx].page();
    }

    // Cache miss.
    ++stats_.misses;

    // Select a victim frame.
    idx = selectVictim();
    evictFrame(idx, file_manager);

    // Load page from disk.
    Page page = file_manager.readPage(page_id);
    frames_[idx].loadPage(page);
    frames_[idx].pin();
    page_table_[page_id] = idx;

    return frames_[idx].page();
}

// ---------------------------------------------------------------------------
void BufferPool::pinPage(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t idx = findFrame(page_id);
    if (idx == SIZE_MAX) {
        throw std::runtime_error(
            "BufferPool::pinPage – page " + std::to_string(page_id) +
            " not in pool");
    }
    frames_[idx].pin();
}

// ---------------------------------------------------------------------------
void BufferPool::unpinPage(uint32_t page_id, bool dirty) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t idx = findFrame(page_id);
    if (idx == SIZE_MAX) {
        throw std::runtime_error(
            "BufferPool::unpinPage – page " + std::to_string(page_id) +
            " not in pool");
    }
    frames_[idx].unpin();
    if (dirty) {
        frames_[idx].markDirty();
    }
}

// ---------------------------------------------------------------------------
void BufferPool::flushPage(uint32_t page_id, FileManager& file_manager) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t idx = findFrame(page_id);
    if (idx == SIZE_MAX) return; // Not in pool – nothing to flush.

    if (frames_[idx].isValid() && frames_[idx].isDirty()) {
        bool should_flush = true;
        if (pre_flush_hook_) {
            should_flush = pre_flush_hook_(page_id);
        }
        if (should_flush) {
            file_manager.writePage(frames_[idx].page());
            frames_[idx].markClean();
        }
    }
}

// ---------------------------------------------------------------------------
void BufferPool::flushAllPages(FileManager& file_manager) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& frame : frames_) {
        if (frame.isValid() && frame.isDirty()) {
            bool should_flush = true;
            if (pre_flush_hook_) {
                should_flush = pre_flush_hook_(frame.pageId());
            }
            if (should_flush) {
                file_manager.writePage(frame.page());
                frame.markClean();
            }
        }
    }
}

// ---------------------------------------------------------------------------
BufferPool::Stats BufferPool::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void BufferPool::resetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
}

// ---------------------------------------------------------------------------
std::size_t BufferPool::occupiedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (auto& f : frames_) {
        if (f.isValid()) ++count;
    }
    return count;
}

} // namespace walnut
