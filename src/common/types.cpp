// WALnut – serialisation / deserialisation  (OSDBMS-V-2026-T038)
// Binary layouts authoritative in docs/schema-v2-specification.md §3.
#include "walnut/common/types.hpp"
#include "walnut/common/constants.hpp"
#include "walnut/common/utils.hpp"
#include <cstring>
#include <stdexcept>

namespace walnut {

// ---- Internal helpers ---------------------------------------------------

static void copyStr(const std::string& src, uint8_t* dst,
                    std::size_t len, const char* field) {
    if (src.size() >= len)
        throw std::invalid_argument(
            std::string(field) + " exceeds max length " +
            std::to_string(len - 1) + " (got " + std::to_string(src.size()) + ")");
    utils::safeCopyString(src, reinterpret_cast<char*>(dst), len);
}

static std::string readStr(const uint8_t* src, std::size_t len) {
    return utils::readCString(reinterpret_cast<const char*>(src), len);
}

template<typename T>
static void put(uint8_t* dst, std::size_t& off, const T& val) {
    std::memcpy(dst + off, &val, sizeof(T)); off += sizeof(T);
}
template<typename T>
static void get(const uint8_t* src, std::size_t& off, T& val) {
    std::memcpy(&val, src + off, sizeof(T)); off += sizeof(T);
}

// ---- User  (280 bytes) --------------------------------------------------

void serialiseUser(const User& u, uint8_t* dst) {
    std::size_t off = 0;
    auto s = [&](const std::string& v, std::size_t len, const char* f) {
        copyStr(v, dst + off, len, f); off += len;
    };
    s(u.user_id,       MAX_ID_LEN,            "user_id");
    s(u.username,      MAX_ID_LEN,            "username");
    s(u.email,         USER_EMAIL_LEN,        "email");
    s(u.phone,         USER_PHONE_LEN,        "phone");
    s(u.password_hash, USER_PASSWORD_HASH_LEN,"password_hash");
    s(u.role,          USER_ROLE_LEN,         "role");
}

User deserialiseUser(const uint8_t* src) {
    User u; std::size_t off = 0;
    auto r = [&](std::size_t len) -> std::string {
        std::string v = readStr(src + off, len); off += len; return v;
    };
    u.user_id       = r(MAX_ID_LEN);
    u.username      = r(MAX_ID_LEN);
    u.email         = r(USER_EMAIL_LEN);
    u.phone         = r(USER_PHONE_LEN);
    u.password_hash = r(USER_PASSWORD_HASH_LEN);
    u.role          = r(USER_ROLE_LEN);
    return u;
}

// ---- TopBid  (112 bytes) ------------------------------------------------

void serialiseTopBid(const TopBid& tb, uint8_t* dst) {
    std::size_t off = 0;
    put(dst, off, tb.bid_id);
    copyStr(tb.user_id, dst + off, MAX_ID_LEN, "TopBid::user_id"); off += MAX_ID_LEN;
    put(dst, off, tb.amount);
    put(dst, off, tb.timestamp_ms);
    std::memset(dst + off, 0, TOP_BID_RESERVED_LEN); off += TOP_BID_RESERVED_LEN;
}

TopBid deserialiseTopBid(const uint8_t* src) {
    TopBid tb; std::size_t off = 0;
    get(src, off, tb.bid_id);
    tb.user_id = readStr(src + off, MAX_ID_LEN); off += MAX_ID_LEN;
    get(src, off, tb.amount);
    get(src, off, tb.timestamp_ms);
    off += TOP_BID_RESERVED_LEN;
    return tb;
}

// ---- Item  (800 bytes) --------------------------------------------------

void serialiseItem(const Item& item, uint8_t* dst) {
    std::size_t off = 0;
    auto s = [&](const std::string& v, std::size_t len, const char* f) {
        copyStr(v, dst + off, len, f); off += len;
    };
    s(item.item_id,       ITEM_ID_LEN,       "item_id");
    s(item.host_id,       ITEM_HOST_ID_LEN,  "host_id");
    s(item.title,         ITEM_TITLE_LEN,    "title");
    s(item.category,      ITEM_CATEGORY_LEN, "category");
    s(item.condition,     ITEM_CONDITION_LEN,"condition");
    for (std::size_t i = 0; i < ITEM_IMAGE_URL_COUNT; ++i) {
        s(item.image_urls[i], ITEM_IMAGE_URL_LEN, "image_url");
    }
    put(dst, off, item.starting_price);
    s(item.status,        ITEM_STATUS_LEN,   "status");
    put(dst, off, item.current_bid);
    s(item.current_winner,ITEM_WINNER_LEN,   "current_winner");
    put(dst, off, item.end_time_ms);
    put(dst, off, item.top_bid_count);
    put(dst, off, item.total_bid_count);
    for (std::size_t i = 0; i < TOP_BID_COUNT; ++i) {
        serialiseTopBid(item.top_bids[i], dst + off); off += TOP_BID_SLOT_SIZE;
    }
    std::memset(dst + off, 0, ITEM_RESERVED_LEN);
}

Item deserialiseItem(const uint8_t* src) {
    Item item; std::size_t off = 0;
    auto r = [&](std::size_t len) -> std::string {
        std::string v = readStr(src + off, len); off += len; return v;
    };
    item.item_id       = r(ITEM_ID_LEN);
    item.host_id       = r(ITEM_HOST_ID_LEN);
    item.title         = r(ITEM_TITLE_LEN);
    item.category      = r(ITEM_CATEGORY_LEN);
    item.condition     = r(ITEM_CONDITION_LEN);
    for (std::size_t i = 0; i < ITEM_IMAGE_URL_COUNT; ++i)
        item.image_urls[i] = r(ITEM_IMAGE_URL_LEN);
    get(src, off, item.starting_price);
    item.status        = r(ITEM_STATUS_LEN);
    get(src, off, item.current_bid);
    item.current_winner = r(ITEM_WINNER_LEN);
    get(src, off, item.end_time_ms);
    get(src, off, item.top_bid_count);
    get(src, off, item.total_bid_count);
    for (std::size_t i = 0; i < TOP_BID_COUNT; ++i) {
        item.top_bids[i] = deserialiseTopBid(src + off); off += TOP_BID_SLOT_SIZE;
    }
    return item;
}

// ---- Bid  (176 bytes, unchanged from v1.0) ------------------------------

void serialiseBid(const Bid& bid, uint8_t* dst) {
    std::size_t off = 0;
    put(dst, off, bid.bid_id);
    copyStr(bid.item_id,  dst + off, MAX_ID_LEN,        "Bid::item_id");  off += MAX_ID_LEN;
    copyStr(bid.user_id,  dst + off, MAX_ID_LEN,        "Bid::user_id");  off += MAX_ID_LEN;
    put(dst, off, bid.amount);
    copyStr(bid.bid_time, dst + off, MAX_TIMESTAMP_LEN, "Bid::bid_time"); off += MAX_TIMESTAMP_LEN;
}

Bid deserialiseBid(const uint8_t* src) {
    Bid bid; std::size_t off = 0;
    get(src, off, bid.bid_id);
    bid.item_id  = readStr(src + off, MAX_ID_LEN);        off += MAX_ID_LEN;
    bid.user_id  = readStr(src + off, MAX_ID_LEN);        off += MAX_ID_LEN;
    get(src, off, bid.amount);
    bid.bid_time = readStr(src + off, MAX_TIMESTAMP_LEN); off += MAX_TIMESTAMP_LEN;
    return bid;
}

} // namespace walnut
