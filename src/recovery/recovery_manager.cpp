#include "recovery/recovery_manager.h"

#include <algorithm>
#include <cstdio>
#include <queue>
#include <sstream>

#include "common/crash.h"

namespace sthir {

std::string RecoveryReport::ToString() const {
  std::ostringstream ss;
  ss << "RecoveryReport: "
     << "analysis_scanned=" << analysis_records_scanned
     << ", redo_applied=" << redo_records_applied
     << ", redo_skipped=" << redo_records_skipped
     << ", undo_applied=" << undo_records_applied
     << ", losers=" << loser_txns.size()
     << ", checkpoint_lsn=" << checkpoint_lsn
     << ", redo_start_lsn=" << redo_start_lsn
     << ", time_ms=" << recovery_time_ms;
  return ss.str();
}

RecoveryManager::RecoveryManager(WalManager* wal, BufferPool* bp)
    : wal_(wal), bp_(bp) {
  if (bp_ && wal_) {
    bp_->set_wal_manager(wal_);
  }
}

uint32_t RecoveryManager::Begin() {
  std::lock_guard<std::mutex> lock(latch_);
  uint32_t tid = next_txn_id_++;

  LogRecord rec = LogRecord::MakeBegin(tid);
  uint64_t lsn = wal_->AppendRecord(&rec);

  txn_table_[tid] = {tid, TxnStatus::RUNNING, lsn};

  records_since_checkpoint_++;
  CheckIntervalTrigger();

  return tid;
}

uint64_t RecoveryManager::LogUpdate(uint32_t txn_id, uint32_t page_id, uint16_t slot_num,
                                    uint16_t offset, std::vector<char> before_image,
                                    std::vector<char> after_image) {
  std::lock_guard<std::mutex> lock(latch_);

  uint64_t prev_lsn = 0;
  auto it = txn_table_.find(txn_id);
  if (it != txn_table_.end()) {
    prev_lsn = it->second.last_lsn;
  }

  LogRecord rec = LogRecord::MakeUpdate(txn_id, prev_lsn, page_id, slot_num, offset,
                                        std::move(before_image), std::move(after_image));
  uint64_t lsn = wal_->AppendRecord(&rec);

  txn_table_[txn_id] = {txn_id, TxnStatus::RUNNING, lsn};

  // Add to Dirty Page Table if not present
  if (dirty_page_table_.find(page_id) == dirty_page_table_.end()) {
    dirty_page_table_[page_id] = lsn;
  }

  records_since_checkpoint_++;
  CheckIntervalTrigger();

  return lsn;
}

bool RecoveryManager::Commit(uint32_t txn_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = txn_table_.find(txn_id);
  if (it == txn_table_.end() || it->second.status != TxnStatus::RUNNING) {
    return false;
  }

  uint64_t prev_lsn = it->second.last_lsn;
  LogRecord rec = LogRecord::MakeCommit(txn_id, prev_lsn);
  uint64_t commit_lsn = wal_->AppendRecord(&rec);

  it->second.status = TxnStatus::COMMITTED;
  it->second.last_lsn = commit_lsn;

  // Crash injection point: commit durable before dirty page is flushed to disk
  crash_point("after_page_before_commit");

  // Force log tail up to commit LSN (WAL durability guarantee)
  wal_->FlushUpTo(commit_lsn);

  crash_point("after_commit_before_page_flush");

  txn_table_.erase(it);

  records_since_checkpoint_++;
  CheckIntervalTrigger();

  return true;
}

bool RecoveryManager::Abort(uint32_t txn_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = txn_table_.find(txn_id);
  if (it == txn_table_.end()) {
    return false;
  }

  uint64_t cur_lsn = it->second.last_lsn;
  while (cur_lsn > 0) {
    LogRecord rec;
    if (!wal_->ReadRecord(cur_lsn, &rec)) {
      break;
    }

    if (rec.type == LogRecordType::UPDATE) {
      if (bp_) {
        Page* p = bp_->fetch_page(rec.page_id);
        if (p) {
          p->WriteTupleAt(rec.slot_num, rec.before_image.data(),
                          static_cast<uint16_t>(rec.before_image.size()));

          LogRecord clr = LogRecord::MakeCLR(txn_id, cur_lsn, rec.page_id, rec.slot_num,
                                             rec.offset, rec.before_image, rec.prev_lsn);
          uint64_t clr_lsn = wal_->AppendRecord(&clr);
          p->set_page_lsn(clr_lsn);
          bp_->unpin_page(rec.page_id, true);
        }
      }
      cur_lsn = rec.prev_lsn;
    } else if (rec.type == LogRecordType::CLR) {
      cur_lsn = rec.undo_nxt_lsn;
    } else {
      cur_lsn = rec.prev_lsn;
    }
  }

  LogRecord ab = LogRecord::MakeAbort(txn_id, it->second.last_lsn);
  uint64_t ab_lsn = wal_->AppendRecord(&ab);
  wal_->FlushUpTo(ab_lsn);

  txn_table_.erase(it);

  records_since_checkpoint_++;
  CheckIntervalTrigger();

  return true;
}

uint64_t RecoveryManager::Checkpoint(bool fuzzy) {
  std::lock_guard<std::mutex> lock(latch_);

  crash_point("mid_checkpoint");

  if (!fuzzy && bp_) {
    bp_->flush_all();
  }

  std::vector<TxnTableEntry> tt_snapshot;
  for (const auto& [tid, info] : txn_table_) {
    tt_snapshot.push_back({tid, static_cast<uint32_t>(info.status), info.last_lsn});
  }

  std::vector<DptTableEntry> dpt_snapshot;
  for (const auto& [pid, rlsn] : dirty_page_table_) {
    dpt_snapshot.push_back({pid, rlsn});
  }

  LogRecord cp = LogRecord::MakeCheckpoint(std::move(tt_snapshot), std::move(dpt_snapshot));
  uint64_t cp_lsn = wal_->AppendRecord(&cp);
  wal_->FlushUpTo(cp_lsn);

  last_checkpoint_lsn_ = cp_lsn;
  records_since_checkpoint_ = 0;
  return cp_lsn;
}

void RecoveryManager::CheckIntervalTrigger() {
  if (checkpoint_interval_ > 0 && records_since_checkpoint_ >= checkpoint_interval_) {
    // Release latch temporarily during checkpoint if needed, but here simple inline
    std::vector<TxnTableEntry> tt_snapshot;
    for (const auto& [tid, info] : txn_table_) {
      tt_snapshot.push_back({tid, static_cast<uint32_t>(info.status), info.last_lsn});
    }
    std::vector<DptTableEntry> dpt_snapshot;
    for (const auto& [pid, rlsn] : dirty_page_table_) {
      dpt_snapshot.push_back({pid, rlsn});
    }
    LogRecord cp = LogRecord::MakeCheckpoint(std::move(tt_snapshot), std::move(dpt_snapshot));
    uint64_t cp_lsn = wal_->AppendRecord(&cp);
    wal_->FlushUpTo(cp_lsn);
    last_checkpoint_lsn_ = cp_lsn;
    records_since_checkpoint_ = 0;
  }
}

RecoveryReport RecoveryManager::Recover() {
  std::lock_guard<std::mutex> lock(latch_);
  RecoveryReport report;
  const auto start_time = std::chrono::steady_clock::now();

  wal_->Flush();
  auto all_records = wal_->ReadAllRecords(1);
  if (all_records.empty()) {
    const auto end_time = std::chrono::steady_clock::now();
    report.recovery_time_ms =
        std::chrono::duration<double, std::milli>(end_time - start_time).count();
    return report;
  }

  // Find last checkpoint record
  int last_cp_idx = -1;
  for (int i = static_cast<int>(all_records.size()) - 1; i >= 0; --i) {
    if (all_records[i].type == LogRecordType::CHECKPOINT) {
      last_cp_idx = i;
      break;
    }
  }

  txn_table_.clear();
  dirty_page_table_.clear();

  size_t scan_start_idx = 0;
  if (last_cp_idx >= 0) {
    const auto& cp = all_records[last_cp_idx];
    report.checkpoint_lsn = cp.lsn;
    last_checkpoint_lsn_ = cp.lsn;
    for (const auto& te : cp.txn_table) {
      txn_table_[te.txn_id] = {te.txn_id, static_cast<TxnStatus>(te.status), te.last_lsn};
      if (te.txn_id >= next_txn_id_) next_txn_id_ = te.txn_id + 1;
    }
    for (const auto& de : cp.dpt_table) {
      dirty_page_table_[de.page_id] = de.rec_lsn;
    }
    scan_start_idx = static_cast<size_t>(last_cp_idx + 1);
  }

  // -------------------------------------------------------------
  // PASS 1: Analysis Pass
  // -------------------------------------------------------------
  for (size_t i = scan_start_idx; i < all_records.size(); ++i) {
    const auto& rec = all_records[i];
    report.analysis_records_scanned++;

    if (rec.txn_id >= next_txn_id_) {
      next_txn_id_ = rec.txn_id + 1;
    }

    switch (rec.type) {
      case LogRecordType::BEGIN:
        txn_table_[rec.txn_id] = {rec.txn_id, TxnStatus::RUNNING, rec.lsn};
        break;
      case LogRecordType::COMMIT:
        txn_table_[rec.txn_id] = {rec.txn_id, TxnStatus::COMMITTED, rec.lsn};
        break;
      case LogRecordType::ABORT:
        txn_table_[rec.txn_id] = {rec.txn_id, TxnStatus::ABORTED, rec.lsn};
        break;
      case LogRecordType::UPDATE:
      case LogRecordType::CLR:
        txn_table_[rec.txn_id] = {rec.txn_id, TxnStatus::RUNNING, rec.lsn};
        if (dirty_page_table_.find(rec.page_id) == dirty_page_table_.end()) {
          dirty_page_table_[rec.page_id] = rec.lsn;
        }
        break;
      default:
        break;
    }
  }

  // Collect Loser Transactions (active uncommitted transactions)
  std::vector<uint32_t> losers;
  for (const auto& [tid, info] : txn_table_) {
    if (info.status == TxnStatus::RUNNING) {
      losers.push_back(tid);
    }
  }
  report.loser_txns = losers;

  // Determine RedoLSN
  uint64_t redo_lsn = UINT64_MAX;
  for (const auto& [pid, rlsn] : dirty_page_table_) {
    if (rlsn < redo_lsn) redo_lsn = rlsn;
  }
  if (redo_lsn == UINT64_MAX) {
    redo_lsn = (report.checkpoint_lsn > 0) ? report.checkpoint_lsn : 1;
  }
  report.redo_start_lsn = redo_lsn;

  // -------------------------------------------------------------
  // PASS 2: Redo Pass (Repeat History from RedoLSN)
  // -------------------------------------------------------------
  for (const auto& rec : all_records) {
    if (rec.lsn < redo_lsn) continue;

    if (rec.type == LogRecordType::UPDATE || rec.type == LogRecordType::CLR) {
      auto dpt_it = dirty_page_table_.find(rec.page_id);
      if (dpt_it == dirty_page_table_.end() || rec.lsn < dpt_it->second) {
        report.redo_records_skipped++;
        continue;
      }

      if (bp_) {
        Page* p = bp_->fetch_page(rec.page_id);
        if (p) {
          if (p->page_lsn() >= rec.lsn) {
            // Already reflected on disk page (idempotent skip)
            bp_->unpin_page(rec.page_id, false);
            report.redo_records_skipped++;
          } else {
            // Reapply after-image
            p->WriteTupleAt(rec.slot_num, rec.after_image.data(),
                            static_cast<uint16_t>(rec.after_image.size()));
            p->set_page_lsn(rec.lsn);
            bp_->unpin_page(rec.page_id, true);
            report.redo_records_applied++;
          }
        }
      }
    }
  }

  // -------------------------------------------------------------
  // PASS 3: Undo Pass (Rollback Losers in reverse LSN order)
  // -------------------------------------------------------------
  std::priority_queue<uint64_t> to_undo;
  for (uint32_t loser_tid : losers) {
    auto it = txn_table_.find(loser_tid);
    if (it != txn_table_.end() && it->second.last_lsn > 0) {
      to_undo.push(it->second.last_lsn);
    }
  }

  while (!to_undo.empty()) {
    uint64_t cur_lsn = to_undo.top();
    to_undo.pop();

    LogRecord rec;
    if (!wal_->ReadRecord(cur_lsn, &rec)) {
      continue;
    }

    if (rec.type == LogRecordType::CLR) {
      crash_point("during_undo");
      if (rec.undo_nxt_lsn > 0) {
        to_undo.push(rec.undo_nxt_lsn);
      } else {
        // Loser txn is fully undone
        LogRecord ab = LogRecord::MakeAbort(rec.txn_id, rec.lsn);
        wal_->AppendRecord(&ab);
      }
    } else if (rec.type == LogRecordType::UPDATE) {
      crash_point("during_undo");
      if (bp_) {
        Page* p = bp_->fetch_page(rec.page_id);
        if (p) {
          // Apply before-image
          p->WriteTupleAt(rec.slot_num, rec.before_image.data(),
                          static_cast<uint16_t>(rec.before_image.size()));

          // Emit Compensation Log Record (CLR)
          LogRecord clr = LogRecord::MakeCLR(rec.txn_id, cur_lsn, rec.page_id, rec.slot_num,
                                             rec.offset, rec.before_image, rec.prev_lsn);
          uint64_t clr_lsn = wal_->AppendRecord(&clr);
          p->set_page_lsn(clr_lsn);
          bp_->unpin_page(rec.page_id, true);
          report.undo_records_applied++;
        }
      }

      if (rec.prev_lsn > 0) {
        to_undo.push(rec.prev_lsn);
      } else {
        // Loser txn is fully undone
        LogRecord ab = LogRecord::MakeAbort(rec.txn_id, cur_lsn);
        wal_->AppendRecord(&ab);
      }
    } else {
      if (rec.type == LogRecordType::BEGIN) {
        // Loser txn has reached its beginning and is fully undone
        LogRecord ab = LogRecord::MakeAbort(rec.txn_id, cur_lsn);
        wal_->AppendRecord(&ab);
      } else if (rec.prev_lsn > 0) {
        to_undo.push(rec.prev_lsn);
      }
    }
  }

  wal_->Flush();
  if (bp_) {
    bp_->flush_all();
  }

  // Clean loser txns from active table
  for (uint32_t loser_tid : losers) {
    txn_table_.erase(loser_tid);
  }

  crash_point("during_recovery");

  const auto end_time = std::chrono::steady_clock::now();
  report.recovery_time_ms =
      std::chrono::duration<double, std::milli>(end_time - start_time).count();

  return report;
}

std::unordered_map<uint32_t, TxnInfo> RecoveryManager::GetTransactionTable() const {
  std::lock_guard<std::mutex> lock(latch_);
  return txn_table_;
}

std::unordered_map<uint32_t, uint64_t> RecoveryManager::GetDirtyPageTable() const {
  std::lock_guard<std::mutex> lock(latch_);
  return dirty_page_table_;
}

}  // namespace sthir
