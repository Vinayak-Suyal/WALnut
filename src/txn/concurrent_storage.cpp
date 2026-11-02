#include "walnut/txn/concurrent_storage.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace walnut {

ConcurrentStorageManager::ConcurrentStorageManager(
    StorageManager& storage_manager, TransactionManager& transaction_manager,
    LockManager& lock_manager)
    : storage_manager_(storage_manager),
      transaction_manager_(transaction_manager), lock_manager_(lock_manager) {}

std::uint64_t ConcurrentStorageManager::PlaceBid(
    const std::string& item_id, const std::string& user_id, double amount) {
    Transaction* txn = transaction_manager_.Begin();
    try {
        std::string first  = std::min(item_id, user_id);
        std::string second = std::max(item_id, user_id);
        LockMode first_mode  = (first == item_id) ? LockMode::EXCLUSIVE : LockMode::SHARED;
        LockMode second_mode = (second == item_id) ? LockMode::EXCLUSIVE : LockMode::SHARED;

        lock_manager_.AcquireLock(txn, first, first_mode);
        lock_manager_.AcquireLock(txn, second, second_mode);

        std::uint64_t bid_id = 0;
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            auto item = storage_manager_.getItem(item_id);
            if (!item.has_value()) {
                throw std::runtime_error("Item not found: " + item_id);
            }
            if (item->status == "CLOSED") {
                throw std::runtime_error("Auction is closed for: " + item_id);
            }
            if (amount <= item->current_bid) {
                throw std::runtime_error("Bid must be higher than current bid of " +
                                         std::to_string(item->current_bid));
            }
            if (!storage_manager_.findUserById(user_id).has_value()) {
                throw std::runtime_error("User not found: " + user_id);
            }

            Bid bid{0, item_id, user_id, amount, ""};
            bid_id = storage_manager_.insertBid(bid);
            storage_manager_.updateItemAfterBid(item_id, amount, user_id);
        }

        transaction_manager_.Commit(txn);
        return bid_id;

    } catch (const TransactionAbortedException&) {
        transaction_manager_.Abort(txn);
        throw;
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

void ConcurrentStorageManager::CreateUser(const std::string& user_id,
                                          const std::string& username) {
    Transaction* txn = transaction_manager_.Begin();
    try {
        lock_manager_.AcquireLock(txn, user_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            User user{user_id, username};
            storage_manager_.createUser(user);
        }
        transaction_manager_.Commit(txn);
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

void ConcurrentStorageManager::CreateItem(const std::string& item_id,
                                          const std::string& title,
                                          double starting_price) {
    Transaction* txn = transaction_manager_.Begin();
    try {
        lock_manager_.AcquireLock(txn, item_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            Item item{item_id, title, starting_price, "OPEN", starting_price, ""};
            storage_manager_.createItem(item);
        }
        transaction_manager_.Commit(txn);
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

void ConcurrentStorageManager::CloseItem(const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    try {
        lock_manager_.AcquireLock(txn, item_id, LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            storage_manager_.closeItem(item_id);
        }
        transaction_manager_.Commit(txn);
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

std::optional<Item> ConcurrentStorageManager::GetItem(
    const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    try {
        lock_manager_.AcquireLock(txn, item_id, LockMode::SHARED);
        std::optional<Item> item;
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            item = storage_manager_.getItem(item_id);
        }
        transaction_manager_.Commit(txn);
        return item;
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

std::vector<Item> ConcurrentStorageManager::GetAllItems() {
    Transaction* txn = transaction_manager_.Begin();
    try {
        lock_manager_.AcquireLock(txn, "catalog:items", LockMode::SHARED);
        std::vector<Item> items;
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            items = storage_manager_.getAllItems();
        }
        transaction_manager_.Commit(txn);
        return items;
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

std::vector<Bid> ConcurrentStorageManager::GetBids(
    const std::string& item_id) {
    Transaction* txn = transaction_manager_.Begin();
    try {
        lock_manager_.AcquireLock(txn, item_id, LockMode::SHARED);
        std::vector<Bid> bids;
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            bids = storage_manager_.getBids(item_id);
        }
        transaction_manager_.Commit(txn);
        return bids;
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

std::optional<User> ConcurrentStorageManager::FindUserById(
    const std::string& user_id) {
    Transaction* txn = transaction_manager_.Begin();
    try {
        lock_manager_.AcquireLock(txn, user_id, LockMode::SHARED);
        std::optional<User> user;
        {
            std::lock_guard<std::mutex> storage_lock(storage_mu_);
            user = storage_manager_.findUserById(user_id);
        }
        transaction_manager_.Commit(txn);
        return user;
    } catch (...) {
        transaction_manager_.Abort(txn);
        throw;
    }
}

} // namespace walnut
