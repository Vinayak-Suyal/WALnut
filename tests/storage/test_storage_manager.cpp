// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// tests/storage/test_storage_manager.cpp
//
// Schema v2.0 (OSDBMS-V-2026-T038) — updated test suite.
// Original 18 tests preserved; 12 new tests added for v2 storage methods.
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

    // Helper: build a fully-populated v2 Item.
    static Item makeV2Item(const std::string& item_id,
                           const std::string& host_id,
                           const std::string& category,
                           double             starting_price,
                           const std::string& status = "OPEN") {
        Item item;
        item.item_id        = item_id;
        item.host_id        = host_id;
        item.title          = "Item " + item_id;
        item.category       = category;
        item.condition      = "GOOD";
        item.starting_price = starting_price;
        item.status         = status;
        item.current_bid    = 0.0;
        return item;
    }
};

// ---------------------------------------------------------------------------
// User tests (original — unchanged)
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
// Item tests (original — unchanged)
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
// updateItemAfterBid — original + v2 podium tests
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

/// V2: Top-3 podium is populated and maintains descending order.
TEST_F(StorageManagerTest, TC_STO_V2_01_PodiumMaintainedDescending) {
    auto sm = make_sm();
    sm->createItem(makeV2Item("itm", "host1", "Art", 100.0));

    sm->updateItemAfterBid("itm", 110.0, "u1", 1, 1000);
    sm->updateItemAfterBid("itm", 130.0, "u2", 2, 2000);
    sm->updateItemAfterBid("itm", 150.0, "u3", 3, 3000);

    auto item = sm->getItem("itm");
    ASSERT_TRUE(item.has_value());
    // Podium: slot 0 = newest/highest (150), slot 1 = 130, slot 2 = 110.
    EXPECT_EQ(item->top_bid_count, 3u);
    EXPECT_DOUBLE_EQ(item->top_bids[0].amount, 150.0);
    EXPECT_DOUBLE_EQ(item->top_bids[1].amount, 130.0);
    EXPECT_DOUBLE_EQ(item->top_bids[2].amount, 110.0);
    EXPECT_EQ(item->top_bids[0].user_id, "u3");
    EXPECT_EQ(item->top_bids[1].user_id, "u2");
    EXPECT_EQ(item->top_bids[2].user_id, "u1");
}

/// V2: top_bid_count capped at TOP_BID_COUNT; total_bid_count is uncapped.
TEST_F(StorageManagerTest, TC_STO_V2_02_PodiumCappedTotalBidCountUncapped) {
    auto sm = make_sm();
    sm->createItem(makeV2Item("itm", "host1", "Art", 100.0));

    for (int i = 1; i <= 5; ++i) {
        sm->updateItemAfterBid("itm",
                               100.0 + i * 10.0,
                               "u" + std::to_string(i),
                               static_cast<uint64_t>(i),
                               static_cast<uint64_t>(i * 1000));
    }

    auto item = sm->getItem("itm");
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->top_bid_count,   static_cast<uint32_t>(TOP_BID_COUNT)); // capped at 3
    EXPECT_EQ(item->total_bid_count, 5u);   // all 5 recorded
    // Slot 0 must hold the most recent/highest bid.
    EXPECT_DOUBLE_EQ(item->top_bids[0].amount, 150.0);
}

/// V2: Podium and total_bid_count persist across restart.
TEST_F(StorageManagerTest, TC_STO_V2_03_PodiumPersistsAcrossRestart) {
    {
        auto sm = make_sm();
        sm->createItem(makeV2Item("itm", "host1", "Tech", 200.0));
        sm->updateItemAfterBid("itm", 210.0, "u1", 1, 1000);
        sm->updateItemAfterBid("itm", 230.0, "u2", 2, 2000);
        sm->close();
    }
    {
        auto sm = make_sm();
        auto item = sm->getItem("itm");
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(item->top_bid_count,   2u);
        EXPECT_EQ(item->total_bid_count, 2u);
        EXPECT_DOUBLE_EQ(item->top_bids[0].amount, 230.0);
        EXPECT_DOUBLE_EQ(item->top_bids[1].amount, 210.0);
    }
}

// ---------------------------------------------------------------------------
// closeItem (original — unchanged)
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
// cancelItem (new v2)
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, TC_STO_V2_04_CancelItemSetsStatus) {
    auto sm = make_sm();
    sm->createItem(makeV2Item("ring", "host1", "Jewelry", 500.0));
    sm->cancelItem("ring");

    auto item = sm->getItem("ring");
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->status, "CANCEL");
}

TEST_F(StorageManagerTest, TC_STO_V2_05_CancelItemPersists) {
    {
        auto sm = make_sm();
        sm->createItem(makeV2Item("vase", "host1", "Art", 200.0));
        sm->cancelItem("vase");
        sm->close();
    }
    {
        auto sm = make_sm();
        auto item = sm->getItem("vase");
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(item->status, "CANCEL");
    }
}

// ---------------------------------------------------------------------------
// markUnsoldItem (new v2)
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, TC_STO_V2_06_MarkUnsoldSetsStatus) {
    auto sm = make_sm();
    sm->createItem(makeV2Item("lamp", "host1", "Home", 50.0));
    sm->markUnsoldItem("lamp");

    auto item = sm->getItem("lamp");
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->status, "UNSOLD");
}

// ---------------------------------------------------------------------------
// getItemsWonByUser (new v2)
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, TC_STO_V2_07_GetItemsWonByUser) {
    auto sm = make_sm();
    sm->createItem(makeV2Item("item-a", "host1", "Art",  100.0));
    sm->createItem(makeV2Item("item-b", "host1", "Tech", 200.0));
    sm->createItem(makeV2Item("item-c", "host2", "Art",  50.0));

    // item-a: won by alice (CLOSED)
    sm->updateItemAfterBid("item-a", 150.0, "alice");
    sm->closeItem("item-a");

    // item-b: won by bob (CLOSED)
    sm->updateItemAfterBid("item-b", 250.0, "bob");
    sm->closeItem("item-b");

    // item-c: still OPEN
    sm->updateItemAfterBid("item-c", 80.0, "alice");

    auto alice_wins = sm->getItemsWonByUser("alice");
    ASSERT_EQ(alice_wins.size(), 1u);
    EXPECT_EQ(alice_wins[0].item_id, "item-a");

    auto bob_wins = sm->getItemsWonByUser("bob");
    ASSERT_EQ(bob_wins.size(), 1u);
    EXPECT_EQ(bob_wins[0].item_id, "item-b");

    // Open items are NOT returned even if alice is winning.
    EXPECT_EQ(sm->getItemsWonByUser("nobody").size(), 0u);
}

// ---------------------------------------------------------------------------
// getItemsByHost (new v2)
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, TC_STO_V2_08_GetItemsByHost) {
    auto sm = make_sm();
    sm->createItem(makeV2Item("h1-a", "host1", "Art",  100.0));
    sm->createItem(makeV2Item("h1-b", "host1", "Tech", 200.0));
    sm->createItem(makeV2Item("h2-a", "host2", "Art",   50.0));

    auto h1_items = sm->getItemsByHost("host1");
    EXPECT_EQ(h1_items.size(), 2u);

    auto h2_items = sm->getItemsByHost("host2");
    EXPECT_EQ(h2_items.size(), 1u);

    auto ghost = sm->getItemsByHost("nobody");
    EXPECT_TRUE(ghost.empty());
}

// ---------------------------------------------------------------------------
// updateUser (new v2)
// ---------------------------------------------------------------------------
TEST_F(StorageManagerTest, TC_STO_V2_09_UpdateUserPersistsNewFields) {
    auto sm = make_sm();
    sm->createUser({"alice", "Alice"});

    // Mutate email, role, password_hash.
    User updated{"alice", "Alice Wonderland"};
    updated.email         = "alice@example.com";
    updated.role          = "HOST";
    updated.password_hash = "hash1234";
    sm->updateUser(updated);

    auto found = sm->findUserById("alice");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->email,         "alice@example.com");
    EXPECT_EQ(found->role,          "HOST");
    EXPECT_EQ(found->password_hash, "hash1234");
}

TEST_F(StorageManagerTest, TC_STO_V2_10_UpdateNonexistentUserThrows) {
    auto sm = make_sm();
    EXPECT_THROW(sm->updateUser({"ghost", "Ghost"}), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Bid tests (original — unchanged)
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
// Empty-id guard (original — unchanged)
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

// ===========================================================================
// Phase 4: Category Secondary Index Tests
// ===========================================================================

/// TC_CAT_01: Basic category index lookup.
///   2 Electronics + 1 Art → correct per-category counts, unknown = empty.
TEST_F(StorageManagerTest, TC_CAT_01_CategoryIndexLookup) {
    auto sm = make_sm();

    Item e1 = makeV2Item("e1", "host1", "Electronics", 100.0);
    Item e2 = makeV2Item("e2", "host1", "Electronics", 200.0);
    Item a1 = makeV2Item("a1", "host1", "Art",         50.0);

    sm->createItem(e1);
    sm->createItem(e2);
    sm->createItem(a1);

    auto elec = sm->getItemsByCategory("Electronics");
    EXPECT_EQ(elec.size(), 2u);

    auto art = sm->getItemsByCategory("Art");
    ASSERT_EQ(art.size(), 1u);
    EXPECT_EQ(art[0].item_id, "a1");

    auto unknown = sm->getItemsByCategory("Unknown");
    EXPECT_TRUE(unknown.empty());
}

/// TC_CAT_02: Category index is rebuilt correctly after a StorageManager
///   restart (close + re-open of the same data directory).
TEST_F(StorageManagerTest, TC_CAT_02_CategoryIndexPersistsAcrossRestart) {
    {
        auto sm = make_sm();
        sm->createItem(makeV2Item("x1", "host1", "Electronics", 100.0));
        sm->createItem(makeV2Item("x2", "host1", "Electronics", 150.0));
        sm->createItem(makeV2Item("x3", "host1", "Art",         75.0));
        sm->sync();
        sm->close();
    }

    // Re-open the same directory.
    auto sm2 = make_sm();

    auto elec = sm2->getItemsByCategory("Electronics");
    EXPECT_EQ(elec.size(), 2u);

    auto art = sm2->getItemsByCategory("Art");
    EXPECT_EQ(art.size(), 1u);
}

/// TC_CAT_03: Cancelled items remain in the category index (status-filtering
///   is the caller's responsibility per the spec).
TEST_F(StorageManagerTest, TC_CAT_03_CategoryIndexAfterCancel) {
    auto sm = make_sm();

    sm->createItem(makeV2Item("c1", "host1", "Electronics", 100.0));
    sm->createItem(makeV2Item("c2", "host1", "Electronics", 200.0));

    // Cancel one of them.
    sm->cancelItem("c1");

    // Both must still appear in the category lookup.
    auto elec = sm->getItemsByCategory("Electronics");
    EXPECT_EQ(elec.size(), 2u);

    // Confirm status is correctly reflected when the item is fetched.
    bool found_cancelled = false;
    for (const auto& item : elec) {
        if (item.item_id == "c1") {
            EXPECT_EQ(item.status, "CANCEL");
            found_cancelled = true;
        }
    }
    EXPECT_TRUE(found_cancelled);
}
