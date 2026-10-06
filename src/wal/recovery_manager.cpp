// =============================================================================
// WALnut – WAL / Recovery Module (Kanak Rawat)
// src/wal/recovery_manager.cpp
//
// ARIES crash recovery implementation:
//   1. Analysis phase  – rebuild ATT and DPT from the WAL.
//   2. Redo phase      – replay committed modifications.
//   3. Undo phase      – roll back uncommitted (loser) transactions.
// =============================================================================

#include "walnut/wal/recovery_manager.hpp"
#include "walnut/storage/page.hpp"
#include "walnut/common/constants.hpp"

#include <algorithm>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <iostream>
#include <climits>

namespace walnut {

// ===========================================================================
// Construction
// ===========================================================================

RecoveryManager::RecoveryManager(LogManager& log_mgr, FileManager& file_mgr,
                                 MetadataManager& meta_mgr)
    : log_mgr_(log_mgr)
    , file_mgr_(file_mgr)
    , meta_mgr_(meta_mgr)
{}

// ===========================================================================
// Recover – orchestrate the three ARIES phases
// ===========================================================================

void RecoveryManager::Recover() {
    // Read all records from the WAL.
    auto log = log_mgr_.ReadAllRecords();
    if (log.empty()) {
        // Nothing to recover.
        return;
    }

    AnalysisPhase(log);
    RedoPhase(log);
    UndoPhase(log);
}

RecoveryManager::RecoveryStats RecoveryManager::GetStats() const {
    return stats_;
}

// ===========================================================================
// Analysis Phase
// ===========================================================================

void RecoveryManager::AnalysisPhase(const std::vector<LogRecord>& log) {
    uint64_t checkpoint_lsn = static_cast<uint64_t>(meta_mgr_.meta().checkpoint_lsn);

    for (const auto& rec : log) {
        if (rec.lsn < checkpoint_lsn) continue;

        stats_.records_analyzed++;

        switch (rec.type) {
        case LogRecordType::BEGIN:
            att_[rec.txn_id] = rec.lsn;
            break;

        case LogRecordType::UPDATE:
        case LogRecordType::CLR:
            // Update the transaction's last LSN in the ATT.
            att_[rec.txn_id] = rec.lsn;
            // If this page is not yet in the DPT, add it with this LSN.
            if (dpt_.find(rec.page_id) == dpt_.end()) {
                dpt_[rec.page_id] = rec.lsn;
            }
            break;

        case LogRecordType::COMMIT:
            att_.erase(rec.txn_id);
            break;

        case LogRecordType::ABORT:
            att_.erase(rec.txn_id);
            break;

        case LogRecordType::CHECKPOINT:
            // In our simplified implementation we rebuild ATT/DPT from
            // the scan, so we just note it and continue.
            break;

        case LogRecordType::INVALID:
        default:
            break;
        }
    }

    stats_.loser_txn_count = att_.size();
}

// ===========================================================================
// Redo Phase
// ===========================================================================

void RecoveryManager::RedoPhase(const std::vector<LogRecord>& log) {
    // Find the smallest recovery_lsn in the DPT.
    uint64_t min_lsn = UINT64_MAX;
    for (const auto& [page_id, recovery_lsn] : dpt_) {
        min_lsn = std::min(min_lsn, recovery_lsn);
    }

    if (min_lsn == UINT64_MAX) {
        return;  // No dirty pages — nothing to redo.
    }

    for (const auto& rec : log) {
        if (rec.lsn < min_lsn) continue;

        if (rec.type != LogRecordType::UPDATE && rec.type != LogRecordType::CLR) {
            continue;  // Only redo data modifications.
        }

        // Check if this page is in the DPT.
        auto dpt_it = dpt_.find(rec.page_id);
        if (dpt_it == dpt_.end()) {
            continue;  // Page was flushed after this modification.
        }

        // Check if the DPT's recovery_lsn is > record.lsn.
        if (dpt_it->second > rec.lsn) {
            continue;  // This modification was already on disk.
        }

        // Read the page from disk and compare.
        try {
            Page page = file_mgr_.readPage(rec.page_id);

            // Bounds check: make sure offset + length fits within the page.
            if (rec.offset + rec.length > PAGE_DATA_SIZE) {
                continue;  // Skip malformed record.
            }
            if (rec.after_image.size() != rec.length) {
                continue;  // Skip inconsistent record.
            }

            // Compare the page data at the offset with the after_image.
            const uint8_t* page_data = page.dataRegion() + rec.offset;
            if (std::memcmp(page_data, rec.after_image.data(), rec.length) == 0) {
                continue;  // Already applied. Skip.
            }

            // REDO: Apply the after_image.
            ApplyRedo(rec);
            stats_.records_redone++;
        } catch (const std::exception&) {
            // Page might not exist yet (allocated but never written).
            // In that case, we can't redo — skip.
            continue;
        }
    }
}

// ===========================================================================
// Undo Phase
// ===========================================================================

void RecoveryManager::UndoPhase(const std::vector<LogRecord>& log) {
    if (att_.empty()) {
        return;  // No loser transactions.
    }

    // Build a map of LSN → LogRecord for quick lookup.
    std::unordered_map<uint64_t, const LogRecord*> lsn_to_record;
    for (const auto& rec : log) {
        lsn_to_record[rec.lsn] = &rec;
    }

    // Build a max-heap of (lsn, txn_id) for all loser transactions.
    using UndoEntry = std::pair<uint64_t, uint64_t>;  // (lsn, txn_id)
    std::priority_queue<UndoEntry> undo_list;

    for (const auto& [txn_id, last_lsn] : att_) {
        undo_list.push({last_lsn, txn_id});
    }

    while (!undo_list.empty()) {
        auto [lsn, txn_id] = undo_list.top();
        undo_list.pop();

        auto it = lsn_to_record.find(lsn);
        if (it == lsn_to_record.end()) {
            continue;  // Record not found — skip.
        }

        const LogRecord& record = *(it->second);

        if (record.type == LogRecordType::CLR) {
            // Follow undo_next_lsn pointer.
            if (record.undo_next_lsn != 0) {
                undo_list.push({record.undo_next_lsn, txn_id});
            }
            // else: this transaction is fully undone.

        } else if (record.type == LogRecordType::UPDATE) {
            // UNDO this update by applying the before_image.
            ApplyUndo(record);
            stats_.records_undone++;

            // Write a CLR record to the WAL.
            LogRecord clr;
            clr.txn_id        = txn_id;
            clr.type           = LogRecordType::CLR;
            clr.page_id        = record.page_id;
            clr.offset         = record.offset;
            clr.length         = record.length;
            clr.before_image   = record.after_image;   // CLR's "before" = what we're undoing
            clr.after_image    = record.before_image;   // CLR's "after"  = the restored data
            clr.undo_next_lsn  = record.prev_lsn;      // Next LSN to undo for this txn

            log_mgr_.AppendLogRecord(clr);
            log_mgr_.FlushAll();
            stats_.clrs_written++;

            // Follow the prev_lsn chain.
            if (record.prev_lsn != 0) {
                undo_list.push({record.prev_lsn, txn_id});
            }

        } else if (record.type == LogRecordType::BEGIN) {
            // Reached the beginning of this transaction.
            // Write an ABORT record.
            LogRecord abort_rec;
            abort_rec.txn_id = txn_id;
            abort_rec.type   = LogRecordType::ABORT;
            log_mgr_.AppendLogRecord(abort_rec);
            log_mgr_.FlushAll();
        }
        // Other record types (COMMIT, ABORT, CHECKPOINT) are ignored in undo.
    }
}

// ===========================================================================
// ApplyRedo / ApplyUndo
// ===========================================================================

void RecoveryManager::ApplyRedo(const LogRecord& record) {
    Page page = file_mgr_.readPage(record.page_id);
    std::memcpy(page.dataRegion() + record.offset,
                record.after_image.data(),
                record.length);
    page.updateChecksum();
    file_mgr_.writePage(page);
}

void RecoveryManager::ApplyUndo(const LogRecord& record) {
    Page page = file_mgr_.readPage(record.page_id);
    std::memcpy(page.dataRegion() + record.offset,
                record.before_image.data(),
                record.length);
    page.updateChecksum();
    file_mgr_.writePage(page);
}

} // namespace walnut
