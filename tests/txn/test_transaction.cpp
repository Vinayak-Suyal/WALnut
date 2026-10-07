#include "walnut/txn/transaction.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace walnut {
namespace {
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
}
}
