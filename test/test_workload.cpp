// Phase 7 data-generator tests: determinism (fixed seed -> identical data),
// internal consistency (purchase totals = line totals = invoice totals), and
// the constraint-backed index hand-off to the optimizer.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "exec/execution_engine.h"
#include "parser/parser.h"
#include "workload/anomaly_injector.h"
#include "workload/benchmark.h"
#include "workload/retail_gen.h"

#include "test_macros.h"

namespace fs = std::filesystem;

namespace {

const char* kTestDir = "./data/test_workload";
const char* kCatalogPath = "./data/test_workload/catalog.json";

void CleanDir() {
  std::error_code ec;
  fs::remove_all(kTestDir, ec);
  fs::create_directories(kTestDir, ec);
}

bool LoadSchema(sthir::ExecutionEngine& engine) {
  std::ifstream in("scripts/retail_schema.sql", std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  for (const auto& stmt : sthir::SplitSqlScript(ss.str())) {
    if (!engine.Execute(stmt).success) return false;
  }
  return true;
}

std::string Scalar(sthir::ExecutionEngine& engine, const std::string& sql) {
  auto res = engine.Execute(sql);
  CHECK(res.success);
  CHECK_EQ(res.rows.size(), 1u);
  CHECK_EQ(res.rows[0].size(), 1u);
  return res.rows[0][0];
}

struct Sums {
  std::string purchases_total;
  std::string line_total;
  std::string invoice_total;
  size_t line_count = 0;
  size_t indexes = 0;
};

// Fresh catalog + engine + schema + a small deterministic load.
Sums GenerateOnce(uint64_t seed) {
  CleanDir();
  sthir::Catalog catalog(kCatalogPath);
  CHECK(catalog.Load());
  sthir::ExecutionEngine engine(&catalog, kTestDir);
  CHECK(LoadSchema(engine));

  sthir::RetailGenConfig cfg;
  cfg.shoppers = 20;
  cfg.items = 50;
  cfg.purchases = 100;
  cfg.line_items_per_purchase = 3;
  cfg.seed = seed;
  sthir::RetailGenResult result;
  std::string err;
  const bool ok = sthir::GenerateRetailData(&engine, cfg, &result, &err);
  if (!ok) std::fprintf(stderr, "generate failed: %s\n", err.c_str());
  CHECK(ok);
  CHECK_EQ(result.total_rows, 50u + 100u + 20u + 50u + 100u + 300u + 100u + 100u);

  Sums s;
  s.purchases_total = Scalar(engine, "SELECT SUM(Total) FROM Purchases");
  s.line_total = Scalar(engine, "SELECT SUM(LineTotal) FROM Purchase_Line_Items");
  s.invoice_total = Scalar(engine, "SELECT SUM(Total) FROM Invoices");
  s.line_count = std::stoul(Scalar(engine, "SELECT COUNT(*) FROM Purchase_Line_Items"));

  size_t indexes = 0;
  for (const char* table : {"Staff", "Shoppers", "Vendors", "Items", "Purchases", "Invoices",
                            "Purchase_Line_Items", "Item_Vendor_Catalogs"}) {
    const auto* schema = catalog.GetTable(table);
    CHECK(schema != nullptr);
    indexes += schema->indexes.size();
  }
  s.indexes = indexes;
  return s;
}

void test_generator_determinism() {
  const Sums a = GenerateOnce(42);
  const Sums b = GenerateOnce(42);

  // Same seed -> byte-identical aggregates.
  CHECK_EQ(a.purchases_total, b.purchases_total);
  CHECK_EQ(a.line_total, b.line_total);
  CHECK_EQ(a.invoice_total, b.invoice_total);
  CHECK_EQ(a.line_count, b.line_count);
  CHECK_EQ(a.line_count, 300u);

  // Internal consistency: what the purchases say is exactly what the line
  // items say, and the invoices mirror the purchases.
  CHECK_EQ(a.purchases_total, a.line_total);
  CHECK_EQ(a.purchases_total, a.invoice_total);

  // One constraint-backed index per PRIMARY KEY / UNIQUE (INT/DECIMAL) = 9.
  CHECK_EQ(a.indexes, 9u);

  // A different seed must produce different data (otherwise seed is ignored).
  const Sums c = GenerateOnce(43);
  CHECK(c.purchases_total != a.purchases_total);

  CleanDir();
  test_passed("generator_determinism");
}

void test_generator_refuses_nonempty_db() {
  const Sums a = GenerateOnce(42);
  (void)a;
  sthir::Catalog catalog(kCatalogPath);
  CHECK(catalog.Load());
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  sthir::RetailGenConfig cfg;
  cfg.shoppers = 5;
  cfg.items = 5;
  cfg.purchases = 5;
  sthir::RetailGenResult result;
  std::string err;
  CHECK(!sthir::GenerateRetailData(&engine, cfg, &result, &err));
  CHECK(err.find("not empty") != std::string::npos);

  CleanDir();
  test_passed("generator_refuses_nonempty_db");
}

// Runs the injector on a small generated database and checks every class
// against the given expectation (blocked everywhere vs accepted everywhere).
void CheckInjector(bool constraints_on, bool extended, int rate, size_t expected_classes,
                   size_t expected_injected_first_class) {
  CleanDir();
  sthir::Catalog catalog(kCatalogPath);
  CHECK(catalog.Load());
  sthir::ExecutionEngine engine(&catalog, kTestDir);
  CHECK(LoadSchema(engine));

  sthir::RetailGenConfig g;
  g.shoppers = 20;
  g.items = 50;
  g.purchases = 100;
  sthir::RetailGenResult gr;
  std::string err;
  CHECK(sthir::GenerateRetailData(&engine, g, &gr, &err));

  engine.SetSessionFlag("constraints_enabled", constraints_on ? "on" : "off");

  sthir::InjectConfig cfg;
  cfg.extended = extended;
  cfg.default_count = 20;
  cfg.rate_percent = rate;
  sthir::InjectResult res;
  CHECK(sthir::RunAnomalyInjector(&engine, cfg, &res, &err));
  CHECK_EQ(res.classes.size(), expected_classes);
  CHECK_EQ(res.classes[0].injected, expected_injected_first_class);

  for (const auto& c : res.classes) {
    if (constraints_on) {
      CHECK_EQ(c.accepted, 0u);
      CHECK_EQ(c.blocked, c.injected);
      CHECK_EQ(c.wrong_constraint, 0u);  // blocked by the *named* constraint
      CHECK(!c.first_block.empty());
    } else {
      CHECK_EQ(c.blocked, 0u);
      CHECK_EQ(c.accepted, c.injected);
    }
  }

  const std::string table = sthir::FormatInjectionTable(res, "test");
  CHECK(table.find("interception %") != std::string::npos);
  CHECK(table.find("TOTAL") != std::string::npos);
  const char* pct = constraints_on ? "100.0%" : "0.0%";
  CHECK(table.find(std::string("| ") + pct) != std::string::npos);

  CleanDir();
}

void test_injector_constrained_vs_unconstrained() {
  CheckInjector(true, false, 0, 3, 20);    // default 3 classes x 20: all blocked
  CheckInjector(false, false, 0, 3, 20);   // same statements accepted with the flag off
  CheckInjector(true, true, 0, 8, 20);     // 8 classes incl. our own: all blocked
  // Rate mode: count = rate% of the target table (Shoppers = 20 -> 10% = 2).
  CheckInjector(true, false, 10, 3, 2);
  test_passed("injector_constrained_vs_unconstrained");
}

void test_benchmark_harness() {
  CleanDir();
  sthir::Catalog catalog(kCatalogPath);
  CHECK(catalog.Load());
  sthir::ExecutionEngine engine(&catalog, kTestDir);
  CHECK(LoadSchema(engine));

  sthir::RetailGenConfig g;
  g.shoppers = 20;
  g.items = 50;
  g.purchases = 100;
  sthir::RetailGenResult gr;
  std::string err;
  CHECK(sthir::GenerateRetailData(&engine, g, &gr, &err));

  // Small concurrency run on the generated data: every query must succeed.
  sthir::BenchConfig cfg;
  cfg.thread_counts = {2, 4};
  cfg.iters_per_thread = 2;
  cfg.warmup = 1;
  sthir::BenchResult res;
  int callbacks = 0;
  const bool ok = sthir::RunBenchmark(&engine, cfg, &res, &err,
                                      [&callbacks](const sthir::BenchRow&) { ++callbacks; });
  if (!ok) std::fprintf(stderr, "benchmark failed: %s\n", err.c_str());
  CHECK(ok);
  CHECK_EQ(res.rows.size(), 2u);
  CHECK_EQ(res.rows[0].threads, 2u);
  CHECK_EQ(res.rows[0].queries, 4u);   // 2 threads x 2 iters, all successful
  CHECK_EQ(res.rows[0].failures, 0u);
  CHECK_EQ(res.rows[1].threads, 4u);
  CHECK_EQ(res.rows[1].queries, 8u);
  CHECK_EQ(res.rows[1].failures, 0u);
  CHECK_EQ(callbacks, 2);
  CHECK(res.rows[0].avg_ms > 0.0);
  CHECK(res.rows[0].p99_ms >= res.rows[0].min_ms);
  CHECK(res.rows[0].max_ms >= res.rows[0].avg_ms);

  const std::string table = sthir::FormatBenchTable(res, "test");
  CHECK(table.find("| threads | queries | failures | avg ms | p99 ms |") !=
        std::string::npos);
  CHECK(table.find("| 4 | 8 | 0 |") != std::string::npos);

  // A broken query must fail the run with a clear error, not produce numbers.
  sthir::BenchConfig bad = cfg;
  bad.query = "SELECT * FROM NoSuchTable";
  sthir::BenchResult bad_res;
  CHECK(!sthir::RunBenchmark(&engine, bad, &bad_res, &err));
  CHECK(!err.empty());

  // An empty result set is also a harness failure (guards against a filter
  // that silently matches nothing).
  sthir::BenchConfig empty = cfg;
  empty.query = "SELECT * FROM Items WHERE ItemID > 999999";
  sthir::BenchResult empty_res;
  CHECK(!sthir::RunBenchmark(&engine, empty, &empty_res, &err));
  CHECK(err.find("no rows") != std::string::npos);

  CleanDir();
  test_passed("benchmark_harness");
}

}  // namespace

int main() {
  test_generator_determinism();
  test_generator_refuses_nonempty_db();
  test_injector_constrained_vs_unconstrained();
  test_benchmark_harness();
  return 0;
}