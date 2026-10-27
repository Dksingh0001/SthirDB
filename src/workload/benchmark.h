#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace sthir {

class ExecutionEngine;

// Phase 7C workload: the multi-table reporting query joining Purchases,
// Purchase_Line_Items, Items and Invoices over the Q4-2025 window.
extern const char* const kWorkloadQuery;

struct BenchConfig {
  std::vector<size_t> thread_counts = {100, 300, 500};
  size_t iters_per_thread = 3;
  size_t warmup = 1;
  std::string query;  // empty -> kWorkloadQuery
};

struct BenchRow {
  size_t threads = 0;
  size_t queries = 0;    // successful executions
  size_t failures = 0;   // failed executions (engine error or exception)
  double avg_ms = 0.0;
  double p99_ms = 0.0;
  double min_ms = 0.0;
  double max_ms = 0.0;
  std::string first_error;
};

struct BenchResult {
  std::vector<BenchRow> rows;
};

// Warmup first (fails the run if the query errors or returns no rows), then
// for each thread count: T threads released together, each running the query
// `iters_per_thread` times. Latency = wall time of one Execute call (parse +
// plan + execute). Returns false only for harness failures; per-query errors
// are counted into the row. `on_row` fires after each thread count completes.
bool RunBenchmark(ExecutionEngine* engine, const BenchConfig& cfg, BenchResult* out,
                  std::string* err,
                  const std::function<void(const BenchRow&)>& on_row = nullptr);

std::string FormatBenchTable(const BenchResult& result, const std::string& mode_label);

}  // namespace sthir