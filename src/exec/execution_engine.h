#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <mutex>
#include <thread>

#include "catalog/catalog.h"
#include "common/config.h"
#include "exec/executor.h"
#include "exec/value.h"
#include "planner/plan_node.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "recovery/recovery_manager.h"

namespace sthir {

struct ExecutionResult {
  bool success = true;
  std::string error_message;
  std::vector<std::string> column_names;
  std::vector<std::vector<std::string>> rows;
  size_t rows_affected = 0;
  bool is_query = false;

  // Phase 4 optimizer reporting (filled for SELECT and EXPLAIN).
  std::string chosen_path;                       // e.g. "IndexScan(idx_price)+Filter"
  double estimated_cost = 0.0;
  double rows_estimated = 0.0;
  size_t rows_actual = 0;
  double actual_time_ms = 0.0;
  std::vector<PlanAlternative> alternatives;     // rejected candidates, with costs
  std::string plan_text;                         // indented plan tree with estimates
  // Phase 4B hook: "Applied rules: ..." when constraint_aware = on, else empty.
  // Phase 4 implements no rules, so it currently always reports "(none)".
  std::string rules_line;
  bool has_plan_info = false;
  // Phase 7: pages actually read from disk while executing this statement
  // (buffer-pool misses only). Feeds the Novelty-1 A-vs-B evidence.
  size_t pages_read = 0;

  void PrintTable(std::ostream& os = std::cout) const;
};

class ExecutionEngine {
 public:
  explicit ExecutionEngine(Catalog* catalog, std::string data_dir = "./data/retail");
  ~ExecutionEngine();

  // Wraps ExecuteStatement with the Phase 7 pages-read measurement.
  ExecutionResult Execute(const std::string& sql);

  void SetSessionFlag(const std::string& key, const std::string& val);
  std::string GetSessionFlag(const std::string& key) const;

  // Buffer-pool frames per table/index file (default kDefaultPoolFrames = 64).
  // Must be called before the first statement: concurrency runs (Phase 7C) raise
  // it so 500 threads' pins fit; 64 frames silently cannot.
  void SetPoolFrames(std::size_t frames);
  std::size_t pool_frames() const { return pool_frames_; }

  RecoveryReport Recover();
  class WalManager* wal_manager() const { return wal_.get(); }
  class RecoveryManager* recovery_manager() const { return recovery_.get(); }
  class LockManager* lock_manager() const { return lock_manager_.get(); }

 private:
  struct StorageContext {
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPool> bp;
  };

  // Parses, plans and runs one statement (without the pages-read snapshot).
  // Turns executor exceptions (e.g. buffer-pool exhaustion) into a failed
  // ExecutionResult instead of letting them escape or truncate a result.
  ExecutionResult ExecuteStatement(const std::string& sql);
  ExecutionResult ExecuteInner(const std::string& sql);

  StorageContext* GetStorage(const std::string& table_name);
  // Opens (once) the storage for an index file, keyed by its path.
  StorageContext* GetIndexStorage(const std::string& path);
  std::unique_ptr<Executor> BuildExecutorTree(const PlanNode* plan);

  // Phase 7: constraint-checker services.
  // Loads every composite key of (table, columns) with one scan, counting
  // occurrences (FK child sets are multisets).
  bool LoadKeySet(const std::string& table, const std::vector<std::string>& columns,
                  std::unordered_map<std::string, uint32_t>* out, std::string* err);
  // ON DELETE CASCADE: deletes the rows whose columns equal `key`, one level
  // deep (further levels recurse with cascade_depth_ as the guard).
  size_t DeleteRowsByKey(const std::string& table, const std::vector<std::string>& columns,
                         const std::vector<Value>& key, uint32_t txn_id, std::string* err);

  uint32_t GetCurrentTxnId() const;
  void SetCurrentTxnId(uint32_t tid);

  mutable std::mutex engine_latch_;
  Catalog* catalog_;
  std::string data_dir_;
  std::size_t pool_frames_ = kDefaultPoolFrames;
  std::unordered_map<std::string, std::unique_ptr<StorageContext>> storage_map_;
  std::unordered_map<std::string, std::string> session_flags_;
  std::unordered_map<std::thread::id, uint32_t> thread_txns_;

  std::unique_ptr<class WalManager> wal_;
  std::unique_ptr<class RecoveryManager> recovery_;
  std::unique_ptr<class LockManager> lock_manager_;
  // Phase 7: PK/FK/UNIQUE/CHECK/NOT NULL enforcement behind the
  // `constraints_enabled` session flag.
  std::unique_ptr<class ConstraintChecker> constraint_checker_;
  int cascade_depth_ = 0;  // ON DELETE CASCADE recursion guard
};

}  // namespace sthir
