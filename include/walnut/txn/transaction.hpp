#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace walnut {

using txn_id_t = std::uint64_t;

enum class TransactionState : std::uint8_t { GROWING, SHRINKING, COMMITTED, ABORTED };
enum class LockMode : std::uint8_t { SHARED, EXCLUSIVE };

struct LockInfo {
    std::string resource_id;
    LockMode mode;
};

using UndoAction = std::function<void()>;

class Transaction {
public:
    explicit Transaction(txn_id_t txn_id)
        : txn_id_(txn_id), state_(TransactionState::GROWING),
          start_time_(std::chrono::steady_clock::now()) {}

    txn_id_t GetTxnId() const noexcept { return txn_id_; }
    TransactionState GetState() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    std::chrono::steady_clock::time_point GetStartTime() const noexcept { return start_time_; }

    void SetState(TransactionState new_state) {
        TransactionState current = state_.load(std::memory_order_acquire);
        for (;;) {
            if (current == new_state) return;
            if (current == TransactionState::ABORTED || current == TransactionState::COMMITTED)
                throw std::logic_error("Cannot transition a terminal transaction");
            if (new_state == TransactionState::GROWING)
                throw std::logic_error("A transaction cannot return to GROWING");
            if (state_.compare_exchange_weak(current, new_state, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) return;
        }
    }

    bool TryAbort() noexcept {
        TransactionState current = state_.load(std::memory_order_acquire);
        while (current == TransactionState::GROWING || current == TransactionState::SHRINKING) {
            if (state_.compare_exchange_weak(current, TransactionState::ABORTED,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) return true;
        }
        return false;
    }

    bool TryAbortGrowing() noexcept {
        TransactionState expected = TransactionState::GROWING;
        return state_.compare_exchange_strong(expected, TransactionState::ABORTED,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    void AddLock(const std::string& resource_id, LockMode mode) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& held : locks_) {
            if (held.resource_id == resource_id) { held.mode = mode; return; }
        }
        locks_.push_back(LockInfo{resource_id, mode});
    }

    std::vector<LockInfo> GetLocks() const {
        std::lock_guard<std::mutex> lock(mu_);
        return locks_;
    }

    void PushUndoAction(UndoAction action) {
        if (!action) throw std::invalid_argument("Undo action must be callable");
        std::lock_guard<std::mutex> lock(mu_);
        undo_actions_.push_back(std::move(action));
    }

    void RollbackUndoActions() {
        std::vector<UndoAction> actions;
        {
            std::lock_guard<std::mutex> lock(mu_);
            actions.swap(undo_actions_);
        }
        std::exception_ptr first_error;
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            try { (*it)(); } catch (...) { if (!first_error) first_error = std::current_exception(); }
        }
        if (first_error) std::rethrow_exception(first_error);
    }

    void SetWaitingFor(const std::string& resource_id) {
        std::lock_guard<std::mutex> lock(mu_);
        waiting_for_ = resource_id;
    }
    std::string GetWaitingFor() const {
        std::lock_guard<std::mutex> lock(mu_);
        return waiting_for_;
    }

private:
    const txn_id_t txn_id_;
    std::atomic<TransactionState> state_;
    const std::chrono::steady_clock::time_point start_time_;
    std::vector<LockInfo> locks_;
    std::vector<UndoAction> undo_actions_;
    std::string waiting_for_;
    mutable std::mutex mu_;
};

} // namespace walnut
