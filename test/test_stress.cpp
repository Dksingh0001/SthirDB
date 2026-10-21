#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/config.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/row.h"
#include "test_macros.h"

using sthir::BufferPool;
using sthir::DiskManager;
using sthir::Page;
using sthir::Row;

namespace {

// Writes `rows` tuples through the buffer pool into a fresh table file, then
// reads every page back and verifies every field. `frames` controls the pool:
// 64 is the spec pool (plan Phase 1), 8 forces continuous eviction cycles.
void run_stress(const char* label, size_t rows, size_t frames) {
  const std::string path = "./build/test_stress.dat";
  std::remove(path.c_str());

  DiskManager dm(path);
  BufferPool bp(frames, &dm);

  // ---- write phase ----
  const auto t0 = std::chrono::steady_clock::now();
  uint32_t cur = dm.allocate_page();
  Page* page = bp.fetch_page(cur);
  CHECK(page != nullptr);
  page->Reset(cur);

  size_t inserted = 0;
  while (inserted < rows) {
    const size_t n = inserted + 1;
    const Row row = sthir::make_row(static_cast<int32_t>(n), "item_" + std::to_string(n),
                                    1.0 + 0.01 * static_cast<double>(inserted));
    char buf[sthir::kRowSize];
    sthir::serialize_row(row, buf);
    if (page->InsertTuple(buf, sthir::kRowSize) < 0) {
      bp.unpin_page(cur, true);
      cur = dm.allocate_page();
      page = bp.fetch_page(cur);
      CHECK(page != nullptr);
      page->Reset(cur);
      continue;
    }
    ++inserted;
  }
  bp.unpin_page(cur, true);
  bp.flush_all();
  const auto t1 = std::chrono::steady_clock::now();

  // ---- read-back verification ----
  const auto t2 = std::chrono::steady_clock::now();
  size_t seen = 0;
  size_t mismatches = 0;
  size_t expected_id = 1;
  for (uint32_t pid = 0; pid < dm.num_pages(); ++pid) {
    Page* p = bp.fetch_page(pid);
    CHECK(p != nullptr);
    for (uint16_t slot = 0; slot < p->slot_count(); ++slot) {
      if (!p->tuple_alive(slot)) {
        ++mismatches;  // no tombstones expected in a fresh load
        continue;
      }
      char out[sthir::kRowSize];
      if (p->ReadTuple(slot, out) != static_cast<int>(sthir::kRowSize)) {
        ++mismatches;
        continue;
      }
      const Row r = sthir::deserialize_row(out);
      if (r.id != static_cast<int32_t>(expected_id)) ++mismatches;
      if (sthir::row_name(r) != "item_" + std::to_string(expected_id)) ++mismatches;
      const double want = 1.0 + 0.01 * static_cast<double>(expected_id - 1);
      if (std::fabs(r.price - want) > 1e-6) ++mismatches;
      if (r.deleted) ++mismatches;
      ++expected_id;
      ++seen;
    }
    bp.unpin_page(pid, false);
  }
  const auto t3 = std::chrono::steady_clock::now();

  const double write_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double read_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

  std::printf(
      "stress[%s]: frames=%zu rows=%zu pages=%u write_ms=%.2f read_ms=%.2f "
      "verified=%zu mismatches=%zu\n",
      label, frames, rows, dm.num_pages(), write_ms, read_ms, seen, mismatches);

  CHECK_EQ(seen, rows);
  CHECK_EQ(mismatches, 0u);
  test_passed(label);
}

}  // namespace

int main() {
  // Spec pool from the plan: 5,000 rows with 64 frames.
  run_stress("stress_64_frames", 5000, 64);
  // Same workload with 8 frames: every pass over the data evicts and reloads.
  run_stress("stress_8_frames_forced_eviction", 5000, 8);
  std::remove("./build/test_stress.dat");
  return 0;
}
