// =============================================================================
// WALnut – WAL / Recovery Module (Kanak Rawat)
// src/wal/checkpointer.cpp
//
// Fuzzy checkpointer – writes a CHECKPOINT log record containing serialised
// ATT and DPT snapshots, then updates checkpoint_lsn in walnut.meta.
// =============================================================================

#include "walnut/wal/checkpointer.hpp"

#include <iostream>

namespace walnut {

// ===========================================================================
// Construction / Destruction
// ===========================================================================

Checkpointer::Checkpointer(LogManager& log_mgr, MetadataManager& meta_mgr,
                           std::chrono::seconds interval)
    : log_mgr_(log_mgr)
    , meta_mgr_(meta_mgr)
    , interval_(interval)
{}

Checkpointer::~Checkpointer() {
    Stop();
}

// ===========================================================================
// Start / Stop
// ===========================================================================

void Checkpointer::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;  // Already running.
    }

    worker_ = std::thread([this]() { WorkerLoop(); });
}

void Checkpointer::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;  // Not running.
    }

    // Wake the worker thread so it can exit.
    {
        std::lock_guard<std::mutex> lock(stop_mu_);
        stop_cv_.notify_all();
    }

    if (worker_.joinable()) {
        worker_.join();
    }
}

// ===========================================================================
// WorkerLoop
// ===========================================================================

void Checkpointer::WorkerLoop() {
    while (running_.load()) {
        // Wait for `interval_` or until Stop() is called.
        {
            std::unique_lock<std::mutex> lock(stop_mu_);
            stop_cv_.wait_for(lock, interval_, [this]() {
                return !running_.load();
            });
        }

        if (!running_.load()) break;

        try {
            TakeCheckpoint();
        } catch (const std::exception& e) {
            // Log and continue — don't crash the background thread.
            std::cerr << "Checkpointer – error: " << e.what() << std::endl;
        }
    }
}

// ===========================================================================
// TakeCheckpoint
// ===========================================================================

void Checkpointer::TakeCheckpoint() {
    // Build a CHECKPOINT log record.
    // In a full ARIES implementation, before_image would hold serialised ATT
    // and DPT.  For our simplified version, we write an empty checkpoint
    // record whose LSN serves as the starting point for recovery.
    LogRecord checkpoint;
    checkpoint.txn_id = 0;  // Checkpoint is not associated with a transaction.
    checkpoint.type   = LogRecordType::CHECKPOINT;

    // Append to the WAL.
    uint64_t lsn = log_mgr_.AppendLogRecord(checkpoint);

    // Flush the WAL to disk.
    log_mgr_.FlushAll();

    // Update the checkpoint_lsn in metadata.
    meta_mgr_.meta().checkpoint_lsn = static_cast<uint32_t>(lsn);
    meta_mgr_.save();

    checkpoint_count_.fetch_add(1);
}

// ===========================================================================
// CheckpointCount
// ===========================================================================

uint64_t Checkpointer::CheckpointCount() const {
    return checkpoint_count_.load();
}

} // namespace walnut
