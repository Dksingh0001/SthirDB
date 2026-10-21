#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/config.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "test_macros.h"

using sthir::DiskManager;
using sthir::Page;

namespace {

std::string test_path(const char* name) {
  return std::string("./build/test_") + name + ".dat";
}

void test_offsets_and_allocate(const std::string& path) {
  std::remove(path.c_str());
  DiskManager dm(path);
  CHECK_EQ(dm.num_pages(), 0u);

  const uint32_t p0 = dm.allocate_page();
  const uint32_t p1 = dm.allocate_page();
  const uint32_t p2 = dm.allocate_page();
  CHECK_EQ(p0, 0u);
  CHECK_EQ(p1, 1u);
  CHECK_EQ(p2, 2u);
  CHECK_EQ(dm.num_pages(), 3u);
  test_passed("disk_allocate");
}

void test_write_read_roundtrip(const std::string& path) {
  DiskManager dm(path);
  CHECK_EQ(dm.num_pages(), 3u);

  // Distinct recognizable image per page.
  std::vector<Page> pages(3);
  for (uint32_t i = 0; i < 3; ++i) {
    pages[i].Reset(i);
    pages[i].set_page_lsn(1000 + i);
    char tuple[63];
    std::string body(63, static_cast<char>('A' + i));
    body[0] = static_cast<char>('0' + i);
    std::memcpy(tuple, body.data(), 63);
    pages[i].InsertTuple(tuple, 63);
    dm.write_page(i, pages[i].data());
  }

  for (uint32_t i = 0; i < 3; ++i) {
    char buf[sthir::kPageSize];
    dm.read_page(i, buf);
    Page got;
    std::memcpy(got.data(), buf, sthir::kPageSize);
    CHECK_EQ(got.page_id(), i);
    CHECK_EQ(got.page_lsn(), 1000 + i);
    CHECK_EQ(got.slot_count(), 1);
    char out[63];
    CHECK_EQ(got.ReadTuple(0, out), 63);
    CHECK_EQ(out[0], static_cast<char>('0' + i));
  }
  test_passed("disk_write_read_roundtrip");
}

void test_offset_addressing(const std::string& path) {
  // Page i must live at exactly offset i * 8192 (verified by overwriting page 2
  // and confirming pages 0 and 1 stay intact).
  DiskManager dm(path);
  char before0[sthir::kPageSize];
  dm.read_page(0, before0);

  Page replacement;
  replacement.Reset(2);
  replacement.set_page_lsn(55555);
  dm.write_page(2, replacement.data());

  char after0[sthir::kPageSize];
  dm.read_page(0, after0);
  CHECK(std::memcmp(before0, after0, sthir::kPageSize) == 0);

  char buf2[sthir::kPageSize];
  dm.read_page(2, buf2);
  Page p2;
  std::memcpy(p2.data(), buf2, sthir::kPageSize);
  CHECK_EQ(p2.page_lsn(), 55555u);
  CHECK_EQ(p2.slot_count(), 0);
  test_passed("disk_offset_addressing");
}

void test_reopen_persists(const std::string& path) {
  {
    DiskManager dm(path);
    CHECK_EQ(dm.num_pages(), 3u);
  }
  DiskManager dm(path);  // fresh open: page count comes back from file size
  CHECK_EQ(dm.num_pages(), 3u);
  test_passed("disk_reopen_persists");
}

void test_read_beyond_eof(const std::string& path) {
  DiskManager dm(path);
  char buf[sthir::kPageSize];
  std::memset(buf, 0x7F, sthir::kPageSize);
  dm.read_page(999, buf);
  for (size_t i = 0; i < sthir::kPageSize; ++i) CHECK_EQ(buf[i], 0);
  test_passed("disk_read_beyond_eof");
}

}  // namespace

int main() {
  const std::string path = test_path("disk_manager");
  test_offsets_and_allocate(path);
  test_write_read_roundtrip(path);
  test_offset_addressing(path);
  test_reopen_persists(path);
  test_read_beyond_eof(path);
  std::remove(path.c_str());
  return 0;
}
