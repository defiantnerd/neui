#pragma once

#include <cstdint>

// Paint pacing for a target that presents without waiting for vblank.
//
// Direct2D presents with sync interval 0 (backends/d2d/d2d_backend.cpp), so the
// UI thread never blocks in Present: with vsync, every animating window on a
// thread shared about 170-180 presents a second and each paint waited for the
// display. The compositor still shows at most one frame per refresh, so
// painting faster only burns CPU: a drag on a 1000 Hz mouse would paint about
// three times the refresh rate. A paint that comes less than 90% of a refresh
// interval after the target's last present is therefore held back until then,
// with its damage kept, so input-driven repaints and animation ticks share one
// frame per refresh. 90%, not 100%: timers fire late and on a coarse grid
// (Windows: ~15.6 ms), and a held paint should not slip a whole refresh. For
// the same reason a wait shorter than k_min_wait (which the timer would round
// up to its next tick, losing a whole frame) is not held either, unless that
// would put two presents within half a refresh. Times are taken when a paint
// starts, so the paint's own duration does not count as idle time.
//
// This is the decision; the OS side (keeping the damage, the one-shot timer,
// the refresh rate) is win32/paint_pacer_win32.h. Tier-1 tested.

namespace neui_detail
{
  struct PaintPacer
  {
    static constexpr uint64_t k_min_wait = 4000000;  // ns

    uint64_t last_present = 0;     // ns, monotonic: when the last presented paint started
    bool     presented    = false;
    bool     forced       = true;  // the next paint may not wait (first paint, resize, DPI)

    // Whether a paint at `now` should wait for a refresh `interval` (ns);
    // `wait` gets how long.
    bool hold(uint64_t now, uint64_t interval, uint64_t& wait) const
    {
      wait = 0;
      if (forced || !presented || interval == 0) return false;
      const uint64_t due = interval - interval / 10;
      if (now < last_present) return false;  // a clock oddity: never stall
      const uint64_t elapsed = now - last_present;
      if (elapsed >= due) return false;
      if (due - elapsed < k_min_wait && elapsed >= interval / 2) return false;
      wait = due - elapsed;
      return true;
    }

    // A paint that started at `start` presented.
    void on_present(uint64_t start)
    {
      last_present = start;
      presented = true;
      forced = false;
    }

    void force() { forced = true; }
  };
}  // namespace neui_detail
