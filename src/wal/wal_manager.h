#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "wal/log_record.h"

namespace sthir {

class WalManager {
 public:
  explicit WalManager(std::string wal_path = "./data/retail/wal.log");
  ~WalManager();

  WalManager(const WalManager&) = delete;
  WalManager& operator=(const WalManager&) = delete;

  // Appends a log record. Assigns monotonic LSN (byte offset) and returns it.
  uint64_t AppendRecord(LogRecord* record);

  // Flushes the log to disk up to at least `lsn`. If lsn == 0, flushes everything pending.
  void FlushUpTo(uint64_t lsn);

  // Convenience flush for all buffered writes.
  void Flush() { FlushUpTo(0); }

  uint64_t flushed_lsn() const {
    std::lock_guard<std::mutex> lock(latch_);
    return flushed_lsn_;
  }

  uint64_t next_lsn() const {
    std::lock_guard<std::mutex> lock(latch_);
    return next_lsn_;
  }

  // Reads a single log record at the given LSN offset. Returns false if invalid/EOF.
  bool ReadRecord(uint64_t lsn, LogRecord* out);

  // Reads all log records from `start_lsn` up to the end of the log.
  std::vector<LogRecord> ReadAllRecords(uint64_t start_lsn = 0);

  // Truncates / resets the WAL file.
  void Clear();

  const std::string& wal_path() const { return wal_path_; }

 private:
  std::string wal_path_;
  int fd_ = -1;
  mutable std::mutex latch_;

  uint64_t next_lsn_ = 1;     // Start at 1 (0 is reserved for invalid/null LSN)
  uint64_t flushed_lsn_ = 0;  // Up to which LSN is durable on disk
  std::vector<char> buffer_;  // In-memory buffer before flushing
  uint64_t buffer_start_lsn_ = 1;
};

}  // namespace sthir
