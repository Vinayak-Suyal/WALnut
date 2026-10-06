// =============================================================================
// WALnut – Test Suite (Kanak Rawat)
// tests/wal/test_recovery_manager.cpp
//
// Unit tests for ARIES RecoveryManager: redo, undo, CLR, idempotency.
// =============================================================================

#include "walnut/wal/recovery_manager.hpp"
#include "walnut/wal/log_manager.hpp"
#include "walnut/storage/file_manager.hpp"
#include "walnut/storage/metadata.hpp"
#include "walnut/storage/page.hpp"
#include "walnut/common/constants.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

namespace walnut {
namespace {

// ---------------------------------------------------------------------------
// Test fixture – creates a temp data directory with DB + WAL files.
// ---------------------------------------------------------------------------
class RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = "data/test_recovery_" + std::to_string(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        fs::create_directories(test_dir_);
        wal_path_ = test_dir_ + "/walnut.wal";
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string test_dir_;
    std::string wal_path_;

    /// Helper: create a page with known data and write it to disk.
    void WriteTestPage(FileManager& fm, uint32_t page_id, uint32_t page_type,
                       uint16_t offset, const std::vector<uint8_t>& data) {
        // Ensure the page exists.
        while (fm.pageCount() <= page_id) {
            fm.allocatePage(page_type);
        }
        Page page = fm.readPage(page_id);
        std::memcpy(page.dataRegion() + offset, data.data(), data.size());
        page.updateChecksum();
        fm.writePage(page);
    }
};

// ===========================================================================
// TC_REC_01: Redo restores lost data (committed but not flushed to page).
// ===========================================================================
TEST_F(RecoveryTest, TC_REC_01_RedoRestoredLostData) {
    FileManager fm(test_dir_);
    fm.initialize();

    // Allocate a page and write initial data.
    uint32_t pid = fm.allocatePage(PAGE_TYPE_BIDS);
    {
        Page page = fm.readPage(pid);
        uint8_t zeros[4] = {0, 0, 0, 0};
        std::memcpy(page.dataRegion() + 0, zeros, 4);
        page.updateChecksum();
        fm.writePage(page);
    }

    // Write WAL records: BEGIN, UPDATE, COMMIT (simulate committed txn).
    LogManager lm(wal_path_);
    {
        LogRecord begin_rec;
        begin_rec.txn_id = 1;
        begin_rec.type   = LogRecordType::BEGIN;
        lm.AppendLogRecord(begin_rec);
    }
    {
        LogRecord update_rec;
        update_rec.txn_id  = 1;
        update_rec.type    = LogRecordType::UPDATE;
        update_rec.page_id = pid;
        update_rec.offset  = 0;
        update_rec.length  = 4;
        update_rec.before_image = {0x00, 0x00, 0x00, 0x00};
        update_rec.after_image  = {0xDE, 0xAD, 0xBE, 0xEF};
        lm.AppendLogRecord(update_rec);
    }
    {
        LogRecord commit_rec;
        commit_rec.txn_id = 1;
        commit_rec.type   = LogRecordType::COMMIT;
        lm.AppendLogRecord(commit_rec);
    }
    lm.FlushAll();

    // The page on disk still has zeros (simulating "crash before page flush").
    // Run recovery.
    MetadataManager mm(test_dir_ + "/walnut.meta");
    mm.load();
    mm.meta().checkpoint_lsn = 0;

    RecoveryManager rm(lm, fm, mm);
    rm.Recover();

    // Verify the page now has the after_image data.
    Page page = fm.readPage(pid);
    uint8_t expected[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(std::memcmp(page.dataRegion(), expected, 4), 0);

    auto stats = rm.GetStats();
    EXPECT_GE(stats.records_analyzed, 3u);
    EXPECT_GE(stats.records_redone, 1u);
    EXPECT_EQ(stats.loser_txn_count, 0u);
}

// ===========================================================================
// TC_REC_02: Undo removes uncommitted data (steal policy: dirty page on disk).
// ===========================================================================
TEST_F(RecoveryTest, TC_REC_02_UndoRemovesUncommittedData) {
    FileManager fm(test_dir_);
    fm.initialize();

    uint32_t pid = fm.allocatePage(PAGE_TYPE_BIDS);

    // Write the "after-image" data directly to the page (simulating steal).
    {
        Page page = fm.readPage(pid);
        uint8_t after[] = {0xDE, 0xAD, 0xBE, 0xEF};
        std::memcpy(page.dataRegion() + 0, after, 4);
        page.updateChecksum();
        fm.writePage(page);
    }

    // Write WAL: BEGIN + UPDATE (no COMMIT — loser txn).
    LogManager lm(wal_path_);
    {
        LogRecord begin_rec;
        begin_rec.txn_id = 1;
        begin_rec.type   = LogRecordType::BEGIN;
        lm.AppendLogRecord(begin_rec);
    }
    {
        LogRecord update_rec;
        update_rec.txn_id  = 1;
        update_rec.type    = LogRecordType::UPDATE;
        update_rec.page_id = pid;
        update_rec.offset  = 0;
        update_rec.length  = 4;
        update_rec.before_image = {0x00, 0x00, 0x00, 0x00};
        update_rec.after_image  = {0xDE, 0xAD, 0xBE, 0xEF};
        lm.AppendLogRecord(update_rec);
    }
    lm.FlushAll();

    // Run recovery.
    MetadataManager mm(test_dir_ + "/walnut.meta");
    mm.load();
    mm.meta().checkpoint_lsn = 0;

    RecoveryManager rm(lm, fm, mm);
    rm.Recover();

    // Verify the page has been restored to before_image (zeros).
    Page page = fm.readPage(pid);
    uint8_t expected[] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(std::memcmp(page.dataRegion(), expected, 4), 0);

    auto stats = rm.GetStats();
    EXPECT_EQ(stats.loser_txn_count, 1u);
    EXPECT_GE(stats.records_undone, 1u);
    EXPECT_GE(stats.clrs_written, 1u);
}

// ===========================================================================
// TC_REC_03: CLR prevents double undo (recovery is idempotent).
// ===========================================================================
TEST_F(RecoveryTest, TC_REC_03_CLRPreventsDoubleUndo) {
    FileManager fm(test_dir_);
    fm.initialize();

    uint32_t pid = fm.allocatePage(PAGE_TYPE_BIDS);
    {
        Page page = fm.readPage(pid);
        uint8_t after[] = {0xDE, 0xAD, 0xBE, 0xEF};
        std::memcpy(page.dataRegion() + 0, after, 4);
        page.updateChecksum();
        fm.writePage(page);
    }

    LogManager lm(wal_path_);
    {
        LogRecord begin_rec;
        begin_rec.txn_id = 1;
        begin_rec.type   = LogRecordType::BEGIN;
        lm.AppendLogRecord(begin_rec);
    }
    {
        LogRecord update_rec;
        update_rec.txn_id  = 1;
        update_rec.type    = LogRecordType::UPDATE;
        update_rec.page_id = pid;
        update_rec.offset  = 0;
        update_rec.length  = 4;
        update_rec.before_image = {0x00, 0x00, 0x00, 0x00};
        update_rec.after_image  = {0xDE, 0xAD, 0xBE, 0xEF};
        lm.AppendLogRecord(update_rec);
    }
    lm.FlushAll();

    // First recovery — should undo and write CLRs.
    MetadataManager mm(test_dir_ + "/walnut.meta");
    mm.load();
    mm.meta().checkpoint_lsn = 0;

    {
        RecoveryManager rm(lm, fm, mm);
        rm.Recover();
    }

    // Second recovery — CLR records should prevent double undo.
    {
        RecoveryManager rm2(lm, fm, mm);
        rm2.Recover();

        // The page should still be zeros (not double-undone to garbage).
        Page page = fm.readPage(pid);
        uint8_t expected[] = {0x00, 0x00, 0x00, 0x00};
        EXPECT_EQ(std::memcmp(page.dataRegion(), expected, 4), 0);
    }
}

// ===========================================================================
// TC_REC_04: Committed transaction is NOT undone.
// ===========================================================================
TEST_F(RecoveryTest, TC_REC_04_CommittedTransactionNotUndone) {
    FileManager fm(test_dir_);
    fm.initialize();

    uint32_t pid = fm.allocatePage(PAGE_TYPE_ITEMS);
    {
        Page page = fm.readPage(pid);
        uint8_t data[] = {0xAA, 0xBB};
        std::memcpy(page.dataRegion() + 10, data, 2);
        page.updateChecksum();
        fm.writePage(page);
    }

    LogManager lm(wal_path_);

    // T1: BEGIN → UPDATE → COMMIT.
    {
        LogRecord r; r.txn_id = 1; r.type = LogRecordType::BEGIN;
        lm.AppendLogRecord(r);
    }
    {
        LogRecord r;
        r.txn_id = 1; r.type = LogRecordType::UPDATE;
        r.page_id = pid; r.offset = 10; r.length = 2;
        r.before_image = {0x00, 0x00};
        r.after_image  = {0xAA, 0xBB};
        lm.AppendLogRecord(r);
    }
    {
        LogRecord r; r.txn_id = 1; r.type = LogRecordType::COMMIT;
        lm.AppendLogRecord(r);
    }
    lm.FlushAll();

    MetadataManager mm(test_dir_ + "/walnut.meta");
    mm.load();
    mm.meta().checkpoint_lsn = 0;

    RecoveryManager rm(lm, fm, mm);
    rm.Recover();

    auto stats = rm.GetStats();
    EXPECT_EQ(stats.loser_txn_count, 0u);
    EXPECT_EQ(stats.records_undone, 0u);

    // Data should still be present.
    Page page = fm.readPage(pid);
    EXPECT_EQ(page.dataRegion()[10], 0xAA);
    EXPECT_EQ(page.dataRegion()[11], 0xBB);
}

// ===========================================================================
// TC_REC_05: Empty WAL – recovery does nothing.
// ===========================================================================
TEST_F(RecoveryTest, TC_REC_05_EmptyWALRecovery) {
    FileManager fm(test_dir_);
    fm.initialize();

    LogManager lm(wal_path_);

    MetadataManager mm(test_dir_ + "/walnut.meta");
    mm.load();

    RecoveryManager rm(lm, fm, mm);
    rm.Recover();

    auto stats = rm.GetStats();
    EXPECT_EQ(stats.records_analyzed, 0u);
    EXPECT_EQ(stats.records_redone, 0u);
    EXPECT_EQ(stats.records_undone, 0u);
}

// ===========================================================================
// TC_REC_06: Multiple transactions – only losers are undone.
// ===========================================================================
TEST_F(RecoveryTest, TC_REC_06_MixedCommitAndLoser) {
    FileManager fm(test_dir_);
    fm.initialize();

    uint32_t pid = fm.allocatePage(PAGE_TYPE_BIDS);
    {
        Page page = fm.readPage(pid);
        // T1 writes at offset 0, T2 writes at offset 10.
        uint8_t t1_data[] = {0x11, 0x11};
        uint8_t t2_data[] = {0x22, 0x22};
        std::memcpy(page.dataRegion() + 0, t1_data, 2);
        std::memcpy(page.dataRegion() + 10, t2_data, 2);
        page.updateChecksum();
        fm.writePage(page);
    }

    LogManager lm(wal_path_);

    // T1: committed.
    { LogRecord r; r.txn_id = 1; r.type = LogRecordType::BEGIN; lm.AppendLogRecord(r); }
    {
        LogRecord r; r.txn_id = 1; r.type = LogRecordType::UPDATE;
        r.page_id = pid; r.offset = 0; r.length = 2;
        r.before_image = {0x00, 0x00}; r.after_image = {0x11, 0x11};
        lm.AppendLogRecord(r);
    }
    { LogRecord r; r.txn_id = 1; r.type = LogRecordType::COMMIT; lm.AppendLogRecord(r); }

    // T2: uncommitted (loser).
    { LogRecord r; r.txn_id = 2; r.type = LogRecordType::BEGIN; lm.AppendLogRecord(r); }
    {
        LogRecord r; r.txn_id = 2; r.type = LogRecordType::UPDATE;
        r.page_id = pid; r.offset = 10; r.length = 2;
        r.before_image = {0x00, 0x00}; r.after_image = {0x22, 0x22};
        lm.AppendLogRecord(r);
    }
    lm.FlushAll();

    MetadataManager mm(test_dir_ + "/walnut.meta");
    mm.load();
    mm.meta().checkpoint_lsn = 0;

    RecoveryManager rm(lm, fm, mm);
    rm.Recover();

    auto stats = rm.GetStats();
    EXPECT_EQ(stats.loser_txn_count, 1u);
    EXPECT_GE(stats.records_undone, 1u);

    // T1's data should be present; T2's should be undone.
    Page page = fm.readPage(pid);
    EXPECT_EQ(page.dataRegion()[0], 0x11);
    EXPECT_EQ(page.dataRegion()[1], 0x11);
    EXPECT_EQ(page.dataRegion()[10], 0x00);
    EXPECT_EQ(page.dataRegion()[11], 0x00);
}

} // namespace
} // namespace walnut
