// =============================================================================
// WALnut – Test Suite (Kanak Rawat)
// tests/index/test_hash_index.cpp
//
// Unit tests for the concurrent HashIndex.
// =============================================================================

#include "walnut/index/hash_index.hpp"
#include "walnut/storage/file_manager.hpp"
#include "walnut/common/constants.hpp"
#include "walnut/common/types.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <string>

namespace fs = std::filesystem;

namespace walnut {
namespace {

// ===========================================================================
// TC_IDX_01: Insert and Lookup a single entry.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_01_InsertAndLookup) {
    HashIndex idx(64);

    idx.Insert("user_001", {5, 3});
    auto result = idx.Lookup("user_001");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->page_id, 5u);
    EXPECT_EQ(result->slot_id, 3u);
}

// ===========================================================================
// TC_IDX_02: Lookup returns nullopt for missing key.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_02_LookupMissReturnsNullopt) {
    HashIndex idx(64);

    auto result = idx.Lookup("nonexistent");
    EXPECT_FALSE(result.has_value());
}

// ===========================================================================
// TC_IDX_03: Insert duplicate key updates the location.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_03_InsertDuplicateUpdates) {
    HashIndex idx(64);

    idx.Insert("item_42", {1, 0});
    idx.Insert("item_42", {2, 5});

    auto result = idx.Lookup("item_42");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->page_id, 2u);
    EXPECT_EQ(result->slot_id, 5u);

    EXPECT_EQ(idx.Size(), 1u);
}

// ===========================================================================
// TC_IDX_04: Delete removes the entry.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_04_DeleteRemoves) {
    HashIndex idx(64);

    idx.Insert("key_to_delete", {0, 0});
    EXPECT_EQ(idx.Size(), 1u);

    bool deleted = idx.Delete("key_to_delete");
    EXPECT_TRUE(deleted);
    EXPECT_EQ(idx.Size(), 0u);

    auto result = idx.Lookup("key_to_delete");
    EXPECT_FALSE(result.has_value());
}

// ===========================================================================
// TC_IDX_05: Delete nonexistent key returns false.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_05_DeleteNonexistentReturnsFalse) {
    HashIndex idx(64);

    bool deleted = idx.Delete("ghost");
    EXPECT_FALSE(deleted);
}

// ===========================================================================
// TC_IDX_06: Update changes the location for an existing key.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_06_UpdateExistingKey) {
    HashIndex idx(64);

    idx.Insert("updatable", {1, 0});
    bool updated = idx.Update("updatable", {9, 9});
    EXPECT_TRUE(updated);

    auto result = idx.Lookup("updatable");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->page_id, 9u);
    EXPECT_EQ(result->slot_id, 9u);
}

// ===========================================================================
// TC_IDX_07: Update nonexistent key returns false.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_07_UpdateNonexistentReturnsFalse) {
    HashIndex idx(64);

    bool updated = idx.Update("no_such_key", {1, 1});
    EXPECT_FALSE(updated);
}

// ===========================================================================
// TC_IDX_08: Size returns correct count after insertions and deletions.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_08_SizeIsAccurate) {
    HashIndex idx(64);

    for (int i = 0; i < 100; ++i) {
        idx.Insert("key_" + std::to_string(i), {static_cast<uint32_t>(i), 0});
    }
    EXPECT_EQ(idx.Size(), 100u);

    for (int i = 0; i < 50; ++i) {
        idx.Delete("key_" + std::to_string(i));
    }
    EXPECT_EQ(idx.Size(), 50u);
}

// ===========================================================================
// TC_IDX_09: Concurrent insert/lookup from multiple threads.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_09_ConcurrentInsertLookup) {
    HashIndex idx(128);

    constexpr int NUM_THREADS = 8;
    constexpr int OPS_PER_THREAD = 500;

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&idx, t, OPS_PER_THREAD]() {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
                idx.Insert(key, {static_cast<uint32_t>(t),
                                 static_cast<uint16_t>(i)});
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(idx.Size(), static_cast<std::size_t>(NUM_THREADS * OPS_PER_THREAD));

    // Verify all entries.
    for (int t = 0; t < NUM_THREADS; ++t) {
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
            auto result = idx.Lookup(key);
            ASSERT_TRUE(result.has_value()) << "Missing key: " << key;
            EXPECT_EQ(result->page_id, static_cast<uint32_t>(t));
            EXPECT_EQ(result->slot_id, static_cast<uint16_t>(i));
        }
    }
}

// ===========================================================================
// TC_IDX_10: BuildFromDisk indexes users and items.
// ===========================================================================
TEST(HashIndexTest, TC_IDX_10_BuildFromDisk) {
    std::string test_dir = "data/test_hash_build_" + std::to_string(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    fs::create_directories(test_dir);

    {
        FileManager fm(test_dir);
        fm.initialize();

        // Create a user page and write a user record.
        uint32_t user_pid = fm.allocatePage(PAGE_TYPE_USERS);
        {
            Page page = fm.readPage(user_pid);

            // Serialize a user record manually (USER_RECORD_SIZE = 192).
            uint8_t rec_buf[USER_RECORD_SIZE] = {};
            std::string user_id = "test_user_42";
            std::memcpy(rec_buf, user_id.c_str(), user_id.size());

            page.appendRecord(rec_buf, USER_RECORD_SIZE);
            page.updateChecksum();
            fm.writePage(page);
        }

        // Create an item page and write an item record.
        uint32_t item_pid = fm.allocatePage(PAGE_TYPE_ITEMS);
        {
            Page page = fm.readPage(item_pid);

            uint8_t rec_buf[ITEM_RECORD_SIZE] = {};
            std::string item_id = "item_007";
            std::memcpy(rec_buf, item_id.c_str(), item_id.size());

            page.appendRecord(rec_buf, ITEM_RECORD_SIZE);
            page.updateChecksum();
            fm.writePage(page);
        }

        fm.sync();

        // Build index from disk.
        HashIndex idx(64);
        idx.BuildFromDisk(fm);

        auto user_loc = idx.Lookup("test_user_42");
        ASSERT_TRUE(user_loc.has_value());
        EXPECT_EQ(user_loc->page_id, user_pid);
        EXPECT_EQ(user_loc->slot_id, 0u);

        auto item_loc = idx.Lookup("item_007");
        ASSERT_TRUE(item_loc.has_value());
        EXPECT_EQ(item_loc->page_id, item_pid);
        EXPECT_EQ(item_loc->slot_id, 0u);

        fm.close();
    }

    fs::remove_all(test_dir);
}

// ===========================================================================
// TC_IDX_11: Small bucket count still works (collision stress).
// ===========================================================================
TEST(HashIndexTest, TC_IDX_11_CollisionStress) {
    HashIndex idx(4);  // Only 4 buckets — lots of collisions.

    for (int i = 0; i < 200; ++i) {
        idx.Insert("collision_key_" + std::to_string(i),
                   {static_cast<uint32_t>(i), 0});
    }
    EXPECT_EQ(idx.Size(), 200u);

    for (int i = 0; i < 200; ++i) {
        auto result = idx.Lookup("collision_key_" + std::to_string(i));
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->page_id, static_cast<uint32_t>(i));
    }
}

} // namespace
} // namespace walnut
