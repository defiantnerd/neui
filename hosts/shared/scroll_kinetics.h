#pragma once

#include <cmath>
#include <cstdint>

// Generic smooth-scroll kinetics: pixel-precise scrolling with inertial
// momentum + WebKit-style elastic rubber-band. Position-px in, position-px
// out - the caller decides how to decompose the resulting commit position
// into its own data model (row index + fine offset for GRID; a flat px
// offset for a scrolling SECTION container; whatever shape comes next).
//
// Shared by:
//   - hosts/shared/grid_model.h (GRID widget; row-indexed scroll + fine px)
//   - hosts/shared/widget_section_scroll.h (planned: scrolling SECTION; flat px)
//
// Hosts that have rich wheel data (macOS NSEvent phase / momentum / precise
// deltas; eventually Win32 high-precision touchpad) feed every wheel event
// in via scroll_wheel. Hosts without that detail (classic mouse wheel
// notches) pass a precise=false delta; the kinetics hard-clamp instead of
// rubber-banding to match the platform default.
//
// All values are logical pixels at 96 DPI.

namespace neui_detail
{
  // ---- Tuning constants ---------------------------------------------------

  // Spring-back lerp factor per 60 Hz tick (higher = snappier). Exponential
  // ease, so halving the per-tick decay roughly doubles the settle time.
  inline constexpr double SCROLL_BOUNCE_LERP = 0.29;
  // Below this pixel distance the spring-back snaps to target and stops.
  inline constexpr double SCROLL_BOUNCE_EPS = 0.5;
  // Hard cap on how far the content can elastically stretch past an edge
  // (logical px) - keeps the rubber-band tight on a hard flick.
  inline constexpr double SCROLL_OVERSCROLL_MAX = 60.0;
  // Stretch stiffness: smaller = more resistance (less travel per unit pull).
  inline constexpr double SCROLL_OVERSCROLL_STIFFNESS = 0.5;

  // ---- Types --------------------------------------------------------------

  // Per-widget kinetics state. Stored inline on whatever owns the scrolling
  // surface (GridModel for GRID; SECTION's scroll state struct).
  struct ScrollKinetics {
    double raw_px            = 0.0;
    int    last_commit_px    = 0;
    bool   suppress_momentum = false;
  };

  // Platform-neutral description of a wheel event. The host fills this in
  // from its native event (NSEvent on macOS; synthesised on Win32 from
  // WM_MOUSEWHEEL + SPI_GETWHEELSCROLLLINES).
  struct ScrollWheelInput {
    double delta_px      = 0.0;    // logical px; subtracted from raw (natural scroll)
    bool   precise       = false;  // trackpad / Magic Mouse (vs classic wheel)
    bool   phase_began   = false;  // finger-down gesture started
    bool   phase_changed = false;  // finger-down gesture moving
    bool   phase_ended   = false;  // finger lifted / cancelled
    bool   momentum      = false;  // an inertial (post-lift) event
    bool   momentum_ended = false; // last inertial event
  };

  // Outcome of scroll_wheel - tells the host how to drive its bounce
  // timer + repaint.
  struct ScrollWheelAction {
    bool stop_bounce  = false;   // a new gesture began - cancel any spring-back
    bool start_bounce = false;   // overscrolled + released - begin spring-back
    bool changed      = false;   // model scroll position changed - repaint
  };

  // ---- Rubber-band mapping ------------------------------------------------

  // WebKit-style rubber-band overshoot: maps an unbounded pull distance `x`
  // (>= 0) into a damped, asymptotically-bounded display distance. It
  // approaches `limit` (a capped fraction of the viewport) but never reaches
  // it, so the further you pull the less it gives.
  inline double scroll_overshoot(double x, double dim)
  {
    double limit = dim * 0.5;
    if (limit > SCROLL_OVERSCROLL_MAX) limit = SCROLL_OVERSCROLL_MAX;
    if (limit <= 0.0) limit = 1.0;
    return (1.0 - 1.0 / (x * SCROLL_OVERSCROLL_STIFFNESS / limit + 1.0)) * limit;
  }

  // Map the unbounded raw scroll position to the elastic display position.
  // In-range values pass through; out-of-range values are damped.
  inline double scroll_rubber(double raw, double max_px, double dim)
  {
    if (raw < 0.0)    return -scroll_overshoot(-raw, dim);
    if (raw > max_px) return  max_px + scroll_overshoot(raw - max_px, dim);
    return raw;
  }

  // ---- Wheel + bounce -----------------------------------------------------

  // Apply a wheel event to the kinetics. Mirrors the elastic logic: free
  // pixel scrolling in-range, damped stretch past the edges, and an
  // immediate spring-back when an inertial event runs into an edge (the
  // remaining momentum stream is then swallowed so it can't re-stretch).
  //
  // Inputs:
  //   - max_px: caller's max legal scroll position (0 when content fits)
  //   - viewport_px: caller's viewport extent along the scrolling axis
  //                  (used by the rubber-band curve to size the overshoot)
  //   - current_committed_px: caller's currently-committed position; the
  //                           kinetics re-syncs raw_px to this if the model
  //                           moved externally (keyboard, drag, API).
  //
  // Side effect: updates k.raw_px. The caller is responsible for mapping
  // k.raw_px through scroll_rubber() and decomposing the result into its
  // own data model + updating k.last_commit_px to match (so the next
  // external-mutation detection works).
  inline ScrollWheelAction scroll_wheel(ScrollKinetics& k,
                                         const ScrollWheelInput& in,
                                         double max_px,
                                         double /*viewport_px*/,
                                         int current_committed_px)
  {
    ScrollWheelAction act;

    // A new finger-down gesture cancels any spring-back + momentum suppression.
    if (in.phase_began) {
      act.stop_bounce = true;
      k.suppress_momentum = false;
    }

    // Already springing back from a momentum overscroll: swallow the rest of
    // the inertial stream so it can't re-stretch the edge mid-settle.
    if (in.momentum && k.suppress_momentum) {
      if (in.momentum_ended) k.suppress_momentum = false;
      return act;
    }

    // Resync the raw integral if something else moved the model since the
    // last commit (keyboard nav, scrollbar drag, public API).
    if (current_committed_px != k.last_commit_px)
      k.raw_px = (double)current_committed_px;

    k.raw_px -= in.delta_px;

    // Classic mouse wheel (no precise deltas, no gesture phase) doesn't
    // rubber-band - hard-clamp instead. Matches the macOS platform default
    // for a regular two-button mouse.
    bool legacy = !in.precise && !in.phase_began && !in.phase_changed &&
                  !in.phase_ended && !in.momentum;
    if (legacy) {
      if (k.raw_px < 0.0)         k.raw_px = 0.0;
      else if (k.raw_px > max_px) k.raw_px = max_px;
    }

    act.changed = true;

    bool overscrolled = (k.raw_px < 0.0 || k.raw_px > max_px);
    bool finger_down  = in.phase_began || in.phase_changed;
    // Spring back the moment we're overscrolled and the user is no longer
    // dragging (finger lifted, or an inertial event hit the edge). Suppress
    // the residual momentum so the bounce isn't fought.
    if (overscrolled && !finger_down) {
      if (in.momentum) k.suppress_momentum = true;
      act.start_bounce = true;
    }
    if (in.momentum_ended) k.suppress_momentum = false;
    return act;
  }

  // One spring-back animation step (call at ~60 Hz). Returns true while
  // still animating, false once settled (the host stops its timer). Does
  // NOT update the caller's model directly - it updates k.raw_px; the
  // caller maps it through scroll_rubber and commits.
  //
  // Returns false (without touching k) if `current_committed_px` no longer
  // matches `k.last_commit_px` - something else moved the model since the
  // last commit, so the spring must yield rather than clobber it.
  inline bool scroll_bounce_step(ScrollKinetics& k, double max_px,
                                  double /*viewport_px*/,
                                  int current_committed_px)
  {
    if (current_committed_px != k.last_commit_px) return false;
    double target = k.raw_px;
    if (target < 0.0)         target = 0.0;
    else if (target > max_px) target = max_px;
    double d = target - k.raw_px;
    if (std::fabs(d) < SCROLL_BOUNCE_EPS) {
      k.raw_px = target;
      // NB: leave suppress_momentum set - the inertial stream can outlast
      // the spring-back; it is cleared on momentum-end / new gesture.
      return false;
    }
    k.raw_px += d * SCROLL_BOUNCE_LERP;
    return true;
  }

} // namespace neui_detail
