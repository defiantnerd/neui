#include "neui_test.h"

#include "tree.h"

#include <algorithm>
#include <memory>

using namespace neui_detail;

static std::unique_ptr<int> box(int v) { return std::make_unique<int>(v); }

TEST_CASE("Tree: add_child links parent -> first child and exists()")
{
  Tree<int> t;
  uint32_t a = t.add_child(kroot.id, box(10));
  uint32_t b = t.add_child(kroot.id, box(20));
  CHECK(t.exists(a));
  CHECK(t.exists(b));
  CHECK_EQ((int)t.child(kroot.id), (int)a);   // first child of root
  CHECK_EQ((int)t.next(a), (int)b);           // b is a's next sibling
  CHECK_EQ(t[a], 10);
  CHECK_EQ(t[b], 20);
}

TEST_CASE("Tree: removal frees the slot and the next add reuses it")
{
  Tree<int> t;
  uint32_t a = t.add_child(kroot.id, box(1));
  uint32_t b = t.add_child(kroot.id, box(2));
  CHECK_FALSE(a == b);

  t.remove(a);
  CHECK_FALSE(t.exists(a));

  uint32_t c = t.add_child(kroot.id, box(3));
  CHECK_EQ((int)c, (int)a);   // freed slot recycled
  CHECK(t.exists(c));
  CHECK_EQ(t[c], 3);
}

TEST_CASE("Tree: remove() on an internal node tears down the whole subtree")
{
  // Removing a node that still has children must recursively drop all
  // descendants and detach the node from its parent's child list.
  Tree<int> t;
  uint32_t p = t.add_child(kroot.id, box(1));
  uint32_t c1 = t.add_child(p, box(2));
  uint32_t c2 = t.add_child(p, box(3));
  uint32_t gc = t.add_child(c1, box(4));   // grandchild, deeper nesting

  t.remove(p);
  CHECK_FALSE(t.exists(p));
  CHECK_FALSE(t.exists(c1));
  CHECK_FALSE(t.exists(c2));
  CHECK_FALSE(t.exists(gc));
  CHECK_EQ((int)t.child(kroot.id), 0);   // root has no children left
}

TEST_CASE("Tree: remove() of an internal node preserves its siblings")
{
  Tree<int> t;
  uint32_t a = t.add_child(kroot.id, box(1));
  uint32_t b = t.add_child(kroot.id, box(2));
  uint32_t c = t.add_child(kroot.id, box(3));
  t.add_child(b, box(20));   // b has a child, so b is an internal node

  t.remove(b);
  CHECK_FALSE(t.exists(b));
  CHECK(t.exists(a));
  CHECK(t.exists(c));
  // Sibling chain now a -> c, b spliced out.
  CHECK_EQ((int)t.child(kroot.id), (int)a);
  CHECK_EQ((int)t.next(a), (int)c);
  CHECK_EQ((int)t.next(c), 0);
}

TEST_CASE("Tree: bottom-up leaf removal (production teardown order)")
{
  Tree<int> t;
  uint32_t p = t.add_child(kroot.id, box(1));
  uint32_t c1 = t.add_child(p, box(2));
  uint32_t c2 = t.add_child(p, box(3));

  t.remove(c1);
  t.remove(c2);
  CHECK_FALSE(t.exists(c1));
  CHECK_FALSE(t.exists(c2));
  CHECK_EQ((int)t.child(p), 0);   // parent is now childless

  t.remove(p);
  CHECK_FALSE(t.exists(p));
  CHECK_EQ((int)t.child(kroot.id), 0);
}

TEST_CASE("Tree: add_after / add_before order siblings correctly")
{
  Tree<int> t;
  uint32_t a = t.add_child(kroot.id, box(1));
  uint32_t c = t.add_after(a, box(3));
  uint32_t b = t.add_before(c, box(2));   // insert between a and c

  // Walk the sibling chain from the root's first child.
  uint32_t n = t.child(kroot.id);
  CHECK_EQ((int)n, (int)a); CHECK_EQ(t[n], 1); n = t.next(n);
  CHECK_EQ((int)n, (int)b); CHECK_EQ(t[n], 2); n = t.next(n);
  CHECK_EQ((int)n, (int)c); CHECK_EQ(t[n], 3); n = t.next(n);
  CHECK_EQ((int)n, 0);   // end of chain
}

TEST_CASE("Tree: get_all_parents walks up to the root")
{
  Tree<int> t;
  uint32_t a = t.add_child(kroot.id, box(1));
  uint32_t b = t.add_child(a, box(2));
  uint32_t c = t.add_child(b, box(3));

  auto parents = t.get_all_parents(c);
  REQUIRE(parents.size() == 3);
  CHECK_EQ((int)parents[0], (int)b);
  CHECK_EQ((int)parents[1], (int)a);
  CHECK_EQ((int)parents[2], (int)kroot.id);
}

TEST_CASE("Tree: release_order lists every node before its parent")
{
  Tree<int> t;
  uint32_t a = t.add_child(kroot.id, box(1));
  uint32_t b = t.add_child(a, box(2));
  uint32_t c = t.add_child(kroot.id, box(3));

  auto order = t.release_order();
  auto pos = [&](uint32_t id) {
    return (int)std::distance(order.begin(),
                              std::find(order.begin(), order.end(), id));
  };
  // Children must be destroyed before their parents.
  CHECK(pos(b) < pos(a));
  CHECK(pos(a) < pos(kroot.id));
  CHECK(pos(c) < pos(kroot.id));
  CHECK_EQ((int)order.back(), (int)kroot.id);   // root destroyed last
}
