#pragma once

#include "walnut/txn/transaction.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace walnut {

class LockManager;

class TransactionManager {
public:
    explicit TransactionManager(LockManager& lock_manager);
    ~TransactionManager();
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    Transaction* Begin();
    void Commit(Transaction* txn);
    void Abort(Transaction* txn);
    Transaction* GetTransaction(txn_id_t txn_id);
    std::size_t ActiveTransactionCount() const;
    std::vector<txn_id_t> GetActiveTransactionIds() const;

private:
    bool IsActiveLocked(const Transaction* txn) const;
    void RemoveActive(Transaction* txn);

    LockManager& lock_manager_;
    std::atomic<txn_id_t> next_txn_id_{1};
    std::unordered_map<txn_id_t, Transaction*> active_txns_;
    std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> owned_txns_;
    mutable std::mutex mu_;
    std::mutex lifecycle_mu_;
};

} // namespace walnut
