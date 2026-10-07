#include "walnut/txn/transaction_manager.hpp"
#include "walnut/txn/lock_manager.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace walnut {

TransactionManager::TransactionManager(LockManager& lock_manager) : lock_manager_(lock_manager) {}

TransactionManager::~TransactionManager() {
    std::vector<Transaction*> active;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& entry : active_txns_) active.push_back(entry.second);
    }
    for (Transaction* txn : active) {
        try { Abort(txn); } catch (...) {}
    }
}

Transaction* TransactionManager::Begin() {
    const txn_id_t id = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
    auto transaction = std::make_unique<Transaction>(id);
    Transaction* raw = transaction.get();
    std::lock_guard<std::mutex> lock(mu_);
    owned_txns_.emplace(id, std::move(transaction));
    active_txns_.emplace(id, raw);
    return raw;
}

bool TransactionManager::IsActiveLocked(const Transaction* txn) const {
    if (!txn) return false;
    const auto it = active_txns_.find(txn->GetTxnId());
    return it != active_txns_.end() && it->second == txn;
}

void TransactionManager::RemoveActive(Transaction* txn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!txn) return;
    const auto it = active_txns_.find(txn->GetTxnId());
    if (it != active_txns_.end() && it->second == txn) active_txns_.erase(it);
}

void TransactionManager::Commit(Transaction* txn) {
    if (!txn) throw std::invalid_argument("Cannot commit a null transaction");
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!IsActiveLocked(txn)) {
            if (txn->GetState() == TransactionState::ABORTED)
                throw std::runtime_error("Cannot commit an aborted transaction");
            throw std::runtime_error("Transaction is not active");
        }
        if (txn->GetState() == TransactionState::ABORTED)
            throw std::runtime_error("Cannot commit an aborted transaction");
        if (txn->GetState() == TransactionState::GROWING)
            txn->SetState(TransactionState::SHRINKING);
    }
    lock_manager_.ReleaseAllLocks(txn);
    txn->SetState(TransactionState::COMMITTED);
    RemoveActive(txn);
}

void TransactionManager::Abort(Transaction* txn) {
    if (!txn) return;
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!IsActiveLocked(txn) || txn->GetState() == TransactionState::COMMITTED) return;
        if (txn->GetState() != TransactionState::ABORTED) txn->TryAbort();
    }
    std::exception_ptr rollback_error;
    try { txn->RollbackUndoActions(); } catch (...) { rollback_error = std::current_exception(); }
    lock_manager_.ReleaseAllLocks(txn);
    RemoveActive(txn);
    if (rollback_error) std::rethrow_exception(rollback_error);
}

Transaction* TransactionManager::GetTransaction(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = active_txns_.find(txn_id);
    return it == active_txns_.end() ? nullptr : it->second;
}

std::size_t TransactionManager::ActiveTransactionCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_txns_.size();
}

std::vector<txn_id_t> TransactionManager::GetActiveTransactionIds() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<txn_id_t> ids;
    ids.reserve(active_txns_.size());
    for (const auto& entry : active_txns_) ids.push_back(entry.first);
    return ids;
}

} // namespace walnut
