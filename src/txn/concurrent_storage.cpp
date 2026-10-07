#include "walnut/txn/concurrent_storage.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace walnut {
namespace {
class TransactionGuard {
public:
    TransactionGuard(TransactionManager& manager, Transaction* txn) : manager_(manager), txn_(txn) {}
    ~TransactionGuard() { if (!finished_) { try { manager_.Abort(txn_); } catch (...) {} } }
    void Commit() { manager_.Commit(txn_); finished_ = true; }
    void Abort() { manager_.Abort(txn_); finished_ = true; }
private:
    TransactionManager& manager_;
    Transaction* txn_;
    bool finished_ = false;
};
}

ConcurrentStorageManager::ConcurrentStorageManager(StorageManager& storage_manager,
    TransactionManager& transaction_manager, LockManager& lock_manager)
    : storage_manager_(storage_manager), transaction_manager_(transaction_manager), lock_manager_(lock_manager) {}

uint64_t ConcurrentStorageManager::PlaceBid(const std::string& item_id,
                                            const std::string& user_id, double amount) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        const std::string item_resource = "item:" + item_id;
        const std::string user_resource = "user:" + user_id;
        if (item_resource < user_resource) {
            lock_manager_.AcquireLock(txn, item_resource, LockMode::EXCLUSIVE);
            lock_manager_.AcquireLock(txn, user_resource, LockMode::SHARED);
        } else {
            lock_manager_.AcquireLock(txn, user_resource, LockMode::SHARED);
            lock_manager_.AcquireLock(txn, item_resource, LockMode::EXCLUSIVE);
        }
        uint64_t bid_id;
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            const auto item = storage_manager_.getItem(item_id);
            if (!item) throw std::runtime_error("Item not found: " + item_id);
            if (item->status != "OPEN") throw std::runtime_error("Auction is closed for: " + item_id);
            if (amount <= item->current_bid)
                throw std::runtime_error("Bid must be higher than current bid of " + std::to_string(item->current_bid));
            if (!storage_manager_.findUserById(user_id)) throw std::runtime_error("User not found: " + user_id);
            bid_id = storage_manager_.insertBid(Bid{0, item_id, user_id, amount, {}});
            storage_manager_.updateItemAfterBid(item_id, amount, user_id);
        }
        guard.Commit();
        return bid_id;
    } catch (...) {
        guard.Abort();
        throw;
    }
}

void ConcurrentStorageManager::CreateUser(const std::string& user_id, const std::string& username) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "user:" + user_id, LockMode::EXCLUSIVE);
        { std::lock_guard<std::mutex> lock(storage_mu_); storage_manager_.createUser(User{user_id, username}); }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

void ConcurrentStorageManager::CreateItem(const std::string& item_id, const std::string& title,
                                          double starting_price) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::EXCLUSIVE);
        { std::lock_guard<std::mutex> lock(storage_mu_);
          storage_manager_.createItem(Item{item_id, title, starting_price, "OPEN", 0.0, {}}); }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

void ConcurrentStorageManager::CloseItem(const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::EXCLUSIVE);
        { std::lock_guard<std::mutex> lock(storage_mu_); storage_manager_.closeItem(item_id); }
        guard.Commit();
    } catch (...) { guard.Abort(); throw; }
}

std::optional<Item> ConcurrentStorageManager::GetItem(const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "item:" + item_id, LockMode::SHARED);
        std::optional<Item> result;
        { std::lock_guard<std::mutex> lock(storage_mu_); result = storage_manager_.getItem(item_id); }
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
        { std::lock_guard<std::mutex> lock(storage_mu_); result = storage_manager_.getAllItems(); }
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
        { std::lock_guard<std::mutex> lock(storage_mu_); result = storage_manager_.getBids(item_id); }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

std::optional<User> ConcurrentStorageManager::FindUserById(const std::string& user_id) {
    Transaction* txn = transaction_manager_.Begin();
    TransactionGuard guard(transaction_manager_, txn);
    try {
        lock_manager_.AcquireLock(txn, "user:" + user_id, LockMode::SHARED);
        std::optional<User> result;
        { std::lock_guard<std::mutex> lock(storage_mu_); result = storage_manager_.findUserById(user_id); }
        guard.Commit();
        return result;
    } catch (...) { guard.Abort(); throw; }
}

} // namespace walnut
