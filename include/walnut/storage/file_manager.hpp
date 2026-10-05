// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/storage/file_manager.hpp
//
// FileManager handles all raw page I/O to walnut.db.
// It delegates metadata persistence to MetadataManager.
//
// Thread-safety: FileManager itself is NOT thread-safe; the BufferPool (which
// wraps it) serialises all disk operations under its own mutex.
// =============================================================================

#pragma once

#include "walnut/storage/page.hpp"
#include "walnut/storage/metadata.hpp"
#include <string>
#include <fstream>
#include <cstdint>

namespace walnut {

class FileManager {
public:
    /// Construct with the directory that holds walnut.db and walnut.meta.
    explicit FileManager(const std::string& data_dir);
    ~FileManager();

    // Non-copyable; move is allowed.
    FileManager(const FileManager&)            = delete;
    FileManager& operator=(const FileManager&) = delete;
    FileManager(FileManager&&)                 = default;
    FileManager& operator=(FileManager&&)      = default;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Open or create walnut.db and walnut.meta, load metadata, mark dirty.
    /// Must be called before any other method.
    void initialize();

    /// Flush both files and close them cleanly.  Sets clean_shutdown flag.
    void close();

    // -----------------------------------------------------------------------
    // Page I/O
    // -----------------------------------------------------------------------

    /// Read page `page_id` from disk and return it as a Page object.
    /// Throws std::runtime_error on I/O failure or checksum mismatch.
    Page readPage(uint32_t page_id);

    /// Write `page` to disk at offset page.pageId() * PAGE_SIZE.
    /// Throws std::runtime_error on I/O failure.
    void writePage(const Page& page);

    // -----------------------------------------------------------------------
    // Allocation
    // -----------------------------------------------------------------------

    /// Allocate a new page of `page_type`, write an empty initialised page,
    /// update allocation bitmap, and return the new page_id.
    uint32_t allocatePage(uint32_t page_type);

    /// Total number of pages ever allocated (= next_page_id).
    uint32_t pageCount() const;

    // -----------------------------------------------------------------------
    // Sync
    // -----------------------------------------------------------------------

    /// Flush the database file and metadata without closing.
    void sync();

    // -----------------------------------------------------------------------
    // Metadata access (for StorageManager's in-memory index rebuilds)
    // -----------------------------------------------------------------------
    const DatabaseMetadata& metadata() const;
    DatabaseMetadata& metadata();

    /// Expose MetadataManager so StorageManager can call allocateBidId().
    MetadataManager& metadataManager() { return meta_mgr_; }
    const MetadataManager& metadataManager() const { return meta_mgr_; }

private:
    std::string      data_dir_;
    std::string      db_path_;
    MetadataManager  meta_mgr_;
    std::fstream     db_file_;
    bool             initialised_{false};

    /// Byte offset of page_id in walnut.db.
    std::streampos pageOffset(uint32_t page_id) const;

    /// Open or create walnut.db.
    void openDbFile();
};

} // namespace walnut
