#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "exec/execution_engine.h"
#include "test_macros.h"

namespace fs = std::filesystem;

namespace {

const std::string kDir = "build/test_index_scan_env";
const std::string kCatalogPath = kDir + "/catalog.json";
const char* kQuery = "SELECT * FROM Items WHERE RetailPrice > 50";
const size_t kRowsOver50 = 9805;  // 9800 rows with 0.25*i > 50 + 5 duplicate 60.00 rows

std::string Price(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", value);
  return buf;
}

struct Env {
  sthir::Catalog catalog{kCatalogPath};
  std::unique_ptr<sthir::ExecutionEngine> engine;

  Env() {
    std::string err;
    catalog.Load(&err);
    engine = std::make_unique<sthir::ExecutionEngine>(&catalog, kDir);
  }
};

sthir::ExecutionResult Run(Env& env, const std::string& sql) {
  auto res = env.engine->Execute(sql);
  if (!res.success) {
    std::fprintf(stderr, "SQL failed: %s -> %s\n", sql.c_str(), res.error_message.c_str());
  }
  CHECK(res.success);
  return res;
}

bool ExpectError(Env& env, const std::string& sql) {
  auto res = env.engine->Execute(sql);
  return !res.success && !res.error_message.empty();
}

void SortRows(std::vector<std::vector<std::string>>* rows) {
  std::sort(rows->begin(), rows->end());
}

void CreateAndFill(Env& env) {
  Run(env, "CREATE TABLE Items (ItemID INT, ItemName VARCHAR(50), RetailPrice DECIMAL)");
  const int n = 10000;
  const int batch = 1000;  // multi-row INSERT keeps the fixture quick
  for (int start = 1; start <= n; start += batch) {
    std::string sql = "INSERT INTO Items VALUES ";
    for (int i = start; i < start + batch && i <= n; ++i) {
      if (i > start) sql += ", ";
      sql += "(" + std::to_string(i) + ", 'item_" + std::to_string(i) + "', " + Price(0.25 * i) +
             ")";
    }
    Run(env, sql);
  }
  // Duplicate prices: the index must return every matching RID.
  for (int i = 0; i < 5; ++i) {
    Run(env, "INSERT INTO Items VALUES (" + std::to_string(10000 + i) + ", 'dup_" + std::to_string(i) +
                 "', 60.00)");
  }
}

// Runs the interesting predicates on the SeqScan+Filter path.
struct Snapshots {
  sthir::ExecutionResult gt;
  sthir::ExecutionResult eq;
  sthir::ExecutionResult range;
  sthir::ExecutionResult count;
};

Snapshots TakeSnapshots(Env& env) {
  Snapshots s;
  s.gt = Run(env, kQuery);
  s.eq = Run(env, "SELECT * FROM Items WHERE RetailPrice = 60.00");
  s.range = Run(env, "SELECT * FROM Items WHERE RetailPrice >= 50 AND RetailPrice <= 60");
  s.count = Run(env, "SELECT COUNT(*) FROM Items WHERE RetailPrice > 50");
  return s;
}

void test_index_equivalence() {
  fs::remove_all(kDir);
  fs::create_directories(kDir);
  Env env;
  CreateAndFill(env);

  Snapshots before = TakeSnapshots(env);
  CHECK_EQ(before.gt.rows.size(), kRowsOver50);
  CHECK(!before.eq.rows.empty());
  CHECK(!before.range.rows.empty());

  auto plan_before = Run(env, std::string("EXPLAIN ") + kQuery);
  CHECK_EQ(plan_before.rows.size(), 1u);
  CHECK(plan_before.rows[0][0].find("SeqScan") != std::string::npos);

  auto created = Run(env, "CREATE INDEX idx_price ON Items (RetailPrice)");
  CHECK_EQ(created.rows_affected, 10005u);

  // 96% of the table matches > 50, so the optimizer keeps SeqScan and lists the
  // index as a rejected alternative with its cost.
  auto plan_after = Run(env, std::string("EXPLAIN ") + kQuery);
  CHECK_EQ(plan_after.rows.size(), 1u);
  CHECK(plan_after.rows[0][0].find("Chosen access path: SeqScan+Filter") != std::string::npos);
  CHECK(plan_after.rows[0][0].find("Rejected alternatives") != std::string::npos);
  CHECK(plan_after.rows[0][0].find("IndexScan(idx_price)") != std::string::npos);

  // A single-row lookup is worth an index seek.
  auto narrow_after = Run(env, "EXPLAIN SELECT * FROM Items WHERE RetailPrice = 25.50");
  CHECK(narrow_after.rows[0][0].find("Chosen access path: IndexScan(idx_price)") !=
        std::string::npos);

  // Same rows, same multiset: IndexScan must match SeqScan+Filter exactly.
  Snapshots after = TakeSnapshots(env);
  CHECK_EQ(after.gt.rows.size(), before.gt.rows.size());
  CHECK_EQ(after.eq.rows.size(), before.eq.rows.size());
  CHECK_EQ(after.range.rows.size(), before.range.rows.size());
  CHECK_EQ(after.count.rows.size(), 1u);
  CHECK_EQ(after.count.rows[0][0], std::to_string(kRowsOver50));

  SortRows(&before.gt.rows);
  SortRows(&after.gt.rows);
  CHECK(before.gt.rows == after.gt.rows);
  SortRows(&before.eq.rows);
  SortRows(&after.eq.rows);
  CHECK(before.eq.rows == after.eq.rows);
  SortRows(&before.range.rows);
  SortRows(&after.range.rows);
  CHECK(before.range.rows == after.range.rows);

  test_passed("index scan matches seq scan + filter");
}

void test_index_metadata_survives_restart() {
  Env env;  // fresh Catalog and engine over the same files
  CHECK(env.catalog.HasTable("items"));
  const auto* table = env.catalog.GetTable("items");
  CHECK(table != nullptr);
  CHECK_EQ(table->indexes.size(), 1u);
  CHECK_EQ(table->indexes[0].index_name, "idx_price");
  CHECK_EQ(table->indexes[0].column_name, "RetailPrice");
  CHECK(table->indexes[0].height >= 1u);
  CHECK_EQ(table->indexes[0].order, 4u);

  auto plan = Run(env, std::string("EXPLAIN ") + kQuery);
  CHECK(plan.rows[0][0].find("IndexScan") != std::string::npos);

  auto rows = Run(env, kQuery);
  CHECK_EQ(rows.rows.size(), kRowsOver50);
  test_passed("index metadata survives restart");
}

void test_int_column_index_and_errors() {
  Env env;
  // Equality on an INT column uses its own index.
  auto created = Run(env, "CREATE INDEX idx_item_id ON Items (ItemID)");
  CHECK_EQ(created.rows_affected, 10005u);

  auto plan = Run(env, "EXPLAIN SELECT * FROM Items WHERE ItemID = 42");
  CHECK(plan.rows[0][0].find("idx_item_id") != std::string::npos);

  auto row = Run(env, "SELECT * FROM Items WHERE ItemID = 42");
  CHECK_EQ(row.rows.size(), 1u);
  CHECK_EQ(row.rows[0][0], "42");

  // Duplicate name, missing table, missing column, and an unindexable key type.
  CHECK(ExpectError(env, "CREATE INDEX idx_price ON Items (RetailPrice)"));
  CHECK(ExpectError(env, "CREATE INDEX idx_bad ON MissingTable (RetailPrice)"));
  CHECK(ExpectError(env, "CREATE INDEX idx_bad ON Items (NoSuchColumn)"));
  CHECK(ExpectError(env, "CREATE INDEX idx_name ON Items (ItemName)"));
  test_passed("int column index and error cases");
}

}  // namespace

int main() {
  test_index_equivalence();
  test_index_metadata_survives_restart();
  test_int_column_index_and_errors();
  std::printf("all index scan tests passed\n");
  return 0;
}
