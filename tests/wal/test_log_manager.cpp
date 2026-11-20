// =============================================================================
// WALnut – Test Suite (Kanak Rawat)
// tests/wal/test_log_manager.cpp
//
// Unit tests for LogManager: append, flush, group commit, serialisation
// round-trip, corruption detection, and the pre-flush hook.
// =============================================================================

#include "walnut/wal/log_manager.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <stdexcept>

namespace fs = std::filesystem;

namespace walnut {
namespace {

// ---------------------------------------------------------------------------
// Test fixture – creates a temp directory for each test.
// ---------------------------------------------------------------------------
class LogManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = "data/test_log_manager_" + std::to_string(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        fs::create_directories(test_dir_);
        wal_path_ = test_dir_ + "/walnut.wal";
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string test_dir_;
    std::string wal_path_;
};

// ===========================================================================
// TC_WAL_01: Append a BEGIN record, flush, read back, assert LSN == 1.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_01_AppendAndFlush) {
    LogManager lm(wal_path_);

    LogRecord rec;
    rec.txn_id = 100;
    rec.type   = LogRecordType::BEGIN;

    uint64_t lsn = lm.AppendLogRecord(rec);
    EXPECT_EQ(lsn, 1u);

    lm.FlushAll();
    EXPECT_EQ(lm.GetFlushedLSN(), 1u);

    // Read back.
    auto records = lm.ReadAllRecords();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].lsn, 1u);
    EXPECT_EQ(records[0].txn_id, 100u);
    EXPECT_EQ(records[0].type, LogRecordType::BEGIN);
}

// ===========================================================================
// TC_WAL_02: Append 100 records, assert LSNs 1..100.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_02_MultipleRecordsPreserveLSNOrder) {
    LogManager lm(wal_path_);

    for (int i = 0; i < 100; ++i) {
        LogRecord rec;
        rec.txn_id = static_cast<uint64_t>(i);
        rec.type   = LogRecordType::BEGIN;
        uint64_t lsn = lm.AppendLogRecord(rec);
        EXPECT_EQ(lsn, static_cast<uint64_t>(i + 1));
    }

    lm.FlushAll();
    auto records = lm.ReadAllRecords();
    ASSERT_EQ(records.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(records[i].lsn, static_cast<uint64_t>(i + 1));
    }
}

// ===========================================================================
// TC_WAL_03: Flush twice with the same target_lsn (idempotent).
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_03_FlushIsIdempotent) {
    LogManager lm(wal_path_);

    LogRecord rec;
    rec.txn_id = 1;
    rec.type   = LogRecordType::BEGIN;
    uint64_t lsn = lm.AppendLogRecord(rec);

    lm.Flush(lsn);
    lm.Flush(lsn);  // Should be a no-op.

    EXPECT_EQ(lm.GetFlushedLSN(), lsn);
    auto records = lm.ReadAllRecords();
    ASSERT_EQ(records.size(), 1u);
}

// ===========================================================================
// TC_WAL_04: 10 threads each append 100 records concurrently.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_04_GroupCommitConcurrency) {
    LogManager lm(wal_path_);

    constexpr int NUM_THREADS = 10;
    constexpr int RECORDS_PER_THREAD = 100;

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&lm, t, RECORDS_PER_THREAD]() {
            for (int i = 0; i < RECORDS_PER_THREAD; ++i) {
                LogRecord rec;
                rec.txn_id = static_cast<uint64_t>(t * 1000 + i);
                rec.type   = LogRecordType::BEGIN;
                lm.AppendLogRecord(rec);
            }
            lm.FlushAll();
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto records = lm.ReadAllRecords();
    EXPECT_EQ(records.size(), NUM_THREADS * RECORDS_PER_THREAD);

    // All LSNs should be unique.
    std::vector<uint64_t> lsns;
    lsns.reserve(records.size());
    for (const auto& r : records) {
        lsns.push_back(r.lsn);
    }
    std::sort(lsns.begin(), lsns.end());
    for (size_t i = 0; i < lsns.size(); ++i) {
        EXPECT_EQ(lsns[i], static_cast<uint64_t>(i + 1));
    }
}

// ===========================================================================
// TC_WAL_05: Append 50 records, ReadLogFrom(25), assert 26 records.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_05_ReadLogFromReturnsCorrectSubset) {
    LogManager lm(wal_path_);

    for (int i = 0; i < 50; ++i) {
        LogRecord rec;
        rec.txn_id = static_cast<uint64_t>(i);
        rec.type   = LogRecordType::BEGIN;
        lm.AppendLogRecord(rec);
    }
    lm.FlushAll();

    auto subset = lm.ReadLogFrom(25);
    EXPECT_EQ(subset.size(), 26u);  // LSNs 25..50
    EXPECT_EQ(subset.front().lsn, 25u);
    EXPECT_EQ(subset.back().lsn, 50u);
}

// ===========================================================================
// TC_WAL_06: Serialize → Deserialize round-trip for an UPDATE record.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_06_SerializeDeserializeRoundTrip) {
    LogRecord rec;
    rec.lsn           = 42;
    rec.prev_lsn      = 10;
    rec.txn_id        = 7;
    rec.type           = LogRecordType::UPDATE;
    rec.page_id        = 5;
    rec.offset         = 100;
    rec.length         = 4;
    rec.undo_next_lsn  = 0;
    rec.before_image   = {0xAA, 0xBB, 0xCC, 0xDD};
    rec.after_image    = {0x11, 0x22, 0x33, 0x44};

    auto serialized = rec.Serialize();
    EXPECT_EQ(serialized.size(), rec.SerializedSize());

    // Skip the 4-byte record_size prefix.
    uint32_t record_size = 0;
    std::memcpy(&record_size, serialized.data(), 4);

    LogRecord deserialized = LogRecord::Deserialize(
        serialized.data() + 4, record_size);

    EXPECT_EQ(deserialized.lsn, rec.lsn);
    EXPECT_EQ(deserialized.prev_lsn, rec.prev_lsn);
    EXPECT_EQ(deserialized.txn_id, rec.txn_id);
    EXPECT_EQ(deserialized.type, rec.type);
    EXPECT_EQ(deserialized.page_id, rec.page_id);
    EXPECT_EQ(deserialized.offset, rec.offset);
    EXPECT_EQ(deserialized.length, rec.length);
    EXPECT_EQ(deserialized.undo_next_lsn, rec.undo_next_lsn);
    EXPECT_EQ(deserialized.before_image, rec.before_image);
    EXPECT_EQ(deserialized.after_image, rec.after_image);
}

// ===========================================================================
// TC_WAL_07: Corrupt a byte in the WAL; ReadAllRecords stops there.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_07_CorruptedRecordDetected) {
    {
        LogManager lm(wal_path_);

        for (int i = 0; i < 5; ++i) {
            LogRecord rec;
            rec.txn_id = static_cast<uint64_t>(i + 1);
            rec.type   = LogRecordType::BEGIN;
            lm.AppendLogRecord(rec);
        }
        lm.FlushAll();
    }

    // Corrupt a byte near the end of the file.
    {
        std::fstream f(wal_path_, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-5, std::ios::end);
        char garbage = '\xFF';
        f.write(&garbage, 1);
        f.close();
    }

    // Re-read: should get fewer than 5 records.
    LogManager lm2(wal_path_);
    auto records = lm2.ReadAllRecords();
    EXPECT_LT(records.size(), 5u);
}

// ===========================================================================
// TC_WAL_08: Pre-flush hook calls FlushAll before allowing page write.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_08_PreFlushHookBlocksPageWrite) {
    LogManager lm(wal_path_);

    // Append an UPDATE record for page 5.
    LogRecord rec;
    rec.txn_id  = 1;
    rec.type    = LogRecordType::UPDATE;
    rec.page_id = 5;
    rec.offset  = 0;
    rec.length  = 2;
    rec.before_image = {0x00, 0x00};
    rec.after_image  = {0xFF, 0xFF};
    lm.AppendLogRecord(rec);

    // Before the hook, the record is NOT flushed.
    EXPECT_EQ(lm.GetFlushedLSN(), 0u);

    // Get the hook and call it for page 5.
    auto hook = lm.MakePreFlushHook();
    bool allowed = hook(5);
    EXPECT_TRUE(allowed);

    // After the hook, the WAL should be flushed.
    EXPECT_GE(lm.GetFlushedLSN(), 1u);
}

// ===========================================================================
// TC_WAL_09: A failed WAL write can be retried without losing records.
// ===========================================================================
TEST_F(LogManagerTest, TC_WAL_09_FlushFailureRecovery) {
    bool fail_once = true;

    LogManager lm(wal_path_, [&fail_once]() {
        if (fail_once) {
            fail_once = false;
            throw std::runtime_error("Simulated WAL disk-write failure");
        }
    });

    LogRecord rec;
    rec.txn_id = 900;
    rec.type = LogRecordType::BEGIN;

    uint64_t lsn = lm.AppendLogRecord(rec);
    EXPECT_EQ(lsn, 1u);

    // First flush should fail deliberately.
    EXPECT_THROW(lm.FlushAll(), std::runtime_error);

    // Failed flush must not report the record as durable.
    EXPECT_EQ(lm.GetFlushedLSN(), 0u);

    // Retry should succeed after the injected failure is disabled.
    EXPECT_NO_THROW(lm.FlushAll());

    EXPECT_EQ(lm.GetFlushedLSN(), 1u);

    // Verify that the record was not lost.
    auto records = lm.ReadAllRecords();

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].lsn, 1u);
    EXPECT_EQ(records[0].txn_id, 900u);
    EXPECT_EQ(records[0].type, LogRecordType::BEGIN);

}

TEST_F(LogManagerTest, TC_WAL_10_RestartRestoresLSN) {

    // First manager writes three records.
    {
        LogManager lm(wal_path_);

        for (uint64_t i = 1; i <= 3; ++i) {
            LogRecord rec;
            rec.txn_id = 900 + i;
            rec.type = LogRecordType::BEGIN;

            EXPECT_EQ(lm.AppendLogRecord(rec), i);
        }

        lm.FlushAll();
        EXPECT_EQ(lm.GetFlushedLSN(), 3u);
    }

    // Second manager reopens the same WAL file.
    LogManager lm(wal_path_);

    LogRecord rec;
    rec.txn_id = 904;
    rec.type = LogRecordType::BEGIN;

    EXPECT_EQ(lm.AppendLogRecord(rec), 4u);
    lm.FlushAll();

    auto records = lm.ReadAllRecords();

    ASSERT_EQ(records.size(), 4u);
    EXPECT_EQ(records.back().lsn, 4u);
}


TEST_F(LogManagerTest, TC_WAL_12_RestartRestoresTransactionLSN) {
    uint64_t first_lsn = 0;
    uint64_t second_lsn = 0;

    // First manager writes two records for the same transaction.
    {
        LogManager lm(wal_path_);

        LogRecord rec;
        rec.txn_id = 920;
        rec.type = LogRecordType::BEGIN;
        first_lsn = lm.AppendLogRecord(rec);

        rec.type = LogRecordType::COMMIT;
        second_lsn = lm.AppendLogRecord(rec);

        lm.FlushAll();

        EXPECT_EQ(lm.GetLastLSN(920), second_lsn);
    }

    // Reopen WAL and verify transaction history is restored.
    {
        LogManager lm(wal_path_);

        EXPECT_EQ(lm.GetLastLSN(920), second_lsn);

        LogRecord rec;
        rec.txn_id = 920;
        rec.type = LogRecordType::BEGIN;

        uint64_t next_lsn = lm.AppendLogRecord(rec);

        EXPECT_GT(next_lsn, second_lsn);
        EXPECT_EQ(lm.GetLastLSN(920), next_lsn);
    }
}


TEST_F(LogManagerTest, TC_WAL_13_RestartRestoresMultipleTransactions) {
    {
        LogManager lm(wal_path_);

        LogRecord rec;
        rec.txn_id = 930;
        rec.type = LogRecordType::BEGIN;
        EXPECT_EQ(lm.AppendLogRecord(rec), 1u);

        rec.txn_id = 931;
        EXPECT_EQ(lm.AppendLogRecord(rec), 2u);

        rec.txn_id = 930;
        rec.type = LogRecordType::COMMIT;
        EXPECT_EQ(lm.AppendLogRecord(rec), 3u);

        rec.txn_id = 931;
        rec.type = LogRecordType::COMMIT;
        EXPECT_EQ(lm.AppendLogRecord(rec), 4u);

        lm.FlushAll();

        EXPECT_EQ(lm.GetLastLSN(930), 3u);
        EXPECT_EQ(lm.GetLastLSN(931), 4u);
    }

    // Reopen and verify each transaction retains its own last LSN.
    {
        LogManager lm(wal_path_);

        EXPECT_EQ(lm.GetLastLSN(930), 3u);
        EXPECT_EQ(lm.GetLastLSN(931), 4u);
    }
}

TEST_F(LogManagerTest, TC_WAL_11_PartialWriteRollback) {
    bool fail_once = true;

    LogManager lm(
        wal_path_,
        {},
        [&fail_once](size_t total_bytes) -> size_t {
            if (fail_once) {
                fail_once = false;
                return total_bytes / 2;
            }
            return total_bytes;
        }
    );

    LogRecord rec;
    rec.txn_id = 911;
    rec.type = LogRecordType::BEGIN;

    EXPECT_EQ(lm.AppendLogRecord(rec), 1u);

    // First flush simulates a partial disk write.
    EXPECT_THROW(lm.FlushAll(), std::runtime_error);

    // Retry should write the complete record.
    EXPECT_NO_THROW(lm.FlushAll());

    auto records = lm.ReadAllRecords();

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].lsn, 1u);
    EXPECT_EQ(records[0].txn_id, 911u);
        EXPECT_TRUE(records[0].type == LogRecordType::BEGIN);

}

TEST_F(LogManagerTest, TC_WAL_14_UnknownTransactionReturnsZero) {
    LogManager lm(wal_path_);

    EXPECT_EQ(lm.GetLastLSN(9999), 0u);

    LogRecord rec;
    rec.txn_id = 940;
    rec.type = LogRecordType::BEGIN;

    uint64_t lsn = lm.AppendLogRecord(rec);

    EXPECT_EQ(lm.GetLastLSN(940), lsn);
    EXPECT_EQ(lm.GetLastLSN(9999), 0u);
}

} // namespace
} // namespace walnut
