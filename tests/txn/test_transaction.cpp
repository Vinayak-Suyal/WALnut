// =============================================================================
// WALnut – Transaction Layer (Sushmit Singh Rawat)
// tests/txn/test_transaction.cpp
//
// Schema v2.0 (OSDBMS-V-2026-T038) — updated test suite.
// Original 5 tests preserved; 3 new tests for RegisterCommitAction /
// RunCommitActions added.
// =============================================================================

#include "walnut/txn/transaction.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace walnut {
namespace {

// ===========================================================================
// ORIGINAL TESTS (preserved, unchanged)
// ===========================================================================

TEST(TransactionTest, StartsGrowingAndTracksId) {
    Transaction txn(1);
    EXPECT_EQ(txn.GetTxnId(), 1U);
    EXPECT_EQ(txn.GetState(), TransactionState::GROWING);
}

TEST(TransactionTest, StateTransitionsAndCannotCommitAfterAbort) {
    Transaction committed(1);
    committed.SetState(TransactionState::SHRINKING);
    committed.SetState(TransactionState::COMMITTED);
    EXPECT_EQ(committed.GetState(), TransactionState::COMMITTED);

    Transaction aborted(2);
    aborted.SetState(TransactionState::ABORTED);
    EXPECT_THROW(aborted.SetState(TransactionState::COMMITTED), std::logic_error);
}

TEST(TransactionTest, TracksAndUpgradesLock) {
    Transaction txn(1);
    txn.AddLock("item:1", LockMode::SHARED);
    txn.AddLock("item:1", LockMode::EXCLUSIVE);
    const auto locks = txn.GetLocks();
    ASSERT_EQ(locks.size(), 1U);
    EXPECT_EQ(locks.front().mode, LockMode::EXCLUSIVE);
}

TEST(TransactionTest, UndoActionsRunInReverseOrder) {
    Transaction txn(1);
    std::vector<int> order;
    txn.PushUndoAction([&] { order.push_back(1); });
    txn.PushUndoAction([&] { order.push_back(2); });
    txn.PushUndoAction([&] { order.push_back(3); });
    txn.RollbackUndoActions();
    EXPECT_EQ(order, (std::vector<int>{3, 2, 1}));
    EXPECT_NO_THROW(txn.RollbackUndoActions());
}

TEST(TransactionTest, WaitingResourceCanBeReadAndCleared) {
    Transaction txn(1);
    txn.SetWaitingFor("item:1");
    EXPECT_EQ(txn.GetWaitingFor(), "item:1");
    txn.SetWaitingFor({});
    EXPECT_TRUE(txn.GetWaitingFor().empty());
}

// ===========================================================================
// V2: CommitAction tests (RegisterCommitAction / RunCommitActions)
// ===========================================================================

/// TC_TXN_CA_01: Commit actions run in registration order (FIFO).
TEST(TransactionTest, TC_TXN_CA_01_CommitActionsRunInRegistrationOrder) {
    Transaction txn(42);
    std::vector<int> order;

    txn.RegisterCommitAction([&] { order.push_back(1); });
    txn.RegisterCommitAction([&] { order.push_back(2); });
    txn.RegisterCommitAction([&] { order.push_back(3); });

    txn.RunCommitActions();

    // FIFO — opposite of undo (which is LIFO).
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

/// TC_TXN_CA_02: RunCommitActions is idempotent (cleared after first run).
TEST(TransactionTest, TC_TXN_CA_02_CommitActionsRunOnce) {
    Transaction txn(1);
    int counter = 0;
    txn.RegisterCommitAction([&] { counter++; });
    txn.RunCommitActions();
    txn.RunCommitActions();   // second call must be a no-op

    EXPECT_EQ(counter, 1);
}

/// TC_TXN_CA_03: A throwing commit action is swallowed; others still run.
TEST(TransactionTest, TC_TXN_CA_03_CommitActionsSwallowErrors) {
    Transaction txn(1);
    std::vector<int> ran;

    txn.RegisterCommitAction([&] { ran.push_back(1); });
    txn.RegisterCommitAction([]  { throw std::runtime_error("boom"); });
    txn.RegisterCommitAction([&] { ran.push_back(3); });

    // Must not throw despite the middle action throwing.
    EXPECT_NO_THROW(txn.RunCommitActions());

    // Actions 1 and 3 must have executed.
    EXPECT_EQ(ran, (std::vector<int>{1, 3}));
}

/// TC_TXN_CA_04: Undo actions are independent of commit actions.
/// Rolling back must NOT trigger commit actions.
TEST(TransactionTest, TC_TXN_CA_04_UndoAndCommitActionsAreIndependent) {
    Transaction txn(1);
    bool undo_ran   = false;
    bool commit_ran = false;

    txn.PushUndoAction(    [&] { undo_ran   = true; });
    txn.RegisterCommitAction([&] { commit_ran = true; });

    // Rollback: only undo actions should fire.
    txn.RollbackUndoActions();
    EXPECT_TRUE(undo_ran);
    EXPECT_FALSE(commit_ran);

    // Running commit actions now: only commit action should fire.
    txn.RunCommitActions();
    EXPECT_TRUE(commit_ran);
}

} // anonymous namespace
} // namespace walnut
