// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/storage/storage_manager.cpp
//
// All public methods are designed to be wrapped by Sushmit's transaction layer
// without modification – just acquire the appropriate lock before calling.
// =============================================================================

#include "walnut/storage/storage_manager.hpp"
#include "walnut/common/types.hpp"
#include "walnut/common/constants.hpp"
#include "walnut/common/utils.hpp"
#include <stdexcept>
#include <algorithm>
#include <iostream>

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

// ---------------------------------------------------------------------------
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
        Page page = fm.readPage(pid);
        if (page.pageType() == page_type) {
            visitor(pid, page);
        }
    }
}

// ---------------------------------------------------------------------------
void StorageManager::rebuildIndexes() {
    user_index_.clear();
    item_index_.clear();
    bid_index_.clear();
    current_user_page_ = UINT32_MAX;
    current_item_page_ = UINT32_MAX;
    current_bid_page_  = UINT32_MAX;

    // ---- Users ----
    scanPages(file_manager_, PAGE_TYPE_USERS, [&](uint32_t pid, const Page& page) {
        uint32_t rc = page.recordCount();
        for (uint32_t slot = 0; slot < rc; ++slot) {
            const uint8_t* raw = page.getRecord(slot, USER_RECORD_SIZE);
            User u = deserialiseUser(raw);
            user_index_[u.user_id] = {pid, slot};
        }
        // The last user page is the current one (receives next append).
        if (page.hasRoom(USER_RECORD_SIZE)) {
            current_user_page_ = pid;
        }
    });

    // ---- Items ----
    scanPages(file_manager_, PAGE_TYPE_ITEMS, [&](uint32_t pid, const Page& page) {
        uint32_t rc = page.recordCount();
        for (uint32_t slot = 0; slot < rc; ++slot) {
            const uint8_t* raw = page.getRecord(slot, ITEM_RECORD_SIZE);
            Item item = deserialiseItem(raw);
            item_index_[item.item_id] = {pid, slot};
        }
        if (page.hasRoom(ITEM_RECORD_SIZE)) {
            current_item_page_ = pid;
        }
    });

    // ---- Bids ----
    scanPages(file_manager_, PAGE_TYPE_BIDS, [&](uint32_t pid, const Page& page) {
        uint32_t rc = page.recordCount();
        for (uint32_t slot = 0; slot < rc; ++slot) {
            const uint8_t* raw = page.getRecord(slot, BID_RECORD_SIZE);
            Bid bid = deserialiseBid(raw);
            bid_index_[bid.item_id].push_back({pid, slot});
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

void StorageManager::flushPage(uint32_t /*page_id*/, const Page& page) {
    file_manager_.writePage(page);
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

void StorageManager::updateItemAfterBid(const std::string& item_id,
                                         double             new_current_bid,
                                         const std::string& winner_user_id) {
    auto it = item_index_.find(item_id);
    if (it == item_index_.end())
        throw std::runtime_error("updateItemAfterBid: item_id '" +
                                 item_id + "' not found");

    Page page = file_manager_.readPage(it->second.page_id);
    uint8_t* raw = page.getRecord(it->second.slot, ITEM_RECORD_SIZE);

    // Deserialise, mutate, re-serialise in place.
    Item item = deserialiseItem(raw);
    item.current_bid    = new_current_bid;
    item.current_winner = winner_user_id;
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
