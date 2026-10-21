#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sthir {

// Replacer contract (plan Phase 1): tracks which frames are *evictable*
// (pin count 0) and orders them for eviction.
//
//   record_access(frame)     — remember this frame was just used
//   set_evictable(f, true)   — frame reached pin count 0; eligible for eviction
//   set_evictable(f, false)  — frame pinned again; not eligible
//   evict()                  — hand back the least-recently-used evictable frame
//   remove(f)                — drop f entirely (evicted or pinned)
//   size()                   — number of evictable frames
class Replacer {
 public:
  virtual ~Replacer() = default;
  virtual void record_access(int frame_id) = 0;
  virtual void set_evictable(int frame_id, bool evictable) = 0;
  virtual std::optional<int> evict() = 0;
  virtual void remove(int frame_id) = 0;
  virtual std::size_t size() const = 0;
};

// LRU policy: list front = most recently used, back = eviction candidate.
// Only evictable frames are ever in the list; pinned frames are absent, so
// evict() can never return a pinned frame.
class LruReplacer final : public Replacer {
 public:
  explicit LruReplacer(std::size_t capacity) : capacity_(capacity) {}

  void record_access(int frame_id) override;
  void set_evictable(int frame_id, bool evictable) override;
  std::optional<int> evict() override;
  void remove(int frame_id) override;
  std::size_t size() const override { return lru_.size(); }

 private:
  std::size_t capacity_;
  std::list<int> lru_;                                        // front = MRU
  std::unordered_map<int, std::list<int>::iterator> where_;   // frame -> iterator
};

}  // namespace sthir
