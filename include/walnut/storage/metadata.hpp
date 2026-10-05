// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/storage/metadata.hpp
//
// DatabaseMetadata persisted to walnut.meta.
//
// Binary layout of walnut.meta:
//   offset  0 : magic          uint32_t  (META_MAGIC = 0x57414C4E)
//   offset  4 : format_version uint32_t  (META_VERSION = 1)
//   offset  8 : next_page_id   uint32_t  – next page to allocate
//   offset 12 : next_bid_id    uint32_t  – next bid ID to assign (uint32 stored, promoted to uint64)
//   offset 16 : checkpoint_lsn uint32_t  – (reserved for Kanak's WAL layer)
//   offset 20 : clean_shutdown uint8_t   – 1 = clean, 0 = dirty
//   offset 21 : reserved[0..2] uint8_t[3]
//   offset 24 : allocation_bitmap_size  uint32_t – number of bytes in the bitmap
//   offset 28 : (padding/reserved)
//   offset 32 .. : allocation_bitmap bytes (variable length, up to 4 KB)
//
// Total header: 32 bytes + bitmap.
// =============================================================================

#pragma once

#include "walnut/common/constants.hpp"
#include <cstdint>
#include <string>
#include <fstream>
#include <vector>

namespace walnut {

// ---------------------------------------------------------------------------
// In-memory representation of database metadata
// ---------------------------------------------------------------------------
struct DatabaseMetadata {
    uint32_t format_version      = META_VERSION;
    uint32_t next_page_id        = 0;
    uint32_t next_bid_id         = 0;    ///< Cast to uint64_t for Bid::bid_id
    uint32_t checkpoint_lsn      = 0;    ///< Reserved for WAL integration (Kanak)
    bool     clean_shutdown      = false;
    uint32_t reserved[3]         = {};
};

// ---------------------------------------------------------------------------
// MetadataManager – loads and saves DatabaseMetadata + allocation bitmap
// ---------------------------------------------------------------------------
class MetadataManager {
public:
    /// Construct with the path to walnut.meta.
    explicit MetadataManager(const std::string& meta_path);
    ~MetadataManager() = default;

    // Non-copyable; move is fine.
    MetadataManager(const MetadataManager&)            = delete;
    MetadataManager& operator=(const MetadataManager&) = delete;
    MetadataManager(MetadataManager&&)                 = default;
    MetadataManager& operator=(MetadataManager&&)      = default;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Load metadata from disk.  Creates the file with defaults if absent.
    void load();

    /// Persist current metadata to disk.
    void save();

    /// Set clean_shutdown = true and save.
    void markCleanShutdown();

    /// Set clean_shutdown = false and save (called at startup before work begins).
    void markDirty();

    // -----------------------------------------------------------------------
    // Accessors / mutators
    // -----------------------------------------------------------------------
    DatabaseMetadata&       meta()        { return meta_; }
    const DatabaseMetadata& meta() const  { return meta_; }

    bool wasCleanShutdown() const         { return meta_.clean_shutdown; }

    uint32_t nextPageId()  const          { return meta_.next_page_id; }
    uint64_t nextBidId()   const          { return static_cast<uint64_t>(meta_.next_bid_id); }

    /// Allocate the next page ID and update next_page_id in metadata.
    /// Does NOT write to disk; caller must call save() at the right time.
    uint32_t allocatePageId();

    /// Allocate the next bid ID and update next_bid_id in metadata.
    uint64_t allocateBidId();

    // -----------------------------------------------------------------------
    // Allocation bitmap (one bit per page_id; 1 = allocated)
    // -----------------------------------------------------------------------
    void markPageAllocated(uint32_t page_id);
    bool isPageAllocated(uint32_t page_id) const;
    std::size_t allocatedPageCount() const;

    // Expose bitmap for FileManager use
    const std::vector<bool>& allocationBitmap() const { return allocation_bitmap_; }

private:
    std::string      meta_path_;
    DatabaseMetadata meta_;
    std::vector<bool> allocation_bitmap_; ///< One bit per page_id

    void writeToStream(std::fstream& fs);
    void readFromStream(std::fstream& fs);
    void ensureFileExists();
};

} // namespace walnut
