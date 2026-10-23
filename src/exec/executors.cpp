#include "exec/executors.h"

#include <algorithm>
#include <iostream>

namespace sthir {

// -------------------------------------------------------------
// SeqScanExecutor
// -------------------------------------------------------------

SeqScanExecutor::SeqScanExecutor(std::string table_name,
                                 TableSchema schema,
                                 BufferPool* bp,
                                 DiskManager* dm)
    : table_name_(std::move(table_name)),
      schema_(std::move(schema)),
      bp_(bp),
      dm_(dm) {}

SeqScanExecutor::~SeqScanExecutor() {
  close();
}

void SeqScanExecutor::init() {
  close();
  cur_page_id_ = 0;
  cur_slot_ = 0;
  cur_page_ = nullptr;
  total_pages_ = dm_ ? dm_->num_pages() : 0;
}

std::optional<Tuple> SeqScanExecutor::next() {
  if (!dm_ || !bp_) return std::nullopt;

  while (cur_page_id_ < total_pages_) {
    if (cur_page_ == nullptr) {
      cur_page_ = bp_->fetch_page(cur_page_id_);
      if (cur_page_ == nullptr) return std::nullopt;
      cur_slot_ = 0;
    }

    while (cur_slot_ < cur_page_->slot_count()) {
      uint16_t slot = cur_slot_++;
      if (!cur_page_->tuple_alive(slot)) continue;
      uint16_t len = cur_page_->tuple_length(slot);
      if (len == 0) continue;

      std::vector<char> buf(len);
      int bytes_read = cur_page_->ReadTuple(slot, buf.data());
      if (bytes_read <= 0) continue;

      Tuple t = Tuple::Deserialize(buf.data(), static_cast<uint16_t>(bytes_read), schema_);
      if (t.is_deleted()) continue;

      t.set_rid(RID{cur_page_id_, slot, true});
      return t;
    }

    // Done with this page
    bp_->unpin_page(cur_page_id_, false);
    cur_page_ = nullptr;
    ++cur_page_id_;
  }

  return std::nullopt;
}

void SeqScanExecutor::close() {
  if (cur_page_ != nullptr && bp_ != nullptr) {
    bp_->unpin_page(cur_page_id_, false);
    cur_page_ = nullptr;
  }
}

// -------------------------------------------------------------
// FilterExecutor
// -------------------------------------------------------------

FilterExecutor::FilterExecutor(std::unique_ptr<Executor> child, std::unique_ptr<Expr> predicate)
    : child_(std::move(child)), predicate_(std::move(predicate)) {}

void FilterExecutor::init() {
  if (child_) child_->init();
}

std::optional<Tuple> FilterExecutor::next() {
  if (!child_) return std::nullopt;

  while (true) {
    auto opt = child_->next();
    if (!opt.has_value()) return std::nullopt;

    if (!predicate_) return opt;

    Tribool tb = EvaluatePredicate(predicate_.get(), *opt, child_->output_schema());
    if (tb == Tribool::True) {
      return opt;
    }
    // Unknown and False are filtered out in SQL WHERE
  }
}

void FilterExecutor::close() {
  if (child_) child_->close();
}

// -------------------------------------------------------------
// ProjectExecutor
// -------------------------------------------------------------

ProjectExecutor::ProjectExecutor(std::unique_ptr<Executor> child,
                                 TableSchema output_schema,
                                 std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projections)
    : child_(std::move(child)),
      output_schema_(std::move(output_schema)),
      projections_(std::move(projections)) {}

void ProjectExecutor::init() {
  if (child_) child_->init();
}

std::optional<Tuple> ProjectExecutor::next() {
  if (!child_) return std::nullopt;

  auto opt = child_->next();
  if (!opt.has_value()) return std::nullopt;

  std::vector<Value> projected_values;
  projected_values.reserve(projections_.size());

  for (const auto& p : projections_) {
    projected_values.push_back(EvaluateExpr(p.first.get(), *opt, child_->output_schema()));
  }

  Tuple t(std::move(projected_values));
  t.set_rid(opt->rid());
  return t;
}

void ProjectExecutor::close() {
  if (child_) child_->close();
}

// -------------------------------------------------------------
// AggregateExecutor
// -------------------------------------------------------------

AggregateExecutor::AggregateExecutor(std::unique_ptr<Executor> child,
                                     std::vector<AggEntry> aggregates,
                                     std::vector<std::string> group_by_cols,
                                     TableSchema output_schema)
    : child_(std::move(child)),
      aggregates_(std::move(aggregates)),
      group_by_cols_(std::move(group_by_cols)),
      output_schema_(std::move(output_schema)) {}

void AggregateExecutor::init() {
  groups_.clear();
  emit_idx_ = 0;
  if (!child_) return;

  child_->init();
  std::unordered_map<std::string, size_t> key_to_group;
  bool had_any_row = false;

  while (true) {
    auto opt = child_->next();
    if (!opt.has_value()) break;
    had_any_row = true;

    // Extract group key
    std::vector<Value> keys;
    std::string key_str;
    for (const auto& col_name : group_by_cols_) {
      int idx = child_->output_schema().FindColumnIndex(col_name);
      Value v = (idx >= 0) ? opt->GetValue(static_cast<size_t>(idx)) : Value::MakeNull();
      key_str += v.ToString() + "|";
      keys.push_back(v);
    }

    size_t g_idx;
    auto it = key_to_group.find(key_str);
    if (it == key_to_group.end()) {
      g_idx = groups_.size();
      key_to_group[key_str] = g_idx;

      GroupState gs;
      gs.group_keys = std::move(keys);
      gs.counts.assign(aggregates_.size(), 0);
      gs.sums.assign(aggregates_.size(), 0.0);
      gs.mins.assign(aggregates_.size(), Value::MakeNull());
      gs.maxs.assign(aggregates_.size(), Value::MakeNull());
      gs.non_null_counts.assign(aggregates_.size(), 0);
      groups_.push_back(std::move(gs));
    } else {
      g_idx = it->second;
    }

    GroupState& gs = groups_[g_idx];

    for (size_t i = 0; i < aggregates_.size(); ++i) {
      const auto& agg = aggregates_[i];
      if (agg.is_star) {
        ++gs.counts[i];
      } else {
        Value val = EvaluateExpr(agg.arg.get(), *opt, child_->output_schema());
        if (!val.is_null()) {
          ++gs.non_null_counts[i];
          if (agg.func == AggFunc::COUNT) {
            ++gs.counts[i];
          } else if (agg.func == AggFunc::SUM || agg.func == AggFunc::AVG) {
            gs.sums[i] += val.as_decimal();
          } else if (agg.func == AggFunc::MIN) {
            if (gs.mins[i].is_null() || val.Compare(BinaryOp::LT, gs.mins[i]) == Tribool::True) {
              gs.mins[i] = val;
            }
          } else if (agg.func == AggFunc::MAX) {
            if (gs.maxs[i].is_null() || val.Compare(BinaryOp::GT, gs.maxs[i]) == Tribool::True) {
              gs.maxs[i] = val;
            }
          }
        }
      }
    }
  }

  // Scalar aggregate over empty table produces 1 row with 0 count / null sums
  if (!had_any_row && group_by_cols_.empty()) {
    GroupState gs;
    gs.counts.assign(aggregates_.size(), 0);
    gs.sums.assign(aggregates_.size(), 0.0);
    gs.mins.assign(aggregates_.size(), Value::MakeNull());
    gs.maxs.assign(aggregates_.size(), Value::MakeNull());
    gs.non_null_counts.assign(aggregates_.size(), 0);
    groups_.push_back(std::move(gs));
  }

  child_->close();
}

std::optional<Tuple> AggregateExecutor::next() {
  if (emit_idx_ >= groups_.size()) return std::nullopt;

  const GroupState& gs = groups_[emit_idx_++];
  std::vector<Value> row_vals;
  row_vals.reserve(gs.group_keys.size() + aggregates_.size());

  // First group keys
  for (const auto& k : gs.group_keys) {
    row_vals.push_back(k);
  }

  // Next aggregates
  for (size_t i = 0; i < aggregates_.size(); ++i) {
    const auto& agg = aggregates_[i];
    switch (agg.func) {
      case AggFunc::COUNT:
        row_vals.push_back(Value::MakeInt(static_cast<int32_t>(gs.counts[i])));
        break;
      case AggFunc::SUM:
        if (gs.non_null_counts[i] == 0) {
          row_vals.push_back(Value::MakeNull(DataType::DECIMAL));
        } else {
          row_vals.push_back(Value::MakeDecimal(gs.sums[i]));
        }
        break;
      case AggFunc::AVG:
        if (gs.non_null_counts[i] == 0) {
          row_vals.push_back(Value::MakeNull(DataType::DECIMAL));
        } else {
          row_vals.push_back(Value::MakeDecimal(gs.sums[i] / static_cast<double>(gs.non_null_counts[i])));
        }
        break;
      case AggFunc::MIN:
        row_vals.push_back(gs.mins[i]);
        break;
      case AggFunc::MAX:
        row_vals.push_back(gs.maxs[i]);
        break;
    }
  }

  return Tuple(std::move(row_vals));
}

void AggregateExecutor::close() {
  groups_.clear();
  emit_idx_ = 0;
  if (child_) child_->close();
}

// -------------------------------------------------------------
// SortExecutor
// -------------------------------------------------------------

SortExecutor::SortExecutor(std::unique_ptr<Executor> child,
                           std::vector<OrderByItem> order_by_items,
                           TableSchema schema)
    : child_(std::move(child)),
      order_by_items_(std::move(order_by_items)),
      schema_(std::move(schema)) {}

void SortExecutor::init() {
  tuples_.clear();
  cur_idx_ = 0;
  if (!child_) return;

  child_->init();
  while (true) {
    auto opt = child_->next();
    if (!opt.has_value()) break;
    tuples_.push_back(std::move(*opt));
  }
  child_->close();

  std::sort(tuples_.begin(), tuples_.end(), [this](const Tuple& a, const Tuple& b) {
    for (const auto& item : order_by_items_) {
      int idx = schema_.FindColumnIndex(item.column_name);
      if (idx < 0) continue;
      const Value& va = a.GetValue(static_cast<size_t>(idx));
      const Value& vb = b.GetValue(static_cast<size_t>(idx));

      if (va.is_null() && vb.is_null()) continue;
      if (va.is_null()) return item.is_desc ? false : true;
      if (vb.is_null()) return item.is_desc ? true : false;

      if (va.Compare(BinaryOp::EQ, vb) == Tribool::True) continue;

      Tribool lt = va.Compare(BinaryOp::LT, vb);
      if (lt == Tribool::True) return !item.is_desc;
      return item.is_desc;
    }
    return false;
  });
}

std::optional<Tuple> SortExecutor::next() {
  if (cur_idx_ < tuples_.size()) {
    return tuples_[cur_idx_++];
  }
  return std::nullopt;
}

void SortExecutor::close() {
  tuples_.clear();
  cur_idx_ = 0;
  if (child_) child_->close();
}

// -------------------------------------------------------------
// NestedLoopJoinExecutor
// -------------------------------------------------------------

NestedLoopJoinExecutor::NestedLoopJoinExecutor(std::unique_ptr<Executor> left,
                                               std::unique_ptr<Executor> right,
                                               std::unique_ptr<Expr> join_predicate,
                                               TableSchema schema)
    : left_(std::move(left)),
      right_(std::move(right)),
      join_predicate_(std::move(join_predicate)),
      schema_(std::move(schema)) {}

void NestedLoopJoinExecutor::init() {
  if (left_) left_->init();
  right_tuples_.clear();
  right_idx_ = 0;

  if (right_) {
    right_->init();
    while (true) {
      auto opt = right_->next();
      if (!opt.has_value()) break;
      right_tuples_.push_back(std::move(*opt));
    }
    right_->close();
  }

  cur_left_ = left_ ? left_->next() : std::nullopt;
}

std::optional<Tuple> NestedLoopJoinExecutor::next() {
  while (cur_left_.has_value()) {
    while (right_idx_ < right_tuples_.size()) {
      const Tuple& r = right_tuples_[right_idx_++];

      std::vector<Value> joined_values;
      joined_values.reserve(cur_left_->size() + r.size());
      for (const auto& v : cur_left_->values()) joined_values.push_back(v);
      for (const auto& v : r.values()) joined_values.push_back(v);

      Tuple joined(std::move(joined_values));

      if (!join_predicate_) {
        return joined;
      }

      Tribool tb = EvaluatePredicate(join_predicate_.get(), joined, schema_);
      if (tb == Tribool::True) {
        return joined;
      }
    }

    // Advance outer
    cur_left_ = left_ ? left_->next() : std::nullopt;
    right_idx_ = 0;
  }

  return std::nullopt;
}

void NestedLoopJoinExecutor::close() {
  if (left_) left_->close();
  right_tuples_.clear();
  right_idx_ = 0;
  cur_left_ = std::nullopt;
}

// -------------------------------------------------------------
// DML operations
// -------------------------------------------------------------

size_t ExecuteInsert(const InsertPlan& plan, BufferPool* bp, DiskManager* dm, std::string* err) {
  if (!bp || !dm) {
    if (err) *err = "storage manager uninitialized";
    return 0;
  }

  const TableSchema& schema = plan.output_schema();
  uint32_t cur_pid = 0;
  if (dm->num_pages() == 0) {
    cur_pid = dm->allocate_page();
    Page* p = bp->fetch_page(cur_pid);
    if (!p) {
      if (err) *err = "buffer pool exhausted";
      return 0;
    }
    p->Reset(cur_pid);
    bp->unpin_page(cur_pid, true);
  } else {
    cur_pid = dm->num_pages() - 1;
  }

  Page* page = bp->fetch_page(cur_pid);
  if (!page) {
    if (err) *err = "buffer pool exhausted";
    return 0;
  }
  if (page->free_space_ptr() < kPageHeaderSize) page->Reset(cur_pid);

  size_t inserted_count = 0;
  for (const auto& val_row : plan.values_matrix()) {
    // Construct tuple
    std::vector<Value> tuple_vals;
    tuple_vals.reserve(schema.columns.size());

    for (size_t col_i = 0; col_i < schema.columns.size(); ++col_i) {
      const auto& col = schema.columns[col_i];
      // Find index in target columns
      int target_idx = -1;
      for (size_t ti = 0; ti < plan.target_columns().size(); ++ti) {
        if (plan.target_columns()[ti] == col.name) {
          target_idx = static_cast<int>(ti);
          break;
        }
      }

      if (target_idx >= 0 && target_idx < static_cast<int>(val_row.size())) {
        Value v = EvaluateExpr(val_row[static_cast<size_t>(target_idx)].get(), Tuple{}, schema);
        tuple_vals.push_back(v.CastTo(col.type));
      } else {
        tuple_vals.push_back(Value::MakeNull(col.type));
      }
    }

    Tuple tuple(std::move(tuple_vals));
    std::vector<char> bytes = tuple.Serialize(schema);

    if (page->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size())) < 0) {
      // Page is full, allocate next page
      bp->unpin_page(cur_pid, true);
      cur_pid = dm->allocate_page();
      page = bp->fetch_page(cur_pid);
      if (!page) {
        if (err) *err = "buffer pool exhausted";
        return inserted_count;
      }
      page->Reset(cur_pid);
      if (page->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size())) < 0) {
        bp->unpin_page(cur_pid, false);
        if (err) *err = "tuple size exceeds page capacity";
        return inserted_count;
      }
    }
    ++inserted_count;
  }

  bp->unpin_page(cur_pid, true);
  bp->flush_all();
  return inserted_count;
}

size_t ExecuteUpdate(const UpdatePlan& plan, std::unique_ptr<Executor> child,
                     BufferPool* bp, DiskManager* dm, std::string* err) {
  if (!child || !bp || !dm) {
    if (err) *err = "storage or child plan uninitialized";
    return 0;
  }

  const TableSchema& schema = plan.output_schema();
  child->init();

  std::vector<Tuple> matching_tuples;
  while (true) {
    auto opt = child->next();
    if (!opt.has_value()) break;
    matching_tuples.push_back(std::move(*opt));
  }
  child->close();

  size_t updated_count = 0;
  for (auto& tuple : matching_tuples) {
    if (!tuple.rid().is_valid) continue;

    // Apply assignments to create updated tuple
    std::vector<Value> new_vals = tuple.values();
    for (const auto& assign : plan.assignments()) {
      int idx = schema.FindColumnIndex(assign.first);
      if (idx >= 0 && idx < static_cast<int>(new_vals.size())) {
        Value nv = EvaluateExpr(assign.second.get(), tuple, schema);
        new_vals[static_cast<size_t>(idx)] = nv.CastTo(schema.columns[idx].type);
      }
    }

    Tuple updated_tuple(std::move(new_vals));
    std::vector<char> bytes = updated_tuple.Serialize(schema);

    Page* p = bp->fetch_page(tuple.rid().page_id);
    if (!p) continue;

    // Tombstone old slot
    p->DeleteTuple(tuple.rid().slot_id);

    // Try inserting into same page
    if (p->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size())) < 0) {
      bp->unpin_page(tuple.rid().page_id, true);
      // Append to last page
      uint32_t last_pid = dm->num_pages() - 1;
      Page* last_p = bp->fetch_page(last_pid);
      if (last_p && last_p->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size())) >= 0) {
        bp->unpin_page(last_pid, true);
      } else {
        if (last_p) bp->unpin_page(last_pid, false);
        uint32_t new_pid = dm->allocate_page();
        Page* new_p = bp->fetch_page(new_pid);
        if (new_p) {
          new_p->Reset(new_pid);
          new_p->InsertTuple(bytes.data(), static_cast<uint16_t>(bytes.size()));
          bp->unpin_page(new_pid, true);
        }
      }
    } else {
      bp->unpin_page(tuple.rid().page_id, true);
    }

    ++updated_count;
  }

  bp->flush_all();
  return updated_count;
}

size_t ExecuteDelete(const DeletePlan& /*plan*/, std::unique_ptr<Executor> child,
                     BufferPool* bp, DiskManager* /*dm*/, std::string* err) {
  if (!child || !bp) {
    if (err) *err = "storage or child plan uninitialized";
    return 0;
  }

  child->init();
  std::vector<RID> rids_to_delete;
  while (true) {
    auto opt = child->next();
    if (!opt.has_value()) break;
    if (opt->rid().is_valid) {
      rids_to_delete.push_back(opt->rid());
    }
  }
  child->close();

  size_t deleted_count = 0;
  for (const auto& rid : rids_to_delete) {
    Page* p = bp->fetch_page(rid.page_id);
    if (!p) continue;
    if (p->DeleteTuple(rid.slot_id)) {
      ++deleted_count;
      bp->unpin_page(rid.page_id, true);
    } else {
      bp->unpin_page(rid.page_id, false);
    }
  }

  bp->flush_all();
  return deleted_count;
}

}  // namespace sthir
