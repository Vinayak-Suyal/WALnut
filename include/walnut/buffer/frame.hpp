// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/buffer/frame.hpp
//
// A Frame is a single slot in the buffer pool.  It holds one 4 KB Page,
// a pin count, a dirty flag, and the reference bit used by the Clock
// replacement algorithm.
// =============================================================================

#pragma once

#include "walnut/storage/page.hpp"
#include <cstdint>

namespace walnut {

class Frame {
public:
    Frame();
    ~Frame() = default;

    // Non-copyable; move is allowed so std::vector<Frame> can resize.
    Frame(const Frame&)            = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&)                 = default;
    Frame& operator=(Frame&&)      = default;

    // -----------------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------------

    /// True if this frame contains a valid (loaded) page.
    bool isValid() const { return valid_; }

    /// Page ID of the page currently loaded in this frame.
    /// Undefined if !isValid().
    uint32_t pageId() const { return page_.pageId(); }

    // -----------------------------------------------------------------------
    // Page access
    // -----------------------------------------------------------------------
    Page&       page()       { return page_; }
    const Page& page() const { return page_; }

    // -----------------------------------------------------------------------
    // Pin counting
    // -----------------------------------------------------------------------

    /// Number of threads currently referencing this frame.
    int pinCount() const { return pin_count_; }

    /// Increment pin count (page will not be evicted while pinned).
    void pin();

    /// Decrement pin count.  Asserts pin_count > 0 before decrementing.
    void unpin();

    // -----------------------------------------------------------------------
    // Dirty tracking
    // -----------------------------------------------------------------------
    bool isDirty()  const { return dirty_; }
    void markDirty()      { dirty_ = true;  }
    void markClean()      { dirty_ = false; }

    // -----------------------------------------------------------------------
    // Clock replacement bit
    // -----------------------------------------------------------------------
    bool referenceBit()  const { return reference_bit_; }
    void setReferenceBit(bool v)   { reference_bit_ = v; }
    void clearReferenceBit()       { reference_bit_ = false; }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Load a page into this frame (sets valid = true, pin_count = 0,
    /// dirty = false, reference_bit = true).
    void loadPage(const Page& page);

    /// Reset to the empty state (valid = false, pin_count = 0).
    void reset();

private:
    Page page_;
    int  pin_count_{0};
    bool valid_{false};
    bool dirty_{false};
    bool reference_bit_{false};
};

} // namespace walnut
