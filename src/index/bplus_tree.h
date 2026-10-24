#pragma once

#include <cstdint>
#include <vector>

#include "common/config.h"
#include "index/btree_node.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

namespace sthir {

// A RID as stored in the index: the page and slot of the base tuple.
using IndexRid = BPlusNode::RIDRef;

// Disk-resident B+tree over int64 keys pointing at table RIDs.
//
// Every node access goes through the buffer pool: a node is pinned while it is
// read or written and unpinned (dirty when modified) immediately after, so the
// descent holds at most one pin and a split holds at most two (the node being
// split plus its new right sibling). The path back to the root is remembered as
// page ids only — never as pinned pointers — so propagation re-fetches parents.
//
// Duplicate keys: entries are kept in sorted order with duplicates adjacent.
// A point lookup descends with "leftmost child that may contain the key" and
// then walks the leaf chain forward while the current leaf's last key is still
// <= the search key, which collects a duplicate run even when it spans leaves.
class BPlusTree {
 public:
  // Attaches to an existing tree. Pass kInvalidPageId / 0 for an empty tree.
  BPlusTree(DiskManager* dm, BufferPool* bp, uint32_t root_page_id, uint32_t height);

  // Allocates a fresh empty leaf root. The caller persists root_page_id() and
  // height() in the catalog.
  bool Create();

  // Inserts (key, rid). Splits overflowed leaves/internals and grows the height
  // when the root splits. Returns false only on a buffer-pool failure.
  bool Insert(int64_t key, IndexRid rid);

  // All RIDs whose key == `key` (duplicates included, insertion order).
  std::vector<IndexRid> Lookup(int64_t key) const;

  // All RIDs with lo <= key <= hi, in key order, via next_leaf_page_id links.
  std::vector<IndexRid> RangeScan(int64_t lo, int64_t hi) const;

  uint32_t root_page_id() const { return root_page_id_; }
  uint32_t height() const { return height_; }
  uint32_t order() const { return kIndexOrder; }
  bool empty() const { return root_page_id_ == kInvalidPageId; }

  // Number of leaves reachable by following next_leaf_page_id from the leftmost
  // leaf. Used by tests and evidence runs.
  uint32_t leaf_count() const;

 private:
  // Fetches, decodes, and unpins node `pid`. False on a buffer-pool failure.
  bool ReadNode(uint32_t pid, BPlusNode* node) const;

  // Walks down from the root taking the first separator >= key (i.e. going
  // left on ties) and returns the leftmost leaf that may hold `key`.
  uint32_t DescendToLeaf(int64_t key) const;

  DiskManager* dm_;
  BufferPool* bp_;
  uint32_t root_page_id_ = kInvalidPageId;
  uint32_t height_ = 0;  // 0 = empty tree, 1 = single leaf root
};

}  // namespace sthir
