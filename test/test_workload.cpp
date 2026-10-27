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

}  // namespace

int main() {
  test_generator_determinism();
  test_generator_refuses_nonempty_db();
  return 0;
}