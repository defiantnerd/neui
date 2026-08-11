// Tier-1 coverage for wheel_physical_delta (hosts/shared/wheel_direction.h).
//
// This exists where it does on purpose. The fact it depends on -
// neui_event_wheel_t::is_flipped, the OS's natural-scrolling inversion - can only
// be OBSERVED on macOS, and cannot be synthesised there either: NSEvent's
// isDirectionInvertedFromDevice is read-only, so no harness can fabricate an
// inverted event. The semantics therefore have to be tested away from the
// platform that supplies the input, on a fabricated payload.
//
// The property under test: a value consumer (a knob, a fader, a behavior value
// handler) must see the direction the user's FINGERS moved, whatever the OS
// preference and whatever the platform layer did with Shift. Content scrollers do
// not use this at all - the OS-adjusted delta is what they want.

#include "neui_test.h"

#include "wheel_direction.h"

using neui_detail::wheel_physical_delta;

namespace {

// The payload as a platform layer would fill it.
neui_event_wheel_t wheel(int delta, bool horizontal, uint32_t bmap, bool flipped)
{
  neui_event_wheel_t w{};
  w.delta         = delta;
  w.is_horizontal = horizontal ? 1 : 0;
  w.buttonmap     = bmap;
  w.is_flipped    = flipped ? 1 : 0;
  return w;
}

} // namespace

TEST_CASE("wheel: an ordinary vertical notch passes straight through")
{
  CHECK_EQ(wheel_physical_delta(wheel(3, false, 0, false)), 3);
  CHECK_EQ(wheel_physical_delta(wheel(-3, false, 0, false)), -3);
  CHECK_EQ(wheel_physical_delta(wheel(0, false, 0, false)), 0);
}

TEST_CASE("wheel: natural scrolling is undone, so the same swipe means the same thing")
{
  // THE REPORTED BUG (issue #21). macOS applies the preference before we see the
  // event, so a swipe DOWN arrives positive with it on and negative with it off.
  // A knob must see one direction for one gesture.
  const int with_pref_on  = wheel_physical_delta(wheel(+1, false, 0, /*flipped*/true));
  const int with_pref_off = wheel_physical_delta(wheel(-1, false, 0, /*flipped*/false));
  CHECK_EQ(with_pref_on, -1);
  CHECK_EQ(with_pref_off, -1);
  CHECK_EQ(with_pref_on, with_pref_off);   // the point: one gesture, one answer

  // ...and the same for the opposite swipe.
  CHECK_EQ(wheel_physical_delta(wheel(-1, false, 0, true)), +1);
  CHECK_EQ(wheel_physical_delta(wheel(+1, false, 0, false)), +1);
}

TEST_CASE("wheel: magnitude survives, so line-count semantics still work")
{
  // Callers multiply by |delta| for "one notch advances by step * lines", so the
  // helper must not flatten a multi-line notch to +/-1.
  CHECK_EQ(wheel_physical_delta(wheel(7, false, 0, true)), -7);
  CHECK_EQ(wheel_physical_delta(wheel(-7, false, 0, true)), 7);
}

TEST_CASE("wheel: the platform's Shift->horizontal flip is undone")
{
  // win32 + macOS xpl turn a Shift-held VERTICAL notch into a horizontal one AND
  // negate it, to match WM_MOUSEHWHEEL's "positive = scroll left". A value
  // handler has no horizontal axis, so Shift would silently reverse the control.
  CHECK_EQ(wheel_physical_delta(wheel(-1, true, NEUI_MK_SHIFT, false)), 1);
  CHECK_EQ(wheel_physical_delta(wheel(1, true, NEUI_MK_SHIFT, false)), -1);
}

TEST_CASE("wheel: a GENUINE horizontal notch is left alone")
{
  // A tilt wheel or a two-finger horizontal swipe is a real horizontal gesture,
  // not a converted vertical one - so the Shift rule must stay narrow. Getting
  // this wrong would reverse every tilt-wheel notch.
  CHECK_EQ(wheel_physical_delta(wheel(1, true, 0, false)), 1);
  CHECK_EQ(wheel_physical_delta(wheel(-1, true, 0, false)), -1);
  // Shift held on a VERTICAL notch is not a flip either - the platform layer only
  // negates when it also sets is_horizontal.
  CHECK_EQ(wheel_physical_delta(wheel(1, false, NEUI_MK_SHIFT, false)), 1);
}

TEST_CASE("wheel: both corrections compose, and only the intended ones")
{
  // Shift-flipped AND OS-inverted: two negations cancel.
  CHECK_EQ(wheel_physical_delta(wheel(1, true, NEUI_MK_SHIFT, true)), 1);
  CHECK_EQ(wheel_physical_delta(wheel(-1, true, NEUI_MK_SHIFT, true)), -1);
  // A genuine horizontal gesture under natural scrolling is inverted too - the
  // preference applies to both axes.
  CHECK_EQ(wheel_physical_delta(wheel(1, true, 0, true)), -1);
  // An unrelated modifier changes nothing.
  CHECK_EQ(wheel_physical_delta(wheel(1, true, NEUI_MK_CONTROL, false)), 1);
  CHECK_EQ(wheel_physical_delta(wheel(1, false, NEUI_MK_CONTROL, true)), -1);
}

TEST_CASE("wheel: the four-argument and payload forms agree")
{
  for (int d : { -2, -1, 0, 1, 2 })
    for (int h : { 0, 1 })
      for (uint32_t b : { 0u, (uint32_t)NEUI_MK_SHIFT })
        for (int f : { 0, 1 }) {
          neui_event_wheel_t w = wheel(d, h != 0, b, f != 0);
          CHECK_EQ(wheel_physical_delta(w),
                   wheel_physical_delta(d, h, b, f));
        }
}
