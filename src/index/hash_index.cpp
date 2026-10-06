// =============================================================================
// WALnut – Index Module (Kanak Rawat)
// src/index/hash_index.cpp
//
// Concurrent hash index with per-bucket shared_mutex locking.
// =============================================================================

#include "walnut/index/hash_index.hpp"
#include "walnut/storage/file_manager.hpp"
#include "walnut/storage/page.hpp"
#include "walnut/common/constants.hpp"
#include "walnut/common/utils.hpp"

#include <algorithm>
#include <cstring>
#include <functional>

namespace walnut {

// ===========================================================================
// Construction
// ===========================================================================

HashIndex::HashIndex(std::size_t num_buckets)
    : buckets_(num_buckets)
    , num_buckets_(num_buckets)
{}

// ===========================================================================
// Hash
// ===========================================================================

std::size_t HashIndex::Hash(const std::string& key) const {
    return std::hash<std::string>{}(key) % num_buckets_;
}

// ===========================================================================
// Insert
// ===========================================================================

void HashIndex::Insert(const std::string& key, RecordLocation location) {
    std::size_t idx = Hash(key);
    std::unique_lock lock(buckets_[idx].mu);

    // Check for duplicate — update if found.
    for (auto& entry : buckets_[idx].entries) {
        if (entry.key == key) {
            entry.location = location;
            return;
        }
    }
    buckets_[idx].entries.push_back({key, location});
}

// ===========================================================================
// Lookup
// ===========================================================================

std::optional<RecordLocation> HashIndex::Lookup(const std::string& key) const {
    std::size_t idx = Hash(key);
    std::shared_lock lock(buckets_[idx].mu);

    for (const auto& entry : buckets_[idx].entries) {
        if (entry.key == key) {
            return entry.location;
        }
    }
    return std::nullopt;
}

// ===========================================================================
// Delete
// ===========================================================================

bool HashIndex::Delete(const std::string& key) {
    std::size_t idx = Hash(key);
    std::unique_lock lock(buckets_[idx].mu);

    auto& entries = buckets_[idx].entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->key == key) {
            // Swap with last and pop (O(1) removal).
            *it = std::move(entries.back());
            entries.pop_back();
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Update
// ===========================================================================

bool HashIndex::Update(const std::string& key, RecordLocation new_location) {
    std::size_t idx = Hash(key);
    std::unique_lock lock(buckets_[idx].mu);

    for (auto& entry : buckets_[idx].entries) {
        if (entry.key == key) {
            entry.location = new_location;
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Size
// ===========================================================================

std::size_t HashIndex::Size() const {
    std::size_t total = 0;
    for (std::size_t i = 0; i < num_buckets_; ++i) {
        std::shared_lock lock(buckets_[i].mu);
        total += buckets_[i].entries.size();
    }
    return total;
}

// ===========================================================================
// BuildFromDisk
// ===========================================================================

void HashIndex::BuildFromDisk(FileManager& file_mgr) {
    uint32_t page_count = file_mgr.pageCount();

    for (uint32_t page_id = 0; page_id < page_count; ++page_id) {
        Page page;
        try {
            page = file_mgr.readPage(page_id);
        } catch (const std::exception&) {
            continue;  // Skip unreadable pages.
        }

        uint32_t page_type = page.pageType();
        uint32_t record_count = page.recordCount();

        if (page_type == PAGE_TYPE_USERS) {
            for (uint32_t slot = 0; slot < record_count; ++slot) {
                try {
                    const uint8_t* rec_ptr = page.getRecord(slot, USER_RECORD_SIZE);
                    // First MAX_ID_LEN bytes are the user_id.
                    std::string user_id = utils::readCString(
                        reinterpret_cast<const char*>(rec_ptr), MAX_ID_LEN);
                    if (!user_id.empty()) {
                        Insert(user_id, {page_id, static_cast<uint16_t>(slot)});
                    }
                } catch (const std::exception&) {
                    continue;
                }
            }
        } else if (page_type == PAGE_TYPE_ITEMS) {
            for (uint32_t slot = 0; slot < record_count; ++slot) {
                try {
                    const uint8_t* rec_ptr = page.getRecord(slot, ITEM_RECORD_SIZE);
                    // First MAX_ID_LEN bytes are the item_id.
                    std::string item_id = utils::readCString(
                        reinterpret_cast<const char*>(rec_ptr), MAX_ID_LEN);
                    if (!item_id.empty()) {
                        Insert(item_id, {page_id, static_cast<uint16_t>(slot)});
                    }
                } catch (const std::exception&) {
                    continue;
                }
            }
        }
        // Bids are not indexed by primary key in this version.
    }
}

} // namespace walnut
