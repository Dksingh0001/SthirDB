#include "optimizer/statistics.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "exec/execution_engine.h"
#include "common/config.h"
#include "optimizer/cost_model.h"
#include "optimizer/optimizer.h"
#include "optimizer/join_order.h"
#include "optimizer/selectivity.h"
#include "parser/lexer.h"
#include "parser/parser.h"
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

bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// Uniform column: 0..100 in three equal buckets, 100 rows each.
sthir::ColumnStats UniformStats(const std::string& column) {
  sthir::ColumnStats stats;
  stats.column_name = column;
  stats.row_count = 300;
  stats.ndv = 300;
  stats.min_value = 0.0;
  stats.max_value = 100.0;
  const double third = 100.0 / 3.0;
  stats.histogram = {{0.0, third, 100}, {third, 2.0 * third, 100}, {2.0 * third, 100.0, 100}};
  stats.valid = true;
  return stats;
}

sthir::TableSchema MakeSchema() {
  sthir::TableSchema schema;
  schema.table_name = "T";
  schema.columns = {{"ItemID", sthir::DataType::INT, 4, false, false, false},
                    {"RetailPrice", sthir::DataType::DECIMAL, 8, false, false, false}};
  schema.column_stats.push_back(UniformStats("ItemID"));
  schema.column_stats.push_back(UniformStats("RetailPrice"));
  return schema;
}

// Parses "SELECT * FROM T WHERE <predicate>" and returns the WHERE expression.
std::unique_ptr<sthir::Expr> WhereExpr(const std::string& predicate) {
  sthir::Lexer lexer("SELECT * FROM T WHERE " + predicate);
  auto tokens = lexer.Tokenize();
  CHECK(!lexer.has_error());
  sthir::Parser parser(std::move(tokens));
  auto stmt = parser.ParseStatement();
  CHECK(stmt != nullptr);
  CHECK(!parser.has_error());
  auto* select = dynamic_cast<sthir::SelectStmt*>(stmt.get());
  CHECK(select != nullptr);
  return std::move(select->where_clause);
}

void test_selectivity_equality_and_range() {
  const sthir::TableSchema schema = MakeSchema();

  auto eq = WhereExpr("RetailPrice = 50");
  CHECK(Near(sthir::EstimateSelectivity(eq.get(), schema), 1.0 / 300.0));

  auto gt = WhereExpr("RetailPrice > 100");
  CHECK(Near(sthir::EstimateSelectivity(gt.get(), schema), 0.0));

  auto lt = WhereExpr("RetailPrice < 0");
  CHECK(Near(sthir::EstimateSelectivity(lt.get(), schema), 0.0));

  // Histogram overlap: [0,50] covers bucket 0 and half of bucket 1.
  auto half = WhereExpr("RetailPrice >= 0 AND RetailPrice <= 50");
  CHECK(Near(sthir::EstimateSelectivity(half.get(), schema), 0.5));

  // Conjunction multiplies selectivities (independence assumption).
  auto conjunction = WhereExpr("RetailPrice > 50 AND RetailPrice > 0");
  CHECK(Near(sthir::EstimateSelectivity(conjunction.get(), schema), 0.5));

  // Disjunction uses inclusion-exclusion: s1 + s2 - s1*s2.
  auto disjunction = WhereExpr("RetailPrice > 50 OR RetailPrice < 20");
  const double s1 = 0.5;
  const double s2 = 0.2;
  CHECK(Near(sthir::EstimateSelectivity(disjunction.get(), schema), s1 + s2 - s1 * s2));

  // Equality row estimate: 300 / 300 distinct values = 1 row.
  CHECK_EQ(sthir::EstimateRows(sthir::EstimateSelectivity(eq.get(), schema), 300), 1u);
  test_passed("selectivity_equality_and_range");
}

void test_selectivity_defaults_without_stats() {
  sthir::TableSchema schema;
  schema.table_name = "T";
  schema.columns = {{"ItemID", sthir::DataType::INT, 4, false, false, false},
                    {"RetailPrice", sthir::DataType::DECIMAL, 8, false, false, false}};
  // No column_stats at all.
  auto eq = WhereExpr("RetailPrice = 50");
  CHECK(Near(sthir::EstimateSelectivity(eq.get(), schema), sthir::kDefaultEqualitySelectivity));
  auto range = WhereExpr("RetailPrice > 50");
  CHECK(Near(sthir::EstimateSelectivity(range.get(), schema), sthir::kDefaultRangeSelectivity));
  test_passed("selectivity_defaults_without_stats");
}

void test_cost_functions() {
  CHECK(Near(sthir::SeqScanCost(41), 41.0));
  // index_height(3) + matching rows packed at average density: 4 rows of a
  // 5000-row/41-page table ceil to one page of matches.
  CHECK(Near(sthir::PagesOfMatchingRows(4, 5000, 41), 1.0));
  CHECK(Near(sthir::IndexScanCost(3, 4, 5000, 41), 3.0 + 1.0 * sthir::kRandPageCost));
  // Half the table matches: 2500 * 41 / 5000 = 20.5 -> 21 pages.
  CHECK(Near(sthir::PagesOfMatchingRows(2500, 5000, 41), 21.0));
  // A full-table match cannot touch more pages than the table has.
  CHECK(Near(sthir::PagesOfMatchingRows(5000, 5000, 41), 41.0));
  CHECK(Near(sthir::IndexScanCost(3, 5000, 5000, 41), 3.0 + 41.0 * sthir::kRandPageCost));
  CHECK(Near(sthir::FilterCost(41.0, 1000), 41.0));
  CHECK(Near(sthir::AggregateCost(41.0, 1000), 41.0));
  CHECK(Near(sthir::SortCost(10, 63), 0.0));  // fits in the buffer pool
  const double spilled = sthir::SortCost(10000, 63);
  CHECK(spilled > 0.0);
  CHECK(Near(sthir::NestedLoopJoinCost(10, 100, 20), 10.0 + 100.0 * 20.0));
  test_passed("cost_functions");
}

void test_join_order_enumeration() {
  std::vector<sthir::JoinTableSummary> tables = {
      {"Items", 41, 5000}, {"Customers", 10, 800}, {"Orders", 60, 9000}};
  const auto plans = sthir::EnumerateJoinOrders(tables);
  CHECK_EQ(plans.size(), 6u);  // 3! orders

  // Every order appears exactly once.
  std::vector<std::string> seen;
  for (const auto& plan : plans) {
    CHECK_EQ(plan.order.size(), 3u);
    std::string key = plan.order[0] + ">" + plan.order[1] + ">" + plan.order[2];
    for (const auto& other : seen) CHECK(other != key);
    seen.push_back(key);
  }

  // Cheap first, and the printed cost matches the formula for the first order.
  for (size_t i = 1; i < plans.size(); ++i) CHECK(plans[i - 1].cost <= plans[i].cost);
  CHECK_EQ(plans[0].order[0], "Customers");  // smallest outer table wins
  const double expected = 10.0 + 800.0 * 41.0 + 800.0 * 5000.0 * 60.0;
  CHECK(Near(plans[0].cost, expected));
  test_passed("join_order_enumeration");
}

void test_access_path_choice() {
  const std::string table_path = kDir + "/choice.dat";
  {
    std::ofstream out(table_path, std::ios::binary);
    const std::vector<char> page(sthir::kPageSize, '\0');
    for (int i = 0; i < 41; ++i) out.write(page.data(), page.size());  // 41 pages
  }

  sthir::TableSchema schema;
  schema.table_name = "Items";
  schema.data_file_path = table_path;
  schema.columns = {{"ItemID", sthir::DataType::INT, 4, false, false, false},
                    {"RetailPrice", sthir::DataType::DECIMAL, 8, false, false, false}};
  sthir::ColumnStats price;
  price.column_name = "RetailPrice";
  price.row_count = 5000;
  price.ndv = 5000;
  price.min_value = 1.0;
  price.max_value = 51.0;
  price.histogram = {{1.0, 17.666, 1667}, {17.666, 34.333, 1667}, {34.333, 51.0, 1666}};
  price.valid = true;
  schema.column_stats.push_back(price);
  schema.indexes.push_back(sthir::IndexMetadata{"idx_price", "RetailPrice", 0, 3, 4});

  // A single-row lookup: the index seek beats a 41-page sequential scan.
  auto narrow = WhereExpr("RetailPrice = 25");
  auto choice = sthir::ChooseAccessPath(schema, narrow.get());
  CHECK(choice.name.find("IndexScan(idx_price)") != std::string::npos);
  CHECK(choice.cost < 41.0);
  CHECK_EQ(choice.rejected.size(), 1u);
  CHECK(choice.rejected[0].name.find("SeqScan") != std::string::npos);

  // A wide range covering the whole table: SeqScan wins and the index is listed.
  auto wide = WhereExpr("RetailPrice > 1");
  auto seq_choice = sthir::ChooseAccessPath(schema, wide.get());
  CHECK_EQ(seq_choice.name, "SeqScan+Filter");
  CHECK_EQ(seq_choice.rejected.size(), 1u);
  CHECK(seq_choice.rejected[0].name.find("IndexScan") != std::string::npos);
  CHECK(seq_choice.rejected[0].cost > seq_choice.cost);

  // Measurement hooks: each path can be forced for the same predicate.
  auto forced_seq = sthir::ChooseAccessPath(schema, narrow.get(), false, true);
  CHECK_EQ(forced_seq.name, "SeqScan+Filter");
  auto forced_index = sthir::ChooseAccessPath(schema, wide.get(), true, false);
  CHECK(forced_index.name.find("IndexScan(idx_price)") != std::string::npos);
  test_passed("access_path_choice");
}

void test_engine_reports_estimate_and_actual() {
  fs::remove_all(kDir);
  fs::create_directories(kDir);
  Env env;
  Run(env, "CREATE TABLE Items (ItemID INT, ItemName VARCHAR(50), RetailPrice DECIMAL)");
  std::string sql = "INSERT INTO Items VALUES ";
  for (int i = 1; i <= 100; ++i) {
    if (i > 1) sql += ", ";
    char price[16];
    std::snprintf(price, sizeof(price), "%.2f", 0.5 * i);
    sql += "(" + std::to_string(i) + ", 'item_" + std::to_string(i) + "', " + price + ")";
  }
  Run(env, sql);
  Run(env, "ANALYZE Items");

  auto res = Run(env, "SELECT * FROM Items WHERE RetailPrice > 25");
  CHECK(res.has_plan_info);
  CHECK_EQ(res.chosen_path, "SeqScan+Filter");  // tiny table: sequential wins
  CHECK(res.estimated_cost > 0.0);
  CHECK(res.rows_estimated > 1.0);
  CHECK_EQ(res.rows_actual, res.rows.size());
  CHECK(res.actual_time_ms >= 0.0);
  CHECK(!res.plan_text.empty());
  CHECK(res.alternatives.empty());  // no index exists: only one candidate

  // With an index the rejected alternative is reported with its cost.
  Run(env, "CREATE INDEX idx_price ON Items (RetailPrice)");
  auto indexed = Run(env, "SELECT * FROM Items WHERE RetailPrice > 25");
  CHECK_EQ(indexed.alternatives.size(), 1u);
  CHECK(indexed.alternatives[0].name.find("IndexScan(idx_price)") != std::string::npos);
  CHECK(indexed.alternatives[0].cost > indexed.estimated_cost);
  CHECK(indexed.rules_line.empty());  // constraint_aware off: no rules section

  // Phase 4B hook: with constraint_aware on, EXPLAIN reports which rules fired
  // (Phase 4 has none, so the list is explicitly empty).
  Run(env, "SET constraint_aware = on");
  auto explained = Run(env, "EXPLAIN SELECT * FROM Items WHERE RetailPrice > 25");
  CHECK(explained.rows[0][0].find("Applied rules:") != std::string::npos);
  CHECK(explained.rows[0][0].find("none") != std::string::npos);
  CHECK_EQ(explained.rules_line.find("Applied rules:"), 0u);
  auto selected = Run(env, "SELECT * FROM Items WHERE RetailPrice > 25");
  CHECK_EQ(selected.rules_line.find("Applied rules:"), 0u);
  Run(env, "SET constraint_aware = off");
  auto unexplained = Run(env, "EXPLAIN SELECT * FROM Items WHERE RetailPrice > 25");
  CHECK(unexplained.rows[0][0].find("Applied rules:") == std::string::npos);
  test_passed("engine_reports_estimate_and_actual");
}

}  // namespace

int main() {
  test_histogram_equi_width();
  test_histogram_constant_column();
  test_empty_column_stats();
  test_catalog_stats_roundtrip();
  test_analyze_and_create_index_collect_stats();
  test_selectivity_equality_and_range();
  test_selectivity_defaults_without_stats();
  test_cost_functions();
  test_join_order_enumeration();
  test_access_path_choice();
  test_engine_reports_estimate_and_actual();
  std::printf("all optimizer tests passed\n");
  return 0;
}
