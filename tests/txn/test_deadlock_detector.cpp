#include "walnut/txn/deadlock_detector.hpp"
#include "walnut/txn/lock_manager.hpp"
#include "walnut/txn/transaction_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace walnut {
namespace {

using namespace std::chrono_literals;

TEST(DeadlockDetectorTest, TC_DLK_01_SimpleDeadlockIsDetected) {
    // Force a classic 2-transaction deadlock:
    // T1 locks A, T2 locks B, T1 requests B (blocks), T2 requests A (blocks).
    // Run DetectAndResolve(). Assert exactly one transaction is aborted.
    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm, tm);

    Transaction* t1 = tm.Begin();
    Transaction* t2 = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(t1, "A", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(t2, "B", LockMode::EXCLUSIVE));

    std::atomic<bool> t1_done{false};
    std::atomic<bool> t2_done{false};

    std::thread th1([&] {
        try {
            lm.AcquireLock(t1, "B", LockMode::EXCLUSIVE);
            tm.Commit(t1);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t1);
        }
        t1_done.store(true);
    });

    while (t1->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    std::thread th2([&] {
        try {
            lm.AcquireLock(t2, "A", LockMode::EXCLUSIVE);
            tm.Commit(t2);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t2);
        }
        t2_done.store(true);
    });

    while (t2->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    int victims = detector.DetectAndResolve();
    EXPECT_EQ(victims, 1);

    th1.join();
    th2.join();

    EXPECT_TRUE(t1_done.load());
    EXPECT_TRUE(t2_done.load());

    // One committed, one aborted
    const bool t1_aborted = (t1->GetState() == TransactionState::ABORTED);
    const bool t2_aborted = (t2->GetState() == TransactionState::ABORTED);
    EXPECT_TRUE(t1_aborted ^ t2_aborted);
}

TEST(DeadlockDetectorTest, TC_DLK_02_NoDeadlockNoVictim) {
    // T1 locks A, T2 locks B. No circular wait.
    // DetectAndResolve() should return 0 victims.
    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm, tm);

    Transaction* t1 = tm.Begin();
    Transaction* t2 = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(t1, "A", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(t2, "B", LockMode::EXCLUSIVE));

    std::thread th2([&] {
        // T2 waits for A (held by T1). No cycle because T1 does not wait for B.
        lm.AcquireLock(t2, "A", LockMode::SHARED);
        tm.Commit(t2);
    });

    while (t2->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    EXPECT_EQ(detector.DetectAndResolve(), 0);

    tm.Commit(t1);
    th2.join();
}

TEST(DeadlockDetectorTest, TC_DLK_03_ThreeWayDeadlock) {
    // T1 → T2 → T3 → T1 cycle.
    // DetectAndResolve() should abort the youngest (highest txn_id).
    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm, tm);

    Transaction* t1 = tm.Begin(); // lowest id
    Transaction* t2 = tm.Begin();
    Transaction* t3 = tm.Begin(); // highest id

    ASSERT_TRUE(lm.AcquireLock(t1, "A", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(t2, "B", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(t3, "C", LockMode::EXCLUSIVE));

    std::thread th1([&] {
        try {
            lm.AcquireLock(t1, "B", LockMode::EXCLUSIVE);
            tm.Commit(t1);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t1);
        }
    });

    while (t1->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    std::thread th2([&] {
        try {
            lm.AcquireLock(t2, "C", LockMode::EXCLUSIVE);
            tm.Commit(t2);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t2);
        }
    });

    while (t2->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    std::thread th3([&] {
        try {
            lm.AcquireLock(t3, "A", LockMode::EXCLUSIVE);
            tm.Commit(t3);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t3);
        }
    });

    while (t3->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    // Detect and resolve
    int aborted = detector.DetectAndResolve();
    EXPECT_EQ(aborted, 1);

    th1.join();
    th2.join();
    th3.join();

    // T3 is the youngest (highest txn_id)
    EXPECT_EQ(t3->GetState(), TransactionState::ABORTED);
    EXPECT_EQ(t1->GetState(), TransactionState::COMMITTED);
    EXPECT_EQ(t2->GetState(), TransactionState::COMMITTED);
}

TEST(DeadlockDetectorTest, TC_DLK_04_VictimIsYoungest) {
    // Force a deadlock between T1 (id=1) and T5 (id=5).
    // Assert that T5 is aborted, not T1.
    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm, tm);

    Transaction* t1 = tm.Begin();
    // Advance IDs
    tm.Begin();
    tm.Begin();
    tm.Begin();
    Transaction* t5 = tm.Begin();

    EXPECT_LT(t1->GetTxnId(), t5->GetTxnId());

    ASSERT_TRUE(lm.AcquireLock(t1, "res1", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(t5, "res2", LockMode::EXCLUSIVE));

    std::thread th1([&] {
        try {
            lm.AcquireLock(t1, "res2", LockMode::EXCLUSIVE);
            tm.Commit(t1);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t1);
        }
    });

    while (t1->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    std::thread th5([&] {
        try {
            lm.AcquireLock(t5, "res1", LockMode::EXCLUSIVE);
            tm.Commit(t5);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t5);
        }
    });

    while (t5->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    EXPECT_EQ(detector.DetectAndResolve(), 1);

    th1.join();
    th5.join();

    EXPECT_EQ(t5->GetState(), TransactionState::ABORTED);
    EXPECT_EQ(t1->GetState(), TransactionState::COMMITTED);
}

TEST(DeadlockDetectorTest, TC_DLK_05_AbortedTxnThrowsException) {
    // After the detector aborts a waiting transaction,
    // the blocked thread should throw TransactionAbortedException.
    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm, tm);

    Transaction* t1 = tm.Begin();
    Transaction* t2 = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(t1, "X", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(t2, "Y", LockMode::EXCLUSIVE));

    std::atomic<bool> caught_expected_exception{false};

    std::thread th1([&] {
        try {
            lm.AcquireLock(t1, "Y", LockMode::EXCLUSIVE);
            tm.Commit(t1);
        } catch (...) {
            tm.Abort(t1);
        }
    });

    while (t1->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    std::thread th2([&] {
        try {
            lm.AcquireLock(t2, "X", LockMode::EXCLUSIVE);
            tm.Commit(t2);
        } catch (const TransactionAbortedException& ex) {
            caught_expected_exception.store(true);
            EXPECT_EQ(ex.GetTxnId(), t2->GetTxnId());
            tm.Abort(t2);
        }
    });

    while (t2->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    EXPECT_EQ(detector.DetectAndResolve(), 1);

    th1.join();
    th2.join();

    EXPECT_TRUE(caught_expected_exception.load());
}

TEST(DeadlockDetectorTest, TC_DLK_06_BackgroundThreadDetectsWithinInterval) {
    // Start the detector with interval=100ms.
    // Force a deadlock. Sleep for 200ms.
    // Assert that the deadlock was resolved automatically.
    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm, tm, 50ms);
    detector.Start();

    Transaction* t1 = tm.Begin();
    Transaction* t2 = tm.Begin();

    ASSERT_TRUE(lm.AcquireLock(t1, "resA", LockMode::EXCLUSIVE));
    ASSERT_TRUE(lm.AcquireLock(t2, "resB", LockMode::EXCLUSIVE));

    std::thread th1([&] {
        try {
            lm.AcquireLock(t1, "resB", LockMode::EXCLUSIVE);
            tm.Commit(t1);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t1);
        }
    });

    std::thread th2([&] {
        try {
            lm.AcquireLock(t2, "resA", LockMode::EXCLUSIVE);
            tm.Commit(t2);
        } catch (const TransactionAbortedException&) {
            tm.Abort(t2);
        }
    });

    // Wait for the background thread to automatically detect and resolve
    th1.join();
    th2.join();

    detector.Stop();

    const bool one_aborted = (t1->GetState() == TransactionState::ABORTED) ||
                             (t2->GetState() == TransactionState::ABORTED);
    const bool one_committed = (t1->GetState() == TransactionState::COMMITTED) ||
                               (t2->GetState() == TransactionState::COMMITTED);
    EXPECT_TRUE(one_aborted);
    EXPECT_TRUE(one_committed);
}

// Aliases for backwards compatibility with any existing suites
TEST(DeadlockDetectorTest, DetectsCycleAndAbortsYoungestTransaction) {
    LockManager locks;
    TransactionManager transactions(locks);
    DeadlockDetector detector(locks, transactions, 10ms);
    Transaction* older = transactions.Begin();
    Transaction* younger = transactions.Begin();
    ASSERT_TRUE(locks.AcquireLock(older, "A_old", LockMode::EXCLUSIVE));
    ASSERT_TRUE(locks.AcquireLock(younger, "B_old", LockMode::EXCLUSIVE));

    std::thread first_waiter([&] {
        EXPECT_TRUE(locks.AcquireLock(older, "B_old", LockMode::EXCLUSIVE));
        transactions.Commit(older);
    });
    while (older->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }
    std::thread second_waiter([&] {
        EXPECT_THROW(locks.AcquireLock(younger, "A_old", LockMode::EXCLUSIVE),
                     TransactionAbortedException);
        transactions.Abort(younger);
    });
    while (younger->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }

    EXPECT_EQ(detector.DetectAndResolve(), 1);
    second_waiter.join();
    first_waiter.join();
    EXPECT_EQ(younger->GetState(), TransactionState::ABORTED);
    EXPECT_EQ(older->GetState(), TransactionState::COMMITTED);
}

TEST(DeadlockDetectorTest, DoesNotAbortWhenGraphHasNoCycle) {
    LockManager locks;
    TransactionManager transactions(locks);
    DeadlockDetector detector(locks, transactions);
    Transaction* holder = transactions.Begin();
    Transaction* waiter = transactions.Begin();
    ASSERT_TRUE(locks.AcquireLock(holder, "A_nc", LockMode::EXCLUSIVE));

    std::thread waiting([&] {
        EXPECT_TRUE(locks.AcquireLock(waiter, "A_nc", LockMode::SHARED));
        transactions.Commit(waiter);
    });
    while (waiter->GetWaitingFor().empty()) {
        std::this_thread::yield();
    }
    EXPECT_EQ(detector.DetectAndResolve(), 0);
    transactions.Commit(holder);
    waiting.join();
}

} // namespace
} // namespace walnut
