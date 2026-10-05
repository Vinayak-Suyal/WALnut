// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/buffer/frame.cpp
// =============================================================================

#include "walnut/buffer/frame.hpp"
#include <stdexcept>
#include <cassert>

namespace walnut {

// ---------------------------------------------------------------------------
Frame::Frame() : page_(), pin_count_(0), valid_(false),
                 dirty_(false), reference_bit_(false) {}

// ---------------------------------------------------------------------------
void Frame::pin() {
    ++pin_count_;
    reference_bit_ = true;
}

void Frame::unpin() {
    if (pin_count_ <= 0) {
        throw std::runtime_error("Frame::unpin – pin_count already 0");
    }
    --pin_count_;
}

// ---------------------------------------------------------------------------
void Frame::loadPage(const Page& page) {
    page_          = page;
    page_.markClean();   // Freshly loaded page is not dirty in the pool.
    pin_count_     = 0;
    valid_         = true;
    dirty_         = false;
    reference_bit_ = true;  // Just loaded = recently used.
}

// ---------------------------------------------------------------------------
void Frame::reset() {
    page_          = Page{};
    pin_count_     = 0;
    valid_         = false;
    dirty_         = false;
    reference_bit_ = false;
}

} // namespace walnut
