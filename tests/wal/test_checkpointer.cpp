// =============================================================================
// WALnut – Test Suite (Kanak Rawat)
// tests/wal/test_checkpointer.cpp
//
// Unit tests for the fuzzy Checkpointer.
// =============================================================================

#include "walnut/wal/checkpointer.hpp"
#include "walnut/wal/log_manager.hpp"
#include "walnut/storage/metadata.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

namespace walnut {
namespace {

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class CheckpointerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = "data/test_checkpointer_" + std::to_string(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        fs::create_directories(test_dir_);
        wal_path_  = test_dir_ + "/walnut.wal";
        meta_path_ = test_dir_ + "/walnut.meta";
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string test_dir_;
    std::string wal_path_;
    std::string meta_path_;
};

// ===========================================================================
// TC_CKP_01: Manual checkpoint writes a CHECKPOINT record and updates meta.
// ===========================================================================
TEST_F(CheckpointerTest, TC_CKP_01_ManualCheckpoint) {
    LogManager lm(wal_path_);
    MetadataManager mm(meta_path_);
    mm.load();

    EXPECT_EQ(mm.meta().checkpoint_lsn, 0u);

    Checkpointer ckp(lm, mm);
    ckp.TakeCheckpoint();

    // checkpoint_lsn should be updated.
    EXPECT_GT(mm.meta().checkpoint_lsn, 0u);

    // The WAL should contain a CHECKPOINT record.
    auto records = lm.ReadAllRecords();
    ASSERT_GE(records.size(), 1u);

    bool found_checkpoint = false;
    for (const auto& rec : records) {
        if (rec.type == LogRecordType::CHECKPOINT) {
            found_checkpoint = true;
            break;
        }
    }
    EXPECT_TRUE(found_checkpoint);
    EXPECT_EQ(ckp.CheckpointCount(), 1u);
}

// ===========================================================================
// TC_CKP_02: Multiple checkpoints increment the count.
// ===========================================================================
TEST_F(CheckpointerTest, TC_CKP_02_MultipleCheckpoints) {
    LogManager lm(wal_path_);
    MetadataManager mm(meta_path_);
    mm.load();

    Checkpointer ckp(lm, mm);
    ckp.TakeCheckpoint();
    ckp.TakeCheckpoint();
    ckp.TakeCheckpoint();

    EXPECT_EQ(ckp.CheckpointCount(), 3u);

    // checkpoint_lsn should reflect the last checkpoint's LSN.
    auto records = lm.ReadAllRecords();
    uint64_t last_ckp_lsn = 0;
    for (const auto& rec : records) {
        if (rec.type == LogRecordType::CHECKPOINT) {
            last_ckp_lsn = rec.lsn;
        }
    }
    EXPECT_EQ(mm.meta().checkpoint_lsn, static_cast<uint32_t>(last_ckp_lsn));
}

// ===========================================================================
// TC_CKP_03: Background thread starts and stops cleanly.
// ===========================================================================
TEST_F(CheckpointerTest, TC_CKP_03_BackgroundStartStop) {
    LogManager lm(wal_path_);
    MetadataManager mm(meta_path_);
    mm.load();

    // Use a very short interval for testing (1 second).
    Checkpointer ckp(lm, mm, std::chrono::seconds(1));

    ckp.Start();
    // Let it run for a bit.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    ckp.Stop();

    // At least one checkpoint should have been taken.
    EXPECT_GE(ckp.CheckpointCount(), 1u);
}

// ===========================================================================
// TC_CKP_04: Checkpoint after WAL records preserves correct LSN.
// ===========================================================================
TEST_F(CheckpointerTest, TC_CKP_04_CheckpointAfterRecords) {
    LogManager lm(wal_path_);
    MetadataManager mm(meta_path_);
    mm.load();

    // Append some records first.
    for (int i = 0; i < 10; ++i) {
        LogRecord rec;
        rec.txn_id = static_cast<uint64_t>(i + 1);
        rec.type   = LogRecordType::BEGIN;
        lm.AppendLogRecord(rec);
    }

    Checkpointer ckp(lm, mm);
    ckp.TakeCheckpoint();

    // checkpoint_lsn should be the LSN of the CHECKPOINT record.
    // That record was appended after the 10 BEGIN records.
    EXPECT_EQ(mm.meta().checkpoint_lsn, 11u);
}

// ===========================================================================
// TC_CKP_05: Double Stop is safe (no crash).
// ===========================================================================
TEST_F(CheckpointerTest, TC_CKP_05_DoubleStopIsSafe) {
    LogManager lm(wal_path_);
    MetadataManager mm(meta_path_);
    mm.load();

    Checkpointer ckp(lm, mm, std::chrono::seconds(60));
    ckp.Start();
    ckp.Stop();
    ckp.Stop();  // Should be a no-op.
}

} // namespace
} // namespace walnut
