#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

#include "catalog/catalog.h"
#include "exec/executor.h"
#include "exec/expr_eval.h"
#include "index/bplus_tree.h"
#include "index/index_scan.h"
#include "parser/ast.h"
#include "planner/plan_node.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

namespace sthir {

class SeqScanExecutor : public Executor {
 public:
  SeqScanExecutor(std::string table_name,
                  TableSchema schema,
                  BufferPool* bp,
                  DiskManager* dm);
  ~SeqScanExecutor() override;

  void init() override;
  std::optional<Tuple> next() override;
  void close() override;

  const TableSchema& output_schema() const override { return schema_; }

 private:
  std::string table_name_;
  TableSchema schema_;
  BufferPool* bp_;
  DiskManager* dm_;

  uint32_t cur_page_id_ = 0;
  uint16_t cur_slot_ = 0;
  Page* cur_page_ = nullptr;
  uint32_t total_pages_ = 0;
};

// Reads RIDs from a B+tree range scan, fetches each base tuple by RID, drops
// RIDs whose slot was deleted after the index was built, and rechecks the
// predicate so index bounds only have to be a superset of the true matches.
class IndexScanExecutor : public Executor {
 public:
  IndexScanExecutor(std::string table_name,
                    TableSchema schema,
                    BufferPool* bp,
                    DiskManager* dm,
                    std::unique_ptr<BPlusTree> tree,
                    size_t key_column,
                    KeyRange range,
                    std::unique_ptr<Expr> predicate);

  void init() override;
  std::optional<Tuple> next() override;
  void close() override;

  const TableSchema& output_schema() const override { return schema_; }
  // Number of RIDs the range scan produced in the last init().
  size_t indexed_rid_count() const { return rids_.size(); }

 private:
  std::string table_name_;
  TableSchema schema_;
  BufferPool* bp_;
  DiskManager* dm_;
  std::unique_ptr<BPlusTree> tree_;
  size_t key_column_;
  KeyRange range_;
  std::unique_ptr<Expr> predicate_;

  std::vector<IndexRid> rids_;
  size_t rid_idx_ = 0;
};

class FilterExecutor : public Executor {
 public:
  FilterExecutor(std::unique_ptr<Executor> child, std::unique_ptr<Expr> predicate);

  void init() override;
  std::optional<Tuple> next() override;
  void close() override;

  const TableSchema& output_schema() const override { return child_->output_schema(); }

 private:
  std::unique_ptr<Executor> child_;
  std::unique_ptr<Expr> predicate_;
};

class ProjectExecutor : public Executor {
 public:
  ProjectExecutor(std::unique_ptr<Executor> child,
                  TableSchema output_schema,
                  std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections);

  void init() override;
  std::optional<Tuple> next() override;
  void close() override;

  const TableSchema& output_schema() const override { return output_schema_; }

 private:
  std::unique_ptr<Executor> child_;
  TableSchema output_schema_;
  std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections_;
};

class AggregateExecutor : public Executor {
 public:
  AggregateExecutor(std::unique_ptr<Executor> child,
                    std::vector<AggEntry> aggregates,
                    std::vector<std::string> group_by_cols,
                    TableSchema output_schema);

  void init() override;
  std::optional<Tuple> next() override;
  void close() override;

  const TableSchema& output_schema() const override { return output_schema_; }

 private:
  struct GroupState {
    std::vector<Value> group_keys;
    std::vector<int64_t> counts;
    std::vector<double> sums;
    std::vector<Value> mins;
    std::vector<Value> maxs;
    std::vector<int64_t> non_null_counts;
  };

  std::unique_ptr<Executor> child_;
  std::vector<AggEntry> aggregates_;
  std::vector<std::string> group_by_cols_;
  TableSchema output_schema_;

  std::vector<GroupState> groups_;
  size_t emit_idx_ = 0;
};

class SortExecutor : public Executor {
 public:
  SortExecutor(std::unique_ptr<Executor> child,
               std::vector<OrderByItem> order_by_items,
               TableSchema schema);

  void init() override;
  std::optional<Tuple> next() override;
  void close() override;

  const TableSchema& output_schema() const override { return schema_; }

 private:
  std::unique_ptr<Executor> child_;
  std::vector<OrderByItem> order_by_items_;
  TableSchema schema_;

  std::vector<Tuple> tuples_;
  size_t cur_idx_ = 0;
};

class NestedLoopJoinExecutor : public Executor {
 public:
  NestedLoopJoinExecutor(std::unique_ptr<Executor> left,
                         std::unique_ptr<Executor> right,
                         std::unique_ptr<Expr> join_predicate,
                         TableSchema schema);

  void init() override;
  std::optional<Tuple> next() override;
  void close() override;

  const TableSchema& output_schema() const override { return schema_; }

 private:
  std::unique_ptr<Executor> left_;
  std::unique_ptr<Executor> right_;
  std::unique_ptr<Expr> join_predicate_;
  TableSchema schema_;

  std::optional<Tuple> cur_left_;
  std::vector<Tuple> right_tuples_;
  size_t right_idx_ = 0;
  bool right_buffered_ = false;
};

// DML execution functions that directly mutate slotted-page storage
size_t ExecuteInsert(const InsertPlan& plan, BufferPool* bp, DiskManager* dm, std::string* err);
size_t ExecuteUpdate(const UpdatePlan& plan, std::unique_ptr<Executor> child,
                     BufferPool* bp, DiskManager* dm, std::string* err);
size_t ExecuteDelete(const DeletePlan& plan, std::unique_ptr<Executor> child,
                     BufferPool* bp, DiskManager* dm, std::string* err);

}  // namespace sthir
