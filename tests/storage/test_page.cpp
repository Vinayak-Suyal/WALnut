// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// tests/storage/test_page.cpp
// =============================================================================

#include <gtest/gtest.h>
#include "walnut/storage/page.hpp"
#include "walnut/common/constants.hpp"
#include <cstring>
#include <stdexcept>

using namespace walnut;

// ---------------------------------------------------------------------------
// Basic construction
// ---------------------------------------------------------------------------
TEST(PageTest, DefaultConstructorIsInvalid) {
    Page p;
    EXPECT_EQ(p.pageType(),  PAGE_TYPE_INVALID);
    EXPECT_EQ(p.recordCount(), 0u);
    EXPECT_EQ(p.freeSpaceOffset(), static_cast<uint32_t>(PAGE_HEADER_SIZE));
    EXPECT_FALSE(p.isDirty());
}

TEST(PageTest, ParameterisedConstructorSetsFields) {
    Page p(42, PAGE_TYPE_USERS);
    EXPECT_EQ(p.pageId(),   42u);
    EXPECT_EQ(p.pageType(), PAGE_TYPE_USERS);
    EXPECT_EQ(p.recordCount(), 0u);
    EXPECT_EQ(p.freeSpaceOffset(), static_cast<uint32_t>(PAGE_HEADER_SIZE));
}

// ---------------------------------------------------------------------------
// Dirty tracking
// ---------------------------------------------------------------------------
TEST(PageTest, DirtyTracking) {
    Page p(1, PAGE_TYPE_ITEMS);
    EXPECT_FALSE(p.isDirty());
    p.markDirty();
    EXPECT_TRUE(p.isDirty());
    p.markClean();
    EXPECT_FALSE(p.isDirty());
}

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------
TEST(PageTest, ChecksumIsConsistent) {
    Page p(7, PAGE_TYPE_BIDS);
    p.updateChecksum();
    EXPECT_TRUE(p.verifyChecksum());
}

TEST(PageTest, ChecksumDetectsCorruption) {
    Page p(3, PAGE_TYPE_USERS);
    p.updateChecksum();
    EXPECT_TRUE(p.verifyChecksum());

    // Corrupt one data byte.
    p.data()[PAGE_HEADER_SIZE] ^= 0xFF;
    EXPECT_FALSE(p.verifyChecksum());
}

TEST(PageTest, ChecksumUpdatesAfterDataChange) {
    Page p(5, PAGE_TYPE_ITEMS);
    p.updateChecksum();
    uint32_t old_cs = p.storedChecksum();

    // Write a data byte.
    p.data()[PAGE_HEADER_SIZE] = 0xAB;
    p.updateChecksum();

    EXPECT_NE(p.storedChecksum(), old_cs);
    EXPECT_TRUE(p.verifyChecksum());
}

// ---------------------------------------------------------------------------
// Buffer size guarantee
// ---------------------------------------------------------------------------
TEST(PageTest, BufferIsExactlyPageSize) {
    Page p;
    EXPECT_EQ(Page::pageSize(), PAGE_SIZE);
    EXPECT_EQ(Page::headerSize(), PAGE_HEADER_SIZE);
    EXPECT_EQ(Page::dataSize(), PAGE_SIZE - PAGE_HEADER_SIZE);
}

// ---------------------------------------------------------------------------
// Record append / retrieval
// ---------------------------------------------------------------------------
TEST(PageTest, AppendAndGetRecord) {
    Page p(0, PAGE_TYPE_USERS);

    uint8_t record[16];
    for (int i = 0; i < 16; ++i) record[i] = static_cast<uint8_t>(i);

    p.appendRecord(record, 16);
    EXPECT_EQ(p.recordCount(), 1u);
    EXPECT_EQ(p.freeSpaceOffset(),
              static_cast<uint32_t>(PAGE_HEADER_SIZE + 16));

    const uint8_t* got = p.getRecord(0, 16);
    EXPECT_EQ(std::memcmp(got, record, 16), 0);
}

TEST(PageTest, AppendMultipleRecords) {
    Page p(0, PAGE_TYPE_BIDS);
    uint8_t r1[8] = {1,2,3,4,5,6,7,8};
    uint8_t r2[8] = {9,10,11,12,13,14,15,16};
    p.appendRecord(r1, 8);
    p.appendRecord(r2, 8);
    EXPECT_EQ(p.recordCount(), 2u);
    EXPECT_EQ(std::memcmp(p.getRecord(0, 8), r1, 8), 0);
    EXPECT_EQ(std::memcmp(p.getRecord(1, 8), r2, 8), 0);
}

TEST(PageTest, AppendThrowsWhenFull) {
    Page p(0, PAGE_TYPE_USERS);
    std::vector<uint8_t> big_rec(PAGE_DATA_SIZE + 1, 0xCC);
    EXPECT_THROW(p.appendRecord(big_rec.data(), big_rec.size()),
                 std::runtime_error);
}

TEST(PageTest, GetRecordOutOfRangeThrows) {
    Page p(0, PAGE_TYPE_ITEMS);
    uint8_t rec[4] = {};
    p.appendRecord(rec, 4);
    EXPECT_THROW(p.getRecord(1, 4), std::out_of_range);
}

TEST(PageTest, HasRoomReturnsFalseWhenFull) {
    Page p(0, PAGE_TYPE_BIDS);
    // Fill it up with the largest aligned records that fit
    std::vector<uint8_t> rec(PAGE_DATA_SIZE, 0xAA);
    p.appendRecord(rec.data(), PAGE_DATA_SIZE);
    EXPECT_FALSE(p.hasRoom(1));
}

// ---------------------------------------------------------------------------
// Copy semantics
// ---------------------------------------------------------------------------
TEST(PageTest, CopyIsDeep) {
    Page p1(10, PAGE_TYPE_USERS);
    uint8_t rec[8] = {1,2,3,4,5,6,7,8};
    p1.appendRecord(rec, 8);

    Page p2 = p1;  // copy
    EXPECT_EQ(p2.recordCount(), 1u);

    // Modifying p1 must not affect p2.
    uint8_t rec2[8] = {9,9,9,9,9,9,9,9};
    p1.appendRecord(rec2, 8);
    EXPECT_EQ(p1.recordCount(), 2u);
    EXPECT_EQ(p2.recordCount(), 1u);
}

// ---------------------------------------------------------------------------
// TC-STO-01: Page write/read round-trip  (just the Page object level)
// ---------------------------------------------------------------------------
TEST(PageTest, TC_STO_01_RoundTripViaRawData) {
    Page original(99, PAGE_TYPE_ITEMS);
    uint8_t payload[32];
    for (int i = 0; i < 32; ++i) payload[i] = static_cast<uint8_t>(i * 3);
    original.appendRecord(payload, 32);
    original.updateChecksum();

    // Simulate disk: copy the raw bytes to a second Page.
    Page restored;
    std::memcpy(restored.data(), original.data(), PAGE_SIZE);

    EXPECT_TRUE(restored.verifyChecksum());
    EXPECT_EQ(restored.pageId(),      99u);
    EXPECT_EQ(restored.pageType(),    PAGE_TYPE_ITEMS);
    EXPECT_EQ(restored.recordCount(), 1u);
    EXPECT_EQ(std::memcmp(restored.getRecord(0, 32), payload, 32), 0);
}
