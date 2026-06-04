#include "neui_test.h"

#include "scrollbar.h"

using namespace neui_detail;

TEST_CASE("compute_scrollbar: invisible when content fits the viewport")
{
  ScrollbarGeom g = compute_scrollbar(/*viewport*/100, /*gutter*/0,
                                      /*content*/50, /*visible*/80, /*pos*/0);
  CHECK_FALSE(g.visible);   // visible >= content
}

TEST_CASE("compute_scrollbar: proportional thumb length + endpoints")
{
  // viewport 100, content 200, visible 100 -> half-length thumb.
  ScrollbarGeom top = compute_scrollbar(100, 0, 200, 100, 0);
  CHECK(top.visible);
  CHECK_EQ(top.track_pos, 1);
  CHECK_EQ(top.track_len, 98);          // viewport - 2 - gutter
  CHECK_EQ(top.thumb_len, 49);          // 98 * 100 / 200
  CHECK_EQ(top.thumb_pos, 1);           // position 0 -> track start

  // At max position the thumb sits at the track end.
  ScrollbarGeom bot = compute_scrollbar(100, 0, 200, 100, 100);
  CHECK_EQ(bot.thumb_pos, bot.track_pos + (bot.track_len - bot.thumb_len));
}

TEST_CASE("compute_scrollbar: enforces a minimum thumb length")
{
  ScrollbarGeom g = compute_scrollbar(100, 0, 10000, 100, 0);
  CHECK_EQ(g.thumb_len, SCROLLBAR_MIN);   // proportional would be < min
}

TEST_CASE("compute_scrollbar: gutter shortens the track")
{
  ScrollbarGeom none = compute_scrollbar(100, 0, 200, 100, 0);
  ScrollbarGeom gut  = compute_scrollbar(100, SCROLLBAR_W, 200, 100, 0);
  CHECK_EQ(gut.track_len, none.track_len - SCROLLBAR_W);
}

TEST_CASE("scrollbar_pos_from_thumb inverts the thumb position")
{
  ScrollbarGeom g = compute_scrollbar(100, 0, 200, 100, 0);
  // Thumb dragged to the track end -> max content position.
  int pos = scrollbar_pos_from_thumb(g.track_pos + g.track_len - g.thumb_len, g, 200, 100);
  CHECK_EQ(pos, 100);   // content - visible
  // Thumb at the start -> position 0.
  CHECK_EQ(scrollbar_pos_from_thumb(g.track_pos, g, 200, 100), 0);
  // Over-drag clamps.
  CHECK_EQ(scrollbar_pos_from_thumb(99999, g, 200, 100), 100);
}

TEST_CASE("scrollbar_drag_apply: proportional delta, clamped to range")
{
  ScrollbarGeom g = compute_scrollbar(100, 0, 200, 100, 0);
  ScrollbarDrag d;
  d.active = true;
  d.start_axis_coord = 0;
  d.start_position = 0;

  int travel = g.track_len - g.thumb_len;
  // Dragging by the full travel moves to the max position.
  CHECK_EQ(scrollbar_drag_apply(d, travel, g, 200, 100), 100);
  // Dragging up past the start clamps to 0.
  CHECK_EQ(scrollbar_drag_apply(d, -500, g, 200, 100), 0);
  // Inactive drag returns the start position untouched.
  ScrollbarDrag idle;
  CHECK_EQ(scrollbar_drag_apply(idle, 50, g, 200, 100), idle.start_position);
}
