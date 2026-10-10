#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

// The animation clock of a session: the time a shader's neui_time reads when
// nothing sets it, and the set of widgets that repaint as it advances.
//
// A widget joins by reading the clock while it paints. The host brackets each
// CUSTOMDRAW paint with begin_widget() / end_widget() and, when the paint read
// time(), calls mark() with the widget and its owner: the thing whose timer
// drives it (the frame on the xpl and win32 hosts, the session on macOS and
// iOS). The owner's timer runs at heartbeat_ms(owner) and calls tick(). When a
// frame is due (the fps cap), the frame time advances and every widget painted
// since the previous frame is invalidated. A widget that was invalidated but did
// not paint again (hidden, scrolled away, in a minimised window, or its layer
// now has a time of its own) is dropped. Dropping is always safe: the next
// paint that reads the clock marks the widget again and restarts the timer, so
// nothing has to track visibility.
//
// Paused by the client, the clock stands still, no timer runs and nothing is
// dropped; resuming is the host's cue to invalidate the marked widgets
// (for_each). Held by the OS's reduced-motion setting, it stands still too, but
// a slow timer keeps polling the setting so animation comes back on its own.
//
// The time is seconds of running time since the session began: a pause does
// not make the animation jump. Header-only and platform-free (the host passes
// the timer events and the OS setting in), so it is Tier-1 tested.

namespace neui_detail
{
  class AnimClock
  {
  public:
    using NowFn = uint64_t (*)();  // monotonic nanoseconds

    static constexpr uint32_t k_default_fps = 30;
    static constexpr uint32_t k_max_fps     = 120;
    static constexpr uint32_t k_poll_ms     = 500;  // timer while reduced motion holds the clock

    static uint64_t steady_ns()
    {
      using namespace std::chrono;
      return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    }

    explicit AnimClock(NowFn now = &steady_ns) : _now(now), _start(now()) {}

    // ---- Paint side ---------------------------------------------------------

    void begin_widget() { _read = false; }

    // Whether the paint since begin_widget() read the clock.
    bool end_widget()
    {
      const bool r = _read;
      _read = false;
      return r;
    }

    // Seconds of running time for the frame being painted. Between two frames
    // every read returns the same value, so the widgets of one frame agree and
    // a repaint between frames (a hover) shows the same picture; with no
    // widget animating, or the clock standing, it is the clock's reading.
    double time()
    {
      _read = true;
      if (_entries.empty() || !running()) _frame = seconds_at(_now());
      return _frame;
    }

    // The current frame's time, without flagging the paint.
    double frame() const { return _frame; }

    // `widget` read the clock while painting; its owner's timer drives it.
    void mark(uint32_t widget, uint32_t owner)
    {
      if (_entries.empty()) _next_due = _now() + interval_ns();
      for (auto& e : _entries) {
        if (e.widget != widget) continue;
        e.owner = owner;
        e.painted = true;
        return;
      }
      _entries.push_back({ widget, owner, true });
    }

    // ---- Timer side ---------------------------------------------------------

    // The interval the owner's timer should run at, 0 when it should not run:
    // half a frame (so a frame is never more than half a frame late), at least
    // 8 ms; k_poll_ms while reduced motion holds the clock.
    uint32_t heartbeat_ms(uint32_t owner) const
    {
      if (_paused || !has_owner(owner)) return 0;
      if (_reduced) return k_poll_ms;
      return running_heartbeat_ms();
    }

    // A timer event. When a frame is due, advances the frame time, drops the
    // widgets not painted since the previous frame, and calls
    // invalidate(widget, owner) for the others. Does nothing while the clock
    // stands.
    template <typename Invalidate>
    void tick(Invalidate&& invalidate)
    {
      if (!running() || _entries.empty()) return;
      const uint64_t now   = _now();
      const uint64_t iv    = interval_ns();
      const uint64_t slack = static_cast<uint64_t>(running_heartbeat_ms()) * 500000u;
      if (now + slack < _next_due) return;
      _next_due += iv;
      if (_next_due <= now) _next_due = now + iv;  // behind by a frame or more: no burst
      _frame = seconds_at(now);

      std::vector<Entry> due;
      due.reserve(_entries.size());
      size_t kept = 0;
      for (size_t i = 0; i < _entries.size(); ++i) {
        Entry e = _entries[i];
        if (!e.painted) continue;
        e.painted = false;
        _entries[kept++] = e;
        due.push_back(e);
      }
      _entries.resize(kept);
      // From a copy: an invalidation may paint (and mark) right away.
      for (const auto& e : due) invalidate(e.widget, e.owner);
    }

    // ---- Control ------------------------------------------------------------

    // Frames per second, clamped to [1, k_max_fps].
    void set_fps(uint32_t fps)
    {
      _fps = std::min(std::max(fps, 1u), k_max_fps);
    }
    uint32_t fps() const { return _fps; }

    void set_paused(bool paused) { set_state(paused, _reduced); }
    void set_reduced_motion(bool reduced) { set_state(_paused, reduced); }
    bool paused() const { return _paused; }
    bool reduced_motion() const { return _reduced; }
    bool running() const { return !_paused && !_reduced; }

    // The marked widgets, as (widget, owner).
    template <typename F>
    void for_each(F&& f) const
    {
      for (const auto& e : _entries) f(e.widget, e.owner);
    }
    bool   empty() const { return _entries.empty(); }
    size_t size() const { return _entries.size(); }
    bool   has_owner(uint32_t owner) const
    {
      for (const auto& e : _entries)
        if (e.owner == owner) return true;
      return false;
    }

  private:
    struct Entry
    {
      uint32_t widget  = 0;
      uint32_t owner   = 0;
      bool     painted = false;  // painted (and read the clock) since the last frame
    };

    uint64_t interval_ns() const { return 1000000000ull / _fps; }
    uint32_t running_heartbeat_ms() const { return std::max<uint32_t>(8u, 500u / _fps); }

    double seconds_at(uint64_t now) const
    {
      if (!running()) return _base;
      return _base + static_cast<double>(now - _start) * 1e-9;
    }

    void set_state(bool paused, bool reduced)
    {
      const uint64_t now = _now();
      const bool was = running();
      if (was) _base = seconds_at(now);
      _paused  = paused;
      _reduced = reduced;
      if (running()) {
        _start = now;
        if (!was) _next_due = now;  // the next timer event draws a frame
      }
    }

    NowFn              _now;
    uint64_t           _start    = 0;     // when running time last resumed
    double             _base     = 0.0;   // running seconds before _start
    double             _frame    = 0.0;   // the current frame's time
    uint64_t           _next_due = 0;
    uint32_t           _fps      = k_default_fps;
    bool               _paused   = false;
    bool               _reduced  = false;
    bool               _read     = false;
    std::vector<Entry> _entries;
  };
}  // namespace neui_detail
