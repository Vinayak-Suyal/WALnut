#pragma once

#include "walnut/storage/storage_manager.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace walnut {

class ConcurrentStorageManager {
public:
    ConcurrentStorageManager(StorageManager& storage_manager, TransactionManager& transaction_manager,
                             LockManager& lock_manager);

    uint64_t PlaceBid(const std::string& item_id, const std::string& user_id, double amount);
    void CreateUser(const std::string& user_id, const std::string& username);
    void CreateItem(const std::string& item_id, const std::string& title, double starting_price);
    void CloseItem(const std::string& item_id);

    std::optional<Item> GetItem(const std::string& item_id);
    std::vector<Item> GetAllItems();
    std::vector<Bid> GetBids(const std::string& item_id);
    std::optional<User> FindUserById(const std::string& user_id);

private:
    StorageManager& storage_manager_;
    TransactionManager& transaction_manager_;
    LockManager& lock_manager_;
    // StorageManager's indexes and FileManager are not thread-safe; this mutex
    // serializes wrapper-mediated calls, including multi-step bid updates.
    std::mutex storage_mu_;
};

} // namespace walnut
