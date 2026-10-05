// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/storage/file_manager.cpp
// =============================================================================

#include "walnut/storage/file_manager.hpp"
#include "walnut/common/constants.hpp"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <filesystem>

namespace walnut {

// ---------------------------------------------------------------------------
FileManager::FileManager(const std::string& data_dir)
    : data_dir_(data_dir),
      db_path_(data_dir + "/walnut.db"),
      meta_mgr_(data_dir + "/walnut.meta"),
      initialised_(false) {}

// ---------------------------------------------------------------------------
FileManager::~FileManager() {
    if (initialised_ && db_file_.is_open()) {
        try { close(); } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
void FileManager::initialize() {
    if (initialised_) return;

    // Ensure data directory exists.
    std::filesystem::create_directories(data_dir_);

    // Load / create metadata.
    meta_mgr_.load();

    // Mark dirty (we are now running; Kanak's WAL recovery checks this).
    meta_mgr_.markDirty();

    openDbFile();

    initialised_ = true;
}

// ---------------------------------------------------------------------------
void FileManager::openDbFile() {
    // Try opening existing file.
    db_file_.open(db_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!db_file_) {
        // File doesn't exist; create it.
        db_file_.open(db_path_,
                      std::ios::binary | std::ios::out | std::ios::trunc);
        db_file_.close();
        db_file_.open(db_path_, std::ios::binary | std::ios::in | std::ios::out);
    }
    if (!db_file_) {
        throw std::runtime_error("FileManager: cannot open " + db_path_);
    }
}

// ---------------------------------------------------------------------------
std::streampos FileManager::pageOffset(uint32_t page_id) const {
    return static_cast<std::streampos>(
        static_cast<uint64_t>(page_id) * PAGE_SIZE);
}

// ---------------------------------------------------------------------------
Page FileManager::readPage(uint32_t page_id) {
    if (!initialised_)
        throw std::runtime_error("FileManager::readPage – not initialised");

    db_file_.seekg(pageOffset(page_id));
    if (!db_file_)
        throw std::runtime_error(
            "FileManager::readPage – seek failed for page " +
            std::to_string(page_id));

    Page page;
    db_file_.read(reinterpret_cast<char*>(page.data()), PAGE_SIZE);
    if (!db_file_ || static_cast<std::size_t>(db_file_.gcount()) != PAGE_SIZE)
        throw std::runtime_error(
            "FileManager::readPage – I/O error reading page " +
            std::to_string(page_id));

    if (!page.verifyChecksum()) {
        throw std::runtime_error(
            "FileManager::readPage – checksum mismatch on page " +
            std::to_string(page_id));
    }

    return page;
}

// ---------------------------------------------------------------------------
void FileManager::writePage(const Page& page) {
    if (!initialised_)
        throw std::runtime_error("FileManager::writePage – not initialised");

    uint32_t page_id = page.pageId();

    db_file_.seekp(pageOffset(page_id));
    if (!db_file_)
        throw std::runtime_error(
            "FileManager::writePage – seek failed for page " +
            std::to_string(page_id));

    db_file_.write(reinterpret_cast<const char*>(page.data()), PAGE_SIZE);
    if (!db_file_)
        throw std::runtime_error(
            "FileManager::writePage – I/O error writing page " +
            std::to_string(page_id));

    // Update allocation bitmap.
    meta_mgr_.markPageAllocated(page_id);
}

// ---------------------------------------------------------------------------
uint32_t FileManager::allocatePage(uint32_t page_type) {
    if (!initialised_)
        throw std::runtime_error("FileManager::allocatePage – not initialised");

    uint32_t new_id = meta_mgr_.allocatePageId();
    Page page(new_id, page_type);
    page.updateChecksum();

    writePage(page);       // Writes to disk and updates bitmap.
    meta_mgr_.save();      // Persist updated next_page_id.
    db_file_.flush();

    return new_id;
}

// ---------------------------------------------------------------------------
uint32_t FileManager::pageCount() const {
    return meta_mgr_.nextPageId();
}

// ---------------------------------------------------------------------------
void FileManager::sync() {
    if (initialised_) {
        db_file_.flush();
        meta_mgr_.save();
    }
}

// ---------------------------------------------------------------------------
void FileManager::close() {
    if (initialised_) {
        db_file_.flush();
        meta_mgr_.markCleanShutdown();
        db_file_.close();
        initialised_ = false;
    }
}

// ---------------------------------------------------------------------------
const DatabaseMetadata& FileManager::metadata() const {
    return meta_mgr_.meta();
}

DatabaseMetadata& FileManager::metadata() {
    return meta_mgr_.meta();
}

} // namespace walnut
