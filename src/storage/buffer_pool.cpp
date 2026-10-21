#include "storage/buffer_pool.h"

#include <stdexcept>

namespace sthir {

BufferPool::BufferPool(std::size_t pool_size, DiskManager* dm)
    : dm_(dm),
      frames_(pool_size),
      pin_count_(pool_size, 0),
      dirty_(pool_size, false),
      present_(pool_size, false),
      frame_page_id_(pool_size, UINT32_MAX),
      replacer_(pool_size) {
  if (pool_size == 0) throw std::invalid_argument("buffer pool needs >= 1 frame");
  free_list_.reserve(pool_size);
  for (int i = static_cast<int>(pool_size) - 1; i >= 0; --i) free_list_.push_back(i);
}

BufferPool::~BufferPool() {
  try {
    flush_all();
  } catch (...) {
    // Destructor cannot throw; write errors surface earlier via write_page.
  }
}

std::optional<int> BufferPool::take_frame() {
  if (!free_list_.empty()) {
    const int f = free_list_.back();
    free_list_.pop_back();
    return f;
  }
  return replacer_.evict();  // LRU among pin-count-0 frames; nullopt if all pinned
}

Page* BufferPool::fetch_page(uint32_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_to_frame_.find(page_id);
  if (it != page_to_frame_.end()) {
    const int f = it->second;
    ++pin_count_[f];
    replacer_.record_access(f);
    if (pin_count_[f] == 1) replacer_.set_evictable(f, false);  // now pinned
    return &frames_[f];
  }

  auto frame = take_frame();
  if (!frame) return nullptr;  // every frame is pinned
  const int f = *frame;

  if (present_[f]) {
    // Victim from eviction: persist if dirty, then unmap the old page.
    if (dirty_[f]) dm_->write_page(frame_page_id_[f], frames_[f].data());
    page_to_frame_.erase(frame_page_id_[f]);
  }

  dm_->read_page(page_id, frames_[f].data());
  page_to_frame_[page_id] = f;
  frame_page_id_[f] = page_id;
  pin_count_[f] = 1;
  dirty_[f] = false;
  present_[f] = true;
  replacer_.remove(f);
  return &frames_[f];
}

bool BufferPool::unpin_page(uint32_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_to_frame_.find(page_id);
  if (it == page_to_frame_.end()) return false;
  const int f = it->second;
  if (pin_count_[f] <= 0) return false;  // unbalanced unpin
  if (is_dirty) dirty_[f] = true;
  --pin_count_[f];
  if (pin_count_[f] == 0) replacer_.set_evictable(f, true);
  return true;
}

bool BufferPool::flush_page(uint32_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_to_frame_.find(page_id);
  if (it == page_to_frame_.end()) return false;
  const int f = it->second;
  if (!dirty_[f]) return false;
  dm_->write_page(page_id, frames_[f].data());
  dirty_[f] = false;
  return true;
}

void BufferPool::flush_all() {
  std::lock_guard<std::mutex> lock(latch_);
  for (std::size_t f = 0; f < frames_.size(); ++f) {
    if (present_[f] && dirty_[f]) {
      dm_->write_page(frame_page_id_[f], frames_[f].data());
      dirty_[f] = false;
    }
  }
}

int BufferPool::pin_count(uint32_t page_id) const {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_to_frame_.find(page_id);
  return it == page_to_frame_.end() ? -1 : pin_count_[it->second];
}

bool BufferPool::is_dirty(uint32_t page_id) const {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_to_frame_.find(page_id);
  return it != page_to_frame_.end() && dirty_[it->second];
}

std::size_t BufferPool::evictable_frames() const {
  std::lock_guard<std::mutex> lock(latch_);
  return replacer_.size();
}

}  // namespace sthir
