#include "storage/page.h"

#include <cstring>
#include <vector>

namespace sthir {

namespace {

template <typename T>
T ReadField(const char* base, uint32_t offset) {
  T value;
  std::memcpy(&value, base + offset, sizeof(T));
  return value;
}

template <typename T>
void WriteField(char* base, uint32_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof(T));
}

}  // namespace

void Page::Reset(uint32_t page_id) {
  std::memset(data_, 0, kPageSize);
  set_page_id(page_id);
  set_page_lsn(0);
  set_slot_count(0);
  set_free_space_ptr(static_cast<uint16_t>(kPageSize));
}

uint32_t Page::page_id() const { return ReadField<uint32_t>(data_, 0); }

void Page::set_page_id(uint32_t page_id) { WriteField<uint32_t>(data_, 0, page_id); }

uint64_t Page::page_lsn() const { return ReadField<uint64_t>(data_, 4); }

void Page::set_page_lsn(uint64_t lsn) { WriteField<uint64_t>(data_, 4, lsn); }

uint16_t Page::slot_count() const { return ReadField<uint16_t>(data_, 12); }

void Page::set_slot_count(uint16_t n) { WriteField<uint16_t>(data_, 12, n); }

uint16_t Page::free_space_ptr() const { return ReadField<uint16_t>(data_, 14); }

void Page::set_free_space_ptr(uint16_t p) { WriteField<uint16_t>(data_, 14, p); }

Page::Slot Page::slot_at(uint16_t i) const {
  Slot s;
  const uint32_t off = kPageHeaderSize + i * kSlotEntrySize;
  s.offset = ReadField<uint16_t>(data_, off);
  s.length = ReadField<uint16_t>(data_, off + 2);
  return s;
}

void Page::write_slot(uint16_t i, const Slot& s) {
  const uint32_t off = kPageHeaderSize + i * kSlotEntrySize;
  WriteField<uint16_t>(data_, off, s.offset);
  WriteField<uint16_t>(data_, off + 2, s.length);
}

uint16_t Page::slot_dir_end() const {
  return static_cast<uint16_t>(kPageHeaderSize + slot_count() * kSlotEntrySize);
}

int Page::find_tombstone() const {
  const uint16_t n = slot_count();
  for (uint16_t i = 0; i < n; ++i) {
    if (slot_at(i).length == 0) return static_cast<int>(i);
  }
  return -1;
}

int Page::InsertTuple(const char* bytes, uint16_t len) {
  if (len == 0 || len >= kPageSize) return -1;

  const int reuse = find_tombstone();
  const uint16_t dir_end = slot_dir_end();
  // Defensive: a zeroed (never initialized) page has free_space_ptr == 0,
  // which would underflow the subtraction below.
  if (free_space_ptr() < dir_end) return -1;
  const uint16_t free_bytes = static_cast<uint16_t>(free_space_ptr() - dir_end);
  const uint16_t extra = (reuse >= 0) ? 0 : static_cast<uint16_t>(kSlotEntrySize);
  if (free_bytes < static_cast<uint16_t>(len + extra)) return -1;

  const uint16_t tuple_off = static_cast<uint16_t>(free_space_ptr() - len);
  std::memcpy(data_ + tuple_off, bytes, len);
  set_free_space_ptr(tuple_off);

  uint16_t slot;
  if (reuse >= 0) {
    slot = static_cast<uint16_t>(reuse);
  } else {
    slot = slot_count();
    set_slot_count(static_cast<uint16_t>(slot_count() + 1));
  }
  write_slot(slot, Slot{tuple_off, len});
  return static_cast<int>(slot);
}

int Page::ReadTuple(uint16_t slot, char* out) const {
  if (slot >= slot_count()) return -1;
  const Slot s = slot_at(slot);
  if (s.length == 0) return -1;  // tombstone
  std::memcpy(out, data_ + s.offset, s.length);
  return s.length;
}

bool Page::DeleteTuple(uint16_t slot) {
  if (slot >= slot_count()) return false;
  if (slot_at(slot).length == 0) return false;
  write_slot(slot, Slot{0, 0});
  return true;
}

bool Page::WriteTupleAt(uint16_t slot, const char* bytes, uint16_t len) {
  if (bytes == nullptr || len == 0) {
    if (slot < slot_count()) {
      write_slot(slot, Slot{0, 0});
      return true;
    }
    return false;
  }

  // Ensure slot count covers slot
  if (slot >= slot_count()) {
    uint16_t old_count = slot_count();
    set_slot_count(slot + 1);
    for (uint16_t i = old_count; i < slot; ++i) {
      write_slot(i, Slot{0, 0});
    }
  }

  // If slot already has enough space in-place:
  Slot current = slot_at(slot);
  if (current.length > 0 && current.length >= len) {
    std::memcpy(data_ + current.offset, bytes, len);
    write_slot(slot, Slot{current.offset, len});
    return true;
  }

  // Otherwise, allocate new space from free_space_ptr
  const uint16_t dir_end = slot_dir_end();
  if (free_space_ptr() < dir_end || free_space_ptr() - dir_end < len) {
    return false;
  }

  const uint16_t tuple_off = static_cast<uint16_t>(free_space_ptr() - len);
  std::memcpy(data_ + tuple_off, bytes, len);
  set_free_space_ptr(tuple_off);
  write_slot(slot, Slot{tuple_off, len});
  return true;
}

void Page::compact() {
  // Collect live tuples in slot order, then rebuild the page image.
  std::vector<char> live;
  std::vector<uint16_t> lengths;
  const uint16_t n = slot_count();
  for (uint16_t i = 0; i < n; ++i) {
    const Slot s = slot_at(i);
    if (s.length == 0) continue;
    const size_t at = live.size();
    live.resize(at + s.length);
    std::memcpy(live.data() + at, data_ + s.offset, s.length);
    lengths.push_back(s.length);
  }

  const uint32_t pid = page_id();
  const uint64_t lsn = page_lsn();
  Reset(pid);
  set_page_lsn(lsn);
  set_slot_count(static_cast<uint16_t>(lengths.size()));

  // Place tuples back-to-front; slot directory keeps live slot order.
  uint16_t cursor = static_cast<uint16_t>(kPageSize);
  size_t pos = 0;
  for (uint16_t i = 0; i < lengths.size(); ++i) {
    const uint16_t len = lengths[i];
    cursor = static_cast<uint16_t>(cursor - len);
    std::memcpy(data_ + cursor, live.data() + pos, len);
    pos += len;
    write_slot(i, Slot{cursor, len});
  }
  set_free_space_ptr(cursor);
}

bool Page::tuple_alive(uint16_t slot) const {
  return slot < slot_count() && slot_at(slot).length != 0;
}

uint16_t Page::tuple_length(uint16_t slot) const {
  if (slot >= slot_count()) return 0;
  return slot_at(slot).length;
}

uint16_t Page::usable_space() const {
  return static_cast<uint16_t>(free_space_ptr() - slot_dir_end());
}

}  // namespace sthir
