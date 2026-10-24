#include "index/bplus_tree.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_macros.h"

using sthir::BPlusTree;
using sthir::BufferPool;
using sthir::DiskManager;
using sthir::IndexRid;
using sthir::kDefaultPoolFrames;

namespace {

const char* kIndexPath = "build/test_bplus_tree.idx";

IndexRid MakeRid(int64_t key) {
  return IndexRid{static_cast<uint32_t>(key / 100 + 1), static_cast<uint16_t>(key % 1000)};
}

// Opens a fresh index file and an empty tree over it.
struct Fixture {
  DiskManager dm{kIndexPath};
  BufferPool bp{kDefaultPoolFrames, &dm};
  BPlusTree tree{&dm, &bp, sthir::kInvalidPageId, 0};

  Fixture() { std::remove(kIndexPath); }

  // Every page must be unpinned when the work is done (pin discipline).
  void CheckNoLeaks() { CHECK_EQ(bp.evictable_frames(), bp.pool_size()); }

  void Report(const char* what, double insert_ms) const {
    std::printf("INFO %s: height=%u leaves=%u pages=%u insert_ms=%.1f\n", what, tree.height(),
                tree.leaf_count(), dm.num_pages(), insert_ms);
  }
};

double MillisSince(std::chrono::steady_clock::time_point start) {
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void test_empty_tree() {
  Fixture f;
  CHECK(f.tree.empty());
  CHECK(f.tree.Lookup(42).empty());
  CHECK(f.tree.RangeScan(0, 100).empty());
  CHECK_EQ(f.tree.leaf_count(), 0u);
  test_passed("empty tree");
}

void test_ascending_insert_and_lookup() {
  Fixture f;
  const int64_t n = 200;
  for (int64_t k = 0; k < n; ++k) CHECK(f.tree.Insert(k, MakeRid(k)));

  CHECK(f.tree.height() >= 2);  // root grew at least once
  for (int64_t k = 0; k < n; ++k) {
    const auto rids = f.tree.Lookup(k);
    CHECK_EQ(rids.size(), 1u);
    CHECK_EQ(rids[0].page_id, MakeRid(k).page_id);
    CHECK_EQ(rids[0].slot_id, MakeRid(k).slot_id);
  }
  CHECK(f.tree.Lookup(n).empty());
  CHECK(f.tree.Lookup(-1).empty());

  const auto r = f.tree.RangeScan(50, 60);
  CHECK_EQ(r.size(), 11u);
  for (size_t i = 0; i < r.size(); ++i) {
    CHECK_EQ(r[i].slot_id, MakeRid(50 + static_cast<int64_t>(i)).slot_id);
  }

  // Full range in key order.
  const auto all = f.tree.RangeScan(0, n - 1);
  CHECK_EQ(all.size(), static_cast<size_t>(n));
  for (size_t i = 0; i < all.size(); ++i) {
    CHECK_EQ(all[i].slot_id, MakeRid(static_cast<int64_t>(i)).slot_id);
  }
  f.CheckNoLeaks();
  test_passed("ascending insert + lookup");
}

void test_duplicate_keys() {
  Fixture f;
  const int64_t k = 7;
  const int n = 300;
  // RIDs of equal keys may come back in any order, but every one exactly once.
  for (int i = 0; i < n; ++i) CHECK(f.tree.Insert(k, IndexRid{99, static_cast<uint16_t>(i)}));

  const auto rids = f.tree.Lookup(k);
  CHECK_EQ(rids.size(), static_cast<size_t>(n));
  std::vector<int> seen(n, 0);
  for (const auto& r : rids) {
    CHECK_EQ(r.page_id, 99u);
    CHECK(r.slot_id < n);
    ++seen[r.slot_id];
  }
  for (int i = 0; i < n; ++i) CHECK_EQ(seen[i], 1);

  CHECK(f.tree.Lookup(k + 1).empty());
  CHECK(f.tree.Lookup(k - 1).empty());
  CHECK_EQ(f.tree.RangeScan(k, k).size(), static_cast<size_t>(n));
  CHECK_EQ(f.tree.RangeScan(k - 1, k).size(), static_cast<size_t>(n));

  // A duplicate run surrounded by other keys is still found in full.
  for (int64_t other = 100; other < 140; ++other) CHECK(f.tree.Insert(other, MakeRid(other)));
  CHECK_EQ(f.tree.RangeScan(k, k).size(), static_cast<size_t>(n));
  CHECK_EQ(f.tree.RangeScan(100, 120).size(), 21u);
  CHECK_EQ(f.tree.RangeScan(0, 200).size(), static_cast<size_t>(n + 40));
  f.CheckNoLeaks();
  test_passed("duplicate keys");
}

void test_large_ascending_10000() {
  Fixture f;
  const int64_t n = 10000;
  const auto start = std::chrono::steady_clock::now();
  for (int64_t k = 0; k < n; ++k) CHECK(f.tree.Insert(k, MakeRid(k)));
  const double insert_ms = MillisSince(start);

  for (int64_t k = 0; k < n; ++k) {
    const auto rids = f.tree.Lookup(k);
    CHECK_EQ(rids.size(), 1u);
    CHECK_EQ(rids[0].slot_id, MakeRid(k).slot_id);
  }
  const auto r = f.tree.RangeScan(9000, 9100);
  CHECK_EQ(r.size(), 101u);
  CHECK_EQ(f.tree.RangeScan(0, n).size(), static_cast<size_t>(n));
  f.CheckNoLeaks();
  f.Report("10000 ascending keys", insert_ms);
  test_passed("10000 ascending keys");
}

void test_large_random_10000() {
  Fixture f;
  const size_t n = 10000;
  std::mt19937_64 rng(424242);
  std::uniform_int_distribution<int64_t> dist(-1000000, 1000000);
  std::unordered_set<int64_t> unique;
  std::vector<int64_t> keys;
  keys.reserve(n);
  while (keys.size() < n) {
    const int64_t k = dist(rng);
    if (unique.insert(k).second) keys.push_back(k);
  }

  const auto start = std::chrono::steady_clock::now();
  for (int64_t k : keys) CHECK(f.tree.Insert(k, MakeRid(k)));
  const double insert_ms = MillisSince(start);

  for (int64_t k : keys) {
    const auto rids = f.tree.Lookup(k);
    CHECK_EQ(rids.size(), 1u);
    CHECK_EQ(rids[0].slot_id, MakeRid(k).slot_id);
  }
  CHECK(f.tree.Lookup(2000000).empty());

  std::vector<int64_t> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  const auto all = f.tree.RangeScan(std::numeric_limits<int64_t>::min(),
                                    std::numeric_limits<int64_t>::max());
  CHECK_EQ(all.size(), n);
  for (size_t i = 0; i < all.size(); ++i) {
    CHECK_EQ(all[i].slot_id, MakeRid(sorted[i]).slot_id);
  }
  f.CheckNoLeaks();
  f.Report("10000 random keys", insert_ms);
  test_passed("10000 random keys");
}

void test_duplicate_run_5000() {
  Fixture f;
  const int distinct_keys = 50;
  const int per_key = 100;
  const auto start = std::chrono::steady_clock::now();
  // Interleave the keys so each key's run is scattered across many leaves.
  for (int i = 0; i < per_key; ++i) {
    for (int k = 0; k < distinct_keys; ++k) {
      const int64_t key = k * 10;
      const uint16_t slot = static_cast<uint16_t>(i * distinct_keys + k);
      CHECK(f.tree.Insert(key, IndexRid{7, slot}));
    }
  }
  const double insert_ms = MillisSince(start);

  for (int k = 0; k < distinct_keys; ++k) {
    const int64_t key = k * 10;
    const auto rids = f.tree.Lookup(key);
    CHECK_EQ(rids.size(), static_cast<size_t>(per_key));
    std::vector<int> seen(per_key, 0);
    for (const auto& r : rids) {
      CHECK_EQ(r.page_id, 7u);
      CHECK_EQ(r.slot_id % distinct_keys, k);  // this key's own group
      ++seen[r.slot_id / distinct_keys];
    }
    for (int i = 0; i < per_key; ++i) CHECK_EQ(seen[i], 1);
  }

  const auto all = f.tree.RangeScan(0, (distinct_keys - 1) * 10);
  CHECK_EQ(all.size(), static_cast<size_t>(distinct_keys * per_key));
  f.CheckNoLeaks();
  f.Report("5000 duplicate keys", insert_ms);
  test_passed("duplicate run spanning leaves");
}

}  // namespace

int main() {
  test_empty_tree();
  test_ascending_insert_and_lookup();
  test_duplicate_keys();
  test_duplicate_run_5000();
  test_large_ascending_10000();
  test_large_random_10000();
  std::printf("all bplus_tree tests passed\n");
  return 0;
}
