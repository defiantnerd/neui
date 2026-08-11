#pragma once

#include <cstdint>

// events.h is not self-sufficient (its payloads use neui_item_t from api.h), so
// include that first rather than relying on the includer's order.
#include <neui/d/api.h>
#include <neui/d/events.h>

// Physical wheel direction, for consumers that have no content to scroll.
//
// THE PROBLEM (issue #21). A wheel delta reaches a client after up to two sign
// changes it cannot see:
//
//   1. The PLATFORM LAYER turns a Shift-held vertical notch into a horizontal one
//      and NEGATES the delta, to match the WM_MOUSEHWHEEL "positive = scroll
//      left" convention. That flip lives in the platform layer by design.
//   2. The OS applies the user's natural-scrolling preference before we ever see
//      the event. macOS says so via NSEvent.isDirectionInvertedFromDevice, which
//      the platform layer forwards as neui_event_wheel_t::is_flipped.
//
// For CONTENT SCROLLING both are right as delivered: moving the content with your
// fingers is exactly what natural scrolling is for, and a SECTION or GRID should
// keep using the delta as-is. For a VALUE CONTROL there is no content to move -
// the user's mental model is the direction their fingers actually travelled - so a
// knob, fader or value handler wants the direction BEFORE the OS inverted it.
// With natural scrolling on (the macOS default) a swipe down was increasing a
// knob's value.
//
// This is the one place that undoes both, so every value consumer agrees. It is
// deliberately portable and Tier-1 tested rather than living in the widgets:
// only macOS can report inversion at all (see is_flipped's documentation), so the
// SEMANTICS need coverage somewhere that is not the one platform that has the
// fact.

namespace neui_detail
{
  // Sign-normalised delta for a value consumer: positive = the user's fingers /
  // wheel moved UP (or away), whatever the OS preference and whatever the
  // platform layer did with Shift. Magnitude is preserved, so a caller can still
  // multiply by |delta| for line-count semantics.
  inline int wheel_physical_delta(int delta, int is_horizontal,
                                  uint32_t buttonmap, int is_flipped)
  {
    // 1. Undo the platform layer's Shift->horizontal flip. DELIBERATELY NARROW:
    //    only a horizontal notch WITH Shift held is treated as a flipped vertical
    //    one. A genuine tilt-wheel / trackpad horizontal notch (is_horizontal, no
    //    Shift) is left exactly as it arrives - it is a real horizontal gesture,
    //    not a converted one.
    if (is_horizontal && (buttonmap & NEUI_MK_SHIFT) != 0)
      delta = -delta;

    // 2. Undo the OS's natural-scrolling inversion. Note this applies to a
    //    genuine horizontal gesture too: the preference inverts both axes.
    if (is_flipped) delta = -delta;

    return delta;
  }

  // Convenience for the common case: the whole payload.
  inline int wheel_physical_delta(const neui_event_wheel_t& w)
  {
    return wheel_physical_delta(w.delta, w.is_horizontal, w.buttonmap,
                                w.is_flipped);
  }
}
