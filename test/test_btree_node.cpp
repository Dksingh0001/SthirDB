#include "index/btree_node.h"

#include <cstring>
#include <string>

#include "test_macros.h"

using sthir::BPlusNode;
using sthir::kIndexMaxKeys;
using sthir::kPageSize;

namespace {

void test_leaf_roundtrip() {
  char image[kPageSize];
  BPlusNode node;
  node.Init(7, true);
  node.LeafInsert(0, 10, BPlusNode::RIDRef{1, 2});
  node.LeafInsert(1, 20, BPlusNode::RIDRef{3, 4});
  node.LeafInsert(2, 30, BPlusNode::RIDRef{5, 6});
  node.set_next_leaf_page_id(42);
  CHECK(node.is_full());
  node.Serialize(image);

  BPlusNode back;
  back.Deserialize(image);
  CHECK_EQ(back.page_id(), 7u);
  CHECK(back.is_leaf());
  CHECK_EQ(back.key_count(), 3);
  CHECK_EQ(back.next_leaf_page_id(), 42u);
  CHECK_EQ(back.key(0), 10);
  CHECK_EQ(back.key(2), 30);
  CHECK_EQ(back.rid(0).page_id, 1u);
  CHECK_EQ(back.rid(0).slot_id, 2);
  CHECK_EQ(back.rid(2).page_id, 5u);
  CHECK_EQ(back.rid(2).slot_id, 6);
  test_passed("leaf roundtrip");
}

void test_internal_roundtrip() {
  char image[kPageSize];
  BPlusNode node;
  node.Init(9, false);
  node.InternalInsert(0, 100, 21);  // key 100, right child 21
  node.InternalInsert(1, 200, 23);  // key 200, right child 23
  CHECK_EQ(node.key_count(), 2);
  CHECK_EQ(node.child(0), sthir::kInvalidPageId);  // original lone child slot
  CHECK_EQ(node.child(1), 21u);
  CHECK_EQ(node.child(2), 23u);
  node.Serialize(image);

  BPlusNode back;
  back.Deserialize(image);
  CHECK_EQ(back.page_id(), 9u);
  CHECK(!back.is_leaf());
  CHECK_EQ(back.key_count(), 2);
  CHECK_EQ(back.key(0), 100);
  CHECK_EQ(back.key(1), 200);
  CHECK_EQ(back.child(0), sthir::kInvalidPageId);
  CHECK_EQ(back.child(1), 21u);
  CHECK_EQ(back.child(2), 23u);
  test_passed("internal roundtrip");
}

void test_sorted_bounds() {
  BPlusNode node;
  node.Init(0, true);
  node.LeafInsert(0, 10, BPlusNode::RIDRef{0, 0});
  node.LeafInsert(1, 10, BPlusNode::RIDRef{0, 1});  // duplicate key
  node.LeafInsert(2, 20, BPlusNode::RIDRef{0, 2});
  CHECK_EQ(node.LowerBound(10), 0);
  CHECK_EQ(node.UpperBound(10), 2);
  CHECK_EQ(node.LowerBound(15), 2);
  CHECK_EQ(node.UpperBound(15), 2);
  CHECK_EQ(node.LowerBound(20), 2);
  CHECK_EQ(node.UpperBound(20), 3);
  CHECK_EQ(node.LowerBound(5), 0);
  CHECK_EQ(node.UpperBound(25), 3);
  test_passed("sorted bounds");
}

void test_leaf_split() {
  BPlusNode left;
  left.Init(1, true);
  BPlusNode right;
  right.Init(2, true);
  left.set_next_leaf_page_id(99);

  // Fill to overflow: 4 entries in an order-4 tree (max 3 keys).
  for (uint32_t i = 0; i <= kIndexMaxKeys; ++i) {
    left.LeafInsert(static_cast<uint16_t>(i), 10 * (i + 1),
                    BPlusNode::RIDRef{static_cast<uint32_t>(i), 0});
  }
  CHECK_EQ(left.key_count(), kIndexMaxKeys + 1);

  int64_t sep = 0;
  left.Split(&right, &sep);

  // 4 entries -> 2 + 2; separator is the right half's first key (copy up).
  CHECK_EQ(left.key_count(), 2);
  CHECK_EQ(right.key_count(), 2);
  CHECK_EQ(sep, 30);
  CHECK_EQ(left.key(0), 10);
  CHECK_EQ(left.key(1), 20);
  CHECK_EQ(right.key(0), 30);
  CHECK_EQ(right.key(1), 40);
  // Leaf chain: left -> right -> old next.
  CHECK_EQ(left.next_leaf_page_id(), 2u);
  CHECK_EQ(right.next_leaf_page_id(), 99u);
  CHECK_EQ(left.rid(0).page_id, 0u);
  CHECK_EQ(right.rid(0).page_id, 2u);
  test_passed("leaf split");
}

void test_internal_split() {
  BPlusNode left;
  left.Init(5, false);
  BPlusNode right;
  right.Init(6, false);

  // Overflowing internal node: keys {10,20,30,40} (max 3 + 1 trigger),
  // children {INVALID, 0, 1, 2, 3}.
  left.InternalInsert(0, 10, 0);
  left.InternalInsert(1, 20, 1);
  left.InternalInsert(2, 30, 2);
  left.InternalInsert(3, 40, 3);
  CHECK_EQ(left.key_count(), kIndexMaxKeys + 1);

  int64_t sep = 0;
  left.Split(&right, &sep);

  // 4 keys -> left keeps 2, median pushed up, right keeps 1 + right subtree.
  CHECK_EQ(sep, 30);
  CHECK_EQ(left.key_count(), 2);
  CHECK_EQ(left.key(0), 10);
  CHECK_EQ(left.key(1), 20);
  CHECK_EQ(left.child(0), sthir::kInvalidPageId);
  CHECK_EQ(left.child(2), 1u);
  CHECK_EQ(right.key_count(), 1);
  CHECK_EQ(right.key(0), 40);
  CHECK_EQ(right.child(0), 2u);
  CHECK_EQ(right.child(1), 3u);
  test_passed("internal split");
}

}  // namespace

int main() {
  test_leaf_roundtrip();
  test_internal_roundtrip();
  test_sorted_bounds();
  test_leaf_split();
  test_internal_split();
  std::printf("PASS test_btree_node\n");
  return 0;
}
