// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/storage/storage_manager.hpp
//
// StorageManager provides the high-level CRUD API for Users, Items, and Bids.
// It uses FileManager for disk I/O and maintains an in-memory index (map) for
// fast lookups.  The on-disk pages are the source of truth; the in-memory maps
// are rebuilt from disk at startup.
//
// Thread-safety: StorageManager is NOT thread-safe by itself.  External
// callers (Sushmit's transaction layer) must serialise access, or StorageManager
// methods must be called while holding an appropriate transaction lock.
//
// Integration points for other team members:
//   Sushmit – wrap every public method in a transaction; acquire item/user locks
//             before calling update methods.
//   Kanak   – call file_manager_.sync() after WAL flush before exposing
//             fetchPage to the buffer pool.
//   Suhavi  – use these methods directly from the fixed-operation API.
// =============================================================================

#pragma once

#include "walnut/common/types.hpp"
#include "walnut/storage/file_manager.hpp"
#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace walnut {

// ---------------------------------------------------------------------------
// StorageManager
// ---------------------------------------------------------------------------
class StorageManager {
public:
    /// Construct with the directory that holds walnut.db / walnut.meta.
    explicit StorageManager(const std::string& data_dir);
    ~StorageManager();

    // Non-copyable.
    StorageManager(const StorageManager&)            = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Open files, load metadata, rebuild in-memory indexes from disk.
    void initialize();

    /// Flush all dirty pages and close files cleanly.
    void close();

    // -----------------------------------------------------------------------
    // Users
    // -----------------------------------------------------------------------

    /// Persist a new user.
    /// Throws std::invalid_argument  – if user_id or username exceeds limits.
    /// Throws std::runtime_error     – if user_id already exists.
    void createUser(const User& user);

    /// Return the user with the given user_id, or std::nullopt if not found.
    std::optional<User> findUserById(const std::string& user_id) const;

    // -----------------------------------------------------------------------
    // Items
    // -----------------------------------------------------------------------

    /// Persist a new item.
    /// Throws std::invalid_argument  – if field lengths exceed limits.
    /// Throws std::runtime_error     – if item_id already exists.
    void createItem(const Item& item);

    /// Return the item with the given item_id, or std::nullopt if not found.
    std::optional<Item> getItem(const std::string& item_id) const;

    /// Return all items (scan all ITEMS pages).
    std::vector<Item> getAllItems() const;

    /// Update current_bid and current_winner on an existing item.
    /// Throws std::runtime_error – if item_id is not found.
    void updateItemAfterBid(const std::string& item_id,
                            double             new_current_bid,
                            const std::string& winner_user_id);

    /// Set item status to "CLOSED".
    /// Throws std::runtime_error – if item_id is not found.
    void closeItem(const std::string& item_id);

    // -----------------------------------------------------------------------
    // Bids
    // -----------------------------------------------------------------------

    /// Append a bid record and return the assigned bid_id.
    /// The Bid::bid_id field in the supplied struct is IGNORED; the returned
    /// value is the assigned ID.
    /// Throws std::invalid_argument – if field lengths exceed limits.
    uint64_t insertBid(const Bid& bid);

    /// Return all bids for the given item_id (sorted by bid_id ascending).
    std::vector<Bid> getBids(const std::string& item_id) const;

    /// Return the bid with the highest amount for the given item_id.
    std::optional<Bid> getHighestBid(const std::string& item_id) const;

    // -----------------------------------------------------------------------
    // Statistics / utilities
    // -----------------------------------------------------------------------

    /// Total number of allocated pages.
    uint32_t pageCount() const;

    /// Flush all dirty data to disk without closing.
    void sync();

    /// Expose the underlying FileManager (for BufferPool integration).
    FileManager& fileManager() { return file_manager_; }
    const FileManager& fileManager() const { return file_manager_; }

private:
    mutable FileManager file_manager_;

    // -----------------------------------------------------------------------
    // In-memory indexes (rebuilt from disk at startup).
    // Key: record primary key.  Value: {page_id, slot_index}.
    // -----------------------------------------------------------------------
    struct RecordLocation {
        uint32_t page_id;
        uint32_t slot;   ///< 0-indexed slot within that page
    };

    std::unordered_map<std::string, RecordLocation> user_index_;  ///< user_id -> loc
    std::unordered_map<std::string, RecordLocation> item_index_;  ///< item_id -> loc

    // For bids we only need item->pages and keep a list of page_ids per item
    // (bids are append-only; we scan all bid pages for getBids).
    // Bids index: item_id -> list of (page_id, slot)
    std::unordered_map<std::string,
        std::vector<RecordLocation>> bid_index_;  ///< item_id -> list of locs

    // Tracking of "current" page per table type (the page receiving appends).
    uint32_t current_user_page_{UINT32_MAX};  ///< UINT32_MAX = none allocated yet
    uint32_t current_item_page_{UINT32_MAX};
    uint32_t current_bid_page_{UINT32_MAX};

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Rebuild all in-memory indexes by scanning disk pages.
    void rebuildIndexes();

    /// Return the current writable page for `page_type`, or allocate one.
    /// If the current page is full, allocates a new one.
    uint32_t getOrAllocatePage(uint32_t page_type, std::size_t record_size,
                               uint32_t& current_page_tracker);

    /// Write a modified page back to disk (via file_manager_).
    void flushPage(uint32_t page_id, const Page& page);
};

} // namespace walnut
