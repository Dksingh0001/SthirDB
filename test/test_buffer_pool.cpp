#include "storage/buffer_pool.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "common/config.h"
#include "storage/page.h"
#include "storage/replacer.h"
#include "test_macros.h"

using sthir::BufferPool;
using sthir::DiskManager;
using sthir::LruReplacer;
using sthir::Page;

namespace {

std::string test_path(const char* name) { return std::string("./build/test_") + name + ".dat"; }

void fill_tag(Page* p, uint32_t pid, char tag) {
  p->Reset(pid);
  char tuple[sthir::kRowSize];
  std::string body(sthir::kRowSize, tag);
  body[0] = tag;
  std::memcpy(tuple, body.data(), sthir::kRowSize);
  p->InsertTuple(tuple, sthir::kRowSize);
}

void check_tag(Page* p, uint32_t pid, char tag) {
  CHECK_EQ(p->page_id(), pid);
  CHECK_EQ(p->slot_count(), 1);
  char out[sthir::kRowSize];
  CHECK_EQ(p->ReadTuple(0, out), (int)sthir::kRowSize);
  CHECK_EQ(out[0], tag);
}

void test_replacer_contract() {
  LruReplacer r(3);
  CHECK_EQ(r.size(), 0u);
  CHECK(!r.evict().has_value());

  // Evictable frames enter at MRU; only they can be evicted.
  r.set_evictable(0, true);
  r.set_evictable(1, true);
  r.record_access(0);           // 0 becomes MRU
  CHECK_EQ(r.size(), 2u);
  const int victim = *r.evict();  // LRU is 1, not 0
  CHECK_EQ(victim, 1);
  CHECK_EQ(r.size(), 1u);

  // Pinned frame: removed from the list, evict skips it entirely.
  r.set_evictable(0, false);
  CHECK_EQ(r.size(), 0u);
  CHECK(!r.evict().has_value());
  r.set_evictable(0, true);     // unpinned again -> re-enters as MRU
  r.set_evictable(2, true);
  r.remove(0);                  // explicit removal (e.g. frame reassigned)
  CHECK_EQ(r.size(), 1u);
  CHECK_EQ(*r.evict(), 2);
  test_passed("replacer_lru_contract");
}

void test_pin_protocol(const std::string& path) {
  std::remove(path.c_str());
  DiskManager dm(path);
  BufferPool bp(3, &dm);

  // Allocate pages 0..2 at the disk level.
  for (int i = 0; i < 3; ++i) CHECK_EQ(dm.allocate_page(), (uint32_t)i);

  Page* a = bp.fetch_page(0);
  CHECK(a != nullptr);
  CHECK_EQ(bp.pin_count(0), 1);
  CHECK_EQ(bp.evictable_frames(), 0u);

  // Double pin: same frame, pin count 2; not evictable until both released.
  Page* a2 = bp.fetch_page(0);
  CHECK(a2 == a);
  CHECK_EQ(bp.pin_count(0), 2);
  CHECK(bp.unpin_page(0, false));
  CHECK_EQ(bp.pin_count(0), 1);
  CHECK_EQ(bp.evictable_frames(), 0u);
  CHECK(bp.unpin_page(0, false));
  CHECK_EQ(bp.pin_count(0), 0);
  CHECK_EQ(bp.evictable_frames(), 1u);

  // Unbalanced unpin and unknown page are rejected, not UB.
  CHECK(!bp.unpin_page(0, false));
  CHECK(!bp.unpin_page(999, false));
  test_passed("buffer_pool_pin_protocol");
}

void test_pool_exhaustion(const std::string& path) {
  DiskManager dm(path);
  BufferPool bp(3, &dm);

  // Pin all three frames (pages 0..2 already exist from the previous test).
  CHECK(bp.fetch_page(0) != nullptr);
  CHECK(bp.fetch_page(1) != nullptr);
  CHECK(bp.fetch_page(2) != nullptr);
  CHECK_EQ(bp.evictable_frames(), 0u);

  dm.allocate_page();  // page 3 exists on disk but has no free frame
  CHECK(bp.fetch_page(3) == nullptr);

  // Release one pin and try again.
  CHECK(bp.unpin_page(0, false));
  CHECK(bp.fetch_page(3) != nullptr);
  CHECK(bp.unpin_page(1, false));
  CHECK(bp.unpin_page(2, false));
  CHECK(bp.unpin_page(3, false));
  test_passed("buffer_pool_exhaustion");
}

void test_dirty_writeback_on_evict(const std::string& path) {
  std::remove(path.c_str());
  DiskManager dm(path);
  // 3 frames but 6 pages: every page after the third forces an eviction, and
  // every evicted frame is dirty, so the pool must write it back first.
  BufferPool bp(3, &dm);

  const char tags[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
  for (uint32_t i = 0; i < 6; ++i) {
    CHECK_EQ(dm.allocate_page(), i);
    Page* p = bp.fetch_page(i);
    CHECK(p != nullptr);
    fill_tag(p, i, tags[i]);
    CHECK(bp.unpin_page(i, true));
  }

  // Re-read everything: contents must have survived writeback + reload.
  for (uint32_t i = 0; i < 6; ++i) {
    Page* p = bp.fetch_page(i);
    CHECK(p != nullptr);
    check_tag(p, i, tags[i]);
    CHECK(bp.unpin_page(i, false));
  }
  test_passed("buffer_pool_dirty_writeback");
}

void test_dirty_marking(const std::string& path) {
  DiskManager dm(path);
  BufferPool bp(64, &dm);

  Page* p = bp.fetch_page(0);
  CHECK(p != nullptr);
  CHECK(!bp.is_dirty(0));
  fill_tag(p, 0, 'z');
  CHECK(bp.unpin_page(0, true));
  CHECK(bp.is_dirty(0));

  // flush_page persists and clears the flag.
  CHECK(bp.flush_page(0));
  CHECK(!bp.is_dirty(0));
  CHECK(!bp.flush_page(0));  // nothing to do

  // A clean page can be evicted and reloaded identically.
  Page* p1 = bp.fetch_page(0);
  check_tag(p1, 0, 'z');
  bp.unpin_page(0, false);
  test_passed("buffer_pool_dirty_marking");
}

void test_flush_all_persists(const std::string& path) {
  std::remove(path.c_str());
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  CHECK_EQ(dm.allocate_page(), 0u);

  Page* p = bp.fetch_page(0);
  fill_tag(p, 0, 'q');
  bp.unpin_page(0, true);
  // No explicit flush: the destructor must persist dirty residents.
  bp.flush_all();

  char buf[sthir::kPageSize];
  dm.read_page(0, buf);
  Page on_disk;
  std::memcpy(on_disk.data(), buf, sthir::kPageSize);
  check_tag(&on_disk, 0, 'q');
  test_passed("buffer_pool_flush_all");
}

}  // namespace

int main() {
  test_replacer_contract();
  const std::string path = test_path("buffer_pool");
  test_pin_protocol(path);
  test_pool_exhaustion(path);
  test_dirty_writeback_on_evict(path);
  test_dirty_marking(path);
  test_flush_all_persists(path);
  std::remove(path.c_str());
  return 0;
}
