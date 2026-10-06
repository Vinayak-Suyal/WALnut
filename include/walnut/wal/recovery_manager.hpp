// =============================================================================
// WALnut – WAL / Recovery Module (Kanak Rawat)
// include/walnut/wal/recovery_manager.hpp
//
// ARIES-style crash recovery: Analysis → Redo → Undo.
// =============================================================================

#pragma once

#include "walnut/wal/log_manager.hpp"
#include "walnut/storage/file_manager.hpp"
#include "walnut/storage/metadata.hpp"
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace walnut {

class RecoveryManager {
public:
    RecoveryManager(LogManager& log_mgr, FileManager& file_mgr,
                    MetadataManager& meta_mgr);

    /// Run the full ARIES recovery protocol.
    /// Should be called at engine startup if wasCleanShutdown() == false.
    void Recover();

    // ── Statistics (for testing and debugging) ──
    struct RecoveryStats {
        uint64_t    records_analyzed = 0;
        uint64_t    records_redone   = 0;
        uint64_t    records_undone   = 0;
        uint64_t    clrs_written     = 0;
        std::size_t loser_txn_count  = 0;
    };
    RecoveryStats GetStats() const;

private:
    LogManager&      log_mgr_;
    FileManager&     file_mgr_;
    MetadataManager& meta_mgr_;
    RecoveryStats    stats_;

    // ── Active Transaction Table (ATT) ──
    // txn_id → last_lsn: transactions active at crash time.
    std::unordered_map<uint64_t, uint64_t> att_;

    // ── Dirty Page Table (DPT) ──
    // page_id → recovery_lsn: earliest LSN that dirtied the page.
    std::unordered_map<uint32_t, uint64_t> dpt_;

    // ── The Three Phases of ARIES ──
    void AnalysisPhase(const std::vector<LogRecord>& log);
    void RedoPhase(const std::vector<LogRecord>& log);
    void UndoPhase(const std::vector<LogRecord>& log);

    // ── Helpers ──
    void ApplyRedo(const LogRecord& record);
    void ApplyUndo(const LogRecord& record);
};

} // namespace walnut
