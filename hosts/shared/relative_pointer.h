#pragma once

#include <cstdint>
#include <cmath>

// Relative (unbounded) pointer mode - the state behind NEUI_API_POINTER
// (<neui/d/pointer.h>).
//
// The problem it solves: a knob or fader drag runs out of screen. A 200 px sweep
// starting 40 px from the top edge can only reach a fifth of its range before
// the pointer hits the edge and stops producing motion. Every serious audio UI
// answers this the same way - pin the visible cursor, keep consuming raw device
// motion, and let the drag continue for as long as the user keeps moving.
//
// THE MODEL: while relative mode is active, the host keeps reporting ordinary
// absolute widget-local MOUSE_MOVE coordinates - they simply stop being bounded
// by the screen. A virtual position is seeded from the press point and advanced
// by each raw delta, while the platform holds the real cursor at its anchor.
//
// This is deliberately NOT a switch to delta-valued events: every existing drag
// handler (KnobWidget, the behavior runtime's DRAG_* kinds, scrollbar drags)
// computes its own delta from the press point already, so they all gain
// unbounded travel with no changes. An event payload that changed meaning
// mid-drag would have needed every one of them touched, and would have made
// `x`/`y` mean two different things depending on invisible state.
//
// WHY THE ACCUMULATOR IS FLOAT: macOS reports fractional deltas
// (NSEvent.deltaX is a double, and a slow trackpad drag emits values well below
// 1.0). Accumulating those into int coordinates truncates every one of them to
// zero, so a careful slow drag - exactly the case fine-adjust exists for - would
// move the value not at all. The virtual position is therefore kept in float and
// only rounded when it is reported.

namespace neui_detail
{

  struct RelativePointer
  {
    bool     active = false;
    uint32_t widget = 0;    // the widget that owns the mode (0 when inactive)

    // Virtual widget-local position, in logical px. Unbounded by design: it may
    // go far outside the widget, and negative, which is what lets a drag keep
    // going past the screen edge.
    float vx = 0.0f;
    float vy = 0.0f;

    // Seed from the press point so the first reported move is continuous with
    // the button-down that started the drag - a jump here would read as a
    // discontinuity in the value being dragged.
    void begin(uint32_t owner, float start_x, float start_y)
    {
      active = true;
      widget = owner;
      vx     = start_x;
      vy     = start_y;
    }

    void end()
    {
      active = false;
      widget = 0;
      vx = vy = 0.0f;
    }

    // Advance by a raw device delta. Fractional deltas accumulate rather than
    // being dropped: three 0.4 px moves add up to 1.2 px and report 1, where
    // int accumulation would have reported 0 three times and lost the motion.
    void accumulate(float dx, float dy)
    {
      vx += dx;
      vy += dy;
    }

    // The values to put in a MOUSE_MOVE payload. Rounded half-away-from-zero so
    // the mapping is symmetric about 0 - plain (int) truncation biases every
    // negative delta toward zero, which would make a drag up the screen travel
    // measurably less far than the same drag down it.
    int report_x() const { return round_half_away(vx); }
    int report_y() const { return round_half_away(vy); }

    static int round_half_away(float v)
    {
      return (int)(v < 0.0f ? -std::floor(-v + 0.5f) : std::floor(v + 0.5f));
    }
  };

  // True when a motion event is the ECHO of our own warp-back to the anchor.
  //
  // win32 (SetCursorPos) and X11 (XWarpPointer) both generate a fresh motion
  // event for the warp itself, so a naive handler would read that as a delta of
  // exactly -(user delta) and the pointer would appear frozen. macOS needs none
  // of this: CGAssociateMouseAndMouseCursorPosition(false) decouples the cursor
  // from the device without moving it, so there is no warp and no echo.
  //
  // Comparing against the anchor is sound because the warp is the ONLY thing
  // that puts the pointer exactly there: a real user motion that happens to land
  // on the anchor is indistinguishable, but it is also a zero-delta event, so
  // treating it as an echo loses nothing.
  inline bool relative_is_warp_echo(int x, int y, int anchor_x, int anchor_y)
  {
    return x == anchor_x && y == anchor_y;
  }

} // namespace neui_detail
