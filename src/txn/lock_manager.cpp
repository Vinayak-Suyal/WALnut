#include "walnut/txn/lock_manager.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace walnut {

bool LockManager::AcquireLock(Transaction* txn, const std::string& resource_id, LockMode mode) {
    if (!txn) throw std::invalid_argument("Cannot acquire a lock for a null transaction");
    if (resource_id.empty()) throw std::invalid_argument("Lock resource ID cannot be empty");
    if (txn->GetState() != TransactionState::GROWING)
        throw std::logic_error("Cannot acquire locks outside the GROWING state");

    std::unique_lock<std::mutex> lock(table_mu_);
    auto& queue_ptr = lock_table_[resource_id];
    if (!queue_ptr) queue_ptr = std::make_unique<LockRequestQueue>();
    LockRequestQueue& queue = *queue_ptr;
    const txn_id_t txn_id = txn->GetTxnId();
    active_txns_[txn_id] = txn;

    auto own_request = queue.requests.end();
    for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
        if (it->txn_id == txn_id) { own_request = it; break; }
    }
    if (own_request != queue.requests.end() && own_request->granted) {
        if (own_request->mode == mode ||
            (own_request->mode == LockMode::EXCLUSIVE && mode == LockMode::SHARED)) return true;
        if (own_request->mode == LockMode::SHARED && mode == LockMode::EXCLUSIVE) {
            if (queue.upgrading && queue.upgrading_txn_id != txn_id)
                throw TransactionAbortedException(txn_id);
            queue.upgrading = true;
            queue.upgrading_txn_id = txn_id;
            own_request->upgrading = true;
            own_request->requested_mode = LockMode::EXCLUSIVE;
            txn->SetWaitingFor(resource_id);
            GrantWaitingRequests(queue);
            if (!own_request->upgrading) {
                txn->SetWaitingFor({});
                txn->AddLock(resource_id, LockMode::EXCLUSIVE);
                return true;
            }
            while (own_request->upgrading) {
                if (txn->GetState() == TransactionState::ABORTED) {
                    queue.requests.erase(own_request);
                    queue.upgrading = false;
                    queue.upgrading_txn_id = 0;
                    txn->SetWaitingFor({});
                    GrantWaitingRequests(queue);
                    queue.cv.notify_all();
                    throw TransactionAbortedException(txn_id);
                }
                queue.cv.wait(lock);
            }
            txn->SetWaitingFor({});
            txn->AddLock(resource_id, LockMode::EXCLUSIVE);
            return true;
        }
    }

    queue.requests.push_back(LockRequest{txn_id, mode, false, false, mode});
    auto request = std::prev(queue.requests.end());
    if (IsCompatible(queue, *request)) {
        request->granted = true;
        txn->AddLock(resource_id, mode);
        return true;
    }
    txn->SetWaitingFor(resource_id);
    while (!request->granted) {
        if (txn->GetState() == TransactionState::ABORTED) {
            queue.requests.erase(request);
            txn->SetWaitingFor({});
            GrantWaitingRequests(queue);
            queue.cv.notify_all();
            throw TransactionAbortedException(txn_id);
        }
        queue.cv.wait(lock);
    }
    txn->SetWaitingFor({});
    txn->AddLock(resource_id, mode);
    return true;
}

bool LockManager::IsCompatible(const LockRequestQueue& queue, const LockRequest& request) const {
    const LockMode requested_mode = request.upgrading ? request.requested_mode : request.mode;
    bool reached_request = false;
    for (const auto& existing : queue.requests) {
        if (&existing == &request) { reached_request = true; continue; }
        if (!reached_request && (!existing.granted || existing.upgrading)) return false;
    }
    for (const auto& existing : queue.requests) {
        if (&existing == &request || existing.txn_id == request.txn_id || !existing.granted) continue;
        if (requested_mode == LockMode::EXCLUSIVE || existing.mode == LockMode::EXCLUSIVE)
            return false;
    }
    return true;
}

void LockManager::GrantWaitingRequests(LockRequestQueue& queue) {
    for (auto& request : queue.requests) {
        if (request.granted && !request.upgrading) continue;
        if (!IsCompatible(queue, request)) break;
        if (request.upgrading) {
            request.mode = request.requested_mode;
            request.upgrading = false;
            queue.upgrading = false;
            queue.upgrading_txn_id = 0;
        } else {
            request.granted = true;
        }
    }
}

void LockManager::ReleaseAllLocks(Transaction* txn) {
    if (!txn) return;
    std::lock_guard<std::mutex> lock(table_mu_);
    const txn_id_t txn_id = txn->GetTxnId();
    for (auto table_it = lock_table_.begin(); table_it != lock_table_.end();) {
        LockRequestQueue& queue = *table_it->second;
        for (auto request_it = queue.requests.begin(); request_it != queue.requests.end();) {
            if (request_it->txn_id == txn_id) request_it = queue.requests.erase(request_it);
            else ++request_it;
        }
        if (queue.upgrading && queue.upgrading_txn_id == txn_id) {
            queue.upgrading = false;
            queue.upgrading_txn_id = 0;
        }
        GrantWaitingRequests(queue);
        queue.cv.notify_all();
        if (queue.requests.empty()) table_it = lock_table_.erase(table_it);
        else ++table_it;
    }
    active_txns_.erase(txn_id);
}

std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> LockManager::BuildWaitForGraph() {
    std::lock_guard<std::mutex> lock(table_mu_);
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> graph;
    for (const auto& entry : lock_table_) {
        const LockRequestQueue& queue = *entry.second;
        std::unordered_set<txn_id_t> preceding_waiters;
        for (const auto& waiter : queue.requests) {
            const bool waiting = !waiter.granted || waiter.upgrading;
            if (waiting) {
                const LockMode requested_mode = waiter.upgrading ? waiter.requested_mode : waiter.mode;
                for (const auto& holder : queue.requests) {
                    if (!holder.granted || holder.txn_id == waiter.txn_id) continue;
                    if (requested_mode == LockMode::EXCLUSIVE || holder.mode == LockMode::EXCLUSIVE)
                        graph[waiter.txn_id].insert(holder.txn_id);
                }
                for (txn_id_t earlier : preceding_waiters)
                    if (earlier != waiter.txn_id) graph[waiter.txn_id].insert(earlier);
            }
            if (waiting) preceding_waiters.insert(waiter.txn_id);
        }
    }
    return graph;
}

void LockManager::AbortTransaction(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(table_mu_);
    for (auto& entry : lock_table_) {
        const auto& requests = entry.second->requests;
        const bool waiting = std::any_of(requests.begin(), requests.end(),
            [txn_id](const LockRequest& request) {
                return request.txn_id == txn_id && (!request.granted || request.upgrading);
            });
        if (waiting) entry.second->cv.notify_all();
    }
}

void LockManager::SetDeadlockDetector(DeadlockDetector* detector) {
    std::lock_guard<std::mutex> lock(table_mu_);
    deadlock_detector_ = detector;
}

} // namespace walnut
