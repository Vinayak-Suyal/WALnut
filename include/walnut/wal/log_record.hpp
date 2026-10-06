// =============================================================================
// WALnut – WAL / Recovery Module (Kanak Rawat)
// include/walnut/wal/log_record.hpp
//
// Log record format for the Write-Ahead Log.
//
// Binary layout on disk (see project specification for full diagram):
//   [record_size: 4B] [lsn: 8B] [prev_lsn: 8B] [txn_id: 8B] [type: 1B]
//   [page_id: 4B] [offset: 2B] [length: 2B] [undo_next_lsn: 8B]
//   [before_image_len: 4B] [before_image: N B]
//   [after_image_len: 4B]  [after_image: M B]
//   [checksum: 4B]
//
// Total fixed overhead: 57 bytes + variable payload.
// =============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace walnut {

// ---------------------------------------------------------------------------
// Log record types
// ---------------------------------------------------------------------------
enum class LogRecordType : uint8_t {
    INVALID    = 0,
    BEGIN      = 1,   // Transaction started
    UPDATE     = 2,   // Page was modified (has before/after images)
    COMMIT     = 3,   // Transaction committed
    ABORT      = 4,   // Transaction aborted
    CLR        = 5,   // Compensation Log Record (undo of an UPDATE during recovery)
    CHECKPOINT = 6    // Fuzzy checkpoint
};

// ---------------------------------------------------------------------------
// LogRecord – in-memory representation of a single WAL entry.
// ---------------------------------------------------------------------------
struct LogRecord {
    // ── Fixed Header (logical, 40 bytes on disk) ──
    uint64_t        lsn            = 0;   // Log Sequence Number (unique, monotonic)
    uint64_t        prev_lsn       = 0;   // Previous LSN for SAME transaction
    uint64_t        txn_id         = 0;   // Transaction ID
    LogRecordType   type           = LogRecordType::INVALID;
    uint32_t        page_id        = 0;   // Page modified (UPDATE/CLR only)
    uint16_t        offset         = 0;   // Byte offset within the page data region
    uint16_t        length         = 0;   // Number of bytes modified

    // ── Variable-Length Payload ──
    std::vector<uint8_t> before_image;    // Old bytes (for UNDO)
    std::vector<uint8_t> after_image;     // New bytes (for REDO)

    // ── CLR-specific field ──
    uint64_t        undo_next_lsn  = 0;   // For CLR: next LSN to undo

    // ── Serialization ──

    /// Serialize this record to a byte vector for writing to walnut.wal.
    std::vector<uint8_t> Serialize() const;

    /// Deserialize a byte vector back into a LogRecord.
    /// `data` must point to the start of the record (past the record_size prefix).
    /// `len` is the number of bytes in the record body (== record_size value).
    static LogRecord Deserialize(const uint8_t* data, std::size_t len);

    /// Compute the total serialized size INCLUDING the 4-byte record_size prefix.
    std::size_t SerializedSize() const;
};

} // namespace walnut
