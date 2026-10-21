#include "storage/replacer.h"

namespace sthir {

void LruReplacer::record_access(int frame_id) {
  auto it = where_.find(frame_id);
  if (it == where_.end()) return;  // pinned (or unknown): not in evictable list
  lru_.splice(lru_.begin(), lru_, it->second);
}

void LruReplacer::set_evictable(int frame_id, bool evictable) {
  auto it = where_.find(frame_id);
  if (evictable) {
    if (it != where_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second);
      return;
    }
    if (lru_.size() >= capacity_) return;  // cannot track more frames than exist
    lru_.push_front(frame_id);
    where_[frame_id] = lru_.begin();
  } else if (it != where_.end()) {
    lru_.erase(it->second);
    where_.erase(it);
  }
}

std::optional<int> LruReplacer::evict() {
  if (lru_.empty()) return std::nullopt;
  const int victim = lru_.back();
  lru_.pop_back();
  where_.erase(victim);
  return victim;
}

void LruReplacer::remove(int frame_id) {
  auto it = where_.find(frame_id);
  if (it == where_.end()) return;
  lru_.erase(it->second);
  where_.erase(it);
}

}  // namespace sthir
