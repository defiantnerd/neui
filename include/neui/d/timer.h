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
// Mutating from inside a NEUI_EVENT_TIMER handler is safe. remove_timer takes
// effect IMMEDIATELY: a timer removed by an earlier handler in the same tick
// does not fire in that tick, and a timer may remove itself. add_timer's new
// timer starts a full interval later, so it never fires in the tick that
// created it.
//
// Two things are NOT safe from a handler, both of which would be a
// use-after-free rather than a surprise:
//   - destroying the session (neui->destroy). Nothing survives it, and the
//     dispatch walk resumes on freed memory. Tear a session down from outside
//     the pump.
//   - relying on nested delivery: if a handler re-enters the pump (opens a
//     modal dialog or message box, shows a popup menu, calls pump_once), timer
//     ticks are SUPPRESSED for the duration of that nested pump rather than
//     nesting inside your handler. An animation therefore pauses while a modal
//     dialog your handler opened is up, and resumes when it closes.
// Host support: exposed by the crossplatform host ("neui.host.crossplatform")
// ONLY, like NEUI_API_EMBED. This matters on Windows and macOS, where
// neui_get_api(NULL) returns the NATIVE host first: a client that takes the
// default there gets NULL from get_interface and has no timers. Plugin builds
// should already be selecting the xpl host explicitly (see <neui/d/embed.h> and
// the README); a standalone app that wants timers has to do the same.
// Feature-detect - never assume the pointer is non-null.
#define NEUI_API_TIMER "com.defiantnerd.neui.extension.timer/0"

// Floor on the requested interval. 1 ms would be a spin loop against a 60 Hz
// compositor for no visible benefit; 4 ms still comfortably outpaces any
// display refresh a client can observe.
#define NEUI_TIMER_MIN_INTERVAL_MS 4

// Per-platform floors above that value, worth knowing before tuning a rate:
//   win32: SetTimer clamps to USER_TIMER_MINIMUM (10 ms), so 4..9 ms all tick
//          at ~10 ms.
//   linux, embedded: the tick rides the DAW's pump_and_tick cadence, so the
//          effective rate is whatever the host calls it at (an 8 ms timer under
//          a 30 Hz host idle fires at ~33 ms).
//   null platform: accepts timers and never fires them (it has no event loop).

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
