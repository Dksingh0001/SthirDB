#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/buffer_pool.h"
#include "wal/log_record.h"
#include "wal/wal_manager.h"

namespace sthir {

enum class TxnStatus : uint32_t {
  RUNNING = 0,
  COMMITTED = 1,
  ABORTED = 2
};

struct TxnInfo {
  uint32_t txn_id = 0;
  TxnStatus status = TxnStatus::RUNNING;
  uint64_t last_lsn = 0;
};

struct RecoveryReport {
  size_t analysis_records_scanned = 0;
  size_t redo_records_applied = 0;
  size_t redo_records_skipped = 0;
  size_t undo_records_applied = 0;
  std::vector<uint32_t> loser_txns;
  uint64_t checkpoint_lsn = 0;
  uint64_t redo_start_lsn = 0;
  double recovery_time_ms = 0.0;
  uint64_t wal_size_bytes = 0;

  std::string ToString() const;
};

class RecoveryManager {
 public:
  RecoveryManager(WalManager* wal, BufferPool* bp);
  ~RecoveryManager() = default;

  // Transaction manager operations
  uint32_t Begin();
  bool Commit(uint32_t txn_id);
  bool Abort(uint32_t txn_id);

  // Called whenever a page modification (insert, update, delete) occurs
  uint64_t LogUpdate(uint32_t txn_id, uint32_t page_id, uint16_t slot_num,
                     uint16_t offset, std::vector<char> before_image,
                     std::vector<char> after_image);

  // Writes a checkpoint record to WAL with current Transaction Table and Dirty Page Table
  uint64_t Checkpoint(bool fuzzy = true);

  // ARIES Three-Pass Restart Recovery
  RecoveryReport Recover();

  // Inspect current tables (useful for tests and verification)
  std::unordered_map<uint32_t, TxnInfo> GetTransactionTable() const;
  std::unordered_map<uint32_t, uint64_t> GetDirtyPageTable() const;

  void SetBufferPool(BufferPool* bp) {
    std::lock_guard<std::mutex> lock(latch_);
    bp_ = bp;
  }

  void SetCheckpointInterval(size_t interval_records) {
    std::lock_guard<std::mutex> lock(latch_);
    checkpoint_interval_ = interval_records;
  }

  size_t checkpoint_interval() const {
    std::lock_guard<std::mutex> lock(latch_);
    return checkpoint_interval_;
  }

 private:
  void CheckIntervalTrigger();

  mutable std::mutex latch_;
  WalManager* wal_;
  BufferPool* bp_;

  uint32_t next_txn_id_ = 1;
  std::unordered_map<uint32_t, TxnInfo> txn_table_;
  std::unordered_map<uint32_t, uint64_t> dirty_page_table_;  // page_id -> rec_lsn

  size_t checkpoint_interval_ = 0;  // 0 = none/disabled
  size_t records_since_checkpoint_ = 0;
  uint64_t last_checkpoint_lsn_ = 0;
};

}  // namespace sthir
