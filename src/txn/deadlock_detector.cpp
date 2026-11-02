#include "walnut/txn/deadlock_detector.hpp"

#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace walnut {

DeadlockDetector::DeadlockDetector(LockManager& lock_manager,
                                   TransactionManager& txn_manager,
                                   std::chrono::milliseconds interval_ms)
    : lock_manager_(lock_manager), txn_manager_(txn_manager), interval_(interval_ms) {
    if (interval_.count() <= 0) {
        throw std::invalid_argument("Deadlock detection interval must be positive");
    }
    lock_manager_.SetDeadlockDetector(this);
}

DeadlockDetector::~DeadlockDetector() {
    Stop();
    lock_manager_.SetDeadlockDetector(nullptr);
}

void DeadlockDetector::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;
    }
    worker_thread_ = std::thread(&DeadlockDetector::WorkerLoop, this);
}

void DeadlockDetector::Stop() {
    running_.store(false, std::memory_order_release);
    wait_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void DeadlockDetector::WorkerLoop() {
    std::unique_lock<std::mutex> lock(wait_mu_);
    while (running_.load(std::memory_order_acquire)) {
        if (wait_cv_.wait_for(lock, interval_, [this] {
                return !running_.load(std::memory_order_acquire);
            })) {
            break;
        }
        lock.unlock();
        DetectAndResolve();
        lock.lock();
    }
}

bool DeadlockDetector::DFS(
    txn_id_t node,
    const std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>>& graph,
    std::unordered_set<txn_id_t>& visited,
    std::unordered_set<txn_id_t>& rec_stack,
    std::vector<txn_id_t>& cycle_members) {
    visited.insert(node);
    rec_stack.insert(node);

    const auto found = graph.find(node);
    if (found != graph.end()) {
        for (txn_id_t neighbor : found->second) {
            if (neighbor == node) {
                continue;
            }
            if (rec_stack.find(neighbor) != rec_stack.end()) {
                cycle_members.push_back(neighbor);
                cycle_members.push_back(node);
                return true;
            }
            if (visited.find(neighbor) == visited.end()) {
                if (DFS(neighbor, graph, visited, rec_stack, cycle_members)) {
                    if (cycle_members.empty() || cycle_members.back() != cycle_members.front()) {
                        cycle_members.push_back(node);
                    }
                    return true;
                }
            }
        }
    }

    rec_stack.erase(node);
    return false;
}

std::vector<txn_id_t> DeadlockDetector::FindCycles(
    const std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>>& graph) {
    std::unordered_set<txn_id_t> visited;
    std::unordered_set<txn_id_t> rec_stack;
    std::vector<txn_id_t> victims;

    for (const auto& entry : graph) {
        if (visited.find(entry.first) == visited.end()) {
            std::vector<txn_id_t> cycle_members;
            if (DFS(entry.first, graph, visited, rec_stack, cycle_members)) {
                victims.push_back(SelectVictim(cycle_members));
            }
            rec_stack.clear();
        }
    }
    return victims;
}

txn_id_t DeadlockDetector::SelectVictim(
    const std::vector<txn_id_t>& cycle_members) const {
    return *std::max_element(cycle_members.begin(), cycle_members.end());
}

int DeadlockDetector::DetectAndResolve() {
    const auto graph = lock_manager_.BuildWaitForGraph();
    const auto victims = FindCycles(graph);
    int aborted = 0;
    for (txn_id_t victim_id : victims) {
        Transaction* txn = txn_manager_.GetTransaction(victim_id);
        if (txn != nullptr && txn->GetState() == TransactionState::GROWING) {
            if (txn->TryAbort()) {
                lock_manager_.AbortTransaction(victim_id);
                ++aborted;
            }
        }
    }
    return aborted;
}

} // namespace walnut
