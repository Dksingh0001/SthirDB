#include "storage/page.h"

#include <cstring>
#include <string>
#include <vector>

#include "test_macros.h"

using sthir::Page;

namespace {

std::string make_tuple(uint8_t tag, uint16_t len) {
  std::string s(len, static_cast<char>('a' + tag % 26));
  s[0] = static_cast<char>(tag);
  return s;
}

void test_header_layout() {
  Page p;
  p.Reset(42);
  CHECK_EQ(p.page_id(), 42u);
  CHECK_EQ(p.page_lsn(), 0u);
  CHECK_EQ(p.slot_count(), 0);
  CHECK_EQ(p.free_space_ptr(), sthir::kPageSize);

  p.set_page_lsn(9001);
  p.set_page_id(7);
  CHECK_EQ(p.page_lsn(), 9001u);
  CHECK_EQ(p.page_id(), 7u);

  // Header lives inside the raw image: reload from bytes and re-verify.
  char image[sthir::kPageSize];
  std::memcpy(image, p.data(), sthir::kPageSize);
  Page q;
  std::memcpy(q.data(), image, sthir::kPageSize);
  CHECK_EQ(q.page_id(), 7u);
  CHECK_EQ(q.page_lsn(), 9001u);
  CHECK_EQ(q.slot_count(), 0);

  // Field offsets from the plan: page_id@0, page_lsn@4, slot_count@12, free@14.
  uint32_t pid;
  uint64_t lsn;
  std::memcpy(&pid, image, 4);
  std::memcpy(&lsn, image + 4, 8);
  CHECK_EQ(pid, 7u);
  CHECK_EQ(lsn, 9001u);
  test_passed("page_header_layout");
}

void test_insert_read_delete() {
  Page p;
  p.Reset(0);

  const std::string t0 = make_tuple(0, 63);
  const std::string t1 = make_tuple(1, 63);
  const std::string t2 = make_tuple(2, 63);

  CHECK_EQ(p.InsertTuple(t0.data(), 63), 0);
  CHECK_EQ(p.InsertTuple(t1.data(), 63), 1);
  CHECK_EQ(p.InsertTuple(t2.data(), 63), 2);
  CHECK_EQ(p.slot_count(), 3);

  // Tuples pack from the end: slot 0 at 8192-63, slot 1 below it, ...
  char out[63];
  CHECK_EQ(p.ReadTuple(0, out), 63);
  CHECK(std::memcmp(out, t0.data(), 63) == 0);
  CHECK_EQ(p.ReadTuple(2, out), 63);
  CHECK(std::memcmp(out, t2.data(), 63) == 0);
  CHECK_EQ(p.free_space_ptr(), sthir::kPageSize - 3 * 63);
  CHECK_EQ(p.usable_space(), sthir::kPageSize - sthir::kPageHeaderSize - 3 * (63 + 4));

  // Tombstone the middle tuple.
  CHECK(p.DeleteTuple(1));
  CHECK(!p.tuple_alive(1));
  CHECK_EQ(p.ReadTuple(1, out), -1);
  CHECK_EQ(p.slot_count(), 3);  // directory does not shrink until compact
  CHECK(!p.DeleteTuple(1));     // already tombstoned

  // The tombstoned slot is reused by the next insert.
  const std::string t3 = make_tuple(3, 63);
  const int slot = p.InsertTuple(t3.data(), 63);
  CHECK_EQ(slot, 1);
  CHECK_EQ(p.slot_count(), 3);
  CHECK_EQ(p.ReadTuple(1, out), 63);
  CHECK(std::memcmp(out, t3.data(), 63) == 0);
  // Slot 0 and 2 untouched.
  CHECK_EQ(p.ReadTuple(0, out), 63);
  CHECK(std::memcmp(out, t0.data(), 63) == 0);
  test_passed("page_insert_read_delete");
}

void test_fill_until_full() {
  Page p;
  p.Reset(0);

  const std::string tuple = make_tuple(9, 63);
  int inserted = 0;
  while (p.InsertTuple(tuple.data(), 63) >= 0) ++inserted;

  // 8192 - 16 header = 8176 bytes for 67 bytes per tuple (63 + 4 slot) => 122.
  CHECK_EQ(inserted, 122);
  CHECK_EQ(p.slot_count(), 122);
  CHECK(p.usable_space() < 63);

  char out[63];
  for (int i = 0; i < inserted; ++i) {
    CHECK_EQ(p.ReadTuple(static_cast<uint16_t>(i), out), 63);
  }
  test_passed("page_fill_until_full");
}

void test_tombstone_compact() {
  Page p;
  p.Reset(0);
  p.set_page_lsn(777);

  const std::string t0 = make_tuple(0, 63);
  const std::string t1 = make_tuple(1, 63);
  const std::string t2 = make_tuple(2, 63);
  p.InsertTuple(t0.data(), 63);
  p.InsertTuple(t1.data(), 63);
  p.InsertTuple(t2.data(), 63);

  // Fragment slots 0 and 2; live tuple is slot 1 only.
  CHECK(p.DeleteTuple(0));
  CHECK(p.DeleteTuple(2));
  const uint16_t fragmented_free = p.usable_space();

  p.compact();
  // Tombstones vanish: slot_count drops to the single live tuple.
  CHECK_EQ(p.slot_count(), 1);
  CHECK_EQ(p.page_lsn(), 777u);        // compact preserves page_lsn
  CHECK_EQ(p.page_id(), 0u);
  CHECK_EQ(p.free_space_ptr(), sthir::kPageSize - 63);
  CHECK(p.usable_space() > fragmented_free);

  char out[63];
  CHECK_EQ(p.ReadTuple(0, out), 63);
  CHECK(std::memcmp(out, t1.data(), 63) == 0);
  CHECK_EQ(p.ReadTuple(1, out), -1);      // no phantom second slot

  // After compaction the page accepts the full 122 tuples again.
  int inserted = 1;  // slot 0 already holds t1
  while (p.InsertTuple(t0.data(), 63) >= 0) ++inserted;
  CHECK_EQ(inserted, 122);
  test_passed("page_tombstone_compact");
}

}  // namespace

int main() {
  test_header_layout();
  test_insert_read_delete();
  test_fill_until_full();
  test_tombstone_compact();
  return 0;
}
