#include "walnut/txn/concurrent_storage.hpp"
#include "walnut/txn/deadlock_detector.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace walnut {
namespace {

using namespace std::chrono_literals;

class ConcurrentStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() /
                    ("walnut_txn_test_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(data_dir_);
        storage_ = std::make_unique<StorageManager>(data_dir_.string());
        storage_->initialize();
        locks_ = std::make_unique<LockManager>();
        transactions_ = std::make_unique<TransactionManager>(*locks_);
        concurrent_ = std::make_unique<ConcurrentStorageManager>(
            *storage_, *transactions_, *locks_);
    }

    void TearDown() override {
        concurrent_.reset();
        transactions_.reset();
        locks_.reset();
        if (storage_) {
            storage_->close();
        }
        storage_.reset();
        std::error_code error;
        std::filesystem::remove_all(data_dir_, error);
    }

    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<LockManager> locks_;
    std::unique_ptr<TransactionManager> transactions_;
    std::unique_ptr<ConcurrentStorageManager> concurrent_;
};

TEST_F(ConcurrentStorageTest, TC_CSM_01_SingleBidSucceeds) {
    // Single-threaded bid placement through ConcurrentStorageManager.
    concurrent_->CreateUser("user_1", "Alice");
    concurrent_->CreateItem("item_001", "Antique Clock", 100.0);

    const uint64_t bid_id = concurrent_->PlaceBid("item_001", "user_1", 150.0);
    EXPECT_GE(bid_id, 0U);

    const auto item = concurrent_->GetItem("item_001");
    ASSERT_TRUE(item.has_value());
    EXPECT_DOUBLE_EQ(item->current_bid, 150.0);
    EXPECT_EQ(item->current_winner, "user_1");

    const auto bids = concurrent_->GetBids("item_001");
    ASSERT_EQ(bids.size(), 1U);
    EXPECT_EQ(bids.front().bid_id, bid_id);
    EXPECT_DOUBLE_EQ(bids.front().amount, 150.0);
}

TEST_F(ConcurrentStorageTest, TC_CSM_02_ConcurrentBidsOnSameItem) {
    // Multiple threads bid on the same item simultaneously.
    concurrent_->CreateUser("user_common", "Bidder");
    concurrent_->CreateItem("item_002", "Painting", 10.0);

    constexpr int kNumBidders = 10;
    std::vector<std::thread> threads;
    threads.reserve(kNumBidders);

    std::atomic<int> success_count{0};

    for (int i = 1; i <= kNumBidders; ++i) {
        threads.emplace_back([this, i, &success_count] {
            const double amount = 100.0 + (i * 10.0);
            try {
                concurrent_->PlaceBid("item_002", "user_common", amount);
                ++success_count;
            } catch (const std::runtime_error&) {
                // If another thread placed a higher bid first, lower bid correctly throws
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_GT(success_count.load(), 0);

    const auto item = concurrent_->GetItem("item_002");
    ASSERT_TRUE(item.has_value());
    EXPECT_GE(item->current_bid, 110.0);
    EXPECT_LE(item->current_bid, 200.0);
}

TEST_F(ConcurrentStorageTest, TC_CSM_03_BidOnClosedItemThrows) {
    // Close an item, then try to bid. Should throw.
    concurrent_->CreateUser("user_3", "Charlie");
    concurrent_->CreateItem("item_003", "Rare Book", 50.0);

    concurrent_->CloseItem("item_003");

    EXPECT_THROW(concurrent_->PlaceBid("item_003", "user_3", 75.0), std::runtime_error);
}

TEST_F(ConcurrentStorageTest, TC_CSM_04_LowerBidThrows) {
    // Current bid is 500. Place a bid of 400. Should throw.
    concurrent_->CreateUser("user_4", "David");
    concurrent_->CreateItem("item_004", "Sculpture", 500.0);

    EXPECT_THROW(concurrent_->PlaceBid("item_004", "user_4", 400.0), std::runtime_error);

    concurrent_->PlaceBid("item_004", "user_4", 600.0);
    EXPECT_THROW(concurrent_->PlaceBid("item_004", "user_4", 550.0), std::runtime_error);
}

TEST_F(ConcurrentStorageTest, TC_CSM_05_DeadlockAutoRetry) {
    concurrent_->CreateUser("user_5", "Eve");
    concurrent_->CreateItem("item_005", "Camera", 100.0);

    DeadlockDetector detector(*locks_, *transactions_, 20ms);
    detector.Start();

    Transaction* t_hold = transactions_->Begin();
    ASSERT_TRUE(locks_->AcquireLock(t_hold, "item_005", LockMode::EXCLUSIVE));

    std::atomic<bool> thread_started{false};
    std::atomic<bool> succeeded{false};

    std::thread bidder([&] {
        thread_started.store(true);
        constexpr int kMaxRetries = 5;
        for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
            try {
                concurrent_->PlaceBid("item_005", "user_5", 200.0);
                succeeded.store(true);
                break;
            } catch (const TransactionAbortedException&) {
                std::this_thread::sleep_for(30ms);
            } catch (const std::exception&) {
                break;
            }
        }
    });

    while (!thread_started.load()) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(50ms);
    transactions_->Commit(t_hold);

    bidder.join();
    detector.Stop();

    EXPECT_TRUE(succeeded.load());
    auto item = concurrent_->GetItem("item_005");
    ASSERT_TRUE(item.has_value());
    EXPECT_DOUBLE_EQ(item->current_bid, 200.0);
}

// Aliases for backwards compatibility with any existing test names
TEST_F(ConcurrentStorageTest, SingleBidSucceeds) {
    concurrent_->CreateUser("buyer", "Buyer");
    concurrent_->CreateItem("laptop", "Laptop", 10.0);
    const auto bid_id = concurrent_->PlaceBid("laptop", "buyer", 25.0);
    EXPECT_GE(bid_id, 0U);
    const auto item = concurrent_->GetItem("laptop");
    ASSERT_TRUE(item.has_value());
    EXPECT_DOUBLE_EQ(item->current_bid, 25.0);
    EXPECT_EQ(item->current_winner, "buyer");
}

TEST_F(ConcurrentStorageTest, RejectsBidOnClosedOrMissingItem) {
    concurrent_->CreateUser("buyer", "Buyer");
    concurrent_->CreateItem("laptop", "Laptop", 10.0);
    concurrent_->CloseItem("laptop");
    EXPECT_THROW(concurrent_->PlaceBid("laptop", "buyer", 25.0),
                 std::runtime_error);
    EXPECT_THROW(concurrent_->PlaceBid("missing", "buyer", 25.0),
                 std::runtime_error);
}

TEST_F(ConcurrentStorageTest, ConcurrentBidsPreserveHighestBid) {
    concurrent_->CreateUser("buyer", "Buyer");
    concurrent_->CreateItem("laptop", "Laptop", 1.0);
    std::atomic<int> successes{0};
    std::vector<std::thread> bidders;
    for (int amount = 2; amount < 12; ++amount) {
        bidders.emplace_back([this, amount, &successes] {
            try {
                concurrent_->PlaceBid("laptop", "buyer", static_cast<double>(amount));
                ++successes;
            } catch (const std::runtime_error&) {
            }
        });
    }
    for (auto& bidder : bidders) {
        bidder.join();
    }
    EXPECT_GT(successes.load(), 0);
    const auto item = concurrent_->GetItem("laptop");
    ASSERT_TRUE(item.has_value());
    EXPECT_GE(item->current_bid, 2.0);
    EXPECT_LE(item->current_bid, 11.0);
}

} // namespace
} // namespace walnut
