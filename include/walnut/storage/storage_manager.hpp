// =============================================================================
// WALnut – Storage / Buffer Module
// include/walnut/storage/storage_manager.hpp
//
// StorageManager provides the high-level CRUD API for Users, Items, and Bids.
// It uses FileManager for disk I/O and maintains an in-memory index (map) for
// fast lookups.  The on-disk pages are the source of truth; the in-memory maps
// are rebuilt from disk at startup.
//
// Schema v2.0 (OSDBMS-V-2026-T038) additions:
//   - updateItemAfterBid  : extended with bid_id + timestamp_ms for Top-3 podium
//   - cancelItem          : sets status "CANCEL" (host cancellation path)
//   - getItemsWonByUser   : relational query – items where current_winner == uid
//                           and status == "CLOSED"
//   - getItemsByHost      : relational query – all items where host_id == uid
//   - updateUser          : persists changes to a user record (credentials, role)
//
// Phase 4 (OSDBMS-V-2026-T038):
//   - getItemsByCategory  : O(1) category lookup via in-memory secondary index
//
// Thread-safety: StorageManager is NOT thread-safe by itself.  The transaction
// layer (ConcurrentStorageManager) must serialise access via storage_mu_.
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

    /// Return all registered users.
    std::vector<User> getAllUsers() const;

    /// Permanently delete a user from disk and remove from in-memory index.
    void deleteUser(const std::string& user_id);

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

    /// Permanently delete an item from disk and indices.
    void deleteItem(const std::string& item_id);

    /// Purge all items and bids from disk (keeps users).
    void purgeItems();

    /// Factory reset: wipe walnut.db, walnut.meta, and re-initialize.
    void resetDatabase();

    /// Update current_bid, current_winner, and the Top-3 bid podium.
    /// bid_id and timestamp_ms must be the values assigned by insertBid so that
    /// the embedded TopBid slot is consistent with the WAL audit trail.
    /// Throws std::runtime_error – if item_id is not found.
    void updateItemAfterBid(const std::string& item_id,
                            double             new_current_bid,
                            const std::string& winner_user_id,
                            uint64_t           bid_id        = 0,
                            uint64_t           timestamp_ms  = 0);

    /// Set item status to "CLOSED" (auction settled with a winner).
    /// Throws std::runtime_error – if item_id is not found.
    void closeItem(const std::string& item_id);

    /// Set item status to "CANCEL" (host-initiated cancellation).
    /// Throws std::runtime_error – if item_id is not found.
    void cancelItem(const std::string& item_id);

    /// Set item status to "UNSOLD" (timer expired with no bids).
    /// Throws std::runtime_error – if item_id is not found.
    void markUnsoldItem(const std::string& item_id);

    /// Extend the auction end time to new_end_time_ms (soft-close anti-sniping).
    /// Throws std::runtime_error – if item_id is not found.
    void extendItemEndTime(const std::string& item_id, uint64_t new_end_time_ms);

    // -----------------------------------------------------------------------
    // Relational queries (Schema v2.0)
    // -----------------------------------------------------------------------

    /// Return all CLOSED items whose current_winner matches user_id.
    /// Used to build the user's "Won Gallery" dashboard tab.
    std::vector<Item> getItemsWonByUser(const std::string& user_id) const;

    /// Return all items whose host_id matches host_user_id.
    /// Used to populate the Host Console item listing table.
    std::vector<Item> getItemsByHost(const std::string& host_user_id) const;

    /// Return all items (any status) belonging to the given category.
    /// Implemented via the in-memory category_index_ (O(k) where k = category size).
    /// Returns an empty vector if the category is unknown.
    std::vector<Item> getItemsByCategory(const std::string& category) const;

    // -----------------------------------------------------------------------
    // Users (extended)
    // -----------------------------------------------------------------------

    /// Overwrite the persisted User record with the supplied struct.
    /// Throws std::runtime_error – if user_id is not found.
    void updateUser(const User& user);

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

    // Secondary index: category -> list of item_ids in that category.
    // Populated at startup (rebuildIndexes) and maintained by createItem.
    // Cancelled/unsold items stay in the index; callers filter by status.
    std::unordered_map<std::string, std::vector<std::string>> category_index_;

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
};

} // namespace walnut
