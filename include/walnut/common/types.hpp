// WALnut – in-memory data types  (OSDBMS-V-2026-T038)
// Serialisation helpers defined in src/common/types.cpp.
// Binary layouts: docs/schema-v2-specification.md §3
//
// Compat contract (do NOT reorder first fields):
//   User{"id", "username"}               – 2-arg aggregate init
//   Item{"id","title",price,"OPEN",0.0,""} – 6-arg aggregate init
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <stdexcept>

namespace walnut {

// TopBid – embedded podium slot (112 bytes on disk, 3× in Item)
struct TopBid {
    uint64_t    bid_id       = 0;
    std::string user_id;
    double      amount       = 0.0;
    uint64_t    timestamp_ms = 0;
    // reserved(24) is zeroed by serialiser; not needed in logical struct
};

// User – 280 bytes on disk
struct User {
    std::string user_id;                    // v1 – position 0
    std::string username;                   // v1 – position 1
    std::string email;                      // v2
    std::string phone;                      // v2
    std::string password_hash;              // v2
    std::string role = "USER";              // v2  "USER" (reserved/admin metadata; host vs bidder derived per-lot via item.host_id)
};

// Item – 800 bytes on disk
struct Item {
    std::string item_id;                    // v1 – position 0
    std::string title;                      // v1 – position 1
    double      starting_price  = 0.0;     // v1 – position 2
    std::string status          = "OPEN";  // v1 – position 3  OPEN|CLOSED|UNSOLD|CANCEL
    double      current_bid     = 0.0;     // v1 – position 4
    std::string current_winner;            // v1 – position 5
    std::string host_id;                   // v2  Anti-Shill Guard key
    std::string category;                  // v2
    std::string condition       = "NEW";   // v2  NEW|LIKE_NEW|GOOD|FAIR
    std::array<std::string, 3> image_urls = {"", "", ""}; // v2
    uint64_t    end_time_ms     = 0;       // v2  Unix epoch ms (0 = no timer)
    uint32_t    top_bid_count   = 0;       // v2  occupied podium slots 0..3
    uint32_t    total_bid_count = 0;       // v2  lifetime bid count (uncapped)
    std::array<TopBid, 3> top_bids = {};   // v2  descending podium
};

// Bid – WAL audit-trail record (176 bytes on disk, PAGE_TYPE_BIDS)
// SEPARATE from TopBid slots embedded in Item. Unchanged from v1.
struct Bid {
    uint64_t    bid_id   = 0;
    std::string item_id;
    std::string user_id;
    double      amount   = 0.0;
    std::string bid_time;
};

// Serialisation helpers (raw buffer ↔ struct).
// Buffer must be exactly *_RECORD_SIZE bytes (from constants.hpp).
// Throws std::invalid_argument on field-length violations.
void serialiseUser  (const User&   u,  uint8_t* dst);
User deserialiseUser(const uint8_t* src);

void serialiseItem  (const Item&   i,  uint8_t* dst);
Item deserialiseItem(const uint8_t* src);

void   serialiseTopBid(const TopBid& tb, uint8_t* dst);
TopBid deserialiseTopBid(const uint8_t* src);

void serialiseBid  (const Bid&    b,  uint8_t* dst);
Bid  deserialiseBid(const uint8_t* src);

} // namespace walnut
