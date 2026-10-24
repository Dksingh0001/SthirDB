#include "concurrency/lock_manager.h"

#include <algorithm>

namespace sthir {

const char* LockModeToString(LockMode mode) {
  switch (mode) {
    case LockMode::SHARED: return "SHARED";
    case LockMode::EXCLUSIVE: return "EXCLUSIVE";
  }
  return "UNKNOWN";
}

LockManager::LockManager(uint32_t timeout_ms) : timeout_ms_(timeout_ms) {}

LockManager::~LockManager() {
  std::lock_guard<std::mutex> lock(global_latch_);
  lock_table_.clear();
  txn_locks_.clear();
}

bool LockManager::AcquireShared(uint32_t txn_id, const std::string& table_name, const RID& rid) {
  if (txn_id == 0 || !rid.is_valid) return true;
  LockKey key{table_name, rid.page_id, rid.slot_id};
  return AcquireInternal(txn_id, key, LockMode::SHARED);
}

bool LockManager::AcquireExclusive(uint32_t txn_id, const std::string& table_name, const RID& rid) {
  if (txn_id == 0 || !rid.is_valid) return true;
  LockKey key{table_name, rid.page_id, rid.slot_id};
  return AcquireInternal(txn_id, key, LockMode::EXCLUSIVE);
}

bool LockManager::AcquireExclusiveBatch(uint32_t txn_id, const std::string& table_name,
                                       std::vector<RID> rids) {
  if (txn_id == 0 || rids.empty()) return true;

  // Global Lock Ordering: sort keys ascending by (page_id, slot_id)
  std::sort(rids.begin(), rids.end(), [](const RID& a, const RID& b) {
    if (a.page_id != b.page_id) return a.page_id < b.page_id;
    return a.slot_id < b.slot_id;
  });
  rids.erase(std::unique(rids.begin(), rids.end(), [](const RID& a, const RID& b) {
    return a.page_id == b.page_id && a.slot_id == b.slot_id;
  }), rids.end());

  // Acquire sequentially according to the global order
  for (const auto& rid : rids) {
    if (!AcquireExclusive(txn_id, table_name, rid)) {
      return false;
    }
  }
  return true;
}

bool LockManager::AcquireInternal(uint32_t txn_id, const LockKey& key, LockMode mode) {
  std::shared_ptr<LockHead> head;
  {
    std::lock_guard<std::mutex> g_lock(global_latch_);
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) {
      head = std::make_shared<LockHead>();
      lock_table_[key] = head;
    } else {
      head = it->second;
    }
  }

  std::unique_lock<std::mutex> lk(head->latch);

  // Check if this transaction already holds the lock
  if (head->granted_txns.count(txn_id)) {
    if (head->current_mode == LockMode::EXCLUSIVE) {
      return true;  // Already holds exclusive
    }
    if (mode == LockMode::SHARED) {
      return true;  // Already holds shared
    }
    // Upgrade from SHARED to EXCLUSIVE
    if (head->granted_txns.size() == 1) {
      head->current_mode = LockMode::EXCLUSIVE;
      return true;
    }
    // Cannot immediately upgrade if other shared holders exist
  }

  // Check if we can grant immediately without waiting (FIFO check: wait queue must be empty)
  bool can_grant_immediately = false;
  if (head->wait_queue.empty()) {
    if (head->granted_txns.empty()) {
      can_grant_immediately = true;
    } else if (mode == LockMode::SHARED && head->current_mode == LockMode::SHARED) {
      can_grant_immediately = true;
    }
  }

  if (can_grant_immediately) {
    head->current_mode = mode;
    head->granted_txns.insert(txn_id);
    {
      std::lock_guard<std::mutex> g_lock(global_latch_);
      txn_locks_[txn_id].insert(key);
    }
    return true;
  }

  // Enqueue wait request (FIFO queue)
  auto req = std::make_shared<LockRequest>();
  req->txn_id = txn_id;
  req->mode = mode;
  req->granted = false;
  head->wait_queue.push_back(req);

  // Wait with timeout for deadlock avoidance
  bool acquired = head->cv.wait_for(
      lk, std::chrono::milliseconds(timeout_ms_), [&] { return req->granted; });

  if (!acquired) {
    // Timed out: remove from wait queue
    auto w_it = std::find(head->wait_queue.begin(), head->wait_queue.end(), req);
    if (w_it != head->wait_queue.end()) {
      head->wait_queue.erase(w_it);
    }
    return false;
  }

  head->granted_txns.insert(txn_id);
  {
    std::lock_guard<std::mutex> g_lock(global_latch_);
    txn_locks_[txn_id].insert(key);
  }
  return true;
}

bool LockManager::Release(uint32_t txn_id, const std::string& table_name, const RID& rid) {
  LockKey key{table_name, rid.page_id, rid.slot_id};
  std::shared_ptr<LockHead> head;
  {
    std::lock_guard<std::mutex> g_lock(global_latch_);
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) return false;
    head = it->second;
    txn_locks_[txn_id].erase(key);
  }

  std::lock_guard<std::mutex> lk(head->latch);
  head->granted_txns.erase(txn_id);

  // If all holders are gone or only shared holders remain, grant next waiter(s)
  if (head->granted_txns.empty()) {
    if (!head->wait_queue.empty()) {
      auto next_req = head->wait_queue.front();
      if (next_req->mode == LockMode::EXCLUSIVE) {
        head->wait_queue.pop_front();
        head->current_mode = LockMode::EXCLUSIVE;
        next_req->granted = true;
        head->cv.notify_all();
      } else {
        // Grant all consecutive SHARED requests at front of queue
        head->current_mode = LockMode::SHARED;
        while (!head->wait_queue.empty() &&
               head->wait_queue.front()->mode == LockMode::SHARED) {
          auto s_req = head->wait_queue.front();
          head->wait_queue.pop_front();
          s_req->granted = true;
        }
        head->cv.notify_all();
      }
    }
  } else if (head->current_mode == LockMode::SHARED) {
    // Check if next in queue is SHARED
    while (!head->wait_queue.empty() &&
           head->wait_queue.front()->mode == LockMode::SHARED) {
      auto s_req = head->wait_queue.front();
      head->wait_queue.pop_front();
      s_req->granted = true;
    }
    head->cv.notify_all();
  }

  return true;
}

void LockManager::ReleaseAll(uint32_t txn_id) {
  if (txn_id == 0) return;

  std::unordered_set<LockKey, LockKeyHash> keys_to_release;
  {
    std::lock_guard<std::mutex> g_lock(global_latch_);
    auto it = txn_locks_.find(txn_id);
    if (it == txn_locks_.end()) return;
    keys_to_release = it->second;
    txn_locks_.erase(it);
  }

  for (const auto& key : keys_to_release) {
    std::shared_ptr<LockHead> head;
    {
      std::lock_guard<std::mutex> g_lock(global_latch_);
      auto it = lock_table_.find(key);
      if (it != lock_table_.end()) {
        head = it->second;
      }
    }
    if (head) {
      std::lock_guard<std::mutex> lk(head->latch);
      head->granted_txns.erase(txn_id);

      if (head->granted_txns.empty()) {
        if (!head->wait_queue.empty()) {
          auto next_req = head->wait_queue.front();
          if (next_req->mode == LockMode::EXCLUSIVE) {
            head->wait_queue.pop_front();
            head->current_mode = LockMode::EXCLUSIVE;
            next_req->granted = true;
            head->cv.notify_all();
          } else {
            head->current_mode = LockMode::SHARED;
            while (!head->wait_queue.empty() &&
                   head->wait_queue.front()->mode == LockMode::SHARED) {
              auto s_req = head->wait_queue.front();
              head->wait_queue.pop_front();
              s_req->granted = true;
            }
            head->cv.notify_all();
          }
        }
      } else if (head->current_mode == LockMode::SHARED) {
        while (!head->wait_queue.empty() &&
               head->wait_queue.front()->mode == LockMode::SHARED) {
          auto s_req = head->wait_queue.front();
          head->wait_queue.pop_front();
          s_req->granted = true;
        }
        head->cv.notify_all();
      }
    }
  }
}

bool LockManager::HoldsExclusive(uint32_t txn_id, const std::string& table_name,
                                 const RID& rid) const {
  LockKey key{table_name, rid.page_id, rid.slot_id};
  std::lock_guard<std::mutex> g_lock(global_latch_);
  auto it = lock_table_.find(key);
  if (it == lock_table_.end()) return false;
  std::lock_guard<std::mutex> lk(it->second->latch);
  return it->second->current_mode == LockMode::EXCLUSIVE &&
         it->second->granted_txns.count(txn_id) > 0;
}

size_t LockManager::TotalLocksHeld() const {
  std::lock_guard<std::mutex> g_lock(global_latch_);
  size_t total = 0;
  for (const auto& [k, head] : lock_table_) {
    std::lock_guard<std::mutex> lk(head->latch);
    total += head->granted_txns.size();
  }
  return total;
}

}  // namespace sthir
