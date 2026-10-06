// =============================================================================
// WALnut – WAL / Recovery Module (Kanak Rawat)
// include/walnut/wal/log_manager.hpp
//
// LogManager – central WAL append/flush engine with group-commit support.
//
// Thread-safety: All public methods are safe to call from multiple threads.
// The buffer is protected by buffer_mu_, and flushing uses a
// condition-variable based group-commit protocol to avoid redundant I/O.
// =============================================================================

#pragma once

#include "walnut/wal/log_record.hpp"
#include <string>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <functional>

namespace walnut {

class LogManager {
public:
    /// Construct with the path to the WAL file (e.g. "./data/walnut.wal").
    explicit LogManager(const std::string& wal_path);
    ~LogManager();

    // Non-copyable.
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    // ── Core Operations ──

    /// Append a log record to the WAL buffer.  Returns the assigned LSN.
    /// This does NOT immediately write to disk (buffered for group commit).
    uint64_t AppendLogRecord(LogRecord record);

    /// Force-flush the WAL buffer to disk up to (at least) the given LSN.
    /// Uses group commit: if another thread is already flushing, this thread
    /// waits on a condition variable instead of doing redundant I/O.
    void Flush(uint64_t target_lsn);

    /// Force-flush ALL buffered records to disk.
    void FlushAll();

    // ── Queries ──

    /// Return the highest LSN that has been flushed to disk.
    uint64_t GetFlushedLSN() const;

    /// Return the highest LSN that has been appended (may not be flushed yet).
    uint64_t GetCurrentLSN() const;

    // ── Recovery Support ──

    /// Read all log records with LSN >= start_lsn from the WAL file.
    std::vector<LogRecord> ReadLogFrom(uint64_t start_lsn);

    /// Read the ENTIRE WAL file and return all valid records.
    std::vector<LogRecord> ReadAllRecords();

    /// Truncate the WAL after the given LSN (for post-recovery cleanup).
    void TruncateAfter(uint64_t lsn);

    // ── Transaction LSN Tracking ──

    /// Get the last LSN written by a specific transaction.
    /// Returns 0 if the transaction has no log records.
    uint64_t GetLastLSN(uint64_t txn_id) const;

    // ── Buffer Pool Hook ──

    /// Returns a lambda suitable for BufferPool::setPreFlushHook.
    /// The lambda enforces the WAL rule: flush WAL before allowing
    /// a dirty page to be written to disk.
    std::function<bool(uint32_t)> MakePreFlushHook();

    // ── Page LSN Tracking ──

    /// Track the highest LSN that modified a given page.
    void TrackPageLSN(uint32_t page_id, uint64_t lsn);

private:
    std::string         wal_path_;
    std::fstream        wal_file_;

    // WAL buffer (serialized records waiting to be flushed to disk).
    std::vector<uint8_t> buffer_;
    mutable std::mutex   buffer_mu_;

    // LSN management.
    std::atomic<uint64_t> current_lsn_{1};   // Next LSN to assign
    std::atomic<uint64_t> flushed_lsn_{0};   // Highest LSN flushed to disk

    // Group commit synchronization.
    std::condition_variable flush_cv_;
    bool                    flush_in_progress_ = false;

    // Per-transaction last LSN tracking.
    std::unordered_map<uint64_t, uint64_t> txn_last_lsn_;

    // Per-page LSN tracking (page_id → highest LSN that modified it).
    std::unordered_map<uint32_t, uint64_t> page_lsn_map_;

    // ── Internal Helpers ──
    void OpenWALFile();
    void WriteBufferToDisk(const std::vector<uint8_t>& data);

    /// Platform-specific fsync.
    void PlatformFsync();

    /// Read raw records from an input stream, returning all valid LogRecords.
    static std::vector<LogRecord> ParseRecords(std::istream& in);
};

} // namespace walnut
