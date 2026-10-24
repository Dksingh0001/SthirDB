#include "optimizer/statistics.h"

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

const std::string kDir = "build/test_optimizer_env";
const std::string kCatalogPath = kDir + "/catalog.json";

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

void test_histogram_equi_width() {
  std::vector<double> values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  sthir::ColumnStats stats;
  sthir::BuildColumnStats(values, 2, &stats);

  CHECK_EQ(stats.row_count, 10u);
  CHECK_EQ(stats.null_count, 2u);
  CHECK_EQ(stats.ndv, 10u);
  CHECK(stats.valid);
  CHECK_EQ(stats.min_value, 1.0);
  CHECK_EQ(stats.max_value, 10.0);
  CHECK_EQ(stats.histogram.size(), static_cast<size_t>(sthir::kHistogramBuckets));
  // Equi-width over [1,10]: [1,4) holds 1-3, [4,7) holds 4-6, [7,10] holds 7-10.
  CHECK_EQ(stats.histogram[0].count, 3u);
  CHECK_EQ(stats.histogram[1].count, 3u);
  CHECK_EQ(stats.histogram[2].count, 4u);
  uint64_t total = 0;
  for (const auto& bucket : stats.histogram) total += bucket.count;
  CHECK_EQ(total, stats.row_count);
  CHECK_EQ(stats.histogram[0].lo, 1.0);
  CHECK_EQ(stats.histogram[2].hi, 10.0);
  test_passed("histogram_equi_width");
}

void test_histogram_constant_column() {
  std::vector<double> values(50, 7.0);
  sthir::ColumnStats stats;
  sthir::BuildColumnStats(values, 0, &stats);
  CHECK_EQ(stats.ndv, 1u);
  CHECK_EQ(stats.histogram[0].count, 50u);
  CHECK_EQ(stats.histogram[1].count, 0u);
  CHECK_EQ(stats.histogram[2].count, 0u);
  test_passed("histogram_constant_column");
}

void test_empty_column_stats() {
  sthir::ColumnStats stats;
  sthir::BuildColumnStats({}, 12, &stats);
  CHECK(!stats.valid);
  CHECK_EQ(stats.row_count, 0u);
  CHECK_EQ(stats.null_count, 12u);
  CHECK(stats.histogram.empty());
  test_passed("empty_column_stats");
}

void test_catalog_stats_roundtrip() {
  fs::remove_all(kDir);
  fs::create_directories(kDir);
  {
    Env env;
    sthir::TableSchema schema;
    schema.table_name = "Stats";
    schema.columns = {{"id", sthir::DataType::INT, 4, false, false, false}};
    std::string err;
    CHECK(env.catalog.CreateTable(schema, &err));

    sthir::ColumnStats stats;
    stats.column_name = "id";
    sthir::BuildColumnStats({1, 2, 3, 4, 5, 6}, 0, &stats);
    CHECK(env.catalog.SetColumnStats("stats", stats, &err));
  }

  sthir::Catalog reloaded(kCatalogPath);
  std::string err;
  CHECK(reloaded.Load(&err));
  const auto* table = reloaded.GetTable("stats");
  CHECK(table != nullptr);
  CHECK_EQ(table->column_stats.size(), 1u);
  const auto* stats = table->GetColumnStats("ID");
  CHECK(stats != nullptr);
  CHECK_EQ(stats->row_count, 6u);
  CHECK_EQ(stats->ndv, 6u);
  CHECK_EQ(stats->min_value, 1.0);
  CHECK_EQ(stats->max_value, 6.0);
  CHECK_EQ(stats->histogram.size(), static_cast<size_t>(sthir::kHistogramBuckets));
  CHECK_EQ(stats->histogram[0].count + stats->histogram[1].count + stats->histogram[2].count, 6u);
  test_passed("catalog_stats_roundtrip");
}

void test_analyze_and_create_index_collect_stats() {
  fs::remove_all(kDir);
  fs::create_directories(kDir);
  Env env;
  Run(env, "CREATE TABLE Items (ItemID INT, ItemName VARCHAR(50), RetailPrice DECIMAL)");
  for (int i = 1; i <= 100; ++i) {
    char price[16];
    std::snprintf(price, sizeof(price), "%.2f", 0.5 * i);
    Run(env, "INSERT INTO Items VALUES (" + std::to_string(i) + ", 'item_" + std::to_string(i) +
                 "', " + price + ")");
  }

  // ANALYZE refreshes statistics for every numeric column (ItemID, RetailPrice).
  auto analyzed = Run(env, "ANALYZE Items");
  CHECK_EQ(analyzed.rows_affected, 2u);

  const auto* table = env.catalog.GetTable("items");
  CHECK(table != nullptr);
  CHECK_EQ(table->column_stats.size(), 2u);
  const auto* price = table->GetColumnStats("RetailPrice");
  CHECK(price != nullptr);
  CHECK(price->valid);
  CHECK_EQ(price->row_count, 100u);
  CHECK_EQ(price->ndv, 100u);
  CHECK_EQ(price->min_value, 0.5);
  CHECK_EQ(price->max_value, 50.0);
  const auto* id = table->GetColumnStats("ItemID");
  CHECK(id != nullptr);
  CHECK_EQ(id->min_value, 1.0);
  CHECK_EQ(id->max_value, 100.0);

  // A later INSERT is invisible to stats until the next ANALYZE.
  Run(env, "INSERT INTO Items VALUES (101, 'item_101', 50.50)");
  CHECK_EQ(table->GetColumnStats("RetailPrice")->row_count, 100u);

  // CREATE INDEX refreshes the statistics of its own column.
  Run(env, "CREATE INDEX idx_price ON Items (RetailPrice)");
  CHECK_EQ(env.catalog.GetTable("items")->GetColumnStats("RetailPrice")->row_count, 101u);

  // ANALYZE catches up every numeric column.
  Run(env, "ANALYZE Items");
  CHECK_EQ(env.catalog.GetTable("items")->GetColumnStats("ItemID")->row_count, 101u);

  // ANALYZE is repeatable and keeps the same numbers.
  auto again = Run(env, "ANALYZE Items");
  CHECK_EQ(again.rows_affected, 2u);
  CHECK_EQ(env.catalog.GetTable("items")->GetColumnStats("ItemID")->row_count, 101u);
  test_passed("analyze_and_create_index_collect_stats");
}

}  // namespace

int main() {
  test_histogram_equi_width();
  test_histogram_constant_column();
  test_empty_column_stats();
  test_catalog_stats_roundtrip();
  test_analyze_and_create_index_collect_stats();
  std::printf("all optimizer statistics tests passed\n");
  return 0;
}
