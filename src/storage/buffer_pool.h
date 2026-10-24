#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/replacer.h"

namespace sthir {

// Fixed-size frame cache over a DiskManager.
//
// Pin protocol (the invariant every caller follows):
//   Page* p = bp.fetch_page(id);   // pin +1; page stays resident while pinned
//   ... use p ...
//   bp.unpin_page(id, is_dirty);   // pin -1; mark dirty if modified
// Eviction may only pick frames whose pin count is 0; dirty victims are
// written back through the disk manager (the crash-injection hook lives there)
// before the frame is reused. One latch guards the whole pool, so
// fetch_page/unpin_page are thread-safe.
class BufferPool {
 public:
  BufferPool(std::size_t pool_size, DiskManager* dm);
  ~BufferPool();

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Pins the page, loading it from disk on a miss. Returns nullptr only when
  // every frame is pinned (pool exhausted by callers).
  Page* fetch_page(uint32_t page_id);

  // Unpins; marks the frame dirty when is_dirty is true. Returns false when
  // the page is not in the pool or was not pinned (caller bug).
  bool unpin_page(uint32_t page_id, bool is_dirty);

  // Writes the page back if dirty (leaves it resident and pinned-state intact).
  bool flush_page(uint32_t page_id);

  // Writes every dirty resident page back to disk.
  void flush_all();

  int pin_count(uint32_t page_id) const;
  bool is_dirty(uint32_t page_id) const;
  std::size_t pool_size() const { return frames_.size(); }
  std::size_t evictable_frames() const;

  void set_wal_manager(class WalManager* wal) { wal_ = wal; }
  class WalManager* wal_manager() const { return wal_; }

 private:
  std::optional<int> take_frame();  // free list first, then LRU eviction

  mutable std::mutex latch_;
  DiskManager* dm_;
  class WalManager* wal_ = nullptr;
  std::vector<Page> frames_;
  std::vector<int> pin_count_;
  std::vector<bool> dirty_;
  std::vector<bool> present_;
  std::vector<uint32_t> frame_page_id_;
  std::unordered_map<uint32_t, int> page_to_frame_;
  std::vector<int> free_list_;
  LruReplacer replacer_;
};

}  // namespace sthir
