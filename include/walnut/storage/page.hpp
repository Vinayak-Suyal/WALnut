// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/storage/page.hpp
//
// Fixed-size 4 KB page with a 64-byte header.
//
// Layout (PAGE_SIZE = 4096 bytes):
//   [0 .. PAGE_HEADER_SIZE)  – PageHeader (fixed, 64 bytes)
//   [PAGE_HEADER_SIZE .. PAGE_SIZE) – record data (PAGE_DATA_SIZE bytes)
//
// The page is the *only* unit of I/O; the buffer pool and file manager
// always read/write exactly PAGE_SIZE bytes.
// =============================================================================

#pragma once

#include "walnut/common/constants.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace walnut {

// ---------------------------------------------------------------------------
// PageHeader – the first PAGE_HEADER_SIZE bytes of every page.
//
// Binary layout (all fields little-endian uint32_t):
//   offset  0 : page_id            – unique page number (0-indexed)
//   offset  4 : page_type          – one of PAGE_TYPE_* constants
//   offset  8 : record_count       – number of records currently stored
//   offset 12 : free_space_offset  – byte offset from page start where
//                                    the next record will be written
//   offset 16 : checksum           – simple XOR+sum checksum of the full page
//   offset 20 : reserved[0..2]     – zeroed, reserved for future use
//   offset 32 .. 63 : padding      – zeroed to fill PAGE_HEADER_SIZE = 64
// ---------------------------------------------------------------------------
struct PageHeader {
    uint32_t page_id;           ///< Unique page ID (0-indexed)
    uint32_t page_type;         ///< PAGE_TYPE_* constant
    uint32_t record_count;      ///< Number of records on this page
    uint32_t free_space_offset; ///< Offset of first free byte (from page start)
    uint32_t checksum;          ///< Integrity checksum (see Page::computeChecksum)
    uint32_t reserved[3];       ///< Zeroed; reserved for future fields

    // Padding to exactly PAGE_HEADER_SIZE bytes.
    // PageHeader itself is 8*4 = 32 bytes; pad to 64.
    uint8_t  padding[PAGE_HEADER_SIZE - 8 * sizeof(uint32_t)];
};

static_assert(sizeof(PageHeader) == PAGE_HEADER_SIZE,
              "PageHeader must be exactly PAGE_HEADER_SIZE bytes");

// ---------------------------------------------------------------------------
// Page – owns a PAGE_SIZE byte buffer and provides typed access.
// ---------------------------------------------------------------------------
class Page {
public:
    // -----------------------------------------------------------------------
    // Construction / assignment
    // -----------------------------------------------------------------------

    /// Default-construct an invalid page (page_id = 0, page_type = INVALID).
    Page();

    /// Construct and initialise a new, empty page of the given type.
    Page(uint32_t page_id, uint32_t page_type);

    // Default copy/move are fine – copies the full 4 KB buffer.
    Page(const Page&)            = default;
    Page& operator=(const Page&) = default;
    Page(Page&&)                 = default;
    Page& operator=(Page&&)      = default;
    ~Page()                      = default;

    // -----------------------------------------------------------------------
    // Header accessors (read directly from buffer_)
    // -----------------------------------------------------------------------
    uint32_t pageId()           const;
    uint32_t pageType()         const;
    uint32_t recordCount()      const;
    uint32_t freeSpaceOffset()  const;
    uint32_t storedChecksum()   const;

    // -----------------------------------------------------------------------
    // Header mutators (write directly into buffer_)
    // -----------------------------------------------------------------------
    void setPageId(uint32_t id);
    void setPageType(uint32_t type);
    void setRecordCount(uint32_t count);
    void incrementRecordCount();
    void decrementRecordCount();
    void setFreeSpaceOffset(uint32_t offset);

    // -----------------------------------------------------------------------
    // Raw buffer access
    // -----------------------------------------------------------------------

    /// Pointer to the start of the page buffer (PAGE_SIZE bytes).
    uint8_t*       data()       { return buffer_; }
    const uint8_t* data() const { return buffer_; }

    /// Pointer to the data region (past the header).
    uint8_t*       dataRegion()       { return buffer_ + PAGE_HEADER_SIZE; }
    const uint8_t* dataRegion() const { return buffer_ + PAGE_HEADER_SIZE; }

    static constexpr std::size_t pageSize()   { return PAGE_SIZE; }
    static constexpr std::size_t headerSize() { return PAGE_HEADER_SIZE; }
    static constexpr std::size_t dataSize()   { return PAGE_DATA_SIZE; }

    // -----------------------------------------------------------------------
    // Dirty tracking
    // -----------------------------------------------------------------------
    bool isDirty() const  { return dirty_; }
    void markDirty()      { dirty_ = true;  }
    void markClean()      { dirty_ = false; }

    // -----------------------------------------------------------------------
    // Integrity
    // -----------------------------------------------------------------------

    /// Compute a checksum over the data bytes [PAGE_HEADER_SIZE .. PAGE_SIZE).
    /// The header itself (including the stored checksum field) is excluded so
    /// the checksum can be verified after it is written into the header.
    uint32_t computeChecksum() const;

    /// Update the checksum field in the header.
    void updateChecksum();

    /// Return true iff the stored checksum matches a freshly computed one.
    bool verifyChecksum() const;

    // -----------------------------------------------------------------------
    // Record helpers
    // -----------------------------------------------------------------------

    /// Return true if there is room for `record_size` bytes starting at the
    /// current free_space_offset.
    bool hasRoom(std::size_t record_size) const;

    /// Write `record_size` bytes from src into the page at the current
    /// free_space_offset, advance the offset, and increment record_count.
    /// Throws std::runtime_error if there is not enough room.
    void appendRecord(const uint8_t* src, std::size_t record_size);

    /// Return a const pointer to the n-th record slot (0-indexed).
    /// Each slot is `record_size` bytes wide.
    /// Throws std::out_of_range if n >= record_count.
    const uint8_t* getRecord(uint32_t n, std::size_t record_size) const;

    /// Return a mutable pointer to the n-th record slot.
    uint8_t* getRecord(uint32_t n, std::size_t record_size);

private:
    uint8_t buffer_[PAGE_SIZE];
    bool dirty_{false};

    // Convenience: typed access to the PageHeader living at buffer_[0].
    PageHeader*       header()       { return reinterpret_cast<PageHeader*>(buffer_); }
    const PageHeader* header() const { return reinterpret_cast<const PageHeader*>(buffer_); }
};

} // namespace walnut
