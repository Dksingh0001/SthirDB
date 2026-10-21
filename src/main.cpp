#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "common/config.h"
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
