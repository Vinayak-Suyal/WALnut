// =============================================================================
// WALnut – WAL / Recovery Module (Kanak Rawat)
// include/walnut/wal/checkpointer.hpp
//
// Fuzzy checkpointer – periodically writes ATT+DPT to the WAL and updates
// the checkpoint_lsn in walnut.meta so that recovery can start from a recent
// point instead of the very beginning of the WAL.
// =============================================================================

#pragma once

#include "walnut/wal/log_manager.hpp"
#include "walnut/storage/metadata.hpp"
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>

namespace walnut {

class Checkpointer {
public:
    /// Construct with references to the LogManager and MetadataManager.
    /// `interval` controls how often the background thread takes a checkpoint.
    Checkpointer(LogManager& log_mgr, MetadataManager& meta_mgr,
                 std::chrono::seconds interval = std::chrono::seconds(60));
    ~Checkpointer();

    // Non-copyable.
    Checkpointer(const Checkpointer&) = delete;
    Checkpointer& operator=(const Checkpointer&) = delete;

    /// Start the background checkpointing thread.
    void Start();

    /// Stop the background thread and join it.
    void Stop();

    /// Manually trigger a checkpoint (useful for testing).
    void TakeCheckpoint();

    /// Return the number of checkpoints taken so far.
    uint64_t CheckpointCount() const;

private:
    LogManager&      log_mgr_;
    MetadataManager& meta_mgr_;
    std::chrono::seconds interval_;
    std::thread      worker_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> checkpoint_count_{0};

    // Used to wake the worker thread early during Stop().
    std::mutex              stop_mu_;
    std::condition_variable stop_cv_;

    void WorkerLoop();
};

} // namespace walnut
