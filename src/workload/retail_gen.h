#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sthir {

class ExecutionEngine;

// Phase 7C data generator: the Yuan 2026 retail scale (100k purchases,
// ~300k line items, 5k items, 1k shoppers) produced deterministically from a
// fixed seed. Every run with the same config inserts byte-identical rows.
struct RetailGenConfig {
  uint32_t shoppers = 1000;
  uint32_t items = 5000;
  uint32_t purchases = 100000;
  uint32_t line_items_per_purchase = 3;  // 100k x 3 = 300k line items
  uint32_t staff = 50;
  uint32_t vendors = 100;
  uint64_t seed = 42;
  // Constraint-backed indexes: one per PRIMARY KEY / UNIQUE declaration whose
  // column is INT/DECIMAL (the int64-keyed B+tree cannot index VARCHAR, so
  // UNIQUE(Email) is enforced but not index-backed).
  // `SET constraints_enabled = off` skips them, which is what makes the
  // unconstrained baseline a genuinely different physical schema.
  bool create_indexes = true;
  bool analyze = true;  // refresh column statistics after the load
};

struct RetailGenResult {
  // (table, row count) in insertion order.
  std::vector<std::pair<std::string, size_t>> rows_by_table;
  size_t total_rows = 0;
  size_t indexes_created = 0;
  double elapsed_ms = 0.0;
};

// Executes batched INSERTs (then ANALYZE + CREATE INDEX) through `engine`.
// Fails if any table already holds rows, so a run is always additive-free.
// Returns false and fills *err on the first failing statement.
bool GenerateRetailData(ExecutionEngine* engine, const RetailGenConfig& cfg,
                        RetailGenResult* out, std::string* err);

}  // namespace sthir