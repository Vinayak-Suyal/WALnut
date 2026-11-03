#include "walnut/txn/deadlock_detector.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool WaitUntilWaiting(walnut::Transaction* txn, const std::string& resource) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (txn->GetWaitingFor() == resource) return true;
        std::this_thread::yield();
    }
    return false;
}

const char* StateName(walnut::TransactionState state) {
    switch (state) {
    case walnut::TransactionState::GROWING: return "GROWING";
    case walnut::TransactionState::SHRINKING: return "SHRINKING";
    case walnut::TransactionState::COMMITTED: return "COMMITTED";
    case walnut::TransactionState::ABORTED: return "ABORTED";
    }
    return "UNKNOWN";
}

bool DemonstrateDeadlock() {
    using namespace walnut;
    const std::string first_item_id = "item_macbook_pro_16";
    const std::string second_item_id = "item_sony_headphones";
    const std::string first_product = "MacBook Pro 16-inch";
    const std::string second_product = "Sony WH-1000XM6 headphones";

    LockManager locks;
    TransactionManager transactions(locks);
    DeadlockDetector detector(locks, transactions);

    Transaction* const older = transactions.Begin();
    Transaction* const younger = transactions.Begin();
    locks.AcquireLock(older, first_item_id, LockMode::EXCLUSIVE);
    locks.AcquireLock(younger, second_item_id, LockMode::EXCLUSIVE);

    std::cout << "[setup] T" << older->GetTxnId() << " holds " << first_product
              << "; T" << younger->GetTxnId() << " holds " << second_product << '\n'
              << "        " << first_product << " (" << first_item_id << ")\n"
              << "        " << second_product << " (" << second_item_id << ")\n";

    std::thread older_thread([&] {
        try {
            std::cout << "[request] T" << older->GetTxnId() << " requests " << second_product << '\n';
            locks.AcquireLock(older, second_item_id, LockMode::EXCLUSIVE);
            std::cout << "[resume]  T" << older->GetTxnId() << " acquired " << second_product << '\n';
            transactions.Commit(older);
        } catch (const TransactionAbortedException& error) {
            std::cout << "[abort]   " << error.what() << '\n';
        }
    });

    if (!WaitUntilWaiting(older, second_item_id)) {
        std::cerr << "Timed out waiting for T" << older->GetTxnId() << " to block.\n";
        transactions.Abort(older);
        transactions.Abort(younger);
        older_thread.join();
        return false;
    }

    std::thread younger_thread([&] {
        try {
            std::cout << "[request] T" << younger->GetTxnId() << " requests " << first_product << '\n';
            locks.AcquireLock(younger, first_item_id, LockMode::EXCLUSIVE);
            std::cout << "[resume]  T" << younger->GetTxnId() << " acquired " << first_product << '\n';
            transactions.Commit(younger);
        } catch (const TransactionAbortedException& error) {
            std::cout << "[abort]   " << error.what() << '\n';
        }
    });

    if (!WaitUntilWaiting(younger, first_item_id)) {
        std::cerr << "Timed out waiting for T" << younger->GetTxnId() << " to block.\n";
        transactions.Abort(older);
        transactions.Abort(younger);
        older_thread.join();
        younger_thread.join();
        return false;
    }

    std::cout << "[cycle]   T" << older->GetTxnId() << " waits for " << second_product
              << "; T" << younger->GetTxnId() << " waits for " << first_product << '\n'
              << "          T" << older->GetTxnId() << " -> T" << younger->GetTxnId()
              << " -> T" << older->GetTxnId() << '\n';
    const int victims = detector.DetectAndResolve();
    older_thread.join();
    younger_thread.join();

    std::cout << "[result]  detector aborted " << victims << " transaction(s)\n"
              << "          T" << older->GetTxnId() << ": " << StateName(older->GetState()) << '\n'
              << "          T" << younger->GetTxnId() << ": " << StateName(younger->GetState()) << '\n';

    const bool youngest_was_victim = younger->GetState() == TransactionState::ABORTED;
    const bool older_completed = older->GetState() == TransactionState::COMMITTED;
    transactions.Abort(older);
    transactions.Abort(younger);
    return victims == 1 && youngest_was_victim && older_completed;
}

bool DemonstrateNoDeadlock() {
    using namespace walnut;
    const std::string first_item_id = "item_macbook_pro_16";
    const std::string second_item_id = "item_sony_headphones";
    LockManager locks;
    TransactionManager transactions(locks);
    DeadlockDetector detector(locks, transactions);
    Transaction* const first = transactions.Begin();
    Transaction* const second = transactions.Begin();
    locks.AcquireLock(first, first_item_id, LockMode::EXCLUSIVE);
    locks.AcquireLock(second, second_item_id, LockMode::EXCLUSIVE);

    const int victims = detector.DetectAndResolve();
    std::cout << "[control] independent product locks: " << victims
              << " deadlock victim(s) (expected 0)\n";
    transactions.Abort(first);
    transactions.Abort(second);
    return victims == 0;
}

} // namespace

int main() {
    std::cout << "WALnut deadlock detector demonstration\n";
    const bool deadlock_passed = DemonstrateDeadlock();
    const bool control_passed = DemonstrateNoDeadlock();
    const bool passed = deadlock_passed && control_passed;
    std::cout << (passed ? "PASS: deadlock handling evaluated successfully\n"
                         : "FAIL: deadlock handling did not meet expectations\n");
    return passed ? 0 : 1;
}
