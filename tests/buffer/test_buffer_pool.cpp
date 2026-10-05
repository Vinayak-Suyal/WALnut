// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// tests/buffer/test_buffer_pool.cpp
// =============================================================================

#include <gtest/gtest.h>
#include "walnut/buffer/buffer_pool.hpp"
#include "walnut/storage/file_manager.hpp"
#include "walnut/common/constants.hpp"
#include <filesystem>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <stdexcept>

using namespace walnut;

// ---------------------------------------------------------------------------
// Test fixture: temp directory + FileManager with pre-allocated pages.
// ---------------------------------------------------------------------------
class BufferPoolTest : public ::testing::Test {
protected:
    std::string test_dir_;
    std::unique_ptr<FileManager> fm_;

    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path().string() +
                    "/walnut_bp_test_" +
                    std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count());
        std::filesystem::create_directories(test_dir_);

        fm_ = std::make_unique<FileManager>(test_dir_);
        fm_->initialize();

        // Pre-allocate 16 pages of each type.
        for (int i = 0; i < 16; ++i) fm_->allocatePage(PAGE_TYPE_USERS);
    }

    void TearDown() override {
        fm_->close();
        std::filesystem::remove_all(test_dir_);
    }
};

// ---------------------------------------------------------------------------
// TC-BUF-04: Hit / miss accounting
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, TC_BUF_04_HitMissAccounting) {
    BufferPool bp(4);
    bp.resetStats();

    // Miss: fetch pages 0,1,2,3  (all misses – pool was empty)
    for (uint32_t pid = 0; pid < 4; ++pid) {
        auto& p = bp.fetchPage(pid, *fm_);
        (void)p;
        bp.unpinPage(pid, false);
    }

    // Hit: fetch page 0 again
    {
        auto& p = bp.fetchPage(0, *fm_);
        (void)p;
        bp.unpinPage(0, false);
    }

    auto stats = bp.getStats();
    EXPECT_EQ(stats.misses, 4u);
    EXPECT_EQ(stats.hits,   1u);
}

// ---------------------------------------------------------------------------
// TC-BUF-01: Pinned page is never evicted
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, TC_BUF_01_PinnedPageNotEvicted) {
    BufferPool bp(4); // 4 frames

    // Pin page 0 without unpinning.
    auto& pinned = bp.fetchPage(0, *fm_);
    (void)pinned;
    // Do NOT unpin – pin_count = 1.

    // Fill remaining 3 frames with other pages.
    for (uint32_t pid = 1; pid <= 3; ++pid) {
        auto& p = bp.fetchPage(pid, *fm_);
        (void)p;
        bp.unpinPage(pid, false);
    }

    // Trigger evictions: access 4 new pages that need frames.
    // The pinned page 0 must survive.
    for (uint32_t pid = 4; pid <= 7; ++pid) {
        auto& p = bp.fetchPage(pid, *fm_);
        (void)p;
        bp.unpinPage(pid, false);
    }

    // Page 0 must still be in the pool (we never freed it).
    // The simplest check: pin it again without a miss (hits counter increases).
    bp.resetStats();
    auto& still_there = bp.fetchPage(0, *fm_);
    (void)still_there;
    bp.unpinPage(0, false); // Now unpin both the original and this extra pin.
    bp.unpinPage(0, false);

    auto stats = bp.getStats();
    EXPECT_EQ(stats.hits, 1u);   // Page 0 was found in pool.
    EXPECT_EQ(stats.misses, 0u);
}

// ---------------------------------------------------------------------------
// TC-BUF-02: Clock replacement selects the correct victim
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, TC_BUF_02_ClockReplacementPolicy) {
    // 3-frame pool.
    BufferPool bp(3);

    // 1. Fill pool with pages 0, 1, 2.
    // Pool: [0, 1, 2]. All ref bits = 1. clock_hand = 0.
    for (uint32_t pid = 0; pid <= 2; ++pid) {
        auto& p = bp.fetchPage(pid, *fm_);
        (void)p;
        bp.unpinPage(pid, false);
    }

    // 2. Fetch page 3 to trigger first eviction.
    // Sweeps 0,1,2 (clearing ref bits), then evicts 0.
    // Pool: [3, 1, 2]. Refs: [1, 0, 0]. clock_hand = 1.
    {
        auto& p = bp.fetchPage(3, *fm_);
        (void)p;
        bp.unpinPage(3, false);
    }

    // 3. Re-access page 1 to raise its reference bit.
    // Pool: [3, 1, 2]. Refs: [1, 1, 0]. clock_hand = 1.
    {
        auto& p = bp.fetchPage(1, *fm_);
        (void)p;
        bp.unpinPage(1, false);
    }

    bp.resetStats();

    // 4. Fetch page 4 -> will cause an eviction.
    // clock_hand is at 1.
    // Sweeps 1 (clears ref bit), then evicts 2 (ref was 0).
    // Pool: [3, 1, 4].
    {
        auto& p = bp.fetchPage(4, *fm_);
        (void)p;
        bp.unpinPage(4, false);
    }

    auto stats = bp.getStats();
    EXPECT_EQ(stats.misses,    1u);
    EXPECT_EQ(stats.evictions, 1u);

    // Page 1 must still be in the pool (was recently referenced).
    bp.resetStats();
    {
        auto& p = bp.fetchPage(1, *fm_);
        (void)p;
        bp.unpinPage(1, false);
    }
    EXPECT_EQ(bp.getStats().hits, 1u);
}

// ---------------------------------------------------------------------------
// Dirty page is flushed before eviction (and the write count goes up)
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, DirtyPageFlushedBeforeEviction) {
    BufferPool bp(2);

    // Fetch page 0 and mark it dirty.
    {
        auto& p = bp.fetchPage(0, *fm_);
        (void)p;
        bp.unpinPage(0, /*dirty=*/true);
    }
    // Fetch page 1 (pool has 1 free frame).
    {
        auto& p = bp.fetchPage(1, *fm_);
        (void)p;
        bp.unpinPage(1, false);
    }

    auto stats_before = bp.getStats();

    // Fetch page 2 → pool full; must evict one of {0,1}.
    // The dirty one must be flushed first.
    {
        auto& p = bp.fetchPage(2, *fm_);
        (void)p;
        bp.unpinPage(2, false);
    }

    auto stats_after = bp.getStats();
    EXPECT_GE(stats_after.dirty_evictions, 1u);
}

// ---------------------------------------------------------------------------
// flushPage flushes only the specified page.
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, FlushPageFlushesSpecificPage) {
    BufferPool bp(4);
    {
        auto& p = bp.fetchPage(0, *fm_);
        (void)p;
        bp.unpinPage(0, /*dirty=*/true);
    }
    // After flushPage, frame should be clean.
    bp.flushPage(0, *fm_);

    // Re-access page 0 (should be a hit).
    bp.resetStats();
    {
        auto& p = bp.fetchPage(0, *fm_);
        (void)p;
        bp.unpinPage(0, false);
    }
    EXPECT_EQ(bp.getStats().hits, 1u);
}

// ---------------------------------------------------------------------------
// flushAllPages
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, FlushAllPagesFlushesDirtyFrames) {
    BufferPool bp(4);
    for (uint32_t pid = 0; pid < 4; ++pid) {
        auto& p = bp.fetchPage(pid, *fm_);
        (void)p;
        bp.unpinPage(pid, /*dirty=*/true);
    }
    EXPECT_NO_THROW(bp.flushAllPages(*fm_));
}

// ---------------------------------------------------------------------------
// Pooling exhaustion: all frames pinned
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, AllFramesPinnedThrows) {
    BufferPool bp(3);
    // Fill and pin all 3 frames.
    for (uint32_t pid = 0; pid < 3; ++pid) {
        auto& p = bp.fetchPage(pid, *fm_);
        (void)p;
        // Do NOT unpin.
    }
    // A 4th fetch must fail.
    EXPECT_THROW(bp.fetchPage(3, *fm_), std::runtime_error);

    // Cleanup pins.
    for (uint32_t pid = 0; pid < 3; ++pid) bp.unpinPage(pid, false);
}

// ---------------------------------------------------------------------------
// Stats reset
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, StatsResetWorks) {
    BufferPool bp(4);
    auto& p = bp.fetchPage(0, *fm_);
    (void)p;
    bp.unpinPage(0, false);

    EXPECT_GE(bp.getStats().misses, 1u);
    bp.resetStats();
    EXPECT_EQ(bp.getStats().misses, 0u);
    EXPECT_EQ(bp.getStats().hits,   0u);
}

// ---------------------------------------------------------------------------
// WAL pre-flush hook
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, PreFlushHookCanBlockFlush) {
    BufferPool bp(2);
    bool hook_called = false;

    // Hook always returns false → no flush should happen.
    bp.setPreFlushHook([&](uint32_t /*pid*/) -> bool {
        hook_called = true;
        return false; // veto the flush
    });

    {
        auto& p = bp.fetchPage(0, *fm_);
        (void)p;
        bp.unpinPage(0, /*dirty=*/true);
    }
    bp.flushPage(0, *fm_);

    EXPECT_TRUE(hook_called);
    // Eviction stats for dirty pages should be 0 (flush was vetoed).
    EXPECT_EQ(bp.getStats().dirty_evictions, 0u);
}

// ---------------------------------------------------------------------------
// Concurrent access safety
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, ConcurrentFetchUnpinIsSafe) {
    BufferPool bp(16);
    std::atomic<int> errors{0};

    // We have 16 pre-allocated pages (pages 0-15).
    auto worker = [&](int thread_id) {
        try {
            for (int iter = 0; iter < 20; ++iter) {
                // Each thread touches 2 pages, cycling through 0-7.
                uint32_t pid_a = static_cast<uint32_t>((thread_id * 2 + iter) % 8);
                uint32_t pid_b = static_cast<uint32_t>((thread_id * 2 + iter + 1) % 8);

                auto& pa = bp.fetchPage(pid_a, *fm_);
                (void)pa;
                auto& pb = bp.fetchPage(pid_b, *fm_);
                (void)pb;

                bp.unpinPage(pid_a, false);
                bp.unpinPage(pid_b, false);
            }
        } catch (...) {
            ++errors;
        }
    };

    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(errors.load(), 0);
}

// ---------------------------------------------------------------------------
// pinPage / unpinPage on a non-resident page throw
// ---------------------------------------------------------------------------
TEST_F(BufferPoolTest, PinNonResidentPageThrows) {
    BufferPool bp(4);
    EXPECT_THROW(bp.pinPage(99), std::runtime_error);
}

TEST_F(BufferPoolTest, UnpinNonResidentPageThrows) {
    BufferPool bp(4);
    EXPECT_THROW(bp.unpinPage(99, false), std::runtime_error);
}
