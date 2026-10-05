// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/common/types.hpp
//
// Logical data types for Users, Items, and Bids.
// Serialisation helpers are declared here and defined in src/common/types.cpp.
//
// String-field size limits (see constants.hpp):
//   user_id / item_id  : up to 63 chars + NUL  (MAX_ID_LEN   = 64)
//   username / title   : up to 127 chars + NUL  (MAX_NAME_LEN = 128)
//   bid_time           : up to 31 chars + NUL   (MAX_TIMESTAMP_LEN = 32)
//   status             : up to 7 chars + NUL    (ITEM_STATUS_LEN = 8)
//     Valid values: "OPEN", "CLOSED"
// =============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <stdexcept>

namespace walnut {

// ---------------------------------------------------------------------------
// Logical structs (in-memory representation)
// ---------------------------------------------------------------------------

struct User {
    std::string user_id;   ///< Primary key – must be unique, max 63 chars
    std::string username;  ///< Display name, max 127 chars
};

struct Item {
    std::string item_id;        ///< Primary key – must be unique, max 63 chars
    std::string title;          ///< Item title, max 127 chars
    double      starting_price; ///< Starting price (>= 0)
    std::string status;         ///< "OPEN" or "CLOSED"
    double      current_bid;    ///< Current highest bid (0.0 if none)
    std::string current_winner; ///< user_id of current winner, empty if none
};

struct Bid {
    uint64_t    bid_id;    ///< Monotonically increasing, assigned by StorageManager
    std::string item_id;   ///< Foreign key -> Item::item_id, max 63 chars
    std::string user_id;   ///< Foreign key -> User::user_id, max 63 chars
    double      amount;    ///< Bid amount (> 0)
    std::string bid_time;  ///< ISO-8601 UTC timestamp, max 31 chars
};

// ---------------------------------------------------------------------------
// Serialisation helpers
// Each function writes/reads exactly N_RECORD_SIZE bytes into/from a raw buffer.
// The buffer must be at least the corresponding *_RECORD_SIZE bytes in size.
// ---------------------------------------------------------------------------

/// Serialise a User into dst[0..USER_RECORD_SIZE).
/// Throws std::invalid_argument if any string field exceeds its limit.
void serialiseUser(const User& user, uint8_t* dst);

/// Deserialise a User from src[0..USER_RECORD_SIZE).
User deserialiseUser(const uint8_t* src);

/// Serialise an Item into dst[0..ITEM_RECORD_SIZE).
void serialiseItem(const Item& item, uint8_t* dst);

/// Deserialise an Item from src[0..ITEM_RECORD_SIZE).
Item deserialiseItem(const uint8_t* src);

/// Serialise a Bid into dst[0..BID_RECORD_SIZE).
void serialiseBid(const Bid& bid, uint8_t* dst);

/// Deserialise a Bid from src[0..BID_RECORD_SIZE).
Bid deserialiseBid(const uint8_t* src);

} // namespace walnut
