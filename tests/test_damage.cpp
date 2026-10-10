#include "neui_test.h"

// Partial-repaint damage model (hosts/shared/damage.h) + the backend-side
// device-pixel snap (backends/shared/backend_util.h). The paint-walk cull and
// the Cairo clip / blit are exercised by neui_cairo_smoke on Linux.
#include "damage.h"
#include "../backends/shared/backend_util.h"

using namespace neui_test;
using neui_detail::DamageRect;
using neui_detail::DamageRegion;

TEST_CASE("damage: a new region is empty, not full")
{
  DamageRegion d;
  CHECK(d.empty());
  CHECK_FALSE(d.full());
  CHECK_FALSE(d.intersects({ 0, 0, 10, 10 }));
  CHECK_EQ(d.area(), static_cast<int64_t>(0));
}

TEST_CASE("damage: empty rects are ignored")
{
  DamageRegion d;
  d.add({ 10, 10, 0, 5 });
  d.add({ 10, 10, 5, -1 });
  CHECK(d.empty());
}

TEST_CASE("damage: distant rects stay separate and disjoint")
{
  DamageRegion d;
  d.add({ 0, 0, 10, 10 });
  d.add({ 100, 100, 10, 10 });
  CHECK_EQ(d.rects().size(), static_cast<size_t>(2));
  CHECK_EQ(d.area(), static_cast<int64_t>(200));
  CHECK(d.intersects({ 5, 5, 1, 1 }));
  CHECK(d.intersects({ 105, 105, 1, 1 }));
  CHECK_FALSE(d.intersects({ 50, 50, 10, 10 }));
}

TEST_CASE("damage: overlapping rects merge into their union")
{
  DamageRegion d;
  d.add({ 0, 0, 20, 20 });
  d.add({ 10, 10, 20, 20 });
  REQUIRE(d.rects().size() == 1);
  const DamageRect& r = d.rects()[0];
  CHECK_EQ(r.x, 0); CHECK_EQ(r.y, 0); CHECK_EQ(r.w, 30); CHECK_EQ(r.h, 30);
}

TEST_CASE("damage: rects within the merge gap merge; just beyond it they don't")
{
  const int g = DamageRegion::k_merge_gap;
  DamageRegion near_;
  near_.add({ 0, 0, 10, 10 });
  near_.add({ 10 + g - 1, 0, 10, 10 });     // gap of g-1 px
  CHECK_EQ(near_.rects().size(), static_cast<size_t>(1));

  DamageRegion far_;
  far_.add({ 0, 0, 10, 10 });
  far_.add({ 10 + g, 0, 10, 10 });          // gap of exactly g px
  CHECK_EQ(far_.rects().size(), static_cast<size_t>(2));
}

TEST_CASE("damage: a bridging rect chains its neighbours into one")
{
  DamageRegion d;
  d.add({ 0, 0, 10, 10 });
  d.add({ 40, 0, 10, 10 });
  REQUIRE(d.rects().size() == 2);
  d.add({ 8, 0, 34, 10 });                  // overlaps both
  REQUIRE(d.rects().size() == 1);
  CHECK_EQ(d.rects()[0].x, 0);
  CHECK_EQ(d.rects()[0].w, 50);
}

TEST_CASE("damage: the rect list is capped, and stays disjoint after capping")
{
  DamageRegion d;
  // A diagonal of small, well-separated rects - more than the cap.
  for (int i = 0; i < 20; ++i) d.add({ i * 50, i * 50, 10, 10 });
  CHECK(d.rects().size() <= DamageRegion::k_max_rects);
  // Every original rect is still covered.
  for (int i = 0; i < 20; ++i) CHECK(d.intersects({ i * 50 + 5, i * 50 + 5, 1, 1 }));
  // Disjoint: no pair shares interior area.
  const auto& rs = d.rects();
  for (size_t i = 0; i < rs.size(); ++i)
    for (size_t j = i + 1; j < rs.size(); ++j)
      CHECK_FALSE(neui_detail::damage_rects_near(rs[i], rs[j], 0));
}

TEST_CASE("damage: FULL absorbs later rects and intersects everything")
{
  DamageRegion d;
  d.add({ 0, 0, 10, 10 });
  d.mark_full();
  CHECK(d.full());
  CHECK_FALSE(d.empty());
  CHECK(d.rects().empty());
  d.add({ 50, 50, 10, 10 });
  CHECK(d.rects().empty());
  CHECK(d.intersects({ 1000, 1000, 1, 1 }));
  CHECK_APPROX(d.coverage(100, 100), 1.0f);
  d.clear();
  CHECK(d.empty());
  CHECK_FALSE(d.full());
}

TEST_CASE("damage: coverage clips rects to the frame")
{
  DamageRegion d;
  d.add({ -10, -10, 30, 30 });              // 20x20 inside a 100x100 frame
  CHECK_APPROX(d.coverage(100, 100), 0.04f);
  DamageRegion big;
  big.add({ 0, 0, 80, 80 });
  CHECK(big.coverage(100, 100) > 0.6f);
  CHECK_APPROX(big.coverage(0, 100), 1.0f); // degenerate frame -> treat as full
}

TEST_CASE("damage: bounds is the union of all rects")
{
  DamageRegion d;
  d.add({ 10, 20, 5, 5 });
  d.add({ 100, 200, 10, 10 });
  DamageRect b = d.bounds();
  CHECK_EQ(b.x, 10); CHECK_EQ(b.y, 20); CHECK_EQ(b.w, 100); CHECK_EQ(b.h, 190);
}

TEST_CASE("damage_rect_to_device: 1x maps 1:1")
{
  int r[4];
  REQUIRE(neui_detail::damage_rect_to_device(10, 20, 30, 40, 1.0f, 1000, 1000, r));
  CHECK_EQ(r[0], 10); CHECK_EQ(r[1], 20); CHECK_EQ(r[2], 30); CHECK_EQ(r[3], 40);
}

TEST_CASE("damage_rect_to_device: fractional DPI snaps outward")
{
  // 150%: x 1..11 -> 1.5..16.5 device px -> [1, 17) after the outward snap.
  int r[4];
  REQUIRE(neui_detail::damage_rect_to_device(1, 1, 10, 10, 1.5f, 1000, 1000, r));
  CHECK_EQ(r[0], 1); CHECK_EQ(r[1], 1); CHECK_EQ(r[2], 16); CHECK_EQ(r[3], 16);
  // 2x is exact.
  REQUIRE(neui_detail::damage_rect_to_device(3, 4, 5, 6, 2.0f, 1000, 1000, r));
  CHECK_EQ(r[0], 6); CHECK_EQ(r[1], 8); CHECK_EQ(r[2], 10); CHECK_EQ(r[3], 12);
}

TEST_CASE("damage_rect_to_device: clamps to the surface; off-surface is rejected")
{
  int r[4];
  REQUIRE(neui_detail::damage_rect_to_device(-4, -4, 20, 20, 1.0f, 10, 10, r));
  CHECK_EQ(r[0], 0); CHECK_EQ(r[1], 0); CHECK_EQ(r[2], 10); CHECK_EQ(r[3], 10);
  CHECK_FALSE(neui_detail::damage_rect_to_device(50, 50, 5, 5, 1.0f, 10, 10, r));
  CHECK_FALSE(neui_detail::damage_rect_to_device(-20, 0, 5, 5, 1.0f, 10, 10, r));
}
