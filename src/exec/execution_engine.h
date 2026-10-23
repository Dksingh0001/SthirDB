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

namespace sthir {

struct ExecutionResult {
  bool success = true;
  std::string error_message;
  std::vector<std::string> column_names;
  std::vector<std::vector<std::string>> rows;
  size_t rows_affected = 0;
  bool is_query = false;

  void PrintTable(std::ostream& os = std::cout) const;
};

class ExecutionEngine {
 public:
  explicit ExecutionEngine(Catalog* catalog, std::string data_dir = "./data/retail");
  ~ExecutionEngine();

  ExecutionResult Execute(const std::string& sql);

  void SetSessionFlag(const std::string& key, const std::string& val);
  std::string GetSessionFlag(const std::string& key) const;

 private:
  struct StorageContext {
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPool> bp;
  };

  StorageContext* GetStorage(const std::string& table_name);
  std::unique_ptr<Executor> BuildExecutorTree(const PlanNode* plan);

  Catalog* catalog_;
  std::string data_dir_;
  std::unordered_map<std::string, std::unique_ptr<StorageContext>> storage_map_;
  std::unordered_map<std::string, std::string> session_flags_;
};

}  // namespace sthir
