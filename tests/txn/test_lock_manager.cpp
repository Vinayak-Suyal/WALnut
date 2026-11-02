#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"
#include "walnut/txn/deadlock_detector.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

namespace walnut {
namespace {

using namespace std::chrono_literals;

TEST(LockManagerTest, TC_LCK_01_SharedLocksAreCompatible) {
    // 5 threads each acquire SHARED lock on "item_001".
    // ALL must succeed without blocking.
    LockManager lm;
    TransactionManager tm(lm);

    constexpr int kNumThreads = 5;
    std::vector<Transaction*> txns(kNumThreads);
    for (int i = 0; i < kNumThreads; ++i) {
        txns[i] = tm.Begin();
    }

    std::atomic<int> acquired_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&, i] {
            if (lm.AcquireLock(txns[i], "item_001", LockMode::SHARED)) {
                ++acquired_count;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(acquired_count.load(), kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
        tm.Commit(txns[i]);
    }
}

TEST(LockManagerTest, TC_LCK_02_ExclusiveLockBlocksOthers) {
    // Thread A acquires EXCLUSIVE on "item_001".
    // Thread B tries to acquire SHARED on "item_001" — must block.
    // Thread A commits → Thread B unblocks.
    LockManager lm;
    TransactionManager tm(lm);

    Transaction* txn_a = tm.Begin();
    Transaction* txn_b = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(txn_a, "item_001", LockMode::EXCLUSIVE));

    std::atomic<bool> b_acquired{false};
    std::thread thread_b([&] {
        lm.AcquireLock(txn_b, "item_001", LockMode::SHARED);
        b_acquired.store(true, std::memory_order_release);
        tm.Commit(txn_b);
    });

    std::this_thread::sleep_for(40ms);
    EXPECT_FALSE(b_acquired.load(std::memory_order_acquire));

    tm.Commit(txn_a);
    thread_b.join();

    EXPECT_TRUE(b_acquired.load(std::memory_order_acquire));
}

TEST(LockManagerTest, TC_LCK_03_ExclusiveBlocksExclusive) {
    // Thread A acquires EXCLUSIVE on "item_001".
    // Thread B tries EXCLUSIVE on same — blocks.
    // A commits → B unblocks.
    LockManager lm;
    TransactionManager tm(lm);

    Transaction* txn_a = tm.Begin();
    Transaction* txn_b = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(txn_a, "item_001", LockMode::EXCLUSIVE));

    std::atomic<bool> b_acquired{false};
    std::thread thread_b([&] {
        lm.AcquireLock(txn_b, "item_001", LockMode::EXCLUSIVE);
        b_acquired.store(true, std::memory_order_release);
        tm.Commit(txn_b);
    });

    std::this_thread::sleep_for(40ms);
    EXPECT_FALSE(b_acquired.load(std::memory_order_acquire));

    tm.Commit(txn_a);
    thread_b.join();

    EXPECT_TRUE(b_acquired.load(std::memory_order_acquire));
}

TEST(LockManagerTest, TC_LCK_04_LockUpgradeStoX) {
    // Thread A acquires SHARED on "item_001".
    // Thread A requests EXCLUSIVE on "item_001" (upgrade).
    // If no other holder: grant immediately.
    LockManager lm;
    TransactionManager tm(lm);

    Transaction* txn_a = tm.Begin();
    ASSERT_TRUE(lm.AcquireLock(txn_a, "item_001", LockMode::SHARED));
    ASSERT_TRUE(lm.AcquireLock(txn_a, "item_001", LockMode::EXCLUSIVE));

    const auto locks = txn_a->GetLocks();
    ASSERT_EQ(locks.size(), 1U);
    EXPECT_EQ(locks.front().mode, LockMode::EXCLUSIVE);

    tm.Commit(txn_a);
}

TEST(LockManagerTest, TC_LCK_05_ReleaseAllLocksWakesWaiters) {
    // A holds X on item_001, item_002.
    // B waits for item_001, C waits for item_002.
    // A commits → both B and C unblock.
    LockManager lm;
    TransactionManager tm(lm);

    Transaction* txn_a = tm.Begin();
    Transaction* txn_b = tm.Begin();
    Transaction* txn_c = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(txn_a, "item_001", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(txn_a, "item_002", LockMode::EXCLUSIVE));

    std::atomic<bool> b_acquired{false};
    std::atomic<bool> c_acquired{false};

    std::thread thread_b([&] {
        lm.AcquireLock(txn_b, "item_001", LockMode::EXCLUSIVE);
        b_acquired.store(true, std::memory_order_release);
        tm.Commit(txn_b);
    });

    std::thread thread_c([&] {
        lm.AcquireLock(txn_c, "item_002", LockMode::EXCLUSIVE);
        c_acquired.store(true, std::memory_order_release);
        tm.Commit(txn_c);
    });

    std::this_thread::sleep_for(40ms);
    EXPECT_FALSE(b_acquired.load(std::memory_order_acquire));
    EXPECT_FALSE(c_acquired.load(std::memory_order_acquire));

    tm.Commit(txn_a);

    thread_b.join();
    thread_c.join();

    EXPECT_TRUE(b_acquired.load(std::memory_order_acquire));
    EXPECT_TRUE(c_acquired.load(std::memory_order_acquire));
}

TEST(LockManagerTest, TC_LCK_06_FIFOFairness) {
    // A holds X. B requests X, then C requests X.
    // When A releases, B should be granted before C.
    LockManager lm;
    TransactionManager tm(lm);

    Transaction* txn_a = tm.Begin();
    Transaction* txn_b = tm.Begin();
    Transaction* txn_c = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(txn_a, "item_001", LockMode::EXCLUSIVE));

    std::vector<int> grant_order;
    std::mutex order_mu;

    std::thread thread_b([&] {
        lm.AcquireLock(txn_b, "item_001", LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lk(order_mu);
            grant_order.push_back(1); // B
        }
        std::this_thread::sleep_for(20ms);
        tm.Commit(txn_b);
    });

    // Ensure B is waiting in queue before C requests
    while (txn_b->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(10ms);

    std::thread thread_c([&] {
        lm.AcquireLock(txn_c, "item_001", LockMode::EXCLUSIVE);
        {
            std::lock_guard<std::mutex> lk(order_mu);
            grant_order.push_back(2); // C
        }
        tm.Commit(txn_c);
    });

    while (txn_c->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    tm.Commit(txn_a);

    thread_b.join();
    thread_c.join();

    ASSERT_EQ(grant_order.size(), 2U);
    EXPECT_EQ(grant_order[0], 1); // B granted first
    EXPECT_EQ(grant_order[1], 2); // C granted second
}

TEST(LockManagerTest, TC_LCK_07_CannotAcquireInShrinkingState) {
    // Set txn state to SHRINKING. AcquireLock should throw.
    LockManager lm;
    TransactionManager tm(lm);

    Transaction* txn = tm.Begin();
    txn->SetState(TransactionState::SHRINKING);

    EXPECT_THROW(lm.AcquireLock(txn, "item_001", LockMode::SHARED), std::logic_error);
    tm.Abort(txn);
}

TEST(LockManagerTest, TC_LCK_08_ConcurrentStressTest) {
    // 50 threads, each doing 100 lock/unlock cycles on random resources.
    // Assert no crashes, no deadlocks (with detector running), no data corruption.
    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm, tm, 50ms);
    detector.Start();

    constexpr int kThreads = 50;
    constexpr int kCycles = 100;
    constexpr int kResources = 10;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    std::atomic<int> completed_ops{0};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng(1337 + t);
            std::uniform_int_distribution<int> res_dist(0, kResources - 1);
            std::uniform_int_distribution<int> mode_dist(0, 1);

            for (int c = 0; c < kCycles; ++c) {
                Transaction* txn = tm.Begin();
                const std::string res = "res_" + std::to_string(res_dist(rng));
                const LockMode mode = (mode_dist(rng) == 0) ? LockMode::SHARED : LockMode::EXCLUSIVE;

                try {
                    lm.AcquireLock(txn, res, mode);
                    tm.Commit(txn);
                } catch (const TransactionAbortedException&) {
                    tm.Abort(txn);
                } catch (...) {
                    tm.Abort(txn);
                }
                ++completed_ops;
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    detector.Stop();
    EXPECT_EQ(completed_ops.load(), kThreads * kCycles);
}

// Aliases for backwards compatibility with any existing suites
TEST(LockManagerTest, SharedLocksAreCompatibleAndExclusiveWaits) {
    LockManager locks;
    TransactionManager transactions(locks);
    Transaction* reader_one = transactions.Begin();
    Transaction* reader_two = transactions.Begin();
    Transaction* writer = transactions.Begin();
    ASSERT_TRUE(locks.AcquireLock(reader_one, "item:1", LockMode::SHARED));
    ASSERT_TRUE(locks.AcquireLock(reader_two, "item:1", LockMode::SHARED));

    std::atomic<bool> writer_acquired{false};
    std::thread waiting_writer([&] {
        locks.AcquireLock(writer, "item:1", LockMode::EXCLUSIVE);
        writer_acquired.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(writer_acquired.load(std::memory_order_acquire));

    transactions.Commit(reader_one);
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(writer_acquired.load(std::memory_order_acquire));
    transactions.Commit(reader_two);
    waiting_writer.join();
    EXPECT_TRUE(writer_acquired.load(std::memory_order_acquire));
    transactions.Commit(writer);
}

TEST(LockManagerTest, ExclusiveLockBlocksAndThenWakesWaiter) {
    LockManager locks;
    TransactionManager transactions(locks);
    Transaction* first = transactions.Begin();
    Transaction* second = transactions.Begin();
    ASSERT_TRUE(locks.AcquireLock(first, "item:2", LockMode::EXCLUSIVE));

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        locks.AcquireLock(second, "item:2", LockMode::SHARED);
        acquired.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(acquired.load(std::memory_order_acquire));
    transactions.Commit(first);
    waiter.join();
    EXPECT_TRUE(acquired.load(std::memory_order_acquire));
    transactions.Commit(second);
}

TEST(LockManagerTest, SharedToExclusiveUpgradeSucceedsWithoutOtherHolders) {
    LockManager locks;
    TransactionManager transactions(locks);
    Transaction* txn = transactions.Begin();
    ASSERT_TRUE(locks.AcquireLock(txn, "item:3", LockMode::SHARED));
    ASSERT_TRUE(locks.AcquireLock(txn, "item:3", LockMode::EXCLUSIVE));
    ASSERT_EQ(txn->GetLocks().size(), 1U);
    EXPECT_EQ(txn->GetLocks().front().mode, LockMode::EXCLUSIVE);
    transactions.Commit(txn);
}

TEST(LockManagerTest, RejectsAcquisitionAfterGrowingPhase) {
    LockManager locks;
    TransactionManager transactions(locks);
    Transaction* txn = transactions.Begin();
    txn->SetState(TransactionState::SHRINKING);
    EXPECT_THROW(locks.AcquireLock(txn, "item:4", LockMode::SHARED),
                 std::logic_error);
    transactions.Abort(txn);
}

} // namespace
} // namespace walnut
