#pragma once

#include <memory>
#include <string>

#include "catalog/catalog.h"
#include "parser/ast.h"
#include "planner/plan_node.h"

namespace sthir {

class Planner {
 public:
  // `allow_index_scan` / `allow_seq_scan` expose the access-path choice to the
  // CLI (SET enable_index_scan / enable_seq_scan) for measurement; both default
  // to on, which is the normal optimizer behaviour.
  explicit Planner(const Catalog& catalog, bool allow_index_scan = true,
                   bool allow_seq_scan = true)
      : catalog_(catalog),
        allow_index_scan_(allow_index_scan),
        allow_seq_scan_(allow_seq_scan) {}

  std::unique_ptr<PlanNode> CreatePlan(const SQLStatement& stmt, std::string* err);

 private:
  std::unique_ptr<PlanNode> PlanSelect(const SelectStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanInsert(const InsertStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanUpdate(const UpdateStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanDelete(const DeleteStmt& stmt, std::string* err);

  const Catalog& catalog_;
  bool allow_index_scan_ = true;
  bool allow_seq_scan_ = true;
};

}  // namespace sthir
