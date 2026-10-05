// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/buffer/buffer_pool.hpp
//
// Thread-safe buffer pool with Clock page-replacement policy.
//
// Thread-safety contract:
//   All public methods acquire mutex_ before touching shared state.
//   Lock is released before any I/O call to FileManager to avoid holding the
//   pool lock across slow disk operations (but the current simple
//   implementation holds the lock throughout to keep the logic correct;
//   a reader-writer lock upgrade is a future optimisation).
//
// Integration notes:
//   Sushmit – transaction layer should call pinPage() before operating on a
//             page and unpinPage() when done; never assume a page stays in the
//             pool between calls without pinning.
//   Kanak   – WAL layer should call flushPage() only after the WAL record for
//             that page has been flushed (Write-Ahead-Log rule). A hook /
//             callback can be registered via setPreFlushHook() for this.
//   Suhavi  – call fetchPage() through StorageManager's CRUD; don't bypass it.
// =============================================================================

#pragma once

#include "walnut/buffer/frame.hpp"
#include "walnut/storage/file_manager.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <stdexcept>

namespace walnut {

class BufferPool {
public:
    // -----------------------------------------------------------------------
    // Statistics
    // -----------------------------------------------------------------------
    struct Stats {
        std::size_t hits           = 0; ///< Page found in pool
        std::size_t misses         = 0; ///< Page not found; read from disk
        std::size_t evictions      = 0; ///< Total frames evicted
        std::size_t dirty_evictions = 0; ///< Evictions that required a flush
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// frame_count: number of in-memory frames (default DEFAULT_BUFFER_POOL_SIZE).
    explicit BufferPool(std::size_t frame_count = DEFAULT_BUFFER_POOL_SIZE);
    ~BufferPool() = default;

    // Non-copyable, non-movable (mutex member).
    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&)                 = delete;
    BufferPool& operator=(BufferPool&&)      = delete;

    // -----------------------------------------------------------------------
    // Core buffer-pool operations
    // -----------------------------------------------------------------------

    /// Fetch a page into the pool (if not already there).
    /// The page's pin count is incremented before returning.
    /// Throws std::runtime_error if all frames are pinned (pool exhausted).
    Page& fetchPage(uint32_t page_id, FileManager& file_manager);

    /// Increment the pin count of a page already in the pool.
    /// Throws std::runtime_error if the page is not currently in the pool.
    void pinPage(uint32_t page_id);

    /// Decrement the pin count of a page.
    /// If `dirty` is true, marks the frame dirty (will be flushed on eviction
    /// or explicit flushPage() call).
    /// Throws std::runtime_error if the page is not in the pool.
    void unpinPage(uint32_t page_id, bool dirty);

    /// Flush a specific page to disk if it is dirty.
    /// Does nothing if the page is not in the pool.
    void flushPage(uint32_t page_id, FileManager& file_manager);

    /// Flush all dirty pages to disk.
    void flushAllPages(FileManager& file_manager);

    // -----------------------------------------------------------------------
    // WAL integration hook (for Kanak)
    // -----------------------------------------------------------------------

    /// Register a callback invoked just before a dirty page is flushed.
    /// The callback receives the page_id being flushed.  If the callback
    /// returns false, the flush is skipped (WAL-not-yet-flushed guard).
    /// Default: no hook (always flush).
    using PreFlushHook = std::function<bool(uint32_t page_id)>;
    void setPreFlushHook(PreFlushHook hook) { pre_flush_hook_ = std::move(hook); }

    // -----------------------------------------------------------------------
    // Statistics
    // -----------------------------------------------------------------------
    Stats  getStats()  const;
    void   resetStats();

    std::size_t frameCount() const { return frame_count_; }

    /// Return how many frames are currently occupied.
    std::size_t occupiedFrames() const;

private:
    std::vector<Frame>                     frames_;
    std::size_t                            frame_count_;
    std::unordered_map<uint32_t, std::size_t> page_table_; ///< page_id -> frame index

    Stats       stats_;
    std::size_t clock_hand_{0};

    mutable std::mutex mutex_;

    PreFlushHook pre_flush_hook_;

    // -----------------------------------------------------------------------
    // Internal helpers (called with mutex_ already held)
    // -----------------------------------------------------------------------

    /// Return frame index for page_id, or SIZE_MAX if not found.
    std::size_t findFrame(uint32_t page_id) const;

    /// Run the Clock algorithm; return an evictable frame index.
    /// Throws std::runtime_error if all frames are pinned.
    std::size_t selectVictim();

    /// Evict the frame at frame_index.  If dirty and pre_flush_hook_ allows,
    /// flush to disk first.
    void evictFrame(std::size_t frame_index, FileManager& file_manager);
};

} // namespace walnut
