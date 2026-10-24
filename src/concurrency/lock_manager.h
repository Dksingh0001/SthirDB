#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "exec/value.h"

namespace sthir {

enum class LockMode {
  SHARED,
  EXCLUSIVE
};

const char* LockModeToString(LockMode mode);

struct LockKey {
  std::string table_name;
  uint32_t page_id = 0;
  uint16_t slot_id = 0;

  bool operator==(const LockKey& o) const {
    return page_id == o.page_id && slot_id == o.slot_id && table_name == o.table_name;
  }

  bool operator<(const LockKey& o) const {
    if (table_name != o.table_name) return table_name < o.table_name;
    if (page_id != o.page_id) return page_id < o.page_id;
    return slot_id < o.slot_id;
  }
};

struct LockKeyHash {
  std::size_t operator()(const LockKey& k) const {
    std::size_t h1 = std::hash<std::string>{}(k.table_name);
    std::size_t h2 = std::hash<uint32_t>{}(k.page_id);
    std::size_t h3 = std::hash<uint16_t>{}(k.slot_id);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
  }
};

class LockManager {
 public:
  explicit LockManager(uint32_t timeout_ms = 3000);
  ~LockManager();

  LockManager(const LockManager&) = delete;
  LockManager& operator=(const LockManager&) = delete;

  // Acquires a Shared (S) lock for txn_id on (table_name, rid).
  bool AcquireShared(uint32_t txn_id, const std::string& table_name, const RID& rid);

  // Acquires an Exclusive (X) lock for txn_id on (table_name, rid).
  bool AcquireExclusive(uint32_t txn_id, const std::string& table_name, const RID& rid);

  // Global lock ordering acquisition: sorts keys in ascending order to prevent deadlocks.
  bool AcquireExclusiveBatch(uint32_t txn_id, const std::string& table_name,
                             std::vector<RID> rids);

  // Releases a single lock held by txn_id.
  bool Release(uint32_t txn_id, const std::string& table_name, const RID& rid);

  // Strict 2PL: Releases ALL locks held by txn_id upon COMMIT or ABORT.
  void ReleaseAll(uint32_t txn_id);

  // Checks if txn_id holds an exclusive lock on (table_name, rid).
  bool HoldsExclusive(uint32_t txn_id, const std::string& table_name, const RID& rid) const;

  // Number of locks currently held across all transactions.
  size_t TotalLocksHeld() const;

 private:
  struct LockRequest {
    uint32_t txn_id = 0;
    LockMode mode = LockMode::SHARED;
    bool granted = false;
  };

  struct LockHead {
    std::mutex latch;
    std::condition_variable cv;
    LockMode current_mode = LockMode::SHARED;
    std::unordered_set<uint32_t> granted_txns;
    std::deque<std::shared_ptr<LockRequest>> wait_queue;
  };

  bool AcquireInternal(uint32_t txn_id, const LockKey& key, LockMode mode);

  uint32_t timeout_ms_;
  mutable std::mutex global_latch_;
  std::unordered_map<LockKey, std::shared_ptr<LockHead>, LockKeyHash> lock_table_;
  std::unordered_map<uint32_t, std::unordered_set<LockKey, LockKeyHash>> txn_locks_;
};

}  // namespace sthir
