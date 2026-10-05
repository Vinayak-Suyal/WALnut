// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// tests/storage/test_storage_manager.cpp
// =============================================================================

#include <gtest/gtest.h>
#include "walnut/storage/storage_manager.hpp"
#include "walnut/common/constants.hpp"
#include <filesystem>
#include <chrono>
#include <string>

using namespace walnut;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class StorageManagerTest : public ::testing::Test {
protected:
    std::string test_dir_;

    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path().string() +
                    "/walnut_sm_test_" +
                    std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count());
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    std::unique_ptr<StorageManager> make_sm() {
        auto sm = std::make_unique<StorageManager>(test_dir_);
        sm->initialize();
        return sm;
    }
};

// ---------------------------------------------------------------------------
// User tests
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, CreateAndFindUser) {
    auto sm = make_sm();
    User u{"alice", "Alice Wonderland"};
    sm->createUser(u);

    auto found = sm->findUserById("alice");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->user_id,  "alice");
    EXPECT_EQ(found->username, "Alice Wonderland");
}

TEST_F(StorageManagerTest, FindNonexistentUserReturnsNullopt) {
    auto sm = make_sm();
    EXPECT_FALSE(sm->findUserById("ghost").has_value());
}

TEST_F(StorageManagerTest, DuplicateUserIdThrows) {
    auto sm = make_sm();
    sm->createUser({"u1", "User One"});
    EXPECT_THROW(sm->createUser({"u1", "Another User"}), std::runtime_error);
}

/// User persistence across process restart.
TEST_F(StorageManagerTest, UserPersistsAcrossRestart) {
    {
        auto sm = make_sm();
        sm->createUser({"bob", "Bob Builder"});
        sm->close();
    }
    {
        auto sm = make_sm();
        auto found = sm->findUserById("bob");
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->username, "Bob Builder");
    }
}

// ---------------------------------------------------------------------------
// Item tests
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, CreateAndGetItem) {
    auto sm = make_sm();
    Item item{"itm1", "Vintage Watch", 1000.0, "OPEN", 0.0, ""};
    sm->createItem(item);

    auto found = sm->getItem("itm1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->item_id,        "itm1");
    EXPECT_EQ(found->title,          "Vintage Watch");
    EXPECT_DOUBLE_EQ(found->starting_price, 1000.0);
    EXPECT_EQ(found->status,         "OPEN");
}

TEST_F(StorageManagerTest, DuplicateItemIdThrows) {
    auto sm = make_sm();
    sm->createItem({"x", "Item X", 100.0, "OPEN", 0.0, ""});
    EXPECT_THROW(
        sm->createItem({"x", "Item X Copy", 200.0, "OPEN", 0.0, ""}),
        std::runtime_error);
}

TEST_F(StorageManagerTest, GetAllItemsReturnsAll) {
    auto sm = make_sm();
    sm->createItem({"a1", "Item A", 10.0, "OPEN", 0.0, ""});
    sm->createItem({"a2", "Item B", 20.0, "OPEN", 0.0, ""});
    sm->createItem({"a3", "Item C", 30.0, "OPEN", 0.0, ""});

    auto all = sm->getAllItems();
    EXPECT_EQ(all.size(), 3u);
}

/// Item persistence across restart.
TEST_F(StorageManagerTest, ItemPersistsAcrossRestart) {
    {
        auto sm = make_sm();
        sm->createItem({"laptop", "Gaming Laptop", 500.0, "OPEN", 0.0, ""});
        sm->close();
    }
    {
        auto sm = make_sm();
        auto found = sm->getItem("laptop");
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->title, "Gaming Laptop");
        EXPECT_DOUBLE_EQ(found->starting_price, 500.0);
    }
}

// ---------------------------------------------------------------------------
// updateItemAfterBid
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, UpdateItemAfterBidCorrectness) {
    auto sm = make_sm();
    sm->createUser({"u1", "Alice"});
    sm->createItem({"itm", "Lamp", 50.0, "OPEN", 0.0, ""});

    sm->updateItemAfterBid("itm", 75.0, "u1");

    auto item = sm->getItem("itm");
    ASSERT_TRUE(item.has_value());
    EXPECT_DOUBLE_EQ(item->current_bid, 75.0);
    EXPECT_EQ(item->current_winner, "u1");
}

TEST_F(StorageManagerTest, UpdateItemAfterBidPersists) {
    {
        auto sm = make_sm();
        sm->createItem({"lamp", "Lamp", 50.0, "OPEN", 0.0, ""});
        sm->updateItemAfterBid("lamp", 80.0, "winner");
        sm->close();
    }
    {
        auto sm = make_sm();
        auto item = sm->getItem("lamp");
        ASSERT_TRUE(item.has_value());
        EXPECT_DOUBLE_EQ(item->current_bid, 80.0);
        EXPECT_EQ(item->current_winner, "winner");
    }
}

TEST_F(StorageManagerTest, UpdateNonexistentItemThrows) {
    auto sm = make_sm();
    EXPECT_THROW(sm->updateItemAfterBid("nope", 100.0, "u1"),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// closeItem
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, CloseItemSetsStatus) {
    auto sm = make_sm();
    sm->createItem({"tbl", "Table", 200.0, "OPEN", 0.0, ""});
    sm->closeItem("tbl");

    auto item = sm->getItem("tbl");
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->status, "CLOSED");
}

TEST_F(StorageManagerTest, CloseItemPersists) {
    {
        auto sm = make_sm();
        sm->createItem({"ch", "Chair", 100.0, "OPEN", 0.0, ""});
        sm->closeItem("ch");
        sm->close();
    }
    {
        auto sm = make_sm();
        auto item = sm->getItem("ch");
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(item->status, "CLOSED");
    }
}

// ---------------------------------------------------------------------------
// Bid tests
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, InsertBidAssignsMonotonicId) {
    auto sm = make_sm();
    sm->createUser({"u1", "Alice"});
    sm->createItem({"itm", "Clock", 100.0, "OPEN", 0.0, ""});

    uint64_t id0 = sm->insertBid({0, "itm", "u1", 110.0, ""});
    uint64_t id1 = sm->insertBid({0, "itm", "u1", 120.0, ""});
    uint64_t id2 = sm->insertBid({0, "itm", "u1", 130.0, ""});

    EXPECT_LT(id0, id1);
    EXPECT_LT(id1, id2);
}

TEST_F(StorageManagerTest, GetBidsReturnsHistory) {
    auto sm = make_sm();
    sm->createUser({"u1", "Alice"});
    sm->createUser({"u2", "Bob"});
    sm->createItem({"itm", "Vase", 100.0, "OPEN", 0.0, ""});

    sm->insertBid({0, "itm", "u1", 110.0, ""});
    sm->insertBid({0, "itm", "u2", 120.0, ""});
    sm->insertBid({0, "itm", "u1", 130.0, ""});

    auto bids = sm->getBids("itm");
    EXPECT_EQ(bids.size(), 3u);
    // Sorted by bid_id.
    EXPECT_LT(bids[0].bid_id, bids[1].bid_id);
    EXPECT_LT(bids[1].bid_id, bids[2].bid_id);
}

TEST_F(StorageManagerTest, GetBidsEmptyForUnknownItem) {
    auto sm = make_sm();
    EXPECT_TRUE(sm->getBids("ghost").empty());
}

TEST_F(StorageManagerTest, GetHighestBidCorrect) {
    auto sm = make_sm();
    sm->createItem({"itm", "Ring", 200.0, "OPEN", 0.0, ""});

    sm->insertBid({0, "itm", "u1", 210.0, ""});
    sm->insertBid({0, "itm", "u2", 250.0, ""});
    sm->insertBid({0, "itm", "u3", 230.0, ""});

    auto highest = sm->getHighestBid("itm");
    ASSERT_TRUE(highest.has_value());
    EXPECT_DOUBLE_EQ(highest->amount, 250.0);
    EXPECT_EQ(highest->user_id, "u2");
}

/// Bid persistence across restart.
TEST_F(StorageManagerTest, BidPersistsAcrossRestart) {
    uint64_t saved_id = 0;
    {
        auto sm = make_sm();
        sm->createItem({"itm", "Coin", 10.0, "OPEN", 0.0, ""});
        saved_id = sm->insertBid({0, "itm", "u1", 15.0, ""});
        sm->close();
    }
    {
        auto sm = make_sm();
        auto bids = sm->getBids("itm");
        ASSERT_EQ(bids.size(), 1u);
        EXPECT_EQ(bids[0].bid_id, saved_id);
        EXPECT_DOUBLE_EQ(bids[0].amount, 15.0);
    }
}

/// TC-STO-03: New page allocation as bids fill a page.
TEST_F(StorageManagerTest, TC_STO_03_BidsAllocateNewPage) {
    auto sm = make_sm();
    sm->createItem({"itm", "Antique", 100.0, "OPEN", 0.0, ""});

    // Fill more than one page worth of bids.
    std::size_t bids_per_page = PAGE_DATA_SIZE / BID_RECORD_SIZE;
    std::size_t total_bids    = bids_per_page + 5; // Overflow into next page.
    for (std::size_t i = 0; i < total_bids; ++i) {
        sm->insertBid({0, "itm", "u1",
                       100.0 + static_cast<double>(i), ""});
    }

    auto bids = sm->getBids("itm");
    EXPECT_EQ(bids.size(), total_bids);
    // There must be at least 2 bid pages (1 item page + at least 2 bid pages).
    EXPECT_GE(sm->pageCount(), 3u);
}

// ---------------------------------------------------------------------------
// Empty-id guard
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, EmptyUserIdThrows) {
    auto sm = make_sm();
    EXPECT_THROW(sm->createUser({"", "NoName"}), std::invalid_argument);
}

TEST_F(StorageManagerTest, EmptyItemIdThrows) {
    auto sm = make_sm();
    EXPECT_THROW(
        sm->createItem({"", "NoName", 1.0, "OPEN", 0.0, ""}),
        std::invalid_argument);
}
