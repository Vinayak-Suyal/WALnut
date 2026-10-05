// =============================================================================
// WALnut – Storage / Buffer Module (Vinayak Suyal)
// include/walnut/common/constants.hpp
//
// Central location for all compile-time constants.  Change PAGE_SIZE here and
// every component picks it up automatically.
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace walnut {

// ---------------------------------------------------------------------------
// Page constants
// ---------------------------------------------------------------------------

/// Size of a single page on disk and in memory (bytes).
inline constexpr std::size_t PAGE_SIZE = 4096;

/// Fixed size of the PageHeader structure (bytes).
inline constexpr std::size_t PAGE_HEADER_SIZE = 64;

/// Maximum bytes available for record data on a page.
inline constexpr std::size_t PAGE_DATA_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;

// ---------------------------------------------------------------------------
// Record field limits (fixed-size layout, documented in storage-format.md)
// ---------------------------------------------------------------------------

/// Max bytes for a user_id / item_id string (null-terminated in buffer).
inline constexpr std::size_t MAX_ID_LEN       = 64;

/// Max bytes for a username / title string (null-terminated in buffer).
inline constexpr std::size_t MAX_NAME_LEN     = 128;

/// Max bytes for bid_time ISO-8601 string (null-terminated in buffer).
inline constexpr std::size_t MAX_TIMESTAMP_LEN = 32;

/// Fixed size of a serialised User record (bytes).
///   user_id   : MAX_ID_LEN   (64)
///   username  : MAX_NAME_LEN (128)
///   Total     : 192 bytes
inline constexpr std::size_t USER_RECORD_SIZE = MAX_ID_LEN + MAX_NAME_LEN;

/// Fixed size of a serialised Item record (bytes).
///   item_id        : MAX_ID_LEN     (64)
///   title          : MAX_NAME_LEN   (128)
///   starting_price : 8 (double)
///   status         : 8 ("OPEN\0\0\0\0" or "CLOSED\0\0")
///   current_bid    : 8 (double)
///   current_winner : MAX_ID_LEN     (64)
///   Total          : 280 bytes
inline constexpr std::size_t ITEM_STATUS_LEN  = 8;
inline constexpr std::size_t ITEM_RECORD_SIZE =
    MAX_ID_LEN + MAX_NAME_LEN +
    sizeof(double) + ITEM_STATUS_LEN +
    sizeof(double) + MAX_ID_LEN;

/// Fixed size of a serialised Bid record (bytes).
///   bid_id   : 8 (uint64_t)
///   item_id  : MAX_ID_LEN       (64)
///   user_id  : MAX_ID_LEN       (64)
///   amount   : 8 (double)
///   bid_time : MAX_TIMESTAMP_LEN (32)
///   Total    : 176 bytes
inline constexpr std::size_t BID_RECORD_SIZE =
    sizeof(uint64_t) + MAX_ID_LEN + MAX_ID_LEN +
    sizeof(double) + MAX_TIMESTAMP_LEN;

// ---------------------------------------------------------------------------
// Buffer pool defaults
// ---------------------------------------------------------------------------

/// Default number of frames in the buffer pool.
inline constexpr std::size_t DEFAULT_BUFFER_POOL_SIZE = 64;

// ---------------------------------------------------------------------------
// Metadata / page-type constants
// ---------------------------------------------------------------------------

/// Magic number written at byte 0 of walnut.meta for format verification.
inline constexpr uint32_t META_MAGIC   = 0x57414C4E; // 'WALN'
inline constexpr uint32_t META_VERSION = 1;

/// Simple XOR/sum seed used by the page checksum function.
inline constexpr uint32_t CHECKSUM_SEED = 0xDEADBEEF;

// ---------------------------------------------------------------------------
// Page types (enum-equivalent, kept as uint32_t for binary compatibility)
// ---------------------------------------------------------------------------

inline constexpr uint32_t PAGE_TYPE_INVALID  = 0;
inline constexpr uint32_t PAGE_TYPE_USERS    = 1;
inline constexpr uint32_t PAGE_TYPE_ITEMS    = 2;
inline constexpr uint32_t PAGE_TYPE_BIDS     = 3;
inline constexpr uint32_t PAGE_TYPE_METADATA = 4;
inline constexpr uint32_t PAGE_TYPE_FREE     = 5;

} // namespace walnut
