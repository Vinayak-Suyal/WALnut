#include "walnut/txn/deadlock_detector.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace walnut {
namespace {
using namespace std::chrono_literals;
TEST(DeadlockDetectorTest, ResolvesCycleByAbortingYoungest) {
    LockManager locks; TransactionManager tm(locks); DeadlockDetector detector(locks, tm, 10ms);
    auto* older = tm.Begin(); auto* younger = tm.Begin();
    locks.AcquireLock(older, "A", LockMode::EXCLUSIVE);
    locks.AcquireLock(younger, "B", LockMode::EXCLUSIVE);
    std::thread older_waiter([&] {
        EXPECT_TRUE(locks.AcquireLock(older, "B", LockMode::EXCLUSIVE));
        tm.Commit(older);
    });
    while (older->GetWaitingFor().empty()) std::this_thread::yield();
    std::thread younger_waiter([&] {
        EXPECT_THROW(locks.AcquireLock(younger, "A", LockMode::EXCLUSIVE), TransactionAbortedException);
        tm.Abort(younger);
    });
    while (younger->GetWaitingFor().empty()) std::this_thread::yield();
    EXPECT_EQ(detector.DetectAndResolve(), 1);
    younger_waiter.join(); older_waiter.join();
    EXPECT_EQ(younger->GetState(), TransactionState::ABORTED);
    EXPECT_EQ(older->GetState(), TransactionState::COMMITTED);
}
TEST(DeadlockDetectorTest, NoCycleHasNoVictim) {
    LockManager locks; TransactionManager tm(locks); DeadlockDetector detector(locks, tm);
    auto* holder = tm.Begin(); auto* waiter_txn = tm.Begin();
    locks.AcquireLock(holder, "A", LockMode::EXCLUSIVE);
    std::thread waiter([&] { locks.AcquireLock(waiter_txn, "A", LockMode::SHARED); tm.Commit(waiter_txn); });
    while (waiter_txn->GetWaitingFor().empty()) std::this_thread::yield();
    EXPECT_EQ(detector.DetectAndResolve(), 0);
    tm.Commit(holder); waiter.join();
}
}
}
