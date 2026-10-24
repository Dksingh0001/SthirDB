#include "index/bplus_tree.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
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
};

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

void test_random_insert_and_lookup() {
  Fixture f;
  std::mt19937_64 rng(20261024);
  std::uniform_int_distribution<int64_t> dist(-100000, 100000);

  std::vector<int64_t> keys;
  while (keys.size() < 2000) {
    const int64_t k = dist(rng);
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  for (int64_t k : keys) CHECK(f.tree.Insert(k, MakeRid(k)));

  for (int64_t k : keys) {
    const auto rids = f.tree.Lookup(k);
    CHECK_EQ(rids.size(), 1u);
    CHECK_EQ(rids[0].slot_id, MakeRid(k).slot_id);
  }

  std::vector<int64_t> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  const auto all = f.tree.RangeScan(-100000, 100000);
  CHECK_EQ(all.size(), sorted.size());
  for (size_t i = 0; i < all.size(); ++i) CHECK_EQ(all[i].slot_id, MakeRid(sorted[i]).slot_id);
  f.CheckNoLeaks();
  test_passed("random insert + lookup");
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
  for (int64_t k = 0; k < n; ++k) CHECK(f.tree.Insert(k, MakeRid(k)));
  for (int64_t k = 0; k < n; k += 97) CHECK_EQ(f.tree.Lookup(k).size(), 1u);
  const auto r = f.tree.RangeScan(9000, 9100);
  CHECK_EQ(r.size(), 101u);
  CHECK_EQ(f.tree.RangeScan(0, n).size(), static_cast<size_t>(n));
  f.CheckNoLeaks();
  std::printf("INFO large ascending: height=%u leaves=%u\n", f.tree.height(), f.tree.leaf_count());
  test_passed("10000 ascending keys");
}

}  // namespace

int main() {
  test_empty_tree();
  test_ascending_insert_and_lookup();
  test_random_insert_and_lookup();
  test_duplicate_keys();
  test_large_ascending_10000();
  std::printf("all bplus_tree tests passed\n");
  return 0;
}
