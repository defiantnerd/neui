// Tier-1 coverage for the portable half of relative (unbounded) pointer mode
// (hosts/shared/relative_pointer.h).
//
// Two properties here are real correctness requirements rather than bookkeeping,
// and both were chosen deliberately over the obvious implementation:
//
//   - FRACTIONAL ACCUMULATION. macOS reports fractional deltas, and a slow
//     trackpad drag emits values well below 1.0. Accumulating into an int would
//     truncate every one to zero, so a careful slow drag - the exact case
//     fine-adjust exists for - would not move the value at all.
//   - SYMMETRIC ROUNDING. Plain (int) truncation biases negative values toward
//     zero, so a drag up the screen would travel measurably less far than the
//     same drag down it.

#include "neui_test.h"

#include "relative_pointer.h"

using namespace neui_detail;

TEST_CASE("relative pointer starts inactive")
{
  RelativePointer rp;
  CHECK_FALSE(rp.active);
  CHECK_EQ(rp.widget, 0u);
}

TEST_CASE("relative begin seeds the virtual position from the press point")
{
  // Continuity with the button-down that started the drag: a jump here would
  // read as a discontinuity in the value being dragged.
  RelativePointer rp;
  rp.begin(42, 30.0f, 17.0f);
  CHECK(rp.active);
  CHECK_EQ(rp.widget, 42u);
  CHECK_EQ(rp.report_x(), 30);
  CHECK_EQ(rp.report_y(), 17);
}

TEST_CASE("relative end clears the state")
{
  RelativePointer rp;
  rp.begin(7, 5.0f, 5.0f);
  rp.accumulate(100.0f, 100.0f);
  rp.end();
  CHECK_FALSE(rp.active);
  CHECK_EQ(rp.widget, 0u);
  CHECK_EQ(rp.report_x(), 0);
  CHECK_EQ(rp.report_y(), 0);
}

TEST_CASE("relative accumulation is unbounded in both directions")
{
  // The whole point: the virtual position must be free to leave the widget and
  // go negative, which is what lets a drag continue past a screen edge.
  RelativePointer rp;
  rp.begin(1, 10.0f, 10.0f);
  for (int i = 0; i < 500; ++i) rp.accumulate(0.0f, -4.0f);
  CHECK_EQ(rp.report_y(), 10 - 2000);
  for (int i = 0; i < 1000; ++i) rp.accumulate(9.0f, 0.0f);
  CHECK_EQ(rp.report_x(), 10 + 9000);
}

TEST_CASE("relative sub-pixel deltas accumulate instead of being dropped")
{
  // The macOS case. Each delta rounds to 0 on its own; together they must move.
  RelativePointer rp;
  rp.begin(1, 0.0f, 0.0f);
  rp.accumulate(0.4f, 0.0f);
  CHECK_EQ(rp.report_x(), 0);          // 0.4 -> 0, correctly
  rp.accumulate(0.4f, 0.0f);
  CHECK_EQ(rp.report_x(), 1);          // 0.8 -> 1
  rp.accumulate(0.4f, 0.0f);
  CHECK_EQ(rp.report_x(), 1);          // 1.2 -> 1

  // Ten 0.3 px moves are 3 px, not 0 - an int accumulator would report 0 ten
  // times and silently swallow the whole gesture.
  RelativePointer slow;
  slow.begin(1, 0.0f, 0.0f);
  for (int i = 0; i < 10; ++i) slow.accumulate(0.3f, 0.0f);
  CHECK_EQ(slow.report_x(), 3);
}

TEST_CASE("relative rounding is symmetric about zero")
{
  // Truncation would make these asymmetric (+2 vs -1 for the same magnitude),
  // so an upward drag would cover less ground than an identical downward one.
  CHECK_EQ(RelativePointer::round_half_away(2.5f),  3);
  CHECK_EQ(RelativePointer::round_half_away(-2.5f), -3);
  CHECK_EQ(RelativePointer::round_half_away(1.7f),  2);
  CHECK_EQ(RelativePointer::round_half_away(-1.7f), -2);
  CHECK_EQ(RelativePointer::round_half_away(0.5f),  1);
  CHECK_EQ(RelativePointer::round_half_away(-0.5f), -1);
  CHECK_EQ(RelativePointer::round_half_away(0.0f),  0);

  // The property that matters: equal and opposite drags travel equally far.
  RelativePointer up, down;
  up.begin(1, 0.0f, 0.0f);
  down.begin(1, 0.0f, 0.0f);
  for (int i = 0; i < 7; ++i) { up.accumulate(0.0f, -1.5f); down.accumulate(0.0f, 1.5f); }
  CHECK_EQ(up.report_y(), -down.report_y());
}

TEST_CASE("relative warp echo is detected only at the anchor")
{
  CHECK(relative_is_warp_echo(100, 200, 100, 200));
  CHECK_FALSE(relative_is_warp_echo(101, 200, 100, 200));
  CHECK_FALSE(relative_is_warp_echo(100, 201, 100, 200));
  CHECK_FALSE(relative_is_warp_echo(-1, -1, 100, 200));
  // Negative anchors are legal (a window on a monitor left of the primary).
  CHECK(relative_is_warp_echo(-40, -5, -40, -5));
}

TEST_CASE("relative accumulate is inert on a fresh object until begin")
{
  // A platform layer that feeds deltas without checking `active` must not build
  // up a phantom position that leaks into the next real drag.
  RelativePointer rp;
  rp.accumulate(50.0f, 50.0f);
  rp.begin(3, 1.0f, 2.0f);
  CHECK_EQ(rp.report_x(), 1);    // begin overwrites, never adds to, the position
  CHECK_EQ(rp.report_y(), 2);
}
