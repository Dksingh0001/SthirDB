#include "index/bplus_tree.h"

namespace sthir {

namespace {

// One level of the descent path. Only ids are remembered: the pages themselves
// are unpinned as soon as they have been read.
struct PathEntry {
  uint32_t page_id;
  uint16_t child_idx;
};

}  // namespace

BPlusTree::BPlusTree(DiskManager* dm, BufferPool* bp, uint32_t root_page_id, uint32_t height)
    : dm_(dm), bp_(bp), root_page_id_(root_page_id), height_(height) {}

bool BPlusTree::Create() {
  const uint32_t root = dm_->allocate_page();
  Page* page = bp_->fetch_page(root);
  if (page == nullptr) return false;
  BPlusNode node;
  node.Init(root, true);
  node.Serialize(page->data());
  bp_->unpin_page(root, true);
  root_page_id_ = root;
  height_ = 1;
  return true;
}

bool BPlusTree::ReadNode(uint32_t pid, BPlusNode* node) const {
  Page* page = bp_->fetch_page(pid);
  if (page == nullptr) return false;
  node->Deserialize(page->data());
  bp_->unpin_page(pid, false);
  return true;
}

uint32_t BPlusTree::DescendToLeaf(int64_t key) const {
  uint32_t pid = root_page_id_;
  while (pid != kInvalidPageId) {
    Page* page = bp_->fetch_page(pid);
    if (page == nullptr) return kInvalidPageId;
    BPlusNode node;
    node.Deserialize(page->data());
    if (node.is_leaf()) {
      bp_->unpin_page(pid, false);
      return pid;
    }
    // First separator >= key: subtrees to the left hold only keys < key, so
    // this is the leftmost subtree that can contain it.
    const uint32_t child = node.child(node.LowerBound(key));
    bp_->unpin_page(pid, false);
    pid = child;
  }
  return kInvalidPageId;
}

bool BPlusTree::Insert(int64_t key, IndexRid rid) {
  if (empty() && !Create()) return false;

  std::vector<PathEntry> path;
  uint32_t pid = root_page_id_;
  Page* page = nullptr;
  BPlusNode node;

  // Descent: at most one pin is held at any time.
  while (true) {
    page = bp_->fetch_page(pid);
    if (page == nullptr) return false;
    node.Deserialize(page->data());
    if (node.is_leaf()) break;
    const uint16_t idx = node.LowerBound(key);
    const uint32_t child = node.child(idx);
    path.push_back(PathEntry{pid, idx});
    bp_->unpin_page(pid, false);
    page = nullptr;
    pid = child;
  }

  node.LeafInsert(node.UpperBound(key), key, rid);

  if (!node.is_overfull()) {
    node.Serialize(page->data());
    bp_->unpin_page(pid, true);
    return true;
  }

  // Leaf overflow: pin the new right sibling while the leaf is still pinned.
  // Exactly two pins are held from here until both are released.
  const uint32_t right_id = dm_->allocate_page();
  Page* right_page = bp_->fetch_page(right_id);
  if (right_page == nullptr) {
    bp_->unpin_page(pid, false);
    return false;
  }
  BPlusNode right;
  right.Init(right_id, true);
  int64_t separator = 0;
  node.Split(&right, &separator);
  node.Serialize(page->data());
  right.Serialize(right_page->data());
  bp_->unpin_page(pid, true);
  bp_->unpin_page(right_id, true);

  uint32_t split_page_id = pid;   // node that just split (may become the root)
  uint32_t right_child = right_id;  // its new right sibling
  page = nullptr;

  // Propagate the separator upwards.
  while (!path.empty()) {
    const PathEntry parent_entry = path.back();
    path.pop_back();

    Page* parent_page = bp_->fetch_page(parent_entry.page_id);
    if (parent_page == nullptr) return false;
    BPlusNode parent;
    parent.Deserialize(parent_page->data());
    parent.InternalInsert(parent_entry.child_idx, separator, right_child);

    if (!parent.is_overfull()) {
      parent.Serialize(parent_page->data());
      bp_->unpin_page(parent_entry.page_id, true);
      return true;
    }

    const uint32_t new_right = dm_->allocate_page();
    Page* new_right_page = bp_->fetch_page(new_right);
    if (new_right_page == nullptr) {
      bp_->unpin_page(parent_entry.page_id, false);
      return false;
    }
    BPlusNode right_internal;
    right_internal.Init(new_right, false);
    parent.Split(&right_internal, &separator);
    parent.Serialize(parent_page->data());
    right_internal.Serialize(new_right_page->data());
    bp_->unpin_page(parent_entry.page_id, true);
    bp_->unpin_page(new_right, true);

    split_page_id = parent_entry.page_id;
    right_child = new_right;
  }

  // The root itself split: grow the tree by one level around the two halves.
  const uint32_t new_root_id = dm_->allocate_page();
  Page* new_root_page = bp_->fetch_page(new_root_id);
  if (new_root_page == nullptr) return false;
  BPlusNode new_root;
  new_root.Init(new_root_id, false);
  new_root.set_child(0, split_page_id);
  new_root.InternalInsert(0, separator, right_child);
  new_root.Serialize(new_root_page->data());
  bp_->unpin_page(new_root_id, true);

  root_page_id_ = new_root_id;
  ++height_;
  return true;
}

std::vector<IndexRid> BPlusTree::Lookup(int64_t key) const {
  std::vector<IndexRid> out;
  if (empty()) return out;

  uint32_t pid = DescendToLeaf(key);
  while (pid != kInvalidPageId) {
    Page* page = bp_->fetch_page(pid);
    if (page == nullptr) break;
    BPlusNode node;
    node.Deserialize(page->data());
    const uint16_t n = node.key_count();

    // Empty leaf (empty tree) or every key here is past the search key.
    if (n == 0 || node.key(0) > key) {
      bp_->unpin_page(pid, false);
      break;
    }
    for (uint16_t i = node.LowerBound(key); i < n && node.key(i) == key; ++i) {
      out.push_back(node.rid(i));
    }
    // If the run may continue, follow the leaf link; a stray key > search key
    // stops the walk on the next iteration.
    const bool more = node.key(n - 1) <= key;
    const uint32_t next = node.next_leaf_page_id();
    bp_->unpin_page(pid, false);
    if (!more || next == kInvalidPageId) break;
    pid = next;
  }
  return out;
}

std::vector<IndexRid> BPlusTree::RangeScan(int64_t lo, int64_t hi) const {
  std::vector<IndexRid> out;
  if (empty() || lo > hi) return out;

  uint32_t pid = DescendToLeaf(lo);
  while (pid != kInvalidPageId) {
    Page* page = bp_->fetch_page(pid);
    if (page == nullptr) break;
    BPlusNode node;
    node.Deserialize(page->data());
    const uint16_t n = node.key_count();

    if (n == 0 || node.key(0) > hi) {
      bp_->unpin_page(pid, false);
      break;
    }
    for (uint16_t i = node.LowerBound(lo); i < n; ++i) {
      if (node.key(i) > hi) break;
      out.push_back(node.rid(i));
    }
    const bool more = node.key(n - 1) <= hi;
    const uint32_t next = node.next_leaf_page_id();
    bp_->unpin_page(pid, false);
    if (!more || next == kInvalidPageId) break;
    pid = next;
  }
  return out;
}

uint32_t BPlusTree::leaf_count() const {
  if (empty()) return 0;
  uint32_t pid = root_page_id_;
  // Walk to the leftmost leaf.
  while (pid != kInvalidPageId) {
    BPlusNode node;
    if (!ReadNode(pid, &node)) return 0;
    if (node.is_leaf()) break;
    pid = node.child(0);
  }
  uint32_t leaves = 0;
  while (pid != kInvalidPageId) {
    BPlusNode node;
    if (!ReadNode(pid, &node)) return leaves;
    ++leaves;
    pid = node.next_leaf_page_id();
  }
  return leaves;
}

}  // namespace sthir
