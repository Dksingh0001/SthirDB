#pragma once

#include <memory>
#include <string>

#include "catalog/catalog.h"
#include "parser/ast.h"
#include "planner/plan_node.h"

namespace sthir {

class Planner {
 public:
  explicit Planner(const Catalog& catalog) : catalog_(catalog) {}

  std::unique_ptr<PlanNode> CreatePlan(const SQLStatement& stmt, std::string* err);

 private:
  std::unique_ptr<PlanNode> PlanSelect(const SelectStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanInsert(const InsertStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanUpdate(const UpdateStmt& stmt, std::string* err);
  std::unique_ptr<PlanNode> PlanDelete(const DeleteStmt& stmt, std::string* err);

  const Catalog& catalog_;
};

}  // namespace sthir
