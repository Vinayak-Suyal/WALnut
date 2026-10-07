#pragma once

#include "walnut/txn/transaction.hpp"

#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace walnut {

class DeadlockDetector;

struct LockRequest {
    txn_id_t txn_id;
    LockMode mode;
    bool granted = false;
    bool upgrading = false;
    LockMode requested_mode = LockMode::SHARED;
};

struct LockRequestQueue {
    std::list<LockRequest> requests;
    std::condition_variable cv;
    bool upgrading = false;
    txn_id_t upgrading_txn_id = 0;
};

class LockManager {
public:
    LockManager() = default;
    ~LockManager() = default;
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    bool AcquireLock(Transaction* txn, const std::string& resource_id, LockMode mode);
    void ReleaseAllLocks(Transaction* txn);
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> BuildWaitForGraph();
    void AbortTransaction(txn_id_t txn_id);
    void SetDeadlockDetector(DeadlockDetector* detector);

private:
    bool IsCompatible(const LockRequestQueue& queue, const LockRequest& request) const;
    void GrantWaitingRequests(LockRequestQueue& queue);

    std::unordered_map<std::string, std::unique_ptr<LockRequestQueue>> lock_table_;
    std::mutex table_mu_;
    std::unordered_map<txn_id_t, Transaction*> active_txns_;
    DeadlockDetector* deadlock_detector_ = nullptr;
};

class TransactionAbortedException : public std::runtime_error {
public:
    explicit TransactionAbortedException(txn_id_t txn_id)
        : std::runtime_error("Transaction " + std::to_string(txn_id) +
                             " aborted (deadlock victim)"), txn_id_(txn_id) {}
    txn_id_t GetTxnId() const noexcept { return txn_id_; }
private:
    txn_id_t txn_id_;
};

} // namespace walnut
