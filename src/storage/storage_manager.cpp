// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/storage/storage_manager.cpp
//
// All public methods are designed to be wrapped by Sushmit's transaction layer
// without modification – just acquire the appropriate lock before calling.
//
// Schema v2.0 (OSDBMS-V-2026-T038) additions:
//   updateItemAfterBid  – maintains descending Top-3 podium + total_bid_count
//   cancelItem          – status → "CANCEL" for host-initiated cancellation
//   getItemsWonByUser   – Ω{item | winner==uid ∧ status=="CLOSED"}
//   getItemsByHost      – Ω{item | host_id==uid}
//   updateUser          – re-serialises a modified User record in-place
// =============================================================================

#include "walnut/storage/storage_manager.hpp"
#include "walnut/common/types.hpp"
#include "walnut/common/constants.hpp"
#include "walnut/common/utils.hpp"
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstring>
#include <filesystem>

namespace walnut {

// ---------------------------------------------------------------------------
StorageManager::StorageManager(const std::string& data_dir)
    : file_manager_(data_dir) {}

StorageManager::~StorageManager() {
    try { close(); } catch (...) {}
}

// ---------------------------------------------------------------------------
void StorageManager::initialize() {
    file_manager_.initialize();
    rebuildIndexes();
}

void StorageManager::close() {
    file_manager_.close();
}

// ---------------------------------------------------------------------------
// --- Internal helpers ---
// ---------------------------------------------------------------------------

/// Scan all pages of `page_type` and call `visitor(page_id, page)` for each.
/// (Helper used by rebuildIndexes to avoid duplication.)
template<typename Visitor>
static void scanPages(FileManager& fm, uint32_t page_type, Visitor&& visitor) {
    uint32_t total = fm.pageCount();
    for (uint32_t pid = 0; pid < total; ++pid) {
        try {
            Page page = fm.readPage(pid);
            if (page.pageType() == page_type) {
                visitor(pid, page);
            }
        } catch (...) {
            // Skip unreadable or corrupted pages during index recovery
        }
    }
}

// ---------------------------------------------------------------------------
void StorageManager::rebuildIndexes() {
    user_index_.clear();
    item_index_.clear();
    bid_index_.clear();
    category_index_.clear();
    current_user_page_ = UINT32_MAX;
    current_item_page_ = UINT32_MAX;
    current_bid_page_  = UINT32_MAX;

    // ---- Users ----
    scanPages(file_manager_, PAGE_TYPE_USERS, [&](uint32_t pid, const Page& page) {
        uint32_t rc = page.recordCount();
        for (uint32_t slot = 0; slot < rc; ++slot) {
            try {
                const uint8_t* raw = page.getRecord(slot, USER_RECORD_SIZE);
                User u = deserialiseUser(raw);
                if (!u.user_id.empty()) {
                    user_index_[u.user_id] = {pid, slot};
                }
            } catch (...) {
                break;
            }
        }
        if (page.hasRoom(USER_RECORD_SIZE)) {
            current_user_page_ = pid;
        }
    });

    // ---- Items ----
    scanPages(file_manager_, PAGE_TYPE_ITEMS, [&](uint32_t pid, const Page& page) {
        uint32_t rc = page.recordCount();
        for (uint32_t slot = 0; slot < rc; ++slot) {
            try {
                const uint8_t* raw = page.getRecord(slot, ITEM_RECORD_SIZE);
                Item item = deserialiseItem(raw);
                if (!item.item_id.empty()) {
                    item_index_[item.item_id] = {pid, slot};
                    if (!item.category.empty()) {
                        category_index_[item.category].push_back(item.item_id);
                    }
                }
            } catch (...) {
                break;
            }
        }
        if (page.hasRoom(ITEM_RECORD_SIZE)) {
            current_item_page_ = pid;
        }
    });

    // ---- Bids ----
    scanPages(file_manager_, PAGE_TYPE_BIDS, [&](uint32_t pid, const Page& page) {
        uint32_t rc = page.recordCount();
        for (uint32_t slot = 0; slot < rc; ++slot) {
            try {
                const uint8_t* raw = page.getRecord(slot, BID_RECORD_SIZE);
                Bid bid = deserialiseBid(raw);
                if (!bid.item_id.empty()) {
                    bid_index_[bid.item_id].push_back({pid, slot});
                }
            } catch (...) {
                break;
            }
        }
        if (page.hasRoom(BID_RECORD_SIZE)) {
            current_bid_page_ = pid;
        }
    });
}

// ---------------------------------------------------------------------------
uint32_t StorageManager::getOrAllocatePage(uint32_t page_type,
                                            std::size_t record_size,
                                            uint32_t& current_page_tracker) {
    if (current_page_tracker != UINT32_MAX) {
        Page p = file_manager_.readPage(current_page_tracker);
        if (p.hasRoom(record_size)) {
            return current_page_tracker;
        }
    }
    // Allocate a fresh page.
    uint32_t new_pid = file_manager_.allocatePage(page_type);
    current_page_tracker = new_pid;
    return new_pid;
}

// ---------------------------------------------------------------------------
// --- Users ---
// ---------------------------------------------------------------------------

void StorageManager::createUser(const User& user) {
    if (user.user_id.empty())
        throw std::invalid_argument("createUser: user_id must not be empty");
    if (user_index_.count(user.user_id))
        throw std::runtime_error("createUser: duplicate user_id '" +
                                 user.user_id + "'");

    uint8_t record_buf[USER_RECORD_SIZE];
    serialiseUser(user, record_buf);  // Throws invalid_argument on length violation.

    uint32_t pid = getOrAllocatePage(PAGE_TYPE_USERS, USER_RECORD_SIZE,
                                     current_user_page_);
    Page page = file_manager_.readPage(pid);
    uint32_t slot = page.recordCount();
    page.appendRecord(record_buf, USER_RECORD_SIZE);
    page.updateChecksum();
    file_manager_.writePage(page);

    user_index_[user.user_id] = {pid, slot};
}

std::optional<User> StorageManager::findUserById(const std::string& user_id) const {
    auto it = user_index_.find(user_id);
    if (it == user_index_.end()) return std::nullopt;

    Page page = file_manager_.readPage(it->second.page_id);
    const uint8_t* raw = page.getRecord(it->second.slot, USER_RECORD_SIZE);
    return deserialiseUser(raw);
}

std::vector<User> StorageManager::getAllUsers() const {
    std::vector<User> result;
    result.reserve(user_index_.size());
    for (auto& [uid, loc] : user_index_) {
        try {
            Page page = file_manager_.readPage(loc.page_id);
            const uint8_t* raw = page.getRecord(loc.slot, USER_RECORD_SIZE);
            User u = deserialiseUser(raw);
            if (!u.user_id.empty()) {
                result.push_back(std::move(u));
            }
        } catch (...) {}
    }
    return result;
}

void StorageManager::deleteUser(const std::string& user_id) {
    auto it = user_index_.find(user_id);
    if (it == user_index_.end())
        throw std::runtime_error("deleteUser: user_id '" + user_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, USER_RECORD_SIZE);
    std::memset(raw, 0, USER_RECORD_SIZE);
    page.updateChecksum();
    file_manager_.writePage(page);

    user_index_.erase(it);
}

// ---------------------------------------------------------------------------
// --- Items ---
// ---------------------------------------------------------------------------

void StorageManager::createItem(const Item& item) {
    if (item.item_id.empty())
        throw std::invalid_argument("createItem: item_id must not be empty");
    if (item_index_.count(item.item_id))
        throw std::runtime_error("createItem: duplicate item_id '" +
                                 item.item_id + "'");

    uint8_t record_buf[ITEM_RECORD_SIZE];
    serialiseItem(item, record_buf);

    uint32_t pid = getOrAllocatePage(PAGE_TYPE_ITEMS, ITEM_RECORD_SIZE,
                                     current_item_page_);
    Page page = file_manager_.readPage(pid);
    uint32_t slot = page.recordCount();
    page.appendRecord(record_buf, ITEM_RECORD_SIZE);
    page.updateChecksum();
    file_manager_.writePage(page);

    item_index_[item.item_id] = {pid, slot};
    // Maintain category secondary index on insert.
    if (!item.category.empty()) {
        category_index_[item.category].push_back(item.item_id);
    }
}

std::optional<Item> StorageManager::getItem(const std::string& item_id) const {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end()) return std::nullopt;

    Page page = file_manager_.readPage(it->second.page_id);
    const uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);
    return deserialiseItem(raw);
}

std::vector<Item> StorageManager::getAllItems() const {
    std::vector<Item> result;
    result.reserve(item_index_.size());
    for (auto& [item_id, loc] : item_index_) {
        Page page = file_manager_.readPage(loc.page_id);
        const uint8_t* raw = page.getRecord(loc.slot, ITEM_RECORD_SIZE);
        result.push_back(deserialiseItem(raw));
    }
    return result;
}

void StorageManager::deleteItem(const std::string& item_id) {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end())
        throw std::runtime_error("deleteItem: item_id '" + item_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);
    std::memset(raw, 0, ITEM_RECORD_SIZE);
    page.updateChecksum();
    file_manager_.writePage(page);

    // Clean up category index
    for (auto& [cat, items] : category_index_) {
        items.erase(std::remove(items.begin(), items.end(), item_id), items.end());
    }

    item_index_.erase(it);
    bid_index_.erase(item_id);
}

void StorageManager::purgeItems() {
    for (auto& [id, loc] : item_index_) {
        try {
            Page page = file_manager_.readPage(loc.page_id);
            uint8_t* raw = page.getRecord(loc.slot, ITEM_RECORD_SIZE);
            std::memset(raw, 0, ITEM_RECORD_SIZE);
            page.updateChecksum();
            file_manager_.writePage(page);
        } catch (...) {}
    }
    item_index_.clear();
    category_index_.clear();
    bid_index_.clear();
}

void StorageManager::resetDatabase() {
    std::string dir = file_manager_.dataDir();
    close();
    std::error_code ec;
    std::filesystem::remove(dir + "/walnut.db", ec);
    std::filesystem::remove(dir + "/walnut.meta", ec);
    std::filesystem::remove(dir + "/walnut.wal", ec);
    initialize();
}

void StorageManager::updateItemAfterBid(const std::string& item_id,
                                         double             new_current_bid,
                                         const std::string& winner_user_id,
                                         uint64_t           bid_id,
                                         uint64_t           timestamp_ms) {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end())
        throw std::runtime_error("updateItemAfterBid: item_id '" +
                                 item_id + "' not found");

    // Derive timestamp_ms from wall clock when caller omits it (legacy path).
    if (timestamp_ms == 0) {
        using namespace std::chrono;
        timestamp_ms = static_cast<uint64_t>(
            duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count());
    }

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);

    // Deserialise, mutate, re-serialise in place.
    Item item = deserialiseItem(raw);
    item.current_bid    = new_current_bid;
    item.current_winner = winner_user_id;
    item.total_bid_count++;

    // ---------------------------------------------------------------------------
    // Top-3 Podium maintenance (descending order, in-place shift)
    //
    // Insertion policy:
    //   - A new winning bid is always >= every previous bid (enforced upstream),
    //     so the new bid always lands at slot 0 (gold).
    //   - Before inserting, shift existing slots downward: 1->2, 0->1.
    //   - top_bid_count is capped at TOP_BID_COUNT (3).
    //
    // This guarantees: top_bids[0].amount >= top_bids[1].amount >= top_bids[2].amount
    // ---------------------------------------------------------------------------
    const std::size_t MAX_SLOTS = TOP_BID_COUNT; // 3

    // Shift: slot[1] -> slot[2], slot[0] -> slot[1]
    // (slot[2] is simply overwritten / dropped from the podium)
    for (std::size_t i = MAX_SLOTS - 1; i > 0; --i) {
        item.top_bids[i] = item.top_bids[i - 1];
    }

    // Insert the new winner at slot 0 (gold).
    TopBid& gold    = item.top_bids[0];
    gold.bid_id      = bid_id;
    gold.user_id     = winner_user_id;
    gold.amount      = new_current_bid;
    gold.timestamp_ms = timestamp_ms;

    // Update occupied slot count (capped at MAX_SLOTS).
    if (item.top_bid_count < static_cast<uint32_t>(MAX_SLOTS)) {
        item.top_bid_count++;
    }

    serialiseItem(item, raw);   // writes back into page buffer

    page.updateChecksum();
    file_manager_.writePage(page);
}

void StorageManager::closeItem(const std::string& item_id) {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end())
        throw std::runtime_error("closeItem: item_id '" + item_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);

    Item item = deserialiseItem(raw);
    item.status = "CLOSED";
    serialiseItem(item, raw);

    page.updateChecksum();
    file_manager_.writePage(page);
}

// ---------------------------------------------------------------------------
void StorageManager::cancelItem(const std::string& item_id) {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end())
        throw std::runtime_error("cancelItem: item_id '" + item_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);

    Item item = deserialiseItem(raw);
    item.status = "CANCEL";
    serialiseItem(item, raw);

    page.updateChecksum();
    file_manager_.writePage(page);
}

// ---------------------------------------------------------------------------
void StorageManager::markUnsoldItem(const std::string& item_id) {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end())
        throw std::runtime_error("markUnsoldItem: item_id '" + item_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);

    Item item = deserialiseItem(raw);
    item.status = "UNSOLD";
    serialiseItem(item, raw);

    page.updateChecksum();
    file_manager_.writePage(page);
}

// ---------------------------------------------------------------------------
void StorageManager::extendItemEndTime(const std::string& item_id,
                                       uint64_t           new_end_time_ms) {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end())
        throw std::runtime_error(
            "extendItemEndTime: item_id '" + item_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);

    Item item = deserialiseItem(raw);
    item.end_time_ms = new_end_time_ms;
    serialiseItem(item, raw);

    page.updateChecksum();
    file_manager_.writePage(page);
}

// ---------------------------------------------------------------------------
// Relational queries (Schema v2.0)
// ---------------------------------------------------------------------------


std::vector<Item> StorageManager::getItemsWonByUser(
        const std::string& user_id) const {
    // Ω{ item ∈ Items | (item.current_winner == user_id ∨ item.current_winner == username) ∧ (status == "CLOSED" ∨ expired) }
    std::vector<Item> result;
    std::string uname = user_id;
    try {
        auto u = findUserById(user_id);
        if (u && !u->username.empty()) uname = u->username;
    } catch (...) {}

    using namespace std::chrono;
    uint64_t now_ms = (uint64_t)duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    for (auto& [id, loc] : item_index_) {
        Page page = file_manager_.readPage(loc.page_id);
        const uint8_t* raw = page.getRecord(loc.slot, ITEM_RECORD_SIZE);
        Item item = deserialiseItem(raw);
        bool is_winner = (!item.current_winner.empty() &&
                          (item.current_winner == user_id || item.current_winner == uname));
        bool is_closed = (item.status == "CLOSED") ||
                         (item.end_time_ms > 0 && now_ms >= item.end_time_ms && item.status == "OPEN");
        if (is_winner && is_closed) {
            item.status = "CLOSED";
            result.push_back(std::move(item));
        }
    }
    return result;
}

std::vector<Item> StorageManager::getItemsByHost(
        const std::string& host_user_id) const {
    // Ω{ item ∈ Items | item.host_id == host_user_id }
    std::vector<Item> result;
    for (auto& [id, loc] : item_index_) {
        Page page = file_manager_.readPage(loc.page_id);
        const uint8_t* raw = page.getRecord(loc.slot, ITEM_RECORD_SIZE);
        Item item = deserialiseItem(raw);
        if (item.host_id == host_user_id) {
            result.push_back(std::move(item));
        }
    }
    return result;
}

std::vector<Item> StorageManager::getItemsByCategory(
        const std::string& category) const {
    auto it = category_index_.find(category);
    if (it == category_index_.end()) return {};

    std::vector<Item> result;
    result.reserve(it->second.size());
    for (const std::string& item_id : it->second) {
        auto item_opt = getItem(item_id);
        if (item_opt) result.push_back(std::move(*item_opt));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Users (extended)
// ---------------------------------------------------------------------------

void StorageManager::updateUser(const User& user) {
    auto it = user_index_.find(user.user_id);
    if (it == user_index_.end())
        throw std::runtime_error("updateUser: user_id '" +
                                 user.user_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, USER_RECORD_SIZE);

    serialiseUser(user, raw);   // overwrite in-place

    page.updateChecksum();
    file_manager_.writePage(page);
}

// ---------------------------------------------------------------------------
// --- Bids ---
// ---------------------------------------------------------------------------

uint64_t StorageManager::insertBid(const Bid& bid) {
    if (bid.item_id.empty())
        throw std::invalid_argument("insertBid: item_id must not be empty");

    // Assign bid_id from metadata (ignore caller's bid.bid_id field).
    uint64_t assigned_id = file_manager_.metadataManager().allocateBidId();
    file_manager_.metadataManager().save();

    Bid to_write        = bid;
    to_write.bid_id     = assigned_id;
    if (to_write.bid_time.empty()) {
        to_write.bid_time = utils::currentUtcTimestamp();
    }

    uint8_t record_buf[BID_RECORD_SIZE];
    serialiseBid(to_write, record_buf);

    uint32_t pid = getOrAllocatePage(PAGE_TYPE_BIDS, BID_RECORD_SIZE,
                                     current_bid_page_);
    Page page = file_manager_.readPage(pid);
    uint32_t slot = page.recordCount();
    page.appendRecord(record_buf, BID_RECORD_SIZE);
    page.updateChecksum();
    file_manager_.writePage(page);

    bid_index_[bid.item_id].push_back({pid, slot});

    return assigned_id;
}

std::vector<Bid> StorageManager::getBids(const std::string& item_id) const {
    auto it = bid_index_.find(item_id);
    if (it == bid_index_.end()) return {};

    std::vector<Bid> result;
    result.reserve(it->second.size());

    for (auto& loc : it->second) {
        Page page = file_manager_.readPage(loc.page_id);
        const uint8_t* raw = page.getRecord(loc.slot, BID_RECORD_SIZE);
        result.push_back(deserialiseBid(raw));
    }

    // Sort by bid_id ascending (append order).
    std::sort(result.begin(), result.end(),
              [](const Bid& a, const Bid& b) { return a.bid_id < b.bid_id; });

    return result;
}

std::optional<Bid> StorageManager::getHighestBid(const std::string& item_id) const {
    auto bids = getBids(item_id);
    if (bids.empty()) return std::nullopt;

    auto it = std::max_element(bids.begin(), bids.end(),
        [](const Bid& a, const Bid& b) { return a.amount < b.amount; });
    return *it;
}

// ---------------------------------------------------------------------------
uint32_t StorageManager::pageCount() const {
    return file_manager_.pageCount();
}

void StorageManager::sync() {
    file_manager_.sync();
}

} // namespace walnut
