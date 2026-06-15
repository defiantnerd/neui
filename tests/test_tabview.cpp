#include "neui_test.h"

#include "widget_tabview.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// parse_tab_position - all 13 documented values
// ---------------------------------------------------------------------------

TEST_CASE("parse_tab_position: edge + alignment for every documented value")
{
  auto eq = [](TabPosition p, TabEdge e, TabAlign a) {
    return p.edge == e && p.align == a;
  };

  CHECK(eq(parse_tab_position("top-left"),      TabEdge::Top,    TabAlign::Begin));
  CHECK(eq(parse_tab_position("top-center"),    TabEdge::Top,    TabAlign::Center));
  CHECK(eq(parse_tab_position("top-right"),     TabEdge::Top,    TabAlign::End));
  CHECK(eq(parse_tab_position("bottom-left"),   TabEdge::Bottom, TabAlign::Begin));
  CHECK(eq(parse_tab_position("bottom-center"), TabEdge::Bottom, TabAlign::Center));
  CHECK(eq(parse_tab_position("bottom-right"),  TabEdge::Bottom, TabAlign::End));
  CHECK(eq(parse_tab_position("left-top"),      TabEdge::Left,   TabAlign::Begin));
  CHECK(eq(parse_tab_position("left-center"),   TabEdge::Left,   TabAlign::Center));
  CHECK(eq(parse_tab_position("left-bottom"),   TabEdge::Left,   TabAlign::End));
  CHECK(eq(parse_tab_position("right-top"),     TabEdge::Right,  TabAlign::Begin));
  CHECK(eq(parse_tab_position("right-center"),  TabEdge::Right,  TabAlign::Center));
  CHECK(eq(parse_tab_position("right-bottom"),  TabEdge::Right,  TabAlign::End));
  CHECK(eq(parse_tab_position("none"),          TabEdge::None,   TabAlign::Begin));
}

TEST_CASE("parse_tab_position: unset / garbage defaults to top-left")
{
  CHECK(parse_tab_position(nullptr).edge == TabEdge::Top);
  CHECK(parse_tab_position("").edge == TabEdge::Top);
  CHECK(parse_tab_position("bogus").edge == TabEdge::Top);
  CHECK(parse_tab_position("top").align == TabAlign::Begin);   // no align token
}

// ---------------------------------------------------------------------------
// compute_tabview_layout - body inset per edge
// ---------------------------------------------------------------------------

TEST_CASE("compute_tabview_layout: body insets on the strip edge")
{
  const float W = 400, H = 300, S = 28;

  TabViewLayout top = compute_tabview_layout(W, H, TabEdge::Top, S);
  CHECK_APPROX(top.strip_y, 0.0);   CHECK_APPROX(top.strip_h, 28.0);
  CHECK_APPROX(top.body_y, 28.0);   CHECK_APPROX(top.body_h, 272.0);
  CHECK_APPROX(top.body_w, 400.0);

  TabViewLayout bot = compute_tabview_layout(W, H, TabEdge::Bottom, S);
  CHECK_APPROX(bot.strip_y, 272.0);
  CHECK_APPROX(bot.body_y, 0.0);    CHECK_APPROX(bot.body_h, 272.0);

  TabViewLayout left = compute_tabview_layout(W, H, TabEdge::Left, S);
  CHECK_APPROX(left.strip_x, 0.0);  CHECK_APPROX(left.strip_w, 28.0);
  CHECK_APPROX(left.body_x, 28.0);  CHECK_APPROX(left.body_w, 372.0);
  CHECK_APPROX(left.body_h, 300.0);

  TabViewLayout right = compute_tabview_layout(W, H, TabEdge::Right, S);
  CHECK_APPROX(right.strip_x, 372.0);
  CHECK_APPROX(right.body_x, 0.0);  CHECK_APPROX(right.body_w, 372.0);

  TabViewLayout none = compute_tabview_layout(W, H, TabEdge::None, S);
  CHECK_APPROX(none.body_x, 0.0);   CHECK_APPROX(none.body_y, 0.0);
  CHECK_APPROX(none.body_w, 400.0); CHECK_APPROX(none.body_h, 300.0);
  CHECK_APPROX(none.strip_w, 0.0);
}

TEST_CASE("compute_tabview_layout: strip clamps to widget extent, body floors at 0")
{
  TabViewLayout L = compute_tabview_layout(100, 20, TabEdge::Top, 28);
  CHECK_APPROX(L.strip_h, 20.0);   // clamped to height
  CHECK_APPROX(L.body_h, 0.0);     // not negative
}

// ---------------------------------------------------------------------------
// layout_tab_chips + tabview_chip_hit
// ---------------------------------------------------------------------------

TEST_CASE("layout_tab_chips: horizontal strip packs left-to-right, Begin anchored")
{
  TabViewLayout L = compute_tabview_layout(400, 300, TabEdge::Top, 28);
  float widths[3] = { 40, 60, 30 };  // measured label widths
  TabChip chips[3];
  layout_tab_chips(L, TabEdge::Top, TabAlign::Begin, widths, 3, chips);

  // First chip starts at strip_x, width = text + 2*pad.
  CHECK_APPROX(chips[0].x, 0.0);
  CHECK_APPROX(chips[0].w, 40.0 + 2 * TAB_CHIP_PAD_X);
  CHECK_APPROX(chips[0].h, 28.0);
  // Second chip follows after a gap.
  CHECK_APPROX(chips[1].x, chips[0].w + TAB_CHIP_GAP);
  // text band is inset by pad.
  CHECK_APPROX(chips[0].text_x, TAB_CHIP_PAD_X);
}

TEST_CASE("layout_tab_chips: End alignment right-justifies the chip run")
{
  TabViewLayout L = compute_tabview_layout(400, 300, TabEdge::Top, 28);
  float widths[2] = { 40, 40 };
  TabChip chips[2];
  layout_tab_chips(L, TabEdge::Top, TabAlign::End, widths, 2, chips);
  float cw = 40.0f + 2 * TAB_CHIP_PAD_X;
  float total = cw * 2 + TAB_CHIP_GAP;
  CHECK_APPROX(chips[0].x, 400.0 - total);
  CHECK_APPROX(chips[1].x + chips[1].w, 400.0);
}

TEST_CASE("layout_tab_chips: vertical strip stacks rows of full strip width")
{
  TabViewLayout L = compute_tabview_layout(400, 300, TabEdge::Left, 80);
  float widths[2] = { 40, 40 };  // ignored on vertical strips
  TabChip chips[2];
  layout_tab_chips(L, TabEdge::Left, TabAlign::Begin, widths, 2, chips);
  CHECK_APPROX(chips[0].x, 0.0);
  CHECK_APPROX(chips[0].w, 80.0);          // full strip width
  CHECK_APPROX(chips[0].y, 0.0);
  CHECK(chips[1].y > chips[0].y);          // stacked below
  CHECK_APPROX(chips[1].y, chips[0].h + TAB_CHIP_GAP);
}

TEST_CASE("tab_resolve_strip_size: vertical strips auto-fit the widest label")
{
  float widths[3] = { 40, 90, 30 };
  // Explicit size always wins.
  CHECK_APPROX(tab_resolve_strip_size(TabEdge::Left, 50.0f, widths, 3), 50.0);
  // Horizontal edges ignore label widths -> default height.
  CHECK_APPROX(tab_resolve_strip_size(TabEdge::Top, 0.0f, widths, 3),
               (double)TAB_STRIP_SIZE_DEFAULT);
  // Vertical, unset -> widest label + 2*pad.
  CHECK_APPROX(tab_resolve_strip_size(TabEdge::Right, 0.0f, widths, 3),
               90.0 + 2 * TAB_CHIP_PAD_X);
  // Narrow labels still floor at the default.
  float narrow[2] = { 4, 6 };
  CHECK_APPROX(tab_resolve_strip_size(TabEdge::Left, 0.0f, narrow, 2),
               (double)TAB_STRIP_SIZE_DEFAULT);
}

TEST_CASE("tabview_chip_hit: returns chip index under the point, -1 otherwise")
{
  TabViewLayout L = compute_tabview_layout(400, 300, TabEdge::Top, 28);
  float widths[3] = { 40, 60, 30 };
  TabChip chips[3];
  layout_tab_chips(L, TabEdge::Top, TabAlign::Begin, widths, 3, chips);

  // Inside chip 1.
  float midx = chips[1].x + chips[1].w * 0.5f;
  CHECK_EQ(tabview_chip_hit(chips, 3, midx, 14.0f), 1);
  // Inside chip 0.
  CHECK_EQ(tabview_chip_hit(chips, 3, chips[0].x + 2.0f, 5.0f), 0);
  // Below the strip (in the body) - no hit.
  CHECK_EQ(tabview_chip_hit(chips, 3, midx, 100.0f), -1);
  // Far right past the last chip.
  CHECK_EQ(tabview_chip_hit(chips, 3, 399.0f, 14.0f), -1);
}
