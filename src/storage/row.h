#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "common/config.h"

namespace sthir {

// Fixed Phase 1 demo row for the Items table:
//   [int32 id][char[50] name][double price][bool deleted]  = 63 bytes
// `name` is a fixed 50-byte NUL-padded field; `deleted` supports logical
// delete from Phase 2 (slot tombstones handle physical reuse).
struct Row {
  int32_t id = 0;
  char name[kRowNameSize] = {};
  double price = 0.0;
  bool deleted = false;
};

inline void serialize_row(const Row& row, char* out) {
  std::memcpy(out, &row.id, kRowIdSize);
  std::memcpy(out + kRowIdSize, row.name, kRowNameSize);
  std::memcpy(out + kRowIdSize + kRowNameSize, &row.price, kRowPriceSize);
  out[kRowIdSize + kRowNameSize + kRowPriceSize] = row.deleted ? 1 : 0;
}

inline Row deserialize_row(const char* in) {
  Row row;
  std::memcpy(&row.id, in, kRowIdSize);
  std::memcpy(row.name, in + kRowIdSize, kRowNameSize);
  std::memcpy(&row.price, in + kRowIdSize + kRowNameSize, kRowPriceSize);
  row.deleted = in[kRowIdSize + kRowNameSize + kRowPriceSize] != 0;
  return row;
}

// Builds a row with `name` copied into the fixed field (truncated or padded).
inline Row make_row(int32_t id, const std::string& name, double price, bool deleted = false) {
  Row row;
  row.id = id;
  const size_t n = name.size() < kRowNameSize ? name.size() : kRowNameSize;
  std::memcpy(row.name, name.data(), n);
  row.price = price;
  row.deleted = deleted;
  return row;
}

inline std::string row_name(const Row& row) {
  size_t n = 0;
  while (n < kRowNameSize && row.name[n] != '\0') ++n;
  return std::string(row.name, n);
}

}  // namespace sthir
