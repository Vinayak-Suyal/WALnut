// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// tests/buffer/test_frame.cpp
// =============================================================================

#include <gtest/gtest.h>
#include "walnut/buffer/frame.hpp"
#include "walnut/common/constants.hpp"
#include <stdexcept>

using namespace walnut;

TEST(FrameTest, DefaultIsInvalid) {
    Frame f;
    EXPECT_FALSE(f.isValid());
    EXPECT_EQ(f.pinCount(), 0);
    EXPECT_FALSE(f.isDirty());
    EXPECT_FALSE(f.referenceBit());
}

TEST(FrameTest, LoadPageSetsValid) {
    Frame f;
    Page p(1, PAGE_TYPE_USERS);
    f.loadPage(p);
    EXPECT_TRUE(f.isValid());
    EXPECT_EQ(f.pageId(), 1u);
    EXPECT_EQ(f.pinCount(), 0);   // loadPage does NOT pin.
    EXPECT_TRUE(f.referenceBit()); // Just loaded = recently used.
}

TEST(FrameTest, PinAndUnpin) {
    Frame f;
    Page p(2, PAGE_TYPE_ITEMS);
    f.loadPage(p);

    f.pin();
    EXPECT_EQ(f.pinCount(), 1);
    f.pin();
    EXPECT_EQ(f.pinCount(), 2);
    f.unpin();
    EXPECT_EQ(f.pinCount(), 1);
    f.unpin();
    EXPECT_EQ(f.pinCount(), 0);
}

TEST(FrameTest, UnpinBelowZeroThrows) {
    Frame f;
    EXPECT_THROW(f.unpin(), std::runtime_error);
}

TEST(FrameTest, DirtyTracking) {
    Frame f;
    EXPECT_FALSE(f.isDirty());
    f.markDirty();
    EXPECT_TRUE(f.isDirty());
    f.markClean();
    EXPECT_FALSE(f.isDirty());
}

TEST(FrameTest, ReferencesBit) {
    Frame f;
    f.setReferenceBit(true);
    EXPECT_TRUE(f.referenceBit());
    f.clearReferenceBit();
    EXPECT_FALSE(f.referenceBit());
}

TEST(FrameTest, ResetClearsEverything) {
    Frame f;
    Page p(5, PAGE_TYPE_BIDS);
    f.loadPage(p);
    f.pin();
    f.markDirty();
    f.reset();

    EXPECT_FALSE(f.isValid());
    EXPECT_EQ(f.pinCount(), 0);
    EXPECT_FALSE(f.isDirty());
    EXPECT_FALSE(f.referenceBit());
}
