// =============================================================================
// WALnut – Concurrency / Transaction Layer (Sushmit Singh Rawat)
// src/txn/concurrent_storage.cpp
//
// Implements ConcurrentStorageManager with Strict 2PL ACID transaction
// boundaries and all Schema v2.0 auction-engine defences.
//
// Guard inventory (all enforced inside PlaceBid):
//   1. Anti-Shill Guard        – host_id != user_id  (spec §4.1 / §6.1)
//   2. Self-Bidding Guard       – current_winner != user_id  (spec §4.1 / §6.2)
//   3. Tiered Increment Floor   – Δ_min: $5 flat (<$100) / max($10, 5%) (§6.3)
//   4. Soft-Close Anti-Sniping  – +30s extension when end_time ≤ 15s away (§6.4)
//
// Race-condition resolution (spec §6.5):
//   CancelAuction + OnTimerExpire both require EXCLUSIVE lock on "item:<id>".
//   Strict 2PL guarantees only one proceeds; the other observes the terminal
//   status and exits cleanly.
//
// Post-commit image deletion (spec §4.3):
//   Filesystem removal is registered as a CommitAction so it only fires after
//   the storage transaction durably commits.  If the transaction rolls back,
//   the image file is preserved.
// =============================================================================

#include "walnut/txn/concurrent_storage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace walnut {

// ---------------------------------------------------------------------------
// RAII guard: automatically aborts the transaction on exception unless
// Commit() is explicitly called.
// ---------------------------------------------------------------------------
namespace {
class TransactionGuard {
public:
    TransactionGuard(TransactionManager& manager, Transaction* txn)
        : manager_(manager), txn_(txn) {}
    ~TransactionGuard() {
        if (!finished_) {
            try { manager_.Abort(txn_); } catch (...) {}
        }
    }
    void Commit() { manager_.Commit(txn_); finished_ = true; }
    void Abort()  { manager_.Abort(txn_);  finished_ = true; }
private:
    TransactionManager& manager_;
    Transaction*        txn_;
    bool                finished_ = false;
};
} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ConcurrentStorageManager::ConcurrentStorageManager(
        StorageManager& storage_manager,
        TransactionManager& transaction_manager,
        LockManager& lock_manager)
    : storage_manager_(storage_manager),
      transaction_manager_(transaction_manager),
      lock_manager_(lock_manager) {}

// ===========================================================================
// PlaceBid  (spec §4.1 – §4.4 / §6.1 – §6.4)
// ===========================================================================
uint64_t ConcurrentStorageManager::PlaceBid(const std::string& item_id,
                                             const std::string& user_id,
                                             double             amount) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        // Consistent lock ordering by lexicographic resource key to avoid
        // deadlock cycles between concurrent PlaceBid calls (spec §4.4).
        const std::string item_resource = "item:" + item_id;
        const std::string user_resource = "user:" + user_id;
        if (item_resource < user_resource) {
            lock_manager_.AcquireLock(txn, item_resource, LockMode::EXCLUSIVE);
            lock_manager_.AcquireLock(txn, user_resource, LockMode::SHARED);
        } else {
            lock_manager_.AcquireLock(txn, user_resource, LockMode::SHARED);
            lock_manager_.AcquireLock(txn, item_resource, LockMode::EXCLUSIVE);
        }

        uint64_t bid_id = 0;
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);

            // ---- Fetch item & user ----------------------------------------
            const auto item_opt = storage_manager_.getItem(item_id);
            if (!item_opt)
                throw std::runtime_error("Item not found: " + item_id);
            const Item& item = *item_opt;

            if (!storage_manager_.findUserById(user_id))
                throw std::runtime_error("User not found: " + user_id);

            // ---- Guard 1: Auction must be OPEN ----------------------------
            if (item.status != "OPEN")
                throw std::runtime_error(
                    "Auction is not open for item: " + item_id +
                    " (status=" + item.status + ")");

            // ---- Guard 2: Anti-Shill (spec §6.1) -------------------------
            // Host cannot bid on their own lot to artificially inflate price.
            if (!item.host_id.empty() && item.host_id == user_id)
                throw std::runtime_error(
                    "Anti-Shill Violation: Host cannot bid on their own item (" +
                    item_id + ")");

            // ---- Guard 3: Self-Bidding Prevention (spec §6.2) ------------
            // Leading bidder cannot redundantly outbid themselves.
            if (!item.current_winner.empty() && item.current_winner == user_id)
                throw std::runtime_error(
                    "Self-Bidding Violation: You are already the highest bidder "
                    "for item: " + item_id);

            // ---- Guard 1b: Timer Expiration Check -----------------------
            if (item.end_time_ms > 0) {
                using namespace std::chrono;
                uint64_t now_ms = static_cast<uint64_t>(
                    duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count());
                if (now_ms >= item.end_time_ms) {
                    try {
                        if (item.total_bid_count > 0 || !item.current_winner.empty()) {
                            storage_manager_.closeItem(item_id);
                        } else {
                            storage_manager_.markUnsoldItem(item_id);
                        }
                    } catch (...) {}
                    throw std::runtime_error("Auction has ended: lot is closed.");
                }
            }

            // ---- Guard 4: Minimum Increment Floor ------------------------
            double min_increment = 1.0;
            if (enforce_tiered_increment_) {
                min_increment = calculateMinIncrement(item.starting_price, item.current_bid);
            }
            double min_required = item.current_bid + min_increment;
            if (amount < min_required)
                throw std::runtime_error(
                    "Bid too low: minimum required is " +
                    std::to_string(min_required) +
                    " (current=" + std::to_string(item.current_bid) +
                    ", Δ_min=" + std::to_string(min_increment) + ")");


            // ---- Guard 5: Soft-Close Anti-Sniping Extension (spec §6.4) --
            // If bid arrives within the final 15 seconds, extend by +30s.
            // end_time_ms == 0 means no timer set (infinite / demo mode).
            if (item.end_time_ms > 0) {
                using namespace std::chrono;
                uint64_t now_ms = static_cast<uint64_t>(
                    duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count());

                constexpr uint64_t SNIPE_WINDOW_MS = 15'000;  // 15 seconds
                constexpr uint64_t EXTENSION_MS    = 30'000;  // 30 seconds

                if (item.end_time_ms > now_ms &&
                    (item.end_time_ms - now_ms) <= SNIPE_WINDOW_MS) {
                    storage_manager_.extendItemEndTime(
                        item_id, now_ms + EXTENSION_MS);
                }
            }

            // ---- Commit bid to WAL trail + item podium --------------------
            bid_id = storage_manager_.insertBid(
                Bid{0, item_id, user_id, amount, {}});
            storage_manager_.updateItemAfterBid(item_id, amount, user_id, bid_id);
        }

        guard.Commit();
        return bid_id;

    } catch (...) {
        guard.Abort();
        throw;
    }
}

// ===========================================================================
// CancelAuction  (spec §6.5 / §4.3)
// ===========================================================================
void ConcurrentStorageManager::CancelAuction(
        const std::string&              item_id,
        const std::vector<std::string>& image_paths) {

    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        // Both CancelAuction and OnTimerExpire require EXCLUSIVE lock on
        // the same key; Strict 2PL guarantees serialisable ordering.
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::EXCLUSIVE);

        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);

            const auto item_opt = storage_manager_.getItem(item_id);
            if (!item_opt)
                throw std::runtime_error("CancelAuction: item not found: " + item_id);

            const std::string& status = item_opt->status;

            // CLOSED items cannot be cancelled — contractual ownership has
            // transferred to the winning bidder (spec §6.5).
            if (status == "CLOSED")
                throw std::runtime_error(
                    "CancelAuction: item " + item_id +
                    " is already CLOSED – cancellation is not permitted "
                    "after auction settlement");

            // If the timer already expired and set CANCEL, silently succeed.
            if (status == "CANCEL") {
                guard.Commit();
                return;
            }

            // Eligible statuses: "OPEN", "UNSOLD"
            storage_manager_.cancelItem(item_id);
        }

        // Register post-commit image file deletion (spec §4.3).
        // Uses std::filesystem::remove; swallows individual errors to avoid
        // rolling back a valid cancellation due to a missing file.
        for (const std::string& rel_path : image_paths) {
            if (rel_path.empty()) continue;
            txn->RegisterCommitAction([rel_path]() noexcept {
                std::error_code ec;
                std::filesystem::remove(
                    std::filesystem::path("data/uploads") / rel_path, ec);
                // ec is intentionally ignored (best-effort cleanup).
            });
        }

        guard.Commit();   // → RunCommitActions() fires file deletions here

    } catch (...) {
        guard.Abort();
        throw;
    }
}

// ===========================================================================
// OnTimerExpire  (spec §6.5 – timer thread settlement)
// ===========================================================================
void ConcurrentStorageManager::OnTimerExpire(const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::EXCLUSIVE);

        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);

            const auto item_opt = storage_manager_.getItem(item_id);
            if (!item_opt) {
                guard.Commit();
                return;
            }

            const Item& item = *item_opt;

            // If CancelAuction won the lock first, respect the CANCEL state
            // and exit cleanly (no double-settlement).
            if (item.status == "CANCEL" || item.status == "CLOSED" ||
                item.status == "UNSOLD") {
                guard.Commit();
                return;
            }

            // Settle: CLOSED if a winner exists, UNSOLD otherwise.
            if (!item.current_winner.empty()) {
                storage_manager_.closeItem(item_id);      // status → "CLOSED"
            } else {
                storage_manager_.markUnsoldItem(item_id); // status → "UNSOLD"
            }
        }

        guard.Commit();

    } catch (...) {
        guard.Abort();
        throw;
    }
}

// ===========================================================================
// CreateUser
// ===========================================================================
void ConcurrentStorageManager::CreateUser(const std::string& user_id,
                                           const std::string& username) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "user:" + user_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            storage_manager_.createUser(User{user_id, username});
        }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

// ===========================================================================
// CreateItem
// ===========================================================================
void ConcurrentStorageManager::CreateItem(const std::string& item_id,
                                           const std::string& title,
                                           double             starting_price) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            storage_manager_.createItem(
                Item{item_id, title, starting_price, "OPEN", 0.0, {}});
        }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

// ===========================================================================
// CloseItem
// ===========================================================================
void ConcurrentStorageManager::CloseItem(const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            storage_manager_.closeItem(item_id);
        }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

// ===========================================================================
// UpdateUser
// ===========================================================================
void ConcurrentStorageManager::UpdateUser(const User& user) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "user:" + user.user_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            storage_manager_.updateUser(user);
        }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

// ===========================================================================
// Relational queries
// ===========================================================================
std::vector<Item> ConcurrentStorageManager::GetItemsWonByUser(
        const std::string& user_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "catalog:items", LockMode::SHARED);
        std::vector<Item> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.getItemsWonByUser(user_id);
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

std::vector<Item> ConcurrentStorageManager::GetItemsByHost(
        const std::string& host_user_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "catalog:items", LockMode::SHARED);
        std::vector<Item> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.getItemsByHost(host_user_id);
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

// ===========================================================================
// GetItemsByCategory  (Phase 4 – category secondary index)
// ===========================================================================
std::vector<Item> ConcurrentStorageManager::GetItemsByCategory(
        const std::string& category) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "catalog:items", LockMode::SHARED);
        std::vector<Item> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.getItemsByCategory(category);
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

// ===========================================================================
// Read wrappers
// ===========================================================================
std::optional<Item> ConcurrentStorageManager::GetItem(
        const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::SHARED);
        std::optional<Item> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.getItem(item_id);
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

std::vector<Item> ConcurrentStorageManager::GetAllItems() {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "catalog:items", LockMode::SHARED);
        std::vector<Item> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.getAllItems();
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

std::vector<Bid> ConcurrentStorageManager::GetBids(const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::SHARED);
        std::vector<Bid> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.getBids(item_id);
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

std::optional<User> ConcurrentStorageManager::FindUserById(
        const std::string& user_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "user:" + user_id, LockMode::SHARED);
        std::optional<User> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.findUserById(user_id);
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

std::vector<User> ConcurrentStorageManager::GetAllUsers() {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "catalog:users", LockMode::SHARED);
        std::vector<User> result;
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            result = storage_manager_.getAllUsers();
        }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

void ConcurrentStorageManager::DeleteItem(const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            storage_manager_.deleteItem(item_id);
        }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

void ConcurrentStorageManager::DeleteUser(const std::string& user_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "user:" + user_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            storage_manager_.deleteUser(user_id);
        }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

void ConcurrentStorageManager::PurgeItems() {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "catalog:items", LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lock(storage_mu_);
            storage_manager_.purgeItems();
        }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

void ConcurrentStorageManager::ResetDatabase() {
    std::lock_guard<std::mutex> lock(storage_mu_);
    storage_manager_.resetDatabase();
}

} // namespace walnut
