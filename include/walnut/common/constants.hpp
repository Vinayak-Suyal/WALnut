// WALnut – Schema v2.0 constants  (OSDBMS-V-2026-T038)
// See docs/schema-v2-specification.md §3 for full byte-offset tables.
#pragma once
#include <cstddef>
#include <cstdint>

namespace walnut {

// ---- Page ---------------------------------------------------------------
inline constexpr std::size_t PAGE_SIZE        = 4096;
inline constexpr std::size_t PAGE_HEADER_SIZE = 64;
inline constexpr std::size_t PAGE_DATA_SIZE   = PAGE_SIZE - PAGE_HEADER_SIZE; // 4032

// ---- Shared field widths ------------------------------------------------
inline constexpr std::size_t MAX_ID_LEN        = 64;  ///< user_id / item_id buffers
inline constexpr std::size_t MAX_NAME_LEN      = 128;
inline constexpr std::size_t MAX_TIMESTAMP_LEN = 32;

// ---- User record  (280 bytes) -------------------------------------------
// user_id(64) + username(64) + email(64) + phone(16) + password_hash(64) + role(8)
inline constexpr std::size_t USER_EMAIL_LEN         = 64;
inline constexpr std::size_t USER_PHONE_LEN         = 16;
inline constexpr std::size_t USER_PASSWORD_HASH_LEN = 64;
inline constexpr std::size_t USER_ROLE_LEN          = 8;
inline constexpr std::size_t USER_RECORD_SIZE =
    MAX_ID_LEN + MAX_ID_LEN + USER_EMAIL_LEN +
    USER_PHONE_LEN + USER_PASSWORD_HASH_LEN + USER_ROLE_LEN; // = 280

// ---- TopBid slot  (112 bytes, embedded 3× in Item) ----------------------
// bid_id(8) + user_id(64) + amount(8) + timestamp_ms(8) + reserved(24)
inline constexpr std::size_t TOP_BID_RESERVED_LEN = 24;
inline constexpr std::size_t TOP_BID_SLOT_SIZE =
    sizeof(uint64_t) + MAX_ID_LEN + sizeof(double) +
    sizeof(uint64_t) + TOP_BID_RESERVED_LEN; // = 112
inline constexpr std::size_t TOP_BID_COUNT = 3;

// ---- Item record  (800 bytes) -------------------------------------------
// item_id(48) + host_id(48) + title(96) + category(32) + condition(16)
// + image_urls[3](96) + starting_price(8) + status(8) + current_bid(8)
// + current_winner(64) + end_time_ms(8) + top_bid_count(4) + total_bid_count(4)
// + top_bids[3](336) + reserved(24)
inline constexpr std::size_t ITEM_ID_LEN          = 48;
inline constexpr std::size_t ITEM_HOST_ID_LEN     = 48;
inline constexpr std::size_t ITEM_TITLE_LEN       = 96;
inline constexpr std::size_t ITEM_CATEGORY_LEN    = 32;
inline constexpr std::size_t ITEM_CONDITION_LEN   = 16;
inline constexpr std::size_t ITEM_IMAGE_URL_LEN   = 32;
inline constexpr std::size_t ITEM_IMAGE_URL_COUNT = 3;
inline constexpr std::size_t ITEM_STATUS_LEN      = 8;
inline constexpr std::size_t ITEM_WINNER_LEN      = 64;
inline constexpr std::size_t ITEM_RESERVED_LEN    = 24;
inline constexpr std::size_t ITEM_RECORD_SIZE =
    ITEM_ID_LEN + ITEM_HOST_ID_LEN + ITEM_TITLE_LEN +
    ITEM_CATEGORY_LEN + ITEM_CONDITION_LEN +
    ITEM_IMAGE_URL_LEN * ITEM_IMAGE_URL_COUNT +
    sizeof(double) + ITEM_STATUS_LEN + sizeof(double) +
    ITEM_WINNER_LEN + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) +
    TOP_BID_SLOT_SIZE * TOP_BID_COUNT + ITEM_RESERVED_LEN; // = 800

// ---- WAL audit-trail Bid record  (176 bytes, unchanged from v1.0) --------
// bid_id(8) + item_id(64) + user_id(64) + amount(8) + bid_time(32)
inline constexpr std::size_t BID_RECORD_SIZE =
    sizeof(uint64_t) + MAX_ID_LEN + MAX_ID_LEN +
    sizeof(double) + MAX_TIMESTAMP_LEN; // = 176

// ---- Misc ---------------------------------------------------------------
inline constexpr std::size_t DEFAULT_BUFFER_POOL_SIZE = 64;
inline constexpr uint32_t    META_MAGIC               = 0x57414C4E; // 'WALN'
inline constexpr uint32_t    META_VERSION             = 2;
inline constexpr uint32_t    CHECKSUM_SEED            = 0xDEADBEEF;

// ---- Page types ---------------------------------------------------------
inline constexpr uint32_t PAGE_TYPE_INVALID  = 0;
inline constexpr uint32_t PAGE_TYPE_USERS    = 1;
inline constexpr uint32_t PAGE_TYPE_ITEMS    = 2;
inline constexpr uint32_t PAGE_TYPE_BIDS     = 3;
inline constexpr uint32_t PAGE_TYPE_METADATA = 4;
inline constexpr uint32_t PAGE_TYPE_FREE     = 5;

// ---- Compile-time sanity checks -----------------------------------------
static_assert(ITEM_RECORD_SIZE == 800, "Item must be exactly 800 bytes");
static_assert(USER_RECORD_SIZE == 280, "User must be exactly 280 bytes");
static_assert(TOP_BID_SLOT_SIZE == 112, "TopBid slot must be exactly 112 bytes");
static_assert(BID_RECORD_SIZE  == 176, "Bid must be exactly 176 bytes");
static_assert(ITEM_RECORD_SIZE % 8 == 0, "ITEM_RECORD_SIZE must be 8-byte aligned");
static_assert(USER_RECORD_SIZE % 8 == 0, "USER_RECORD_SIZE must be 8-byte aligned");
static_assert(TOP_BID_SLOT_SIZE % 8 == 0, "TOP_BID_SLOT_SIZE must be 8-byte aligned");
static_assert(BID_RECORD_SIZE  % 8 == 0, "BID_RECORD_SIZE must be 8-byte aligned");
static_assert(PAGE_DATA_SIZE / ITEM_RECORD_SIZE == 5,  "Must fit 5 items per page");
static_assert(PAGE_DATA_SIZE / USER_RECORD_SIZE == 14, "Must fit 14 users per page");

} // namespace walnut
