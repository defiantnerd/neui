#pragma once

#include <cstdint>
#include <vector>

#include <neui/neui.h>

// Portable bookkeeping behind NEUI_API_TIMER (<neui/d/timer.h>). All the parts
// that are easy to get subtly wrong live here rather than in a platform layer:
// id allocation, deadline arithmetic, the native tick interval, late-tick
// coalescing, and mutation from inside a dispatch walk.
//
// The platform layer's only job is to call tick() periodically at
// native_interval_ms(); everything else is here and Tier-1 tested.
//
// Deliberately a flat vector: a client has a handful of timers, never
// thousands, so a linear scan beats a heap and keeps the ordering obvious.

namespace neui_detail
{

  struct TimerEntry
  {
    uint32_t id          = 0;
    uint32_t interval_ms = 0;
    uint64_t next_due_ms = 0;
    bool     removed     = false;   // tombstone; see remove()
  };

  class TimerTable
  {
  public:
    // Returns a non-zero id, or 0 for a zero interval. `interval_ms` is clamped
    // up to NEUI_TIMER_MIN_INTERVAL_MS.
    uint32_t add(uint32_t interval_ms, uint64_t now_ms)
    {
      if (interval_ms == 0) return 0;
      if (interval_ms < NEUI_TIMER_MIN_INTERVAL_MS)
        interval_ms = NEUI_TIMER_MIN_INTERVAL_MS;

      TimerEntry e;
      e.id          = _next_id++;
      e.interval_ms = interval_ms;
      e.next_due_ms = now_ms + interval_ms;
      // Never hand out 0 - it is the failure value in the public API.
      if (_next_id == 0) _next_id = 1;
      _entries.push_back(e);
      return e.id;
    }

    // Tombstones rather than erases: remove() is legal from inside a
    // NEUI_EVENT_TIMER handler (including for the timer being handled), and
    // erasing mid-walk would invalidate tick()'s iteration. Tombstones are
    // reaped at the top of the next tick.
    bool remove(uint32_t id)
    {
      for (auto& e : _entries) {
        if (e.id == id && !e.removed) { e.removed = true; return true; }
      }
      return false;
    }

    bool set_interval(uint32_t id, uint32_t interval_ms, uint64_t now_ms)
    {
      if (interval_ms == 0) return false;
      if (interval_ms < NEUI_TIMER_MIN_INTERVAL_MS)
        interval_ms = NEUI_TIMER_MIN_INTERVAL_MS;
      for (auto& e : _entries) {
        if (e.id != id || e.removed) continue;
        e.interval_ms = interval_ms;
        e.next_due_ms = now_ms + interval_ms;   // new period applies from now
        return true;
      }
      return false;
    }

    // The native tick period: the shortest live interval, or 0 when there are
    // no live timers (the platform then stops its tick entirely rather than
    // burning wakeups). Recomputed on demand - the set is tiny.
    uint32_t native_interval_ms() const
    {
      uint32_t best = 0;
      for (const auto& e : _entries) {
        if (e.removed) continue;
        if (best == 0 || e.interval_ms < best) best = e.interval_ms;
      }
      return best;
    }

    bool empty_live() const { return native_interval_ms() == 0; }

    // Collect the ids due at `now_ms` and reschedule them. The caller then
    // dispatches one NEUI_EVENT_TIMER per returned id.
    //
    // A late tick fires a timer ONCE and rebases its deadline on `now_ms`
    // rather than advancing by whole periods: catching up would let a slow
    // handler generate a backlog of its own events, which is never what
    // animation or queue-draining wants. `out` is passed in so the caller can
    // reuse one buffer per tick.
    void tick(uint64_t now_ms, std::vector<TimerEntry>& out)
    {
      out.clear();
      reap();
      for (auto& e : _entries) {
        if (e.removed) continue;
        if (e.next_due_ms > now_ms) continue;
        out.push_back(e);
        e.next_due_ms = now_ms + e.interval_ms;
      }
    }

    // True if `id` is live - used to skip an entry that a handler removed
    // earlier in the SAME tick, since `out` above is a snapshot.
    bool is_live(uint32_t id) const
    {
      for (const auto& e : _entries)
        if (e.id == id && !e.removed) return true;
      return false;
    }

    size_t live_count() const
    {
      size_t n = 0;
      for (const auto& e : _entries) if (!e.removed) ++n;
      return n;
    }

    void clear() { _entries.clear(); }

  private:
    void reap()
    {
      for (size_t i = _entries.size(); i-- > 0;)
        if (_entries[i].removed) _entries.erase(_entries.begin() + (long)i);
    }

    std::vector<TimerEntry> _entries;
    uint32_t                _next_id = 1;
  };

} // namespace neui_detail
