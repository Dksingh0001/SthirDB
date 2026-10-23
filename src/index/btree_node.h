#pragma once

#include <cstdint>
#include <vector>

#include "common/config.h"

namespace sthir {

// A B+tree index node stored in one 8 KB page. Phase 3 simplification: keys
// are int64 and the tree order is fixed at kIndexOrder (= 4), so every node
// holds at most kIndexMaxKeys (= 3) keys. Integer keys keep split logic easy
// to reason about and to demo (documented in docs/report.md §4).
//
// Page layout (all fields little-endian host layout via memcpy):
//
//   leaf page:
//     [0 .. 16)                          header
//     [16 .. 20)                         next_leaf_page_id (uint32)
//     [20 .. 20 + 8*n)                   keys[0..n) (int64, sorted)
//     [20 + 8*n .. + 6*n)                rids[0..n) (uint32 page_id, uint16 slot_id)
//
//   internal page:
//     [0 .. 16)                          header
//     [16 .. 16 + 8*n)                   keys[0..n) (int64, sorted separators)
//     [16 + 8*n .. + 4*(n+1))            children[0..n] (uint32 page ids)
//
// Header (16 bytes):
//     uint32 page_id      — page identifier within the index file
//     uint64 page_lsn     — LSN of last modifying log record (Phase 5 ARIES)
//     uint8  is_leaf      — 1 = leaf node, 0 = internal node
//     uint8  reserved     — padding, must be 0
//     uint16 key_count    — number of keys currently stored
//
// RID encoding inside a leaf: 6 bytes (uint32 page_id + uint16 slot_id),
// pointing at a tuple slot in the *table* file.
class BPlusNode {
 public:
  BPlusNode() { Init(0, true); }

  // Initializes an empty node belonging to `page_id`.
  void Init(uint32_t page_id, bool is_leaf);

  // Decodes a node from a raw 8 KB page image.
  void Deserialize(const char* data);
  // Encodes this node into a raw 8 KB page image.
  void Serialize(char* data) const;

  uint32_t page_id() const { return page_id_; }
  uint64_t page_lsn() const { return page_lsn_; }
  void set_page_lsn(uint64_t lsn) { page_lsn_ = lsn; }
  bool is_leaf() const { return is_leaf_; }
  uint16_t key_count() const { return static_cast<uint16_t>(keys_.size()); }
  bool is_full() const { return keys_.size() >= kIndexMaxKeys; }

  int64_t key(uint16_t i) const { return keys_[i]; }
  const std::vector<int64_t>& keys() const { return keys_; }

  // Leaf accessors.
  uint32_t next_leaf_page_id() const { return next_leaf_page_id_; }
  void set_next_leaf_page_id(uint32_t pid) { next_leaf_page_id_ = pid; }
  struct RIDRef {
    uint32_t page_id;
    uint16_t slot_id;
  };
  RIDRef rid(uint16_t i) const { return rids_[i]; }
  const std::vector<RIDRef>& rids() const { return rids_; }

  // Internal accessors: children_.size() == keys_.size() + 1.
  uint32_t child(uint16_t i) const { return children_[i]; }
  const std::vector<uint32_t>& children() const { return children_; }

  // First index i with keys_[i] >= key (keys are sorted, duplicates allowed).
  uint16_t LowerBound(int64_t key) const;
  // First index i with keys_[i] > key.
  uint16_t UpperBound(int64_t key) const;

  // Leaf entry insert at sorted position `pos` (call LowerBound/UpperBound).
  void LeafInsert(uint16_t pos, int64_t key, RIDRef rid);
  // Internal entry insert: key at `pos`, `right_child` becomes child[pos+1].
  void InternalInsert(uint16_t pos, int64_t key, uint32_t right_child);

  // Splits a full node. On entry this node holds kIndexMaxKeys + 1 entries;
  // on return the left half stays here and `right` receives the right half.
  // `*separator` is the key promoted to the parent: for a leaf it is the
  // first key of the right node (copy up); for an internal node it is the
  // median key that moves up and is removed here (push up).
  void Split(BPlusNode* right, int64_t* separator);

 private:
  uint32_t page_id_ = 0;
  uint64_t page_lsn_ = 0;
  bool is_leaf_ = true;
  uint32_t next_leaf_page_id_ = kInvalidPageId;  // leaf only
  std::vector<int64_t> keys_;
  std::vector<RIDRef> rids_;      // leaf only
  std::vector<uint32_t> children_;  // internal only
};

}  // namespace sthir
