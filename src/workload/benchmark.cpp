#include "workload/benchmark.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "exec/execution_engine.h"

namespace sthir {

const char* const kWorkloadQuery =
    "SELECT Items.CategoryName, SUM(Purchase_Line_Items.LineTotal), COUNT(*) "
    "FROM Purchases "
    "JOIN Purchase_Line_Items ON Purchases.PurchaseID = Purchase_Line_Items.PurchaseID "
    "JOIN Items ON Purchase_Line_Items.ItemID = Items.ItemID "
    "JOIN Invoices ON Purchases.PurchaseID = Invoices.PurchaseID "
    "WHERE Purchases.PurchaseDate >= 20251101 AND Invoices.IssuedDate >= 20251102 "
    "GROUP BY Items.CategoryName";

namespace {

double NearestRankP99(std::vector<double>* sorted) {
  if (sorted->empty()) return 0.0;
  std::sort(sorted->begin(), sorted->end());
  const double n = static_cast<double>(sorted->size());
  size_t rank = static_cast<size_t>(std::ceil(0.99 * n));
  if (rank < 1) rank = 1;
  if (rank > sorted->size()) rank = sorted->size();
  return (*sorted)[rank - 1];
}

// One thread-count run: all threads start together (so the requested
// concurrency is real, not ramped), each executes the query `iters` times.
bool RunOnce(ExecutionEngine* engine, const std::string& query, size_t threads, size_t iters,
             BenchRow* out, std::string* err) {
  out->threads = threads;

  std::atomic<bool> go{false};
  std::vector<std::vector<double>> samples(threads);
  std::vector<size_t> failures(threads, 0);
  std::vector<std::string> errors(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  try {
    for (size_t t = 0; t < threads; ++t) {
      workers.emplace_back([&, t] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        auto& mine = samples[t];
        mine.reserve(iters);
        for (size_t i = 0; i < iters; ++i) {
          const auto t0 = std::chrono::steady_clock::now();
          try {
            ExecutionResult res = engine->Execute(query);
            const auto t1 = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (res.success) {
              mine.push_back(ms);
            } else {
              ++failures[t];
              if (errors[t].empty()) errors[t] = res.error_message;
            }
          } catch (const std::exception& e) {
            ++failures[t];
            if (errors[t].empty()) errors[t] = e.what();
          } catch (...) {
            ++failures[t];
            if (errors[t].empty()) errors[t] = "unknown error";
          }
        }
      });
    }
  } catch (const std::exception& e) {
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    if (err) *err = std::string("thread spawn failed: ") + e.what();
    return false;
  }

  go.store(true, std::memory_order_release);
  for (auto& w : workers) w.join();

  std::vector<double> all;
  all.reserve(threads * iters);
  for (size_t t = 0; t < threads; ++t) {
    out->failures += failures[t];
    if (out->first_error.empty() && !errors[t].empty()) out->first_error = errors[t];
    all.insert(all.end(), samples[t].begin(), samples[t].end());
  }
  out->queries = all.size();
  if (!all.empty()) {
    double sum = 0.0;
    double minv = all[0];
    double maxv = all[0];
    for (double v : all) {
      sum += v;
      if (v < minv) minv = v;
      if (v > maxv) maxv = v;
    }
    out->avg_ms = sum / static_cast<double>(all.size());
    out->min_ms = minv;
    out->max_ms = maxv;
    out->p99_ms = NearestRankP99(&all);
  }
  return true;
}

}  // namespace

bool RunBenchmark(ExecutionEngine* engine, const BenchConfig& cfg, BenchResult* out,
                  std::string* err, const std::function<void(const BenchRow&)>& on_row) {
  if (engine == nullptr) {
    if (err) *err = "null engine";
    return false;
  }
  if (cfg.thread_counts.empty()) {
    if (err) *err = "no thread counts configured";
    return false;
  }
  const std::string query = cfg.query.empty() ? kWorkloadQuery : cfg.query;

  // Warmup: proves the query works on this data and faults in plans/frames.
  for (size_t i = 0; i < std::max<size_t>(1, cfg.warmup); ++i) {
    const ExecutionResult res = engine->Execute(query);
    if (!res.success) {
      if (err) *err = "warmup query failed: " + res.error_message;
      return false;
    }
    if (res.rows.empty()) {
      if (err) *err = "warmup query returned no rows; check the data and filters";
      return false;
    }
  }

  for (size_t threads : cfg.thread_counts) {
    BenchRow row;
    std::string row_err;
    if (!RunOnce(engine, query, threads, std::max<size_t>(1, cfg.iters_per_thread), &row,
                  &row_err)) {
      if (err) *err = row_err;
      return false;
    }
    out->rows.push_back(row);
    if (on_row) on_row(row);
  }
  return true;
}

std::string FormatBenchTable(const BenchResult& result, const std::string& mode_label) {
  std::string out = "### Concurrent reporting query — " + mode_label + "\n\n";
  out += "| threads | queries | failures | avg ms | p99 ms | min ms | max ms |\n";
  out += "|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const auto& r : result.rows) {
    char line[256];
    std::snprintf(line, sizeof(line), "| %zu | %zu | %zu | %.1f | %.1f | %.1f | %.1f |\n",
                  r.threads, r.queries, r.failures, r.avg_ms, r.p99_ms, r.min_ms, r.max_ms);
    out += line;
  }
  out += "\n";
  for (const auto& r : result.rows) {
    if (r.failures > 0) {
      out += "> " + std::to_string(r.threads) + " threads: first failure: " + r.first_error +
             "\n";
    }
  }
  return out;
}

}  // namespace sthir