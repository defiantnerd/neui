#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Periodic timers - the repaint / polling driver a client cannot build itself.
//
// Before this, the only way to drive the UI was widgets->invalidate, which a
// client can only call when it already knows something changed. Anything that
// has to WAKE UP to notice - draining an audio->UI queue, animating a VU meter,
// polling a rebuild flag - had no seam: a standalone app had to abandon run()
// for a hand-rolled pump_once() loop, and a plugin had to borrow a host timer.
//
// Usage:
//
//   neui_timer_api_t* timers =
//     (neui_timer_api_t*) neui->get_interface(sess, NEUI_API_TIMER);
//   uint32_t t = timers->add_timer(sess, 16);      // ~60 Hz
//   ...
//   case NEUI_EVENT_TIMER:
//     if (event->data.timer.timer_id == t) { drain_queue(); widgets->invalidate(...); }
//     break;
//   ...
//   timers->remove_timer(sess, t);
//
// Timers are SESSION-scoped, not widget-scoped: what they drive is usually the
// client's own state, not one widget, and the payload carries no widget (unlike
// every other event) precisely so a handler is not tempted to treat it as one.
//
// Granularity + coalescing. `interval_ms` is a MINIMUM, not a guarantee: neui
// runs one native tick per session at the shortest live interval, and fires
// every timer whose deadline has passed. A tick that arrives late (a blocking
// client callback, a busy DAW) fires the timer ONCE rather than catching up on
// missed periods - which is what animation and queue-draining want, and it
// means a slow handler cannot build an unbounded backlog of its own events.
// Anything below NEUI_TIMER_MIN_INTERVAL_MS is clamped to it; a 0 interval is
// rejected rather than becoming a spin loop.
//
// Event-loop contract. Timers are driven by whatever pumps the session, so they
// work under run(), under a hand-rolled pump_once() loop, and - the point for
// plugins - under NEUI_API_EMBED: on win32 / macOS the DAW's own pump services
// them, and on Linux embed->pump_and_tick() advances them. A client never needs
// its own thread, and must not call run() from a plugin (see <neui/d/embed.h>).
//
// Firing from inside a NEUI_EVENT_TIMER handler is safe: add_timer /
// remove_timer during dispatch take effect after the current tick completes,
// so removing the timer you are handling does not invalidate the walk.
#define NEUI_API_TIMER "com.defiantnerd.neui.extension.timer/0"

// Floor on the requested interval. 1 ms would be a spin loop against a 60 Hz
// compositor for no visible benefit; 4 ms still comfortably outpaces any
// display refresh a client can observe.
#define NEUI_TIMER_MIN_INTERVAL_MS 4

  typedef struct neui_timer_api
  {
    uint32_t neui_version;

    // Start a periodic timer. Returns a non-zero id, or 0 on failure (an
    // invalid session, a 0 interval, or the platform having no timer seam).
    // `interval_ms` is clamped up to NEUI_TIMER_MIN_INTERVAL_MS.
    uint32_t (NEUI_ABI* add_timer)(neui_session_t session, uint32_t interval_ms);

    // Stop and release a timer. Safe to call with an unknown / already-removed
    // id (returns false) and safe from inside a NEUI_EVENT_TIMER handler,
    // including for the timer being handled.
    bool (NEUI_ABI* remove_timer)(neui_session_t session, uint32_t timer_id);

    // Change a live timer's interval. The new period applies from the next
    // fire; returns false for an unknown id or a 0 interval. Equivalent to
    // remove + add except the id survives, so a client animating at a variable
    // rate does not have to rewrite its stored id.
    bool (NEUI_ABI* set_timer_interval)(neui_session_t session,
                                        uint32_t timer_id,
                                        uint32_t interval_ms);

    // Append new methods at the end (vtable-append evolution rule).
  } neui_timer_api_t;

#ifdef __cplusplus
}
#endif
