#include "storage/row.h"

#include <cstring>

#include "storage/page.h"
#include "test_macros.h"

using sthir::Row;
using sthir::deserialize_row;
using sthir::make_row;
using sthir::row_name;
using sthir::serialize_row;

namespace {

void test_round_trip() {
  Row r = make_row(42, "Widget", 9.99);
  CHECK_EQ(sthir::kRowSize, 63u);

  char buf[sthir::kRowSize];
  serialize_row(r, buf);
  Row back = deserialize_row(buf);
  CHECK_EQ(back.id, 42);
  CHECK_EQ(row_name(back), std::string("Widget"));
  CHECK(back.price > 9.98 && back.price < 10.00);
  CHECK(!back.deleted);
  test_passed("row_round_trip");
}

void test_fixed_name_field() {
  Row r = make_row(1, "short", 1.0);
  // Fixed 50-byte field is NUL-padded past the string.
  for (size_t i = 5; i < sthir::kRowNameSize; ++i) CHECK_EQ(r.name[i], '\0');

  // Names longer than 50 bytes are truncated, never overflow.
  const std::string long_name(80, 'x');
  Row t = make_row(2, long_name, 2.0);
  CHECK_EQ(row_name(t).size(), sthir::kRowNameSize);
  test_passed("row_fixed_name_field");
}

void test_deleted_flag() {
  Row r = make_row(7, "Gadget", 3.5, /*deleted=*/true);
  char buf[sthir::kRowSize];
  serialize_row(r, buf);
  CHECK_EQ(buf[62], 1);
  CHECK(deserialize_row(buf).deleted);

  r.deleted = false;
  serialize_row(r, buf);
  CHECK_EQ(buf[62], 0);
  CHECK(!deserialize_row(buf).deleted);
  test_passed("row_deleted_flag");
}

void test_row_in_page() {
  sthir::Page p;
  p.Reset(0);
  Row r = make_row(5, "Sprocket", 19.49);

  char buf[sthir::kRowSize];
  serialize_row(r, buf);
  const int slot = p.InsertTuple(buf, sthir::kRowSize);
  CHECK(slot >= 0);

  char out[sthir::kRowSize];
  CHECK_EQ(p.ReadTuple(static_cast<uint16_t>(slot), out), (int)sthir::kRowSize);
  Row back = deserialize_row(out);
  CHECK_EQ(back.id, 5);
  CHECK_EQ(row_name(back), std::string("Sprocket"));
  CHECK(back.price > 19.4 && back.price < 19.6);
  test_passed("row_in_page");
}

}  // namespace

int main() {
  test_round_trip();
  test_fixed_name_field();
  test_deleted_flag();
  test_row_in_page();
  return 0;
}
