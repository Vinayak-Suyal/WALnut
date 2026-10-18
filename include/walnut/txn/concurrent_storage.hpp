// =============================================================================
// WALnut – Concurrency / Transaction Layer (Sushmit Singh Rawat)
// include/walnut/txn/concurrent_storage.hpp
//
// ConcurrentStorageManager wraps StorageManager with Strict 2PL locking and
// ACID transaction boundaries.
//
// Schema v2.0 (OSDBMS-V-2026-T038) additions:
//   PlaceBid        – Anti-Shill guard, self-bidding guard, tiered minimum
//                     increment floor, soft-close anti-sniping extension
//   CancelAuction   – Strict 2PL race vs. OnTimerExpire; post-commit image
//                     file deletion via RegisterCommitAction
//   OnTimerExpire   – Sets CLOSED (winner) or UNSOLD (no bids), safe against
//                     concurrent cancel
//   GetItemsWonByUser / GetItemsByHost – relational query wrappers
//   GetItemsByCategory                 – category secondary index lookup (Phase 4)
//   UpdateUser      – credential / role update wrapper
// =============================================================================

#pragma once

#include "walnut/storage/storage_manager.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace walnut {

// ---------------------------------------------------------------------------
// Tiered minimum bid increment (spec §6.3 / §4.2)
//
// Δ_min(price) = $5.00                     if starting_price < $100
//              = max($10.00, 0.05 × price)  if starting_price ≥ $100
//
// A bid is accepted only if new_amount >= current_bid + Δ_min.
// ---------------------------------------------------------------------------
inline double calculateMinIncrement(double starting_price, double current_bid) {
    constexpr double TIER_THRESHOLD  = 100.0;
    constexpr double FIXED_INCREMENT = 5.0;
    constexpr double MIN_PREMIUM_INC = 10.0;
    constexpr double PERCENT_RATE    = 0.05;

    if (starting_price < TIER_THRESHOLD) {
        return FIXED_INCREMENT;
    }
    double active_price = (current_bid > 0.0) ? current_bid : starting_price;
    return std::max(MIN_PREMIUM_INC, std::ceil(active_price * PERCENT_RATE));
}

// ---------------------------------------------------------------------------
// ConcurrentStorageManager
// ---------------------------------------------------------------------------
class ConcurrentStorageManager {
public:
    ConcurrentStorageManager(StorageManager& storage_manager,
                             TransactionManager& transaction_manager,
                             LockManager& lock_manager);

    // -----------------------------------------------------------------------
    // Core auction operations (all ACID, Strict 2PL)
    // -----------------------------------------------------------------------

    /// Place a bid with full v2.0 defensive checks:
    ///   1. Anti-Shill: host_id != user_id
    ///   2. Self-bidding guard: current_winner != user_id
    ///   3. Tiered minimum increment floor (§6.3)
    ///   4. Soft-close anti-sniping (+30s when < 15s remain) (§6.4)
    /// Returns the assigned bid_id.
    uint64_t PlaceBid(const std::string& item_id,
                      const std::string& user_id,
                      double             amount);

    /// Create a new user.
    void CreateUser(const std::string& user_id, const std::string& username);

    /// Create a new item (simplified, no host_id or v2 fields – for test compat).
    void CreateItem(const std::string& item_id,
                    const std::string& title,
                    double             starting_price);

    /// Set item status to "CLOSED" (legacy path / internal use).
    void CloseItem(const std::string& item_id);

    /// Host-initiated cancellation (spec §6.5 / §4.4):
    ///   - Requires EXCLUSIVE lock on "item:<item_id>"
    ///   - Only OPEN or UNSOLD items may be cancelled; CLOSED = 409 Conflict
    ///   - Registers image file deletion as a post-commit action
    /// image_paths: relative filenames from item.image_urls (may be empty).
    void CancelAuction(const std::string&              item_id,
                       const std::vector<std::string>& image_paths = {});

    /// Timer expiry settlement (spec §6.5):
    ///   - Requires EXCLUSIVE lock on "item:<item_id>"
    ///   - Sets CLOSED if winner exists; UNSOLD if no bids
    ///   - Safely skips if item is already CANCEL or CLOSED
    void OnTimerExpire(const std::string& item_id);

    // -----------------------------------------------------------------------
    // Relational queries (v2.0)
    // -----------------------------------------------------------------------

    /// Return all CLOSED items won by user_id.
    std::vector<Item> GetItemsWonByUser(const std::string& user_id);

    /// Return all items hosted by host_user_id.
    std::vector<Item> GetItemsByHost(const std::string& host_user_id);

    /// Return all items (any status) belonging to the given category.
    /// Uses the StorageManager's O(1) category secondary index.
    std::vector<Item> GetItemsByCategory(const std::string& category);

    // -----------------------------------------------------------------------
    // Read / utility wrappers
    // -----------------------------------------------------------------------

    std::optional<Item> GetItem(const std::string& item_id);
    std::vector<Item>   GetAllItems();
    std::vector<Bid>    GetBids(const std::string& item_id);
    std::optional<User> FindUserById(const std::string& user_id);
    std::vector<User>   GetAllUsers();

    /// Permanently delete an item (Strict 2PL EXCLUSIVE on item:<item_id>).
    void DeleteItem(const std::string& item_id);

    /// Permanently delete a user (Strict 2PL EXCLUSIVE on user:<user_id>).
    void DeleteUser(const std::string& user_id);

    /// Purge all items and bids (keeps user accounts).
    void PurgeItems();

    /// Reset database files to fresh state.
    void ResetDatabase();

    /// Overwrite user record (credentials, role update).
    void UpdateUser(const User& user);

    void SetEnforceTieredIncrement(bool enforce) { enforce_tiered_increment_ = enforce; }
    bool GetEnforceTieredIncrement() const { return enforce_tiered_increment_; }

private:
    StorageManager&     storage_manager_;
    TransactionManager& transaction_manager_;
    LockManager&        lock_manager_;

    /// Single mutex serialising all StorageManager calls (SM is not thread-safe).
    std::mutex storage_mu_;
    bool enforce_tiered_increment_ = true;
};

} // namespace walnut
