#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace walnut {
namespace {
using namespace std::chrono_literals;
TEST(LockManagerTest, SharedCompatibleAndExclusiveWaits) {
    LockManager locks;
    TransactionManager tm(locks);
    auto* reader1 = tm.Begin(); auto* reader2 = tm.Begin(); auto* writer = tm.Begin();
    ASSERT_TRUE(locks.AcquireLock(reader1, "item", LockMode::SHARED));
    ASSERT_TRUE(locks.AcquireLock(reader2, "item", LockMode::SHARED));
    std::atomic<bool> acquired{false};
    std::thread waiter([&] { locks.AcquireLock(writer, "item", LockMode::EXCLUSIVE); acquired = true; });
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(acquired.load());
    tm.Commit(reader1);
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(acquired.load());
    tm.Commit(reader2);
    waiter.join();
    EXPECT_TRUE(acquired.load());
    tm.Commit(writer);
}
TEST(LockManagerTest, ExclusiveBlocksSharedThenWakes) {
    LockManager locks; TransactionManager tm(locks);
    auto* holder = tm.Begin(); auto* waiter_txn = tm.Begin();
    locks.AcquireLock(holder, "item", LockMode::EXCLUSIVE);
    std::atomic<bool> acquired{false};
    std::thread waiter([&] { locks.AcquireLock(waiter_txn, "item", LockMode::SHARED); acquired = true; });
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(acquired.load());
    tm.Commit(holder); waiter.join();
    EXPECT_TRUE(acquired.load());
    tm.Commit(waiter_txn);
}
TEST(LockManagerTest, UpgradeSucceedsWhenAlone) {
    LockManager locks; TransactionManager tm(locks); auto* txn = tm.Begin();
    locks.AcquireLock(txn, "item", LockMode::SHARED);
    EXPECT_TRUE(locks.AcquireLock(txn, "item", LockMode::EXCLUSIVE));
    ASSERT_EQ(txn->GetLocks().size(), 1U);
    EXPECT_EQ(txn->GetLocks().front().mode, LockMode::EXCLUSIVE);
    tm.Commit(txn);
}
TEST(LockManagerTest, RejectsAcquireAfterGrowing) {
    LockManager locks; TransactionManager tm(locks); auto* txn = tm.Begin();
    txn->SetState(TransactionState::SHRINKING);
    EXPECT_THROW(locks.AcquireLock(txn, "item", LockMode::SHARED), std::logic_error);
    tm.Abort(txn);
}
}
}
