// Tier-1 coverage for the declared popup navigation (hosts/shared/popup_nav.h).
//
// This is the host's arrow / Home / End / Page / Enter walk over the item list a
// client DECLARES on a popup surface (NEUI_ATTR_NAV_*). It lives away from the
// host for the usual reason: the keys are trivial and the EDGES are not, and the
// edges are pure arithmetic that no harness should need a window to reach.
//
// The properties worth pinning: wrap vs clamp at both ends, paging that never
// wraps however wrap is set, the "nothing selected yet" state a freshly opened
// menu is in, and a live count that shrinks under a filter while the index still
// points past its end.

#include "neui_test.h"

#include "popup_nav.h"

using neui_detail::PopupNav;
using neui_detail::popup_nav_key;
using neui_detail::popup_nav_clamp_index;

namespace {

PopupNav list(int count, int index, bool wrap = true, int page = 10)
{
  PopupNav n;
  n.count = count;
  n.index = index;
  n.wrap  = wrap;
  n.page  = page;
  return n;
}

} // namespace

TEST_CASE("popup nav: an undeclared list is not ours to walk")
{
  // The default for every popup surface: no count, so every key comes back
  // unhandled and the gate's caller is free to do nothing with it. This is what
  // keeps host navigation out of the way of a client that drives its own.
  for (uint32_t k : { (uint32_t)NEUI_KEY_UP, (uint32_t)NEUI_KEY_DOWN,
                      (uint32_t)NEUI_KEY_HOME, (uint32_t)NEUI_KEY_RETURN }) {
    auto r = popup_nav_key(list(0, -1), k);
    CHECK(!r.handled);
    CHECK(!r.changed);
    CHECK(!r.activate);
  }
  CHECK(!popup_nav_key(list(-5, 2), NEUI_KEY_DOWN).handled);
}

TEST_CASE("popup nav: a key that is not navigation is left alone")
{
  auto r = popup_nav_key(list(5, 2), NEUI_KEY_A);
  CHECK(!r.handled);
  CHECK_EQ(r.index, 2);
  CHECK(!popup_nav_key(list(5, 2), NEUI_KEY_LEFT).handled);
  CHECK(!popup_nav_key(list(5, 2), NEUI_KEY_RIGHT).handled);
  // Left / Right in particular: in a cascade they mean "open / close a submenu",
  // which only the client knows how to do, so the host must not eat them.
}

TEST_CASE("popup nav: arrows step, and report whether anything moved")
{
  auto down = popup_nav_key(list(5, 1), NEUI_KEY_DOWN);
  CHECK(down.handled);
  CHECK(down.changed);
  CHECK_EQ(down.index, 2);

  auto up = popup_nav_key(list(5, 1), NEUI_KEY_UP);
  CHECK(up.handled);
  CHECK(up.changed);
  CHECK_EQ(up.index, 0);
}

TEST_CASE("popup nav: nothing selected enters the list from the near end")
{
  // What an OS menu does when it is opened by keyboard: Down starts at the top,
  // Up starts at the bottom.
  auto down = popup_nav_key(list(5, -1), NEUI_KEY_DOWN);
  CHECK(down.changed);
  CHECK_EQ(down.index, 0);

  auto up = popup_nav_key(list(5, -1), NEUI_KEY_UP);
  CHECK(up.changed);
  CHECK_EQ(up.index, 4);
}

TEST_CASE("popup nav: wrap is the menu default")
{
  CHECK_EQ(popup_nav_key(list(3, 2, true), NEUI_KEY_DOWN).index, 0);
  CHECK_EQ(popup_nav_key(list(3, 0, true), NEUI_KEY_UP).index, 2);
}

TEST_CASE("popup nav: wrap off clamps, and reports NO change at the end")
{
  // The `changed` flag is what decides whether an event is fired, so a clamped
  // arrow at the end of a list must not spam ATTR_CHANGED on every repeat.
  auto down = popup_nav_key(list(3, 2, false), NEUI_KEY_DOWN);
  CHECK(down.handled);
  CHECK(!down.changed);
  CHECK_EQ(down.index, 2);

  auto up = popup_nav_key(list(3, 0, false), NEUI_KEY_UP);
  CHECK(up.handled);
  CHECK(!up.changed);
  CHECK_EQ(up.index, 0);
}

TEST_CASE("popup nav: Home and End are absolute")
{
  CHECK_EQ(popup_nav_key(list(7, 3), NEUI_KEY_HOME).index, 0);
  CHECK_EQ(popup_nav_key(list(7, 3), NEUI_KEY_END).index, 6);
  CHECK_EQ(popup_nav_key(list(7, -1), NEUI_KEY_HOME).index, 0);
  CHECK_EQ(popup_nav_key(list(7, -1), NEUI_KEY_END).index, 6);
  // Already there: handled, but nothing moved.
  CHECK(!popup_nav_key(list(7, 0), NEUI_KEY_HOME).changed);
  CHECK(popup_nav_key(list(7, 0), NEUI_KEY_HOME).handled);
}

TEST_CASE("popup nav: paging clamps even when wrapping is on")
{
  // A page step that jumped from the top of the list to the bottom would lose
  // the user completely, and no platform's list does it - so this is deliberate
  // asymmetry with the arrows above, not an oversight.
  CHECK_EQ(popup_nav_key(list(100, 2, true, 10), NEUI_KEY_PAGEUP).index, 0);
  CHECK_EQ(popup_nav_key(list(100, 95, true, 10), NEUI_KEY_PAGEDOWN).index, 99);
  CHECK_EQ(popup_nav_key(list(100, 50, true, 10), NEUI_KEY_PAGEUP).index, 40);
  CHECK_EQ(popup_nav_key(list(100, 50, true, 10), NEUI_KEY_PAGEDOWN).index, 60);
}

TEST_CASE("popup nav: a page step of zero or less falls back to ten")
{
  CHECK_EQ(popup_nav_key(list(100, 50, true, 0), NEUI_KEY_PAGEDOWN).index, 60);
  CHECK_EQ(popup_nav_key(list(100, 50, true, -3), NEUI_KEY_PAGEUP).index, 40);
}

TEST_CASE("popup nav: Enter commits a real selection and nothing else")
{
  auto hit = popup_nav_key(list(5, 3), NEUI_KEY_RETURN);
  CHECK(hit.handled);
  CHECK(hit.activate);
  CHECK(!hit.changed);
  CHECK_EQ(hit.index, 3);

  // Enter with nothing selected is deliberately NOT handled: there is nothing to
  // commit, and swallowing it would let a client's own "Enter accepts what I
  // typed" fall silently on the floor.
  auto miss = popup_nav_key(list(5, -1), NEUI_KEY_RETURN);
  CHECK(!miss.handled);
  CHECK(!miss.activate);
}

TEST_CASE("popup nav: a shrinking live count cannot leave a stale index behind")
{
  // THE FILTERED-LIST CASE. A browser re-declares its count on every keystroke,
  // so an index left pointing at row 40 of a list that is now 3 long has to be
  // treated as "none" rather than walked from - otherwise Up from a stale 40
  // steps to 39, which is off the end of what the client is drawing.
  CHECK_EQ(popup_nav_clamp_index(40, 3), -1);
  CHECK_EQ(popup_nav_clamp_index(3, 3), -1);     // one past the end
  CHECK_EQ(popup_nav_clamp_index(2, 3), 2);
  CHECK_EQ(popup_nav_clamp_index(-7, 3), -1);
  CHECK_EQ(popup_nav_clamp_index(0, 0), -1);

  auto r = popup_nav_key(list(3, 40), NEUI_KEY_UP);
  CHECK(r.handled);
  CHECK_EQ(r.index, 2);                          // entered from the bottom
  auto d = popup_nav_key(list(3, 40), NEUI_KEY_DOWN);
  CHECK_EQ(d.index, 0);                          // ...and from the top
}

TEST_CASE("popup nav: a one-item list is stable under every key")
{
  CHECK(!popup_nav_key(list(1, 0), NEUI_KEY_DOWN).changed);
  CHECK(!popup_nav_key(list(1, 0), NEUI_KEY_UP).changed);
  CHECK(!popup_nav_key(list(1, 0), NEUI_KEY_HOME).changed);
  CHECK(!popup_nav_key(list(1, 0), NEUI_KEY_END).changed);
  CHECK(!popup_nav_key(list(1, 0), NEUI_KEY_PAGEDOWN).changed);
  CHECK_EQ(popup_nav_key(list(1, -1), NEUI_KEY_DOWN).index, 0);
}
