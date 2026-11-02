#include "walnut/txn/transaction.hpp"
#include "walnut/txn/transaction_manager.hpp"
#include "walnut/txn/lock_manager.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace walnut {
namespace {

TEST(TransactionTest, NewTransactionIsInGrowingState) {
    Transaction txn(1);
    EXPECT_EQ(txn.GetTxnId(), 1U);
    EXPECT_EQ(txn.GetState(), TransactionState::GROWING);
}

TEST(TransactionTest, SetStateToCOMMITTED) {
    Transaction txn(1);
    txn.SetState(TransactionState::SHRINKING);
    txn.SetState(TransactionState::COMMITTED);
    EXPECT_EQ(txn.GetState(), TransactionState::COMMITTED);
}

TEST(TransactionTest, SetStateToABORTED) {
    Transaction txn(1);
    txn.SetState(TransactionState::ABORTED);
    EXPECT_EQ(txn.GetState(), TransactionState::ABORTED);
    // Double abort transition is idempotent
    EXPECT_NO_THROW(txn.SetState(TransactionState::ABORTED));
}

TEST(TransactionTest, CannotCommitAfterAbort) {
    Transaction txn(1);
    txn.SetState(TransactionState::ABORTED);
    EXPECT_THROW(txn.SetState(TransactionState::COMMITTED), std::logic_error);
}

TEST(TransactionTest, AddLockTracksCorrectly) {
    Transaction txn(1);
    txn.AddLock("item_001", LockMode::SHARED);
    auto locks = txn.GetLocks();
    ASSERT_EQ(locks.size(), 1U);
    EXPECT_EQ(locks.front().resource_id, "item_001");
    EXPECT_EQ(locks.front().mode, LockMode::SHARED);

    // Upgrade existing lock
    txn.AddLock("item_001", LockMode::EXCLUSIVE);
    locks = txn.GetLocks();
    ASSERT_EQ(locks.size(), 1U);
    EXPECT_EQ(locks.front().mode, LockMode::EXCLUSIVE);

    // Add another lock
    txn.AddLock("item_002", LockMode::SHARED);
    locks = txn.GetLocks();
    EXPECT_EQ(locks.size(), 2U);
}

TEST(TransactionTest, UndoActionsExecuteInReverseOrder) {
    Transaction txn(1);
    std::vector<int> execution_order;
    txn.PushUndoAction([&execution_order] { execution_order.push_back(1); });
    txn.PushUndoAction([&execution_order] { execution_order.push_back(2); });
    txn.PushUndoAction([&execution_order] { execution_order.push_back(3); });

    txn.RollbackUndoActions();
    EXPECT_EQ(execution_order, (std::vector<int>{3, 2, 1}));
}

TEST(TransactionTest, EmptyUndoIsNoOp) {
    Transaction txn(1);
    EXPECT_NO_THROW(txn.RollbackUndoActions());
}

TEST(TransactionTest, WaitingForTracking) {
    Transaction txn(1);
    EXPECT_TRUE(txn.GetWaitingFor().empty());

    txn.SetWaitingFor("item_001");
    EXPECT_EQ(txn.GetWaitingFor(), "item_001");

    txn.SetWaitingFor("");
    EXPECT_TRUE(txn.GetWaitingFor().empty());
}

TEST(TransactionTest, TxnIdIsUnique) {
    LockManager lock_mgr;
    TransactionManager txn_mgr(lock_mgr);

    Transaction* t1 = txn_mgr.Begin();
    Transaction* t2 = txn_mgr.Begin();
    Transaction* t3 = txn_mgr.Begin();

    EXPECT_NE(t1->GetTxnId(), t2->GetTxnId());
    EXPECT_NE(t2->GetTxnId(), t3->GetTxnId());
    EXPECT_NE(t1->GetTxnId(), t3->GetTxnId());

    txn_mgr.Commit(t1);
    txn_mgr.Commit(t2);
    txn_mgr.Commit(t3);
}

// Aliases for backwards compatibility with any existing suites
TEST(TransactionTest, NewTransactionStartsGrowing) {
    Transaction txn(10);
    EXPECT_EQ(txn.GetTxnId(), 10U);
    EXPECT_EQ(txn.GetState(), TransactionState::GROWING);
}

TEST(TransactionTest, TracksAndUpgradesLocks) {
    Transaction txn(11);
    txn.AddLock("item:1", LockMode::SHARED);
    txn.AddLock("item:1", LockMode::EXCLUSIVE);
    const auto locks = txn.GetLocks();
    ASSERT_EQ(locks.size(), 1U);
    EXPECT_EQ(locks.front().resource_id, "item:1");
    EXPECT_EQ(locks.front().mode, LockMode::EXCLUSIVE);
}

TEST(TransactionTest, UndoActionsRunInReverseOrder) {
    Transaction txn(12);
    std::vector<int> order;
    txn.PushUndoAction([&order] { order.push_back(10); });
    txn.PushUndoAction([&order] { order.push_back(20); });
    txn.RollbackUndoActions();
    EXPECT_EQ(order, (std::vector<int>{20, 10}));
}

} // namespace
} // namespace walnut
