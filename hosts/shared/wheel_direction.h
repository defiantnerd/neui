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

  // ---------------------------------------------------------------------------
  // WHICH WAY A VALUE CONTROL MOVES - the single source of truth.
  //
  // THE CONVENTION: wheel/fingers UP DECREASES the value, DOWN INCREASES it.
  // That is the audio-plugin convention, decided by the project owner
  // (2026-08-11), and it is what a behavior asset's WHEEL handler has always
  // done. The built-in KNOB and SLIDER used to do the OPPOSITE - on all three
  // hosts - so a behavior-driven knob and a native one felt opposite in the same
  // UI, and the behavior runtime's comment claimed it "matches the existing KNOB"
  // while doing the reverse of it.
  //
  // WHY IT IS A FUNCTION AND NOT FOUR OPEN-CODED TERNARIES. That inconsistency
  // survived because the sign lived in five places (xpl KNOB, xpl SLIDER, the
  // behavior runtime, the native macOS knob, the native win32 knob) and nothing
  // tied them together. Everything that moves a VALUE from a wheel now calls
  // this, so the convention is one line, Tier-1 tested, and reversing it is a
  // one-line edit rather than an archaeology exercise.
  //
  // Takes the PHYSICAL delta (run it through wheel_physical_delta first), so the
  // answer does not depend on the user's natural-scrolling preference.
  inline float wheel_value_sign(int physical_delta)
  {
    return (physical_delta > 0) ? -1.0f : 1.0f;
  }

  // |delta|, for callers that scale by the line count (one notch should advance
  // by step * lines, not by a single step - otherwise the wheel feels dead at
  // typical step values).
  inline int wheel_magnitude(int physical_delta)
  {
    return (physical_delta < 0) ? -physical_delta : physical_delta;
  }
}
