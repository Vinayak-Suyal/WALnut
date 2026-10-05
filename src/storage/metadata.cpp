// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/storage/metadata.cpp
// =============================================================================

#include "walnut/storage/metadata.hpp"
#include "walnut/common/constants.hpp"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace walnut {

// ---------------------------------------------------------------------------
// Binary file layout (walnut.meta)
//
// Offset  0 : uint32_t magic                  (META_MAGIC)
// Offset  4 : uint32_t format_version
// Offset  8 : uint32_t next_page_id
// Offset 12 : uint32_t next_bid_id
// Offset 16 : uint32_t checkpoint_lsn
// Offset 20 : uint8_t  clean_shutdown
// Offset 21 : uint8_t  padding[3]
// Offset 24 : uint32_t bitmap_byte_count
// Offset 28 : uint8_t  padding[4]
// Offset 32 : bitmap bytes (bitmap_byte_count bytes)
// ---------------------------------------------------------------------------

static constexpr std::size_t HEADER_BYTES = 32;

// ---------------------------------------------------------------------------
MetadataManager::MetadataManager(const std::string& meta_path)
    : meta_path_(meta_path), meta_(), allocation_bitmap_() {}

// ---------------------------------------------------------------------------
void MetadataManager::ensureFileExists() {
    // Open in binary append mode to create if missing, then close.
    std::fstream f(meta_path_, std::ios::binary | std::ios::app);
    if (!f) {
        throw std::runtime_error(
            "MetadataManager: cannot open/create " + meta_path_);
    }
}

// ---------------------------------------------------------------------------
void MetadataManager::load() {
    ensureFileExists();

    std::fstream fs(meta_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!fs) {
        throw std::runtime_error("MetadataManager: cannot open " + meta_path_);
    }

    // Check file size
    fs.seekg(0, std::ios::end);
    auto file_size = fs.tellg();
    fs.seekg(0, std::ios::beg);

    if (file_size < static_cast<std::streamoff>(HEADER_BYTES)) {
        // New / empty file – initialise defaults
        meta_            = DatabaseMetadata{};
        allocation_bitmap_.clear();
        save();
        return;
    }

    readFromStream(fs);

    // Warn if previous shutdown was unclean (recovery is Kanak's job).
    if (!meta_.clean_shutdown) {
        std::cerr << "[WALnut WARNING] Previous shutdown was unclean. "
                     "WAL recovery may be required (contact Kanak Rawat).\n";
    }
}

// ---------------------------------------------------------------------------
void MetadataManager::save() {
    std::fstream fs(meta_path_,
                    std::ios::binary | std::ios::in | std::ios::out);
    if (!fs) {
        // File may not exist yet; try creating it.
        fs.open(meta_path_,
                std::ios::binary | std::ios::out | std::ios::trunc);
        if (!fs) {
            throw std::runtime_error(
                "MetadataManager: cannot create " + meta_path_);
        }
    }
    fs.seekp(0, std::ios::beg);
    writeToStream(fs);
    fs.flush();
}

// ---------------------------------------------------------------------------
void MetadataManager::writeToStream(std::fstream& fs) {
    // --- Fixed header (32 bytes) ---
    uint32_t magic = META_MAGIC;
    fs.write(reinterpret_cast<const char*>(&magic),                sizeof(uint32_t));
    fs.write(reinterpret_cast<const char*>(&meta_.format_version), sizeof(uint32_t));
    fs.write(reinterpret_cast<const char*>(&meta_.next_page_id),   sizeof(uint32_t));
    fs.write(reinterpret_cast<const char*>(&meta_.next_bid_id),    sizeof(uint32_t));
    fs.write(reinterpret_cast<const char*>(&meta_.checkpoint_lsn), sizeof(uint32_t));

    uint8_t cs = meta_.clean_shutdown ? 1u : 0u;
    fs.write(reinterpret_cast<const char*>(&cs), 1);

    // 3 bytes padding
    uint8_t pad3[3] = {};
    fs.write(reinterpret_cast<const char*>(pad3), 3);

    // Bitmap size in bytes
    std::size_t bitmap_bits  = allocation_bitmap_.size();
    std::size_t bitmap_bytes = (bitmap_bits + 7) / 8;
    auto bbc = static_cast<uint32_t>(bitmap_bytes);
    fs.write(reinterpret_cast<const char*>(&bbc), sizeof(uint32_t));

    // 4 bytes padding to reach offset 32
    uint8_t pad4[4] = {};
    fs.write(reinterpret_cast<const char*>(pad4), 4);

    // --- Bitmap bytes ---
    if (bitmap_bytes > 0) {
        std::vector<uint8_t> bitmapBuf(bitmap_bytes, 0);
        for (std::size_t i = 0; i < bitmap_bits; ++i) {
            if (allocation_bitmap_[i]) {
                bitmapBuf[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            }
        }
        fs.write(reinterpret_cast<const char*>(bitmapBuf.data()),
                 static_cast<std::streamsize>(bitmap_bytes));
    }
}

// ---------------------------------------------------------------------------
void MetadataManager::readFromStream(std::fstream& fs) {
    uint32_t magic = 0;
    fs.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));
    if (magic != META_MAGIC) {
        throw std::runtime_error(
            "MetadataManager: bad magic in " + meta_path_ +
            " – file may be corrupt");
    }

    fs.read(reinterpret_cast<char*>(&meta_.format_version), sizeof(uint32_t));
    fs.read(reinterpret_cast<char*>(&meta_.next_page_id),   sizeof(uint32_t));
    fs.read(reinterpret_cast<char*>(&meta_.next_bid_id),    sizeof(uint32_t));
    fs.read(reinterpret_cast<char*>(&meta_.checkpoint_lsn), sizeof(uint32_t));

    uint8_t cs = 0;
    fs.read(reinterpret_cast<char*>(&cs), 1);
    meta_.clean_shutdown = (cs != 0);

    uint8_t pad3[3];
    fs.read(reinterpret_cast<char*>(pad3), 3);

    uint32_t bbc = 0;
    fs.read(reinterpret_cast<char*>(&bbc), sizeof(uint32_t));

    uint8_t pad4[4];
    fs.read(reinterpret_cast<char*>(pad4), 4);

    // Bitmap
    allocation_bitmap_.assign(bbc * 8, false);
    if (bbc > 0) {
        std::vector<uint8_t> bitmapBuf(bbc);
        fs.read(reinterpret_cast<char*>(bitmapBuf.data()),
                static_cast<std::streamsize>(bbc));
        for (std::size_t byte_idx = 0; byte_idx < bbc; ++byte_idx) {
            for (int bit = 0; bit < 8; ++bit) {
                bool val = (bitmapBuf[byte_idx] >> bit) & 1u;
                std::size_t bit_idx = byte_idx * 8 + static_cast<std::size_t>(bit);
                if (bit_idx < allocation_bitmap_.size()) {
                    allocation_bitmap_[bit_idx] = val;
                }
            }
        }
        // Trim trailing false bits back to next_page_id size.
        allocation_bitmap_.resize(meta_.next_page_id, false);
    }
}

// ---------------------------------------------------------------------------
void MetadataManager::markCleanShutdown() {
    meta_.clean_shutdown = true;
    save();
}

void MetadataManager::markDirty() {
    meta_.clean_shutdown = false;
    save();
}

// ---------------------------------------------------------------------------
uint32_t MetadataManager::allocatePageId() {
    uint32_t id = meta_.next_page_id++;
    return id;
}

uint64_t MetadataManager::allocateBidId() {
    uint64_t id = static_cast<uint64_t>(meta_.next_bid_id++);
    return id;
}

// ---------------------------------------------------------------------------
void MetadataManager::markPageAllocated(uint32_t page_id) {
    if (page_id >= allocation_bitmap_.size()) {
        allocation_bitmap_.resize(page_id + 1, false);
    }
    allocation_bitmap_[page_id] = true;
}

bool MetadataManager::isPageAllocated(uint32_t page_id) const {
    if (page_id >= allocation_bitmap_.size()) return false;
    return allocation_bitmap_[page_id];
}

std::size_t MetadataManager::allocatedPageCount() const {
    std::size_t count = 0;
    for (bool b : allocation_bitmap_) {
        if (b) ++count;
    }
    return count;
}

} // namespace walnut
