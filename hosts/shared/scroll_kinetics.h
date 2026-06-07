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
  // Quiet-time debounce in 60 Hz ticks. The bounce step refuses to lerp
  // until this many consecutive ticks have elapsed without a wheel event,
  // so the spring-back can't run concurrently with an active scroll stream
  // (which would otherwise look like the rubber-band oscillating in place
  // while the user keeps wheeling). Windows precision-touchpad inertia
  // arrives as plain WM_MOUSEWHEEL with no "this is inertia" flag, so the
  // library can't distinguish active scrolling from coast-down - debouncing
  // is the only portable signal. ~150 ms covers typical inter-notch
  // intervals on classic wheels and inertia decay on precision touchpads.
  inline constexpr int    SCROLL_BOUNCE_DEBOUNCE_TICKS = 9;   // ~150 ms at 60 Hz

  // ---- Types --------------------------------------------------------------

  // Per-widget kinetics state. Stored inline on whatever owns the scrolling
  // surface (GridModel for GRID; SECTION's scroll state struct).
  //
  // `raw_px` is the **damped display position** (== the value the caller
  // commits to its model) - not an unbounded integral of input. The rubber-
  // band damping is applied at INPUT time inside scroll_wheel, so raw_px
  // is bounded by `[-L, max_px + L]` (L = the asymptotic rubber limit).
  // Without that bound, a user who keeps wheeling past an edge would pile
  // an arbitrary amount of input onto raw_px while the display saturates
  // - and on release the bounce would have to crawl all of it back at
  // 29% per tick, making the rubber-band visibly linger long after the
  // visible position had reached its asymptote. With damping-on-input,
  // bounce time is constant in L regardless of how hard the edge was hit.
  struct ScrollKinetics {
    double raw_px            = 0.0;
    int    last_commit_px    = 0;
    bool   suppress_momentum = false;
    // Consecutive bounce-timer ticks with no wheel event since the last
    // edge-extending input. While < SCROLL_BOUNCE_DEBOUNCE_TICKS the
    // bounce step keeps the timer alive but skips the lerp - so a wheel
    // stream that runs alongside the bounce (Win32 inertia notches, a
    // user who's still wheeling past the edge) doesn't fight a partially-
    // settled spring. Reset to 0 by scroll_wheel on every event that
    // mutates raw_px while overscrolled.
    int    quiet_ticks       = 0;
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

  // Asymptotic rubber-band limit in logical px (capped fraction of the
  // viewport, hard-capped at SCROLL_OVERSCROLL_MAX). Single source of
  // truth so scroll_wheel and the legacy `scroll_rubber` / `scroll_overshoot`
  // shims agree on what "the edge of the rubber" means.
  inline double scroll_rubber_limit(double dim)
  {
    double limit = dim * 0.5;
    if (limit > SCROLL_OVERSCROLL_MAX) limit = SCROLL_OVERSCROLL_MAX;
    if (limit < 1.0) limit = 1.0;
    return limit;
  }

  // WebKit-style rubber-band overshoot: maps a non-negative cumulative-
  // input distance `x` (the amount the user pulled past the edge) into the
  // damped, asymptotically-bounded display distance. Approaches `limit`
  // (a capped fraction of the viewport) but never reaches it.
  inline double scroll_overshoot(double x, double dim)
  {
    double limit = scroll_rubber_limit(dim);
    return (1.0 - 1.0 / (x * SCROLL_OVERSCROLL_STIFFNESS / limit + 1.0)) * limit;
  }

  // Inverse of scroll_overshoot: given a damped display overshoot in
  // [0, limit) returns the cumulative-input distance that would have
  // produced it. Used by scroll_wheel to fold a new input delta into the
  // stored display position - re-extracts the implicit input integral,
  // adds the delta, re-applies the forward map. Asymptotes to infinity as
  // `over` approaches `limit`; the caller clamps just below limit so the
  // result stays finite.
  inline double scroll_overshoot_inverse(double over, double dim)
  {
    double limit = scroll_rubber_limit(dim);
    // Clamp `over` slightly below `limit` so the divisor stays finite.
    // The forward map can never legitimately produce `over >= limit`, so
    // any caller hitting this clamp has a numerical glitch (or seeded
    // raw_px externally outside the elastic range).
    double safe_max = limit * 0.999;
    if (over > safe_max) over = safe_max;
    if (over < 0.0)      over = 0.0;
    return limit * over / (SCROLL_OVERSCROLL_STIFFNESS * (limit - over));
  }

  // Legacy display-from-input rubber map. Retained as a thin utility for
  // tests / callers that still want to map an unbounded input position to
  // its damped display position. NOT used by the new scroll_wheel: that
  // path applies damping at input time so raw_px IS the damped position.
  inline double scroll_rubber(double raw, double max_px, double dim)
  {
    if (raw < 0.0)    return -scroll_overshoot(-raw, dim);
    if (raw > max_px) return  max_px + scroll_overshoot(raw - max_px, dim);
    return raw;
  }

  // Fold a wheel-input delta into the damped display position. Inverts the
  // rubber map to recover the implicit cumulative input, adds the delta,
  // re-applies the forward map. In-range portions are linear; portions
  // that lie past an edge are damped. raw_px (in/out) is the damped
  // display position - bounded by `[-limit, max_px + limit]`.
  inline double scroll_apply_damped_input(double raw, double delta,
                                            double max_px, double dim)
  {
    double limit = scroll_rubber_limit(dim);

    // Step 1: invert raw -> cumulative input integral.
    double integral;
    if (raw < 0.0) {
      integral = -scroll_overshoot_inverse(-raw, dim);
    } else if (raw > max_px) {
      integral = max_px + scroll_overshoot_inverse(raw - max_px, dim);
    } else {
      integral = raw;
    }

    // Step 2: apply the delta to the integral (natural-scroll subtract).
    integral -= delta;

    // Step 3: re-apply the forward rubber map.
    if (integral < 0.0) {
      return -scroll_overshoot(-integral, dim);
    } else if (integral > max_px) {
      return max_px + scroll_overshoot(integral - max_px, dim);
    }
    (void)limit;
    return integral;
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
                                         double viewport_px,
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

    // Resync to the committed model position if something else moved it
    // (keyboard nav, scrollbar drag, public API).
    if (current_committed_px != k.last_commit_px)
      k.raw_px = (double)current_committed_px;

    // Classic mouse wheel (no precise deltas, no gesture phase) doesn't
    // rubber-band - hard-clamp instead. Matches the macOS platform default
    // for a regular two-button mouse.
    bool legacy = !in.precise && !in.phase_began && !in.phase_changed &&
                  !in.phase_ended && !in.momentum;
    if (legacy) {
      k.raw_px -= in.delta_px;
      if (k.raw_px < 0.0)         k.raw_px = 0.0;
      else if (k.raw_px > max_px) k.raw_px = max_px;
    } else {
      // Precise / finger / momentum: apply rubber damping at input time so
      // raw_px stays the damped display position. A user who keeps wheeling
      // past the edge sees the rubber stretch asymptotically toward
      // `limit` (further notches give diminishing pixels), instead of
      // piling unbounded cumulative input into raw_px - the latter would
      // make the spring-back's settle-time grow with input magnitude even
      // though the visible position had long since saturated.
      k.raw_px = scroll_apply_damped_input(k.raw_px, in.delta_px,
                                             max_px, viewport_px);
    }

    act.changed = true;

    bool overscrolled = (k.raw_px < 0.0 || k.raw_px > max_px);
    bool finger_down  = in.phase_began || in.phase_changed;
    // Any wheel event that touched raw_px resets the quiet-tick counter -
    // the bounce step will hold off the lerp until the stream goes silent
    // for SCROLL_BOUNCE_DEBOUNCE_TICKS.
    k.quiet_ticks = 0;
    // Spring back the moment we're overscrolled and the user is no longer
    // dragging (finger lifted, or an inertial event hit the edge). Arm
    // momentum-suppression unconditionally: a finger-throw past the edge
    // fires phase_ended (not momentum), but macOS will then emit a
    // momentum stream in the same direction. Without arming here, the
    // first of those momentum events sails through the early-return check
    // and pushes raw_px further past the edge for one frame before the
    // suppression takes hold - visible as a one-frame "kick back" against
    // the bounce. Arm now so the entire inertial stream is swallowed.
    if (overscrolled && !finger_down) {
      k.suppress_momentum = true;
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
    // Already at-or-within-eps of the target: snap and stop. The debounce
    // gate doesn't apply here - there's nothing to lerp, so we hand the
    // timer back immediately rather than spinning for the quiet window.
    if (std::fabs(d) < SCROLL_BOUNCE_EPS) {
      k.raw_px = target;
      // NB: leave suppress_momentum set - the inertial stream can outlast
      // the spring-back; it is cleared on momentum-end / new gesture.
      return false;
    }

    // Quiet-time gate: while wheel events are still arriving (or recently
    // arrived) keep the timer alive but skip the lerp - otherwise the
    // bounce would visibly pull the rubber back between events while the
    // user is still scrolling. Each wheel event resets quiet_ticks; once
    // the stream has been silent for SCROLL_BOUNCE_DEBOUNCE_TICKS we
    // proceed with the normal spring-back.
    if (k.quiet_ticks < SCROLL_BOUNCE_DEBOUNCE_TICKS) {
      ++k.quiet_ticks;
      return true;
    }

    k.raw_px += d * SCROLL_BOUNCE_LERP;
    return true;
  }

} // namespace neui_detail
