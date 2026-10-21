#pragma once

#include <cstdint>

#include "common/config.h"

namespace sthir {

// Fixed 8 KB slotted page.
//
// Layout inside data_:
//   [0 .. 16)                      header
//   [16 .. 16 + slot_count*4)      slot directory (grows forward)
//   [slot_dir_end .. free_ptr)     free space
//   [free_ptr .. 8192)             tuples (grow backward from the page end)
//
// Header fields (all little-endian host layout via memcpy):
//   uint32 page_id        — page identifier within the table file
//   uint64 page_lsn       — LSN of last modifying log record (reserved in
//                           Phase 1; written from Phase 5 for ARIES redo)
//   uint16 slot_count     — number of slot-directory entries
//   uint16 free_space_ptr — offset of the lowest-addressed tuple; starts at
//                           kPageSize on an empty page and decreases on insert
//
// Slot entry: uint16 offset, uint16 length. length == 0 marks a tombstone —
// the slot is free for reuse but its tuple space is reclaimed only by
// compact().
class Page {
 public:
  Page() { Reset(0); }

  // Reinitialize as an empty page belonging to `page_id`. Keeps page_lsn at 0.
  void Reset(uint32_t page_id);

  uint32_t page_id() const;
  void set_page_id(uint32_t page_id);
  uint64_t page_lsn() const;
  void set_page_lsn(uint64_t lsn);
  uint16_t slot_count() const;
  void set_slot_count(uint16_t n);
  uint16_t free_space_ptr() const;
  void set_free_space_ptr(uint16_t p);

  // Inserts `len` bytes; reuses a tombstoned slot when one exists.
  // Returns the slot id, or -1 when the page cannot hold the tuple.
  int InsertTuple(const char* bytes, uint16_t len);

  // Copies the tuple at `slot` into `out` (caller provides >= kRowSize or the
  // recorded length). Returns the tuple length, or -1 if the slot is invalid
  // or tombstoned.
  int ReadTuple(uint16_t slot, char* out) const;

  // Tombstones the slot. Returns false if the slot is invalid or already free.
  bool DeleteTuple(uint16_t slot);

  // Moves every live tuple to the end of the page and rebuilds the slot
  // directory, reclaiming fragmented and tombstoned space. Live slot order is
  // preserved (tombstones vanish).
  void compact();

  bool tuple_alive(uint16_t slot) const;
  uint16_t tuple_length(uint16_t slot) const;

  // Bytes insertable right now: tombstone slots need no new directory entry,
  // so this reports the conservative no-reuse figure.
  uint16_t usable_space() const;

  char* data() { return data_; }
  const char* data() const { return data_; }

 private:
  struct Slot {
    uint16_t offset;
    uint16_t length;
  };

  Slot slot_at(uint16_t i) const;
  void write_slot(uint16_t i, const Slot& s);
  int find_tombstone() const;
  uint16_t slot_dir_end() const;

  char data_[kPageSize];
};

}  // namespace sthir
