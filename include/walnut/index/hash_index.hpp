// =============================================================================
// WALnut – Index Module (Kanak Rawat)
// include/walnut/index/hash_index.hpp
//
// Concurrent hash index with per-bucket shared_mutex locking.
// Provides O(1) average-case lookups for user_id and item_id keys.
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <shared_mutex>
#include <optional>
#include <functional>
#include <cstdint>
#include <cstddef>

namespace walnut {

// Forward declarations.
class FileManager;

// ---------------------------------------------------------------------------
// RecordLocation – identifies a record's physical position on disk.
// ---------------------------------------------------------------------------
struct RecordLocation {
    uint32_t page_id = 0;
    uint16_t slot_id = 0;
};

// ---------------------------------------------------------------------------
// HashIndex – concurrent hash map from string keys to RecordLocations.
// ---------------------------------------------------------------------------
class HashIndex {
public:
    /// Construct with a fixed number of buckets.
    explicit HashIndex(std::size_t num_buckets = 1024);

    /// Insert a key → location mapping.
    /// If the key already exists, updates its location.
    void Insert(const std::string& key, RecordLocation location);

    /// Look up a key.  Returns std::nullopt if not found.
    std::optional<RecordLocation> Lookup(const std::string& key) const;

    /// Delete a key.  Returns true if the key was found and removed.
    bool Delete(const std::string& key);

    /// Update the location for an existing key.
    /// Returns true if the key was found and updated.
    bool Update(const std::string& key, RecordLocation new_location);

    /// Return the total number of entries across all buckets.
    std::size_t Size() const;

    /// Rebuild the index from disk by scanning all pages.
    /// Reads every page from the FileManager and indexes users and items.
    void BuildFromDisk(FileManager& file_mgr);

private:
    struct Entry {
        std::string    key;
        RecordLocation location;
    };

    struct Bucket {
        std::vector<Entry>        entries;
        mutable std::shared_mutex mu;   // Per-bucket reader-writer lock
    };

    std::vector<Bucket> buckets_;
    std::size_t         num_buckets_;

    /// Hash function: maps a string key to a bucket index.
    std::size_t Hash(const std::string& key) const;
};

} // namespace walnut
