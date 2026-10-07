#include "walnut/txn/concurrent_storage.hpp"
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
class ConcurrentStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = (std::filesystem::temp_directory_path() /
            ("walnut_txn_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
        std::filesystem::create_directories(test_dir_);
        storage_ = std::make_unique<StorageManager>(test_dir_);
        storage_->initialize();
        locks_ = std::make_unique<LockManager>();
        tm_ = std::make_unique<TransactionManager>(*locks_);
        concurrent_ = std::make_unique<ConcurrentStorageManager>(*storage_, *tm_, *locks_);
    }
    void TearDown() override {
        concurrent_.reset(); tm_.reset(); locks_.reset();
        if (storage_) storage_->close();
        storage_.reset();
        std::error_code ec; std::filesystem::remove_all(test_dir_, ec);
    }
    std::string test_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<LockManager> locks_;
    std::unique_ptr<TransactionManager> tm_;
    std::unique_ptr<ConcurrentStorageManager> concurrent_;
};
TEST_F(ConcurrentStorageTest, SingleBidSucceeds) {
    concurrent_->CreateUser("buyer", "Buyer");
    concurrent_->CreateItem("laptop", "Laptop", 10.0);
    (void)concurrent_->PlaceBid("laptop", "buyer", 25.0);
    const auto item = concurrent_->GetItem("laptop");
    ASSERT_TRUE(item); EXPECT_DOUBLE_EQ(item->current_bid, 25.0);
    EXPECT_EQ(item->current_winner, "buyer");
}
TEST_F(ConcurrentStorageTest, RejectsClosedAndLowerBids) {
    concurrent_->CreateUser("buyer", "Buyer");
    concurrent_->CreateItem("laptop", "Laptop", 10.0);
    (void)concurrent_->PlaceBid("laptop", "buyer", 10.0);
    EXPECT_THROW(concurrent_->PlaceBid("laptop", "buyer", 9.0), std::runtime_error);
    concurrent_->CloseItem("laptop");
    EXPECT_THROW(concurrent_->PlaceBid("laptop", "buyer", 25.0), std::runtime_error);
}
TEST_F(ConcurrentStorageTest, SimultaneousBidsKeepHighestBid) {
    concurrent_->CreateUser("buyer", "Buyer"); concurrent_->CreateItem("laptop", "Laptop", 1.0);
    std::vector<std::thread> threads;
    for (int amount = 2; amount <= 11; ++amount) {
        threads.emplace_back([this, amount] {
            try { concurrent_->PlaceBid("laptop", "buyer", static_cast<double>(amount)); }
            catch (const std::runtime_error&) {}
        });
    }
    for (auto& thread : threads) thread.join();
    const auto item = concurrent_->GetItem("laptop");
    ASSERT_TRUE(item); EXPECT_DOUBLE_EQ(item->current_bid, 11.0);
    EXPECT_GE(concurrent_->GetBids("laptop").size(), 1U);
}
}
}
