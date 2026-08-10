#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"   // neui_widget_t

#ifdef __cplusplus
extern "C" {
#endif

// Relative (unbounded) pointer mode - the seam that makes a knob or fader drag
// keep working when the pointer runs out of screen.
//
// THE PROBLEM. A 200 px sweep that starts 40 px from the top of the display can
// only reach a fifth of its range: the pointer hits the edge, stops producing
// motion, and the drag dies with the value stuck. Users read this as the control
// being broken, and it is worst exactly where it hurts most - a knob near the
// top of a plugin editor docked at the top of the screen.
//
// THE FIX. While relative mode is active the platform pins the VISIBLE cursor in
// place and keeps consuming raw device motion, so the drag continues for as long
// as the user keeps moving. On end the cursor is restored to where the drag
// began, which is what every DAW and audio plugin does and what users expect.
//
// WHAT YOUR EVENT HANDLER SEES: nothing new. MOUSE_MOVE keeps carrying ordinary
// absolute widget-local x/y - those coordinates simply stop being bounded by the
// screen, and may run far outside the widget or go negative. A handler that
// computes its delta from the press point (which is what every drag handler
// already does) gains unbounded travel with NO code changes. This is deliberate:
// switching the payload to delta values would have made x/y mean two different
// things depending on invisible state.
//
// Usage - bracket it around a drag, which pairs 1:1 with the gesture events:
//
//   case NEUI_EVENT_MOUSE_BUTTON_DOWN:
//     if (ev->data.mouse.widget.id == my_knob) {
//       pointer->begin_relative(sess, my_knob);      // cursor pins + hides
//       ...
//     }
//     break;
//   case NEUI_EVENT_MOUSE_BUTTON_UP:
//     pointer->end_relative(sess);                   // cursor restored
//     break;
//
// The built-in KNOB and SLIDER, and the behavior asset's DRAG_* handlers, do NOT
// enter relative mode on their own - it stays opt-in. Hiding the cursor is a
// visible, slightly startling change, and a client that wants the plain bounded
// behaviour (or that draws its own readout at the cursor) should not have it
// forced on. A client wanting it for a native KNOB brackets it from the widget's
// own DOWN / UP events exactly as above.
//
// CURSOR VISIBILITY. begin_relative hides the pointer, because a cursor pinned
// in place while the value moves reads as a frozen UI. That hide is independent
// of NEUI_ATTR_CURSOR ("none") and is restored on end_relative. If the session is
// torn down mid-drag the host releases it anyway - an unbalanced hide would
// otherwise cost the whole process its pointer.
//
// Host support: the CROSSPLATFORM host only ("neui.host.crossplatform"), like
// NEUI_API_TIMER and NEUI_API_EMBED. On Windows and macOS neui_get_api(NULL)
// returns the NATIVE host first, so a client taking the default there gets NULL.
// Feature-detect; never assume the pointer is non-null. begin_relative also
// returns false on a platform with no warping seam (iOS, null), so a touch
// platform degrades to ordinary bounded drags rather than misbehaving.
#define NEUI_API_POINTER "com.defiantnerd.neui.extension.pointer/0"

  typedef struct neui_pointer_api
  {
    uint32_t neui_version;

    // Enter relative mode for a drag on `widget`. Returns false when the
    // platform has no warping seam, the widget is invalid, or relative mode is
    // already active (it is NOT reference-counted - a second begin without an
    // end is a bug, not a nesting request).
    //
    // Call from a MOUSE_BUTTON_DOWN handler. The virtual position is seeded from
    // the current pointer position, so the first MOUSE_MOVE is continuous with
    // the press.
    bool (NEUI_ABI* begin_relative)(neui_session_t session, neui_widget_t widget);

    // Leave relative mode: the cursor is unhidden and warped back to where
    // begin_relative was called. Safe to call when not active (returns without
    // doing anything), so a MOUSE_BUTTON_UP handler can call it unconditionally.
    void (NEUI_ABI* end_relative)(neui_session_t session);

    // Whether relative mode is currently active for this session.
    bool (NEUI_ABI* is_relative)(neui_session_t session);

    // Append new methods at the end (vtable-append evolution rule).
  } neui_pointer_api_t;

#ifdef __cplusplus
}
#endif
