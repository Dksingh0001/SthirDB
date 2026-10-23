#include "index/btree_node.h"

#include <cstring>

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

// Header offsets (see btree_node.h).
constexpr uint32_t kOffPageId = 0;
constexpr uint32_t kOffPageLsn = 4;
constexpr uint32_t kOffIsLeaf = 12;
constexpr uint32_t kOffKeyCount = 14;
// Payload starts after the 16-byte header.
constexpr uint32_t kOffNextLeaf = kPageHeaderSize;  // leaf only, uint32
constexpr uint32_t kLeafKeysOff = kPageHeaderSize + 4;
constexpr uint32_t kInternalKeysOff = kPageHeaderSize;
constexpr uint32_t kLeafRidSize = 6;  // uint32 page_id + uint16 slot_id

}  // namespace

void BPlusNode::Init(uint32_t page_id, bool is_leaf) {
  page_id_ = page_id;
  page_lsn_ = 0;
  is_leaf_ = is_leaf;
  next_leaf_page_id_ = kInvalidPageId;
  keys_.clear();
  rids_.clear();
  children_.clear();
  if (!is_leaf) children_.push_back(kInvalidPageId);  // lone child slot
}

void BPlusNode::Deserialize(const char* data) {
  page_id_ = ReadField<uint32_t>(data, kOffPageId);
  page_lsn_ = ReadField<uint64_t>(data, kOffPageLsn);
  is_leaf_ = data[kOffIsLeaf] != 0;
  const uint16_t n = ReadField<uint16_t>(data, kOffKeyCount);

  keys_.resize(n);
  rids_.clear();
  children_.clear();

  if (is_leaf_) {
    next_leaf_page_id_ = ReadField<uint32_t>(data, kOffNextLeaf);
    for (uint16_t i = 0; i < n; ++i) {
      keys_[i] = ReadField<int64_t>(data, kLeafKeysOff + i * 8);
    }
    rids_.resize(n);
    const uint32_t rid_base = kLeafKeysOff + n * 8;
    for (uint16_t i = 0; i < n; ++i) {
      rids_[i].page_id = ReadField<uint32_t>(data, rid_base + i * kLeafRidSize);
      rids_[i].slot_id = ReadField<uint16_t>(data, rid_base + i * kLeafRidSize + 4);
    }
  } else {
    next_leaf_page_id_ = kInvalidPageId;
    for (uint16_t i = 0; i < n; ++i) {
      keys_[i] = ReadField<int64_t>(data, kInternalKeysOff + i * 8);
    }
    children_.resize(n + 1);
    const uint32_t child_base = kInternalKeysOff + n * 8;
    for (uint16_t i = 0; i <= n; ++i) {
      children_[i] = ReadField<uint32_t>(data, child_base + i * 4);
    }
  }
}

void BPlusNode::Serialize(char* data) const {
  std::memset(data, 0, kPageSize);
  WriteField<uint32_t>(data, kOffPageId, page_id_);
  WriteField<uint64_t>(data, kOffPageLsn, page_lsn_);
  data[kOffIsLeaf] = is_leaf_ ? 1 : 0;
  data[kOffIsLeaf + 1] = 0;  // reserved
  const uint16_t n = key_count();
  WriteField<uint16_t>(data, kOffKeyCount, n);

  if (is_leaf_) {
    WriteField<uint32_t>(data, kOffNextLeaf, next_leaf_page_id_);
    for (uint16_t i = 0; i < n; ++i) {
      WriteField<int64_t>(data, kLeafKeysOff + i * 8, keys_[i]);
    }
    const uint32_t rid_base = kLeafKeysOff + n * 8;
    for (uint16_t i = 0; i < n; ++i) {
      WriteField<uint32_t>(data, rid_base + i * kLeafRidSize, rids_[i].page_id);
      WriteField<uint16_t>(data, rid_base + i * kLeafRidSize + 4, rids_[i].slot_id);
    }
  } else {
    for (uint16_t i = 0; i < n; ++i) {
      WriteField<int64_t>(data, kInternalKeysOff + i * 8, keys_[i]);
    }
    const uint32_t child_base = kInternalKeysOff + n * 8;
    for (uint16_t i = 0; i <= n; ++i) {
      WriteField<uint32_t>(data, child_base + i * 4, children_[i]);
    }
  }
}

uint16_t BPlusNode::LowerBound(int64_t key) const {
  uint16_t lo = 0;
  uint16_t hi = key_count();
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    if (keys_[mid] < key) lo = static_cast<uint16_t>(mid + 1);
    else hi = mid;
  }
  return lo;
}

uint16_t BPlusNode::UpperBound(int64_t key) const {
  uint16_t lo = 0;
  uint16_t hi = key_count();
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    if (keys_[mid] <= key) lo = static_cast<uint16_t>(mid + 1);
    else hi = mid;
  }
  return lo;
}

void BPlusNode::LeafInsert(uint16_t pos, int64_t key, RIDRef rid) {
  keys_.insert(keys_.begin() + pos, key);
  rids_.insert(rids_.begin() + pos, rid);
}

void BPlusNode::InternalInsert(uint16_t pos, int64_t key, uint32_t right_child) {
  keys_.insert(keys_.begin() + pos, key);
  // right_child becomes the child immediately right of the inserted key.
  children_.insert(children_.begin() + pos + 1, right_child);
}

void BPlusNode::Split(BPlusNode* right, int64_t* separator) {
  // Precondition: this node holds kIndexMaxKeys + 1 entries after the
  // triggering insert. Split at the median; both halves stay at least half
  // full for order 4 (3 + 1 entries -> 2 + 2).
  const uint16_t n = key_count();
  const uint16_t mid = static_cast<uint16_t>(n / 2);

  right->is_leaf_ = is_leaf_;
  right->page_lsn_ = 0;

  if (is_leaf_) {
    // Copy up: the right node's first key stays in the right node too.
    right->keys_.assign(keys_.begin() + mid, keys_.end());
    *separator = keys_[mid];
    right->rids_.assign(rids_.begin() + mid, rids_.end());
    right->next_leaf_page_id_ = next_leaf_page_id_;
    next_leaf_page_id_ = right->page_id_;
    keys_.resize(mid);
    rids_.resize(mid);
  } else {
    // Push up: the median key leaves this node and moves to the parent.
    right->keys_.assign(keys_.begin() + mid + 1, keys_.end());
    *separator = keys_[mid];
    right->children_.assign(children_.begin() + mid + 1, children_.end());
    keys_.resize(mid);
    children_.resize(mid + 1);
  }
}

}  // namespace sthir
