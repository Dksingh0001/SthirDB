#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "catalog/catalog.h"
#include "common/config.h"
#include "exec/execution_engine.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/row.h"

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
  std::printf(
      "SthirDB %s — disk-resident relational engine\n"
      "\n"
      "Usage:\n"
      "  sthirdb                     Print this help and exit (status 0).\n"
      "  sthirdb --version           Print version and exit.\n"
      "  sthirdb run \"<sql>\"         Execute a SQL query, DDL, or DML statement.\n"
      "  sthirdb explain \"<sql>\"     Plan tree with estimated rows/cost + rejected paths.\n"
      "  sthirdb optimizer \"<sql>\"   Chosen plan, estimated cost/rows and actual time.\n"
      "                             --seq / --index force one access path (measurement).\n"
      "  sthirdb analyze <table>     Refresh and persist column statistics.\n"
      "  sthirdb init_schema         Create ./data/retail and the Items table file.\n"
      "  sthirdb load_data --rows N  Bulk-insert N rows into Items.\n"
      "  sthirdb count               Count live rows in Items.\n",
      sthir::kVersion);
}

int CmdInitSchema() {
  std::error_code ec;
  fs::create_directories(sthir::kDataDir, ec);
  if (ec) {
    std::fprintf(stderr, "init_schema: cannot create directory %s: %s\n", sthir::kDataDir,
                 ec.message().c_str());
    return 1;
  }

  // Ensure catalog has Items entry
  sthir::Catalog catalog(sthir::kCatalogPath);
  catalog.Load();
  if (!catalog.HasTable("items")) {
    sthir::TableSchema schema;
    schema.table_name = "Items";
    schema.data_file_path = sthir::kItemsFilePath;
    schema.columns = {
        {"id", sthir::DataType::INT, 4, true, true, false},
        {"name", sthir::DataType::VARCHAR, 50, false, false, false},
        {"price", sthir::DataType::DECIMAL, 8, false, false, false}};
    schema.primary_key_columns = {"id"};
    catalog.CreateTable(schema);
  }

  sthir::DiskManager dm(sthir::kItemsFilePath);
  sthir::BufferPool bp(sthir::kDefaultPoolFrames, &dm);
  bool created_first_page = false;
  if (dm.num_pages() == 0) {
    const uint32_t pid = dm.allocate_page();
    sthir::Page* page = bp.fetch_page(pid);
    if (page == nullptr) {
      std::fprintf(stderr, "init_schema: buffer pool exhausted\n");
      return 1;
    }
    page->Reset(pid);
    bp.unpin_page(pid, true);
    bp.flush_all();
    created_first_page = true;
  }
  std::printf("init_schema: table Items ready at %s (%u page(s)%s)\n", sthir::kItemsFilePath,
              dm.num_pages(), created_first_page ? ", first page created" : ", already initialized");
  return 0;
}

int CmdLoadData(size_t rows) {
  sthir::DiskManager dm(sthir::kItemsFilePath);
  if (dm.num_pages() == 0) {
    std::fprintf(stderr, "load_data: no table file at %s; run 'sthirdb init_schema' first\n",
                 sthir::kItemsFilePath);
    return 1;
  }
  sthir::BufferPool bp(sthir::kDefaultPoolFrames, &dm);

  uint32_t cur = dm.num_pages() - 1;  // append onto the last page
  sthir::Page* page = bp.fetch_page(cur);
  if (page == nullptr) {
    std::fprintf(stderr, "load_data: buffer pool exhausted\n");
    return 1;
  }
  if (page->free_space_ptr() < sthir::kPageHeaderSize) page->Reset(cur);  // never-initialized page

  size_t inserted = 0;
  while (inserted < rows) {
    const size_t n = inserted + 1;
    const sthir::Row row = sthir::make_row(static_cast<int32_t>(n), "item_" + std::to_string(n),
                                           1.0 + 0.01 * static_cast<double>(inserted));
    char buf[sthir::kRowSize];
    sthir::serialize_row(row, buf);
    if (page->InsertTuple(buf, sthir::kRowSize) < 0) {
      // Current page is full: release it (it was modified), grow the file.
      bp.unpin_page(cur, true);
      cur = dm.allocate_page();
      page = bp.fetch_page(cur);
      if (page == nullptr) {
        std::fprintf(stderr, "load_data: buffer pool exhausted\n");
        return 1;
      }
      page->Reset(cur);
      continue;
    }
    ++inserted;
  }
  bp.unpin_page(cur, true);
  bp.flush_all();

  std::printf("load_data: inserted %zu row(s) into Items; file now %u page(s)\n", inserted,
              dm.num_pages());
  return 0;
}

int CmdCount() {
  sthir::DiskManager dm(sthir::kItemsFilePath);
  if (dm.num_pages() == 0) {
    std::fprintf(stderr, "count: no table file at %s; run 'sthirdb init_schema' first\n",
                 sthir::kItemsFilePath);
    return 1;
  }
  sthir::BufferPool bp(sthir::kDefaultPoolFrames, &dm);

  size_t live = 0;
  size_t physical = 0;
  for (uint32_t pid = 0; pid < dm.num_pages(); ++pid) {
    sthir::Page* page = bp.fetch_page(pid);
    if (page == nullptr) {
      std::fprintf(stderr, "count: buffer pool exhausted\n");
      return 1;
    }
    for (uint16_t slot = 0; slot < page->slot_count(); ++slot) {
      if (!page->tuple_alive(slot)) continue;
      char out[sthir::kRowSize];
      const int len = page->ReadTuple(slot, out);
      if (len != static_cast<int>(sthir::kRowSize)) continue;
      ++physical;
      const sthir::Row row = sthir::deserialize_row(out);
      if (!row.deleted) ++live;
    }
    bp.unpin_page(pid, false);
  }
  std::printf("count: %zu live row(s) in Items (%zu physical tuple(s), %u page(s))\n", live,
              physical, dm.num_pages());
  return 0;
}

int RunSql(const std::string& sql) {
  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "run: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  auto res = engine.Execute(sql);
  res.PrintTable(std::cout);
  return res.success ? 0 : 1;
}

int CmdRun(const std::string& sql) { return RunSql(sql); }

// EXPLAIN "<sql>": plan tree, predicted rows/cost per node, rejected candidates.
int CmdExplain(const std::string& sql) {
  if (sql.size() >= 7 && sql.compare(0, 7, "EXPLAIN") == 0) return RunSql(sql);
  return RunSql("EXPLAIN " + sql);
}

// ANALYZE <table>: refresh and persist column statistics.
int CmdAnalyze(const std::string& table) {
  if (table.empty()) {
    std::fprintf(stderr, "analyze: expected 'sthirdb analyze <table>'\n");
    return 2;
  }
  return RunSql("ANALYZE " + table);
}

// optimizer "<sql>": chosen plan, estimated cost/rows, actual time and rows.
int CmdOptimizer(const std::string& sql, bool force_seq = false, bool force_index = false) {
  sthir::Catalog catalog(sthir::kCatalogPath);
  std::string err;
  if (!catalog.Load(&err)) {
    std::fprintf(stderr, "optimizer: cannot load catalog: %s\n", err.c_str());
    return 1;
  }
  sthir::ExecutionEngine engine(&catalog, sthir::kDataDir);
  // Measurement hooks: run the same query on a forced path to compare runtimes.
  if (force_seq) engine.SetSessionFlag("enable_index_scan", "off");
  if (force_index) engine.SetSessionFlag("enable_seq_scan", "off");
  auto res = engine.Execute(sql);
  if (!res.success) {
    res.PrintTable(std::cout);
    return 1;
  }
  if (!res.has_plan_info) {
    std::fprintf(stderr, "optimizer: statement produced no plan\n");
    return 1;
  }

  std::printf("Chosen plan (%s):\n%s\n", res.chosen_path.c_str(), res.plan_text.c_str());
  if (!res.rules_line.empty()) std::printf("%s\n", res.rules_line.c_str());

  sthir::ExecutionResult summary;
  summary.is_query = true;
  summary.column_names = {"METRIC", "VALUE"};
  auto add = [&summary](const std::string& metric, const std::string& value) {
    summary.rows.push_back({metric, value});
  };
  char buf[64];
  add("chosen_path", res.chosen_path);
  std::snprintf(buf, sizeof(buf), "%.1f", res.estimated_cost);
  add("estimated_cost", buf);
  std::snprintf(buf, sizeof(buf), "%.0f", res.rows_estimated);
  add("rows_estimated", buf);
  std::snprintf(buf, sizeof(buf), "%.3f", res.actual_time_ms);
  add("actual_time_ms", buf);
  add("rows_actual", std::to_string(res.rows_actual));
  for (const auto& alternative : res.alternatives) {
    std::snprintf(buf, sizeof(buf), "cost=%.1f rows=%.0f", alternative.cost, alternative.rows);
    add("rejected " + alternative.name, buf);
  }
  if (res.rows_estimated > 0.0) {
    std::snprintf(buf, sizeof(buf), "%.2fx",
                  static_cast<double>(res.rows_actual) / res.rows_estimated);
    add("estimation_error", buf);
  }
  summary.PrintTable(std::cout);
  return 0;
}

bool ParseRowsArg(int argc, char** argv, size_t* out_rows) {
  if (argc != 4 || std::string(argv[2]) != "--rows") return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(argv[3], &end, 10);
  if (errno != 0 || end == argv[3] || *end != '\0' || v == 0) return false;
  *out_rows = static_cast<size_t>(v);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 0;
  }
  const std::string cmd = argv[1];

  if (cmd == "--version" || cmd == "-v") {
    std::printf("SthirDB %s\n", sthir::kVersion);
    return 0;
  }
  if (cmd == "run") {
    if (argc != 3) {
      std::fprintf(stderr, "run: expected 'sthirdb run \"<sql>\"'\n");
      return 2;
    }
    return CmdRun(argv[2]);
  }
  if (cmd == "explain") {
    if (argc != 3) {
      std::fprintf(stderr, "explain: expected 'sthirdb explain \"<sql>\"'\n");
      return 2;
    }
    return CmdExplain(argv[2]);
  }
  if (cmd == "optimizer") {
    if (argc == 3) return CmdOptimizer(argv[2]);
    if (argc == 4) {
      const std::string flag = argv[2];
      if (flag == "--seq") return CmdOptimizer(argv[3], true, false);
      if (flag == "--index") return CmdOptimizer(argv[3], false, true);
    }
    std::fprintf(stderr,
                 "optimizer: expected 'sthirdb optimizer [--seq|--index] \"<sql>\"'\n");
    return 2;
  }
  if (cmd == "analyze") {
    if (argc != 3) {
      std::fprintf(stderr, "analyze: expected 'sthirdb analyze <table>'\n");
      return 2;
    }
    return CmdAnalyze(argv[2]);
  }
  if (cmd == "init_schema") {
    if (argc != 2) {
      std::fprintf(stderr, "init_schema: takes no arguments\n");
      return 2;
    }
    return CmdInitSchema();
  }
  if (cmd == "load_data") {
    size_t rows = 0;
    if (!ParseRowsArg(argc, argv, &rows)) {
      std::fprintf(stderr, "load_data: expected 'load_data --rows N' with N a positive integer\n");
      return 2;
    }
    return CmdLoadData(rows);
  }
  if (cmd == "count") {
    if (argc != 2) {
      std::fprintf(stderr, "count: takes no arguments\n");
      return 2;
    }
    return CmdCount();
  }

  std::fprintf(stderr, "sthirdb: unknown command '%s'\n", cmd.c_str());
  PrintUsage();
  return 2;
}
