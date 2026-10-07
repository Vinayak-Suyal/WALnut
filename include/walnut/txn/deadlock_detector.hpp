#pragma once

#include "walnut/txn/transaction.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace walnut {

class LockManager;
class TransactionManager;

class DeadlockDetector {
public:
    DeadlockDetector(LockManager& lock_manager, TransactionManager& txn_manager,
                     std::chrono::milliseconds interval_ms = std::chrono::milliseconds(500));
    ~DeadlockDetector();
    DeadlockDetector(const DeadlockDetector&) = delete;
    DeadlockDetector& operator=(const DeadlockDetector&) = delete;

    void Start();
    void Stop();
    int DetectAndResolve();

private:
    void WorkerLoop();
    std::vector<txn_id_t> FindCycles(
        const std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>>& graph);
    txn_id_t SelectVictim(const std::vector<txn_id_t>& cycle_members) const;

    LockManager& lock_manager_;
    TransactionManager& txn_manager_;
    std::chrono::milliseconds interval_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
};

} // namespace walnut
