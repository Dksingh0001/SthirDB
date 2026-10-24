#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>

#include "catalog/catalog.h"
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

  void PrintTable(std::ostream& os = std::cout) const;
};

class ExecutionEngine {
 public:
  explicit ExecutionEngine(Catalog* catalog, std::string data_dir = "./data/retail");
  ~ExecutionEngine();

  ExecutionResult Execute(const std::string& sql);

  void SetSessionFlag(const std::string& key, const std::string& val);
  std::string GetSessionFlag(const std::string& key) const;

  RecoveryReport Recover();
  class WalManager* wal_manager() const { return wal_.get(); }
  class RecoveryManager* recovery_manager() const { return recovery_.get(); }

 private:
  struct StorageContext {
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPool> bp;
  };

  StorageContext* GetStorage(const std::string& table_name);
  // Opens (once) the storage for an index file, keyed by its path.
  StorageContext* GetIndexStorage(const std::string& path);
  std::unique_ptr<Executor> BuildExecutorTree(const PlanNode* plan);

  Catalog* catalog_;
  std::string data_dir_;
  std::unordered_map<std::string, std::unique_ptr<StorageContext>> storage_map_;
  std::unordered_map<std::string, std::string> session_flags_;

  std::unique_ptr<class WalManager> wal_;
  std::unique_ptr<class RecoveryManager> recovery_;
  uint32_t current_txn_id_ = 0;
};

}  // namespace sthir
