#pragma once

#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "catalog/catalog.h"
#include "parser/ast.h"

namespace sthir {

enum class PlanNodeType {
  SEQ_SCAN,
  FILTER,
  PROJECT,
  AGGREGATE,
  SORT,
  NESTED_LOOP_JOIN,
  INSERT,
  UPDATE,
  DELETE_NODE
};

class PlanNode {
 public:
  virtual ~PlanNode() = default;
  virtual PlanNodeType type() const = 0;
  virtual const TableSchema& output_schema() const = 0;
  virtual std::vector<const PlanNode*> children() const = 0;
  virtual std::string ToString(int indent = 0) const = 0;

 protected:
  static std::string IndentStr(int indent) {
    return std::string(indent * 2, ' ');
  }
};

class SeqScanPlan : public PlanNode {
 public:
  SeqScanPlan(std::string table_name, TableSchema schema)
      : table_name_(std::move(table_name)), schema_(std::move(schema)) {}

  PlanNodeType type() const override { return PlanNodeType::SEQ_SCAN; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }

  std::vector<const PlanNode*> children() const override { return {}; }
  std::string ToString(int indent = 0) const override {
    return IndentStr(indent) + "SeqScan(" + table_name_ + ")\n";
  }

 private:
  std::string table_name_;
  TableSchema schema_;
};

class FilterPlan : public PlanNode {
 public:
  FilterPlan(std::unique_ptr<Expr> predicate, std::unique_ptr<PlanNode> child)
      : predicate_(std::move(predicate)), child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::FILTER; }
  const TableSchema& output_schema() const override { return child_->output_schema(); }
  const Expr* predicate() const { return predicate_.get(); }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string ToString(int indent = 0) const override {
    return IndentStr(indent) + "Filter\n" + child_->ToString(indent + 1);
  }

 private:
  std::unique_ptr<Expr> predicate_;
  std::unique_ptr<PlanNode> child_;
};

class ProjectPlan : public PlanNode {
 public:
  ProjectPlan(std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections,
              TableSchema schema, std::unique_ptr<PlanNode> child)
      : projections_(std::move(projections)), schema_(std::move(schema)), child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::PROJECT; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::vector<std::pair<std::unique_ptr<Expr>, std::string>>& projections() const {
    return projections_;
  }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string ToString(int indent = 0) const override {
    std::string s = IndentStr(indent) + "Project(";
    for (size_t i = 0; i < projections_.size(); ++i) {
      s += projections_[i].second;
      if (i + 1 < projections_.size()) s += ", ";
    }
    s += ")\n";
    if (child_) s += child_->ToString(indent + 1);
    return s;
  }

 private:
  std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections_;
  TableSchema schema_;
  std::unique_ptr<PlanNode> child_;
};

struct AggEntry {
  AggFunc func;
  bool is_star = false;
  std::unique_ptr<Expr> arg;
  std::string output_name;
};

class AggregatePlan : public PlanNode {
 public:
  AggregatePlan(std::vector<AggEntry> aggregates,
                std::vector<std::string> group_by_cols,
                TableSchema schema,
                std::unique_ptr<PlanNode> child)
      : aggregates_(std::move(aggregates)),
        group_by_cols_(std::move(group_by_cols)),
        schema_(std::move(schema)),
        child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::AGGREGATE; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::vector<AggEntry>& aggregates() const { return aggregates_; }
  const std::vector<std::string>& group_by_cols() const { return group_by_cols_; }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string ToString(int indent = 0) const override {
    std::string s = IndentStr(indent) + "Aggregate(group_by=[";
    for (size_t i = 0; i < group_by_cols_.size(); ++i) {
      s += group_by_cols_[i];
      if (i + 1 < group_by_cols_.size()) s += ", ";
    }
    s += "])\n";
    if (child_) s += child_->ToString(indent + 1);
    return s;
  }

 private:
  std::vector<AggEntry> aggregates_;
  std::vector<std::string> group_by_cols_;
  TableSchema schema_;
  std::unique_ptr<PlanNode> child_;
};

class SortPlan : public PlanNode {
 public:
  SortPlan(std::vector<OrderByItem> order_by_items, std::unique_ptr<PlanNode> child)
      : order_by_items_(std::move(order_by_items)), child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::SORT; }
  const TableSchema& output_schema() const override { return child_->output_schema(); }
  const std::vector<OrderByItem>& order_by_items() const { return order_by_items_; }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string ToString(int indent = 0) const override {
    std::string s = IndentStr(indent) + "Sort(";
    for (size_t i = 0; i < order_by_items_.size(); ++i) {
      s += order_by_items_[i].column_name + (order_by_items_[i].is_desc ? " DESC" : " ASC");
      if (i + 1 < order_by_items_.size()) s += ", ";
    }
    s += ")\n";
    if (child_) s += child_->ToString(indent + 1);
    return s;
  }

 private:
  std::vector<OrderByItem> order_by_items_;
  std::unique_ptr<PlanNode> child_;
};

class NestedLoopJoinPlan : public PlanNode {
 public:
  NestedLoopJoinPlan(std::unique_ptr<Expr> join_predicate,
                     std::unique_ptr<PlanNode> left,
                     std::unique_ptr<PlanNode> right,
                     TableSchema schema)
      : join_predicate_(std::move(join_predicate)),
        left_(std::move(left)),
        right_(std::move(right)),
        schema_(std::move(schema)) {}

  PlanNodeType type() const override { return PlanNodeType::NESTED_LOOP_JOIN; }
  const TableSchema& output_schema() const override { return schema_; }
  const Expr* join_predicate() const { return join_predicate_.get(); }
  const PlanNode* left() const { return left_.get(); }
  const PlanNode* right() const { return right_.get(); }
  std::unique_ptr<PlanNode>& left() { return left_; }
  std::unique_ptr<PlanNode>& right() { return right_; }

  std::vector<const PlanNode*> children() const override { return {left_.get(), right_.get()}; }
  std::string ToString(int indent = 0) const override {
    std::string s = IndentStr(indent) + "NestedLoopJoin\n";
    if (left_) s += left_->ToString(indent + 1);
    if (right_) s += right_->ToString(indent + 1);
    return s;
  }

 private:
  std::unique_ptr<Expr> join_predicate_;
  std::unique_ptr<PlanNode> left_;
  std::unique_ptr<PlanNode> right_;
  TableSchema schema_;
};

class InsertPlan : public PlanNode {
 public:
  InsertPlan(std::string table_name,
             TableSchema schema,
             std::vector<std::string> target_columns,
             std::vector<std::vector<std::unique_ptr<Expr>>> values_matrix)
      : table_name_(std::move(table_name)),
        schema_(std::move(schema)),
        target_columns_(std::move(target_columns)),
        values_matrix_(std::move(values_matrix)) {}

  PlanNodeType type() const override { return PlanNodeType::INSERT; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }
  const std::vector<std::string>& target_columns() const { return target_columns_; }
  const std::vector<std::vector<std::unique_ptr<Expr>>>& values_matrix() const {
    return values_matrix_;
  }

  std::vector<const PlanNode*> children() const override { return {}; }
  std::string ToString(int indent = 0) const override {
    return IndentStr(indent) + "InsertInto(" + table_name_ + ")\n";
  }

 private:
  std::string table_name_;
  TableSchema schema_;
  std::vector<std::string> target_columns_;
  std::vector<std::vector<std::unique_ptr<Expr>>> values_matrix_;
};

class UpdatePlan : public PlanNode {
 public:
  UpdatePlan(std::string table_name,
             TableSchema schema,
             std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments,
             std::unique_ptr<PlanNode> child)
      : table_name_(std::move(table_name)),
        schema_(std::move(schema)),
        assignments_(std::move(assignments)),
        child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::UPDATE; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }
  const std::vector<std::pair<std::string, std::unique_ptr<Expr>>>& assignments() const {
    return assignments_;
  }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string ToString(int indent = 0) const override {
    std::string s = IndentStr(indent) + "Update(" + table_name_ + ")\n";
    if (child_) s += child_->ToString(indent + 1);
    return s;
  }

 private:
  std::string table_name_;
  TableSchema schema_;
  std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments_;
  std::unique_ptr<PlanNode> child_;
};

class DeletePlan : public PlanNode {
 public:
  DeletePlan(std::string table_name,
             TableSchema schema,
             std::unique_ptr<PlanNode> child)
      : table_name_(std::move(table_name)),
        schema_(std::move(schema)),
        child_(std::move(child)) {}

  PlanNodeType type() const override { return PlanNodeType::DELETE_NODE; }
  const TableSchema& output_schema() const override { return schema_; }
  const std::string& table_name() const { return table_name_; }
  const PlanNode* child() const { return child_.get(); }
  std::unique_ptr<PlanNode>& child() { return child_; }

  std::vector<const PlanNode*> children() const override { return {child_.get()}; }
  std::string ToString(int indent = 0) const override {
    std::string s = IndentStr(indent) + "Delete(" + table_name_ + ")\n";
    if (child_) s += child_->ToString(indent + 1);
    return s;
  }

 private:
  std::string table_name_;
  TableSchema schema_;
  std::unique_ptr<PlanNode> child_;
};

}  // namespace sthir
