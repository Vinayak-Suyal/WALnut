// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// tests/storage/test_file_manager.cpp
// =============================================================================

#include <gtest/gtest.h>
#include "walnut/storage/file_manager.hpp"
#include "walnut/common/constants.hpp"
#include <filesystem>
#include <cstring>
#include <stdexcept>

using namespace walnut;

// ---------------------------------------------------------------------------
// Test fixture: creates a fresh temp directory for each test.
// ---------------------------------------------------------------------------
class FileManagerTest : public ::testing::Test {
protected:
    std::string test_dir_;

    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path().string() +
                    "/walnut_fm_test_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }
};

// ---------------------------------------------------------------------------
TEST_F(FileManagerTest, InitializeCreatesFiles) {
    FileManager fm(test_dir_);
    fm.initialize();
    EXPECT_TRUE(std::filesystem::exists(test_dir_ + "/walnut.db"));
    EXPECT_TRUE(std::filesystem::exists(test_dir_ + "/walnut.meta"));
    fm.close();
}

TEST_F(FileManagerTest, AllocatePageIncreasesPageCount) {
    FileManager fm(test_dir_);
    fm.initialize();
    EXPECT_EQ(fm.pageCount(), 0u);

    uint32_t pid = fm.allocatePage(PAGE_TYPE_USERS);
    EXPECT_EQ(pid, 0u);
    EXPECT_EQ(fm.pageCount(), 1u);

    uint32_t pid2 = fm.allocatePage(PAGE_TYPE_ITEMS);
    EXPECT_EQ(pid2, 1u);
    EXPECT_EQ(fm.pageCount(), 2u);
    fm.close();
}

// ---------------------------------------------------------------------------
// TC-STO-01: byte-identical page round-trip
// ---------------------------------------------------------------------------
TEST_F(FileManagerTest, TC_STO_01_PageRoundTrip) {
    FileManager fm(test_dir_);
    fm.initialize();

    uint32_t pid = fm.allocatePage(PAGE_TYPE_BIDS);
    Page page = fm.readPage(pid);

    // Write known payload into the data region.
    uint8_t payload[32];
    for (int i = 0; i < 32; ++i) payload[i] = static_cast<uint8_t>(i + 1);
    page.appendRecord(payload, 32);
    page.updateChecksum();
    fm.writePage(page);

    // Read it back.
    Page read_back = fm.readPage(pid);
    EXPECT_TRUE(read_back.verifyChecksum());
    EXPECT_EQ(read_back.recordCount(), 1u);
    EXPECT_EQ(std::memcmp(read_back.getRecord(0, 32), payload, 32), 0);

    fm.close();
}

// ---------------------------------------------------------------------------
// TC-STO-03: New page allocation when current page is full
// ---------------------------------------------------------------------------
TEST_F(FileManagerTest, TC_STO_03_NewPageAllocation) {
    FileManager fm(test_dir_);
    fm.initialize();

    // Fill page 0 with BID records.
    uint32_t pid0 = fm.allocatePage(PAGE_TYPE_BIDS);
    Page page = fm.readPage(pid0);

    uint8_t rec[BID_RECORD_SIZE] = {};
    std::size_t max_records = PAGE_DATA_SIZE / BID_RECORD_SIZE;
    for (std::size_t i = 0; i < max_records; ++i) {
        page.appendRecord(rec, BID_RECORD_SIZE);
    }
    page.updateChecksum();
    fm.writePage(page);

    // Verify page is full.
    Page reread = fm.readPage(pid0);
    EXPECT_FALSE(reread.hasRoom(BID_RECORD_SIZE));

    // Allocate a new page.
    uint32_t pid1 = fm.allocatePage(PAGE_TYPE_BIDS);
    EXPECT_NE(pid1, pid0);
    EXPECT_EQ(fm.pageCount(), 2u);

    fm.close();
}

// ---------------------------------------------------------------------------
// Checksum mismatch is caught on read
// ---------------------------------------------------------------------------
TEST_F(FileManagerTest, CorruptedPageThrowsOnRead) {
    FileManager fm(test_dir_);
    fm.initialize();
    uint32_t pid = fm.allocatePage(PAGE_TYPE_USERS);
    fm.close();

    // Corrupt the data region directly in the file.
    std::fstream f(test_dir_ + "/walnut.db",
                   std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(pid * PAGE_SIZE + PAGE_HEADER_SIZE));
    uint8_t garbage[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
    f.write(reinterpret_cast<const char*>(garbage), 8);
    f.close();

    FileManager fm2(test_dir_);
    fm2.initialize();
    EXPECT_THROW(fm2.readPage(pid), std::runtime_error);
    fm2.close();
}

// ---------------------------------------------------------------------------
// Persistence across FileManager restart
// ---------------------------------------------------------------------------
TEST_F(FileManagerTest, PageCountPersistsAcrossRestart) {
    {
        FileManager fm(test_dir_);
        fm.initialize();
        fm.allocatePage(PAGE_TYPE_USERS);
        fm.allocatePage(PAGE_TYPE_ITEMS);
        fm.allocatePage(PAGE_TYPE_BIDS);
        fm.close();
    }
    {
        FileManager fm(test_dir_);
        fm.initialize();
        EXPECT_EQ(fm.pageCount(), 3u);
        fm.close();
    }
}

TEST_F(FileManagerTest, SyncDoesNotCloseFiles) {
    FileManager fm(test_dir_);
    fm.initialize();
    fm.allocatePage(PAGE_TYPE_USERS);
    fm.sync(); // Should not throw and files should remain open.
    uint32_t pid = fm.allocatePage(PAGE_TYPE_ITEMS);
    EXPECT_EQ(pid, 1u); // Still works after sync.
    fm.close();
}
