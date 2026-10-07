#include "walnut/txn/deadlock_detector.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace walnut {

DeadlockDetector::DeadlockDetector(LockManager& lock_manager, TransactionManager& txn_manager,
                                   std::chrono::milliseconds interval_ms)
    : lock_manager_(lock_manager), txn_manager_(txn_manager), interval_(interval_ms) {
    if (interval_.count() <= 0) throw std::invalid_argument("Deadlock detection interval must be positive");
    lock_manager_.SetDeadlockDetector(this);
}

DeadlockDetector::~DeadlockDetector() {
    Stop();
    lock_manager_.SetDeadlockDetector(nullptr);
}

void DeadlockDetector::Start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        worker_thread_ = std::thread(&DeadlockDetector::WorkerLoop, this);
}

void DeadlockDetector::Stop() {
    running_.store(false, std::memory_order_release);
    wait_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();
}

void DeadlockDetector::WorkerLoop() {
    std::unique_lock<std::mutex> lock(wait_mu_);
    while (running_.load(std::memory_order_acquire)) {
        if (wait_cv_.wait_for(lock, interval_, [this] {
                return !running_.load(std::memory_order_acquire);
            })) break;
        lock.unlock();
        DetectAndResolve();
        lock.lock();
    }
}

std::vector<txn_id_t> DeadlockDetector::FindCycles(
    const std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>>& graph) {
    std::unordered_map<txn_id_t, std::size_t> index;
    std::unordered_map<txn_id_t, std::size_t> low_link;
    std::unordered_set<txn_id_t> on_stack;
    std::vector<txn_id_t> stack;
    std::vector<txn_id_t> victims;
    std::size_t next_index = 0;
    std::function<void(txn_id_t)> visit = [&](txn_id_t node) {
        index[node] = low_link[node] = next_index++;
        stack.push_back(node);
        on_stack.insert(node);
        const auto edges = graph.find(node);
        if (edges != graph.end()) {
            for (txn_id_t neighbor : edges->second) {
                if (neighbor == node) continue;
                if (index.find(neighbor) == index.end()) {
                    visit(neighbor);
                    low_link[node] = std::min(low_link[node], low_link[neighbor]);
                } else if (on_stack.find(neighbor) != on_stack.end()) {
                    low_link[node] = std::min(low_link[node], index[neighbor]);
                }
            }
        }
        if (low_link[node] == index[node]) {
            std::vector<txn_id_t> component;
            while (!stack.empty()) {
                const txn_id_t member = stack.back();
                stack.pop_back();
                on_stack.erase(member);
                component.push_back(member);
                if (member == node) break;
            }
            bool cyclic = component.size() > 1;
            if (!cyclic) {
                const auto self_edges = graph.find(component.front());
                cyclic = self_edges != graph.end() &&
                         self_edges->second.find(component.front()) != self_edges->second.end();
            }
            if (cyclic) victims.push_back(SelectVictim(component));
        }
    };
    for (const auto& entry : graph)
        if (index.find(entry.first) == index.end()) visit(entry.first);
    return victims;
}

txn_id_t DeadlockDetector::SelectVictim(const std::vector<txn_id_t>& cycle_members) const {
    return *std::max_element(cycle_members.begin(), cycle_members.end());
}

int DeadlockDetector::DetectAndResolve() {
    const auto graph = lock_manager_.BuildWaitForGraph();
    const auto victims = FindCycles(graph);
    int aborted = 0;
    for (txn_id_t victim_id : victims) {
        Transaction* txn = txn_manager_.GetTransaction(victim_id);
        if (txn && txn->TryAbortGrowing()) {
            // The blocked AcquireLock call removes its own queue node after
            // waking; releasing all locks here would invalidate its iterator.
            lock_manager_.AbortTransaction(victim_id);
            ++aborted;
        }
    }
    return aborted;
}

} // namespace walnut
