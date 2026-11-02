#include "walnut/txn/lock_manager.hpp"

#include <stdexcept>

namespace walnut {

bool LockManager::AcquireLock(Transaction* txn, const std::string& resource_id,
                              LockMode mode) {
    if (txn == nullptr) {
        throw std::invalid_argument("Cannot acquire a lock for a null transaction");
    }
    if (resource_id.empty()) {
        throw std::invalid_argument("Lock resource ID cannot be empty");
    }
    if (txn->GetState() != TransactionState::GROWING) {
        throw std::logic_error("Cannot acquire locks outside the GROWING state");
    }

    std::unique_lock<std::mutex> lock(table_mu_);
    auto& queue_ptr = lock_table_[resource_id];
    if (!queue_ptr) {
        queue_ptr = std::make_unique<LockRequestQueue>();
    }
    LockRequestQueue& queue = *queue_ptr;
    const txn_id_t txn_id = txn->GetTxnId();
    active_txns_[txn_id] = txn;

    auto own_request = queue.requests.end();
    for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
        if (it->txn_id == txn_id) {
            own_request = it;
            break;
        }
    }

    if (own_request != queue.requests.end() && own_request->granted) {
        if (own_request->mode == mode ||
            (own_request->mode == LockMode::EXCLUSIVE && mode == LockMode::SHARED)) {
            return true;
        }
        if (own_request->mode == LockMode::SHARED && mode == LockMode::EXCLUSIVE) {
            if (queue.upgrading && queue.upgrading_txn_id != txn_id) {
                txn->SetState(TransactionState::ABORTED);
                throw TransactionAbortedException(txn_id);
            }
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
            if (txn->GetState() == TransactionState::ABORTED) {
                queue.requests.erase(own_request);
                queue.upgrading = false;
                queue.upgrading_txn_id = 0;
                txn->SetWaitingFor({});
                GrantWaitingRequests(queue);
                queue.cv.notify_all();
                throw TransactionAbortedException(txn_id);
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
    if (txn->GetState() == TransactionState::ABORTED) {
        queue.requests.erase(request);
        txn->SetWaitingFor({});
        GrantWaitingRequests(queue);
        queue.cv.notify_all();
        throw TransactionAbortedException(txn_id);
    }

    txn->SetWaitingFor({});
    txn->AddLock(resource_id, mode);
    return true;
}

bool LockManager::IsCompatible(const LockRequestQueue& queue,
                               const LockRequest& request) const {
    const LockMode requested_mode = request.upgrading
                                        ? request.requested_mode
                                        : request.mode;
    bool reached_request = false;
    for (const auto& existing : queue.requests) {
        if (&existing == &request) {
            reached_request = true;
            continue;
        }
        if (!reached_request && (!existing.granted || existing.upgrading)) {
            // A prior waiter reserves its place in the FIFO queue.
            return false;
        }
    }

    for (const auto& existing : queue.requests) {
        if (&existing == &request || existing.txn_id == request.txn_id ||
            !existing.granted) {
            continue;
        }
        if (requested_mode == LockMode::EXCLUSIVE ||
            existing.mode == LockMode::EXCLUSIVE) {
            return false;
        }
    }
    return true;
}

void LockManager::GrantWaitingRequests(LockRequestQueue& queue) {
    for (auto& request : queue.requests) {
        if (request.granted && !request.upgrading) {
            continue;
        }
        if (!IsCompatible(queue, request)) {
            break;
        }
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
    if (txn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(table_mu_);
    const txn_id_t txn_id = txn->GetTxnId();

    std::vector<LockInfo> locks = txn->GetLocks();
    std::unordered_set<std::string> resources;
    for (const auto& lock_info : locks) {
        resources.insert(lock_info.resource_id);
    }
    const std::string waiting_res = txn->GetWaitingFor();
    if (!waiting_res.empty()) {
        resources.insert(waiting_res);
    }

    for (const auto& resource_id : resources) {
        auto table_it = lock_table_.find(resource_id);
        if (table_it == lock_table_.end()) {
            continue;
        }
        LockRequestQueue& queue = *table_it->second;
        for (auto request_it = queue.requests.begin();
             request_it != queue.requests.end();) {
            if (request_it->txn_id == txn_id) {
                request_it = queue.requests.erase(request_it);
            } else {
                ++request_it;
            }
        }
        if (queue.upgrading && queue.upgrading_txn_id == txn_id) {
            queue.upgrading = false;
            queue.upgrading_txn_id = 0;
        }
        GrantWaitingRequests(queue);
        queue.cv.notify_all();
        if (queue.requests.empty()) {
            lock_table_.erase(table_it);
        }
    }
    active_txns_.erase(txn_id);
}

std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>>
LockManager::BuildWaitForGraph() {
    std::lock_guard<std::mutex> lock(table_mu_);
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> graph;

    for (const auto& entry : lock_table_) {
        const LockRequestQueue& queue = *entry.second;
        std::unordered_set<txn_id_t> holders;
        for (const auto& req : queue.requests) {
            if (req.granted && !req.upgrading) {
                holders.insert(req.txn_id);
            }
        }
        for (const auto& req : queue.requests) {
            if (!req.granted || req.upgrading) {
                for (txn_id_t holder : holders) {
                    if (holder != req.txn_id) {
                        graph[req.txn_id].insert(holder);
                    }
                }
            }
        }
    }
    return graph;
}

void LockManager::AbortTransaction(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(table_mu_);
    auto it = active_txns_.find(txn_id);
    if (it != active_txns_.end() && it->second != nullptr) {
        const std::string waiting_res = it->second->GetWaitingFor();
        if (!waiting_res.empty()) {
            auto q_it = lock_table_.find(waiting_res);
            if (q_it != lock_table_.end()) {
                q_it->second->cv.notify_all();
                return;
            }
        }
    }
    for (auto& entry : lock_table_) {
        const auto& requests = entry.second->requests;
        const bool transaction_waiting = std::any_of(
            requests.begin(), requests.end(), [txn_id](const LockRequest& request) {
                return request.txn_id == txn_id && (!request.granted || request.upgrading);
            });
        if (transaction_waiting) {
            entry.second->cv.notify_all();
        }
    }
}

void LockManager::SetDeadlockDetector(DeadlockDetector* detector) {
    std::lock_guard<std::mutex> lock(table_mu_);
    deadlock_detector_ = detector;
}

} // namespace walnut
