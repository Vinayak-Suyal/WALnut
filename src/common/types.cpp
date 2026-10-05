// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// src/common/types.cpp
//
// Serialisation / deserialisation for User, Item, Bid.
// All records are fixed-size; see constants.hpp for size definitions.
// =============================================================================

#include "walnut/common/types.hpp"
#include "walnut/common/constants.hpp"
#include "walnut/common/utils.hpp"
#include <cstring>
#include <stdexcept>

namespace walnut {

// ---------------------------------------------------------------------------
// Helper: checked string copy into a fixed-width char array
// ---------------------------------------------------------------------------
static void checkAndCopyStr(const std::string& src, uint8_t* dst,
                             std::size_t max_len, const char* field_name) {
    if (src.size() >= max_len) {
        throw std::invalid_argument(
            std::string(field_name) + " exceeds max length " +
            std::to_string(max_len - 1));
    }
    utils::safeCopyString(src, reinterpret_cast<char*>(dst), max_len);
}

// ---------------------------------------------------------------------------
// User serialisation  (USER_RECORD_SIZE bytes)
//
// Layout:
//   [0  .. 63]  user_id  (MAX_ID_LEN = 64, NUL-terminated)
//   [64 .. 191] username (MAX_NAME_LEN = 128, NUL-terminated)
// ---------------------------------------------------------------------------
void serialiseUser(const User& user, uint8_t* dst) {
    checkAndCopyStr(user.user_id,  dst,                       MAX_ID_LEN,   "user_id");
    checkAndCopyStr(user.username, dst + MAX_ID_LEN,          MAX_NAME_LEN, "username");
}

User deserialiseUser(const uint8_t* src) {
    User u;
    u.user_id  = utils::readCString(reinterpret_cast<const char*>(src),              MAX_ID_LEN);
    u.username = utils::readCString(reinterpret_cast<const char*>(src + MAX_ID_LEN), MAX_NAME_LEN);
    return u;
}

// ---------------------------------------------------------------------------
// Item serialisation  (ITEM_RECORD_SIZE bytes)
//
// Layout:
//   [0   .. 63 ]  item_id        (MAX_ID_LEN = 64)
//   [64  .. 191]  title          (MAX_NAME_LEN = 128)
//   [192 .. 199]  starting_price (double, 8 bytes)
//   [200 .. 207]  status         (ITEM_STATUS_LEN = 8, NUL-terminated: "OPEN" or "CLOSED")
//   [208 .. 215]  current_bid    (double, 8 bytes)
//   [216 .. 279]  current_winner (MAX_ID_LEN = 64)
//   Total = 280 bytes
// ---------------------------------------------------------------------------
static_assert(ITEM_RECORD_SIZE == 64 + 128 + 8 + 8 + 8 + 64,
              "ITEM_RECORD_SIZE mismatch");

void serialiseItem(const Item& item, uint8_t* dst) {
    std::size_t off = 0;

    checkAndCopyStr(item.item_id, dst + off, MAX_ID_LEN, "item_id");
    off += MAX_ID_LEN;

    checkAndCopyStr(item.title, dst + off, MAX_NAME_LEN, "title");
    off += MAX_NAME_LEN;

    std::memcpy(dst + off, &item.starting_price, sizeof(double));
    off += sizeof(double);

    checkAndCopyStr(item.status, dst + off, ITEM_STATUS_LEN, "status");
    off += ITEM_STATUS_LEN;

    std::memcpy(dst + off, &item.current_bid, sizeof(double));
    off += sizeof(double);

    checkAndCopyStr(item.current_winner, dst + off, MAX_ID_LEN, "current_winner");
    off += MAX_ID_LEN;

    // Sanity check
    if (off != ITEM_RECORD_SIZE) {
        throw std::logic_error("serialiseItem: offset mismatch");
    }
}

Item deserialiseItem(const uint8_t* src) {
    Item item;
    std::size_t off = 0;

    item.item_id = utils::readCString(reinterpret_cast<const char*>(src + off), MAX_ID_LEN);
    off += MAX_ID_LEN;

    item.title = utils::readCString(reinterpret_cast<const char*>(src + off), MAX_NAME_LEN);
    off += MAX_NAME_LEN;

    std::memcpy(&item.starting_price, src + off, sizeof(double));
    off += sizeof(double);

    item.status = utils::readCString(reinterpret_cast<const char*>(src + off), ITEM_STATUS_LEN);
    off += ITEM_STATUS_LEN;

    std::memcpy(&item.current_bid, src + off, sizeof(double));
    off += sizeof(double);

    item.current_winner = utils::readCString(reinterpret_cast<const char*>(src + off), MAX_ID_LEN);
    off += MAX_ID_LEN;

    return item;
}

// ---------------------------------------------------------------------------
// Bid serialisation  (BID_RECORD_SIZE bytes)
//
// Layout:
//   [0  .. 7 ]  bid_id   (uint64_t, 8 bytes)
//   [8  .. 71]  item_id  (MAX_ID_LEN = 64)
//   [72 .. 135] user_id  (MAX_ID_LEN = 64)
//   [136.. 143] amount   (double, 8 bytes)
//   [144.. 175] bid_time (MAX_TIMESTAMP_LEN = 32)
//   Total = 176 bytes
// ---------------------------------------------------------------------------
static_assert(BID_RECORD_SIZE == 8 + 64 + 64 + 8 + 32,
              "BID_RECORD_SIZE mismatch");

void serialiseBid(const Bid& bid, uint8_t* dst) {
    std::size_t off = 0;

    std::memcpy(dst + off, &bid.bid_id, sizeof(uint64_t));
    off += sizeof(uint64_t);

    checkAndCopyStr(bid.item_id, dst + off, MAX_ID_LEN, "item_id");
    off += MAX_ID_LEN;

    checkAndCopyStr(bid.user_id, dst + off, MAX_ID_LEN, "user_id");
    off += MAX_ID_LEN;

    std::memcpy(dst + off, &bid.amount, sizeof(double));
    off += sizeof(double);

    checkAndCopyStr(bid.bid_time, dst + off, MAX_TIMESTAMP_LEN, "bid_time");
    off += MAX_TIMESTAMP_LEN;

    if (off != BID_RECORD_SIZE) {
        throw std::logic_error("serialiseBid: offset mismatch");
    }
}

Bid deserialiseBid(const uint8_t* src) {
    Bid bid;
    std::size_t off = 0;

    std::memcpy(&bid.bid_id, src + off, sizeof(uint64_t));
    off += sizeof(uint64_t);

    bid.item_id = utils::readCString(reinterpret_cast<const char*>(src + off), MAX_ID_LEN);
    off += MAX_ID_LEN;

    bid.user_id = utils::readCString(reinterpret_cast<const char*>(src + off), MAX_ID_LEN);
    off += MAX_ID_LEN;

    std::memcpy(&bid.amount, src + off, sizeof(double));
    off += sizeof(double);

    bid.bid_time = utils::readCString(reinterpret_cast<const char*>(src + off), MAX_TIMESTAMP_LEN);
    off += MAX_TIMESTAMP_LEN;

    return bid;
}

} // namespace walnut
