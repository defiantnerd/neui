// Tier-1 coverage for the portable half of NEUI_API_TIMER
// (hosts/shared/timer_table.h): id allocation, deadline arithmetic, the native
// tick interval, late-tick coalescing, and mutation from inside a dispatch walk.
//
// The platform layers only call tick() at native_interval_ms(); everything that
// can be subtly wrong is here, and `now_ms` is a parameter, so none of this
// needs a clock or a sleep.

#include "neui_test.h"

#include "timer_table.h"

using namespace neui_detail;

namespace {
  // How many timers were due at `now` (and reschedule them).
  size_t due_count(TimerTable& t, uint64_t now)
  {
    std::vector<TimerEntry> out;
    t.tick(now, out);
    return out.size();
  }

  // The single id due at `now`, or 0 for none / more than one. The test macros
  // stream scalars, not containers, so the assertions below stay readable.
  uint32_t due_one(TimerTable& t, uint64_t now)
  {
    std::vector<TimerEntry> out;
    t.tick(now, out);
    return (out.size() == 1) ? out[0].id : 0u;
  }
}

// ---------------------------------------------------------------------------
// Ids + validation
// ---------------------------------------------------------------------------

TEST_CASE("TimerTable: add returns distinct non-zero ids")
{
  TimerTable t;
  uint32_t a = t.add(16, 0);
  uint32_t b = t.add(16, 0);
  CHECK(a != 0);
  CHECK(b != 0);
  CHECK(a != b);
  CHECK_EQ(t.live_count(), (size_t)2);
}

TEST_CASE("TimerTable: a zero interval is rejected rather than spinning")
{
  TimerTable t;
  CHECK_EQ(t.add(0, 0), 0u);
  CHECK_EQ(t.live_count(), (size_t)0);
}

TEST_CASE("TimerTable: sub-minimum intervals clamp up")
{
  TimerTable t;
  uint32_t id = t.add(1, 0);
  CHECK(id != 0);
  CHECK_EQ(t.native_interval_ms(), (uint32_t)NEUI_TIMER_MIN_INTERVAL_MS);
  // The clamped value is what the event reports, not the requested 1.
  std::vector<TimerEntry> out;
  t.tick(NEUI_TIMER_MIN_INTERVAL_MS, out);
  REQUIRE_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].interval_ms, (uint32_t)NEUI_TIMER_MIN_INTERVAL_MS);
}

// ---------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------

TEST_CASE("TimerTable: nothing is due before the first deadline")
{
  TimerTable t;
  t.add(100, 1000);
  CHECK_EQ(due_count(t, 1000), (size_t)0);
  CHECK_EQ(due_count(t, 1099), (size_t)0);
  CHECK_EQ(due_count(t, 1100), (size_t)1);   // inclusive at the deadline
}

TEST_CASE("TimerTable: a timer reschedules by its interval")
{
  TimerTable t;
  uint32_t id = t.add(100, 0);
  CHECK_EQ(due_one(t, 100), id);
  CHECK_EQ(due_count(t, 150), (size_t)0);
  CHECK_EQ(due_one(t, 200), id);
}

TEST_CASE("TimerTable: only the due timers fire on a mixed tick")
{
  TimerTable t;
  uint32_t fast = t.add(10, 0);
  uint32_t slow = t.add(100, 0);
  CHECK(fast != slow);
  CHECK_EQ(due_one(t, 10), fast);
  CHECK_EQ(due_one(t, 20), fast);
  // At 100 both are due (fast was last rescheduled to 30).
  CHECK_EQ(due_count(t, 100), (size_t)2);
}

TEST_CASE("TimerTable: a LATE tick fires once, it does not catch up")
{
  // The whole point of the coalescing rule: a handler that blocks for a second
  // must not then receive a hundred queued 10 ms events.
  TimerTable t;
  uint32_t id = t.add(10, 0);
  CHECK_EQ(due_one(t, 1000), id);        // 100 periods late -> fires ONCE
  // And it rebases on `now`, so the next fire is one interval after the LATE
  // tick rather than immediately.
  CHECK_EQ(due_count(t, 1005), (size_t)0);
  CHECK_EQ(due_one(t, 1010), id);
}

// ---------------------------------------------------------------------------
// Native tick interval
// ---------------------------------------------------------------------------

TEST_CASE("TimerTable: native interval is the shortest live one, 0 when idle")
{
  TimerTable t;
  CHECK_EQ(t.native_interval_ms(), 0u);   // idle -> platform stops its tick
  CHECK(t.empty_live());

  uint32_t slow = t.add(500, 0);
  CHECK(slow != 0);
  CHECK_EQ(t.native_interval_ms(), 500u);
  uint32_t fast = t.add(16, 0);
  CHECK_EQ(t.native_interval_ms(), 16u);

  t.remove(fast);
  CHECK_EQ(t.native_interval_ms(), 500u);   // back up to the slow one
  t.remove(slow);
  CHECK_EQ(t.native_interval_ms(), 0u);
  CHECK(t.empty_live());
}

TEST_CASE("TimerTable: set_interval retimes without changing the id")
{
  TimerTable t;
  uint32_t id = t.add(100, 0);
  CHECK(t.set_interval(id, 10, 1000));
  CHECK_EQ(t.native_interval_ms(), 10u);
  // The new period runs from the set_interval call, not from the old deadline.
  CHECK_EQ(due_count(t, 1005), (size_t)0);
  CHECK_EQ(due_one(t, 1010), id);
}

TEST_CASE("TimerTable: set_interval rejects unknown ids and zero")
{
  TimerTable t;
  uint32_t id = t.add(100, 0);
  CHECK_FALSE(t.set_interval(id + 999, 10, 0));
  CHECK_FALSE(t.set_interval(id, 0, 0));
  CHECK_EQ(t.native_interval_ms(), 100u);   // unchanged
}

// ---------------------------------------------------------------------------
// Removal, including from inside a dispatch walk
// ---------------------------------------------------------------------------

TEST_CASE("TimerTable: remove stops the timer and is idempotent")
{
  TimerTable t;
  uint32_t id = t.add(10, 0);
  CHECK(t.remove(id));
  CHECK_FALSE(t.remove(id));          // already gone
  CHECK_FALSE(t.remove(id + 999));    // never existed
  CHECK_EQ(due_count(t, 100), (size_t)0);
}

TEST_CASE("TimerTable: removing DURING a tick keeps the walk valid")
{
  // The caller iterates the snapshot tick() returned; a handler removing a
  // timer must not invalidate that (hence tombstones, not erase).
  TimerTable t;
  uint32_t a = t.add(10, 0);
  uint32_t b = t.add(10, 0);
  std::vector<TimerEntry> out;
  t.tick(10, out);
  CHECK_EQ(out.size(), (size_t)2);

  // Simulate a's handler removing b, then the caller reaching b in the snapshot.
  CHECK(t.remove(b));
  CHECK(t.is_live(a));
  CHECK_FALSE(t.is_live(b));   // caller skips it -> b never fires this tick
}

TEST_CASE("TimerTable: a timer can remove ITSELF from its own handler")
{
  TimerTable t;
  uint32_t id = t.add(10, 0);
  std::vector<TimerEntry> out;
  t.tick(10, out);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK(t.remove(id));             // the handler removes itself
  CHECK_EQ(t.live_count(), (size_t)0);
  CHECK_EQ(due_count(t, 100), (size_t)0);  // and never fires again
}

TEST_CASE("TimerTable: adding DURING a tick does not fire in the same tick")
{
  TimerTable t;
  uint32_t a = t.add(10, 0);
  std::vector<TimerEntry> out;
  t.tick(10, out);
  CHECK_EQ(out.size(), (size_t)1);
  uint32_t b = t.add(10, 10);       // added by a's handler at now=10
  CHECK(b != a);
  CHECK(t.is_live(b));
  // The snapshot already taken (`out`) holds only `a`, so b cannot fire in the
  // tick that created it...
  REQUIRE_EQ(out.size(), (size_t)1);
  CHECK_EQ(out[0].id, a);
  // ...and b waits a full interval before its first fire.
  CHECK_EQ(due_count(t, 15), (size_t)0);
  // At 20 BOTH are due: b reaches its first deadline and a its second. Two is
  // the correct answer here, not one.
  CHECK_EQ(due_count(t, 20), (size_t)2);
}

TEST_CASE("TimerTable: tombstones are reaped, so ids do not accumulate")
{
  TimerTable t;
  for (int i = 0; i < 50; ++i) {
    uint32_t id = t.add(10, 0);
    t.remove(id);
  }
  CHECK_EQ(t.live_count(), (size_t)0);
  std::vector<TimerEntry> out;
  t.tick(100, out);                 // reaps
  CHECK(out.empty());
  CHECK_EQ(t.native_interval_ms(), 0u);
}

TEST_CASE("TimerTable: clear drops everything")
{
  TimerTable t;
  t.add(10, 0);
  t.add(20, 0);
  t.clear();
  CHECK_EQ(t.live_count(), (size_t)0);
  CHECK_EQ(t.native_interval_ms(), 0u);
}

TEST_CASE("TimerTable: a removed timer is not reported live mid-snapshot")
{
  TimerTable t;
  uint32_t a = t.add(10, 0);
  CHECK(t.is_live(a));
  t.remove(a);
  CHECK_FALSE(t.is_live(a));
}

// ---------------------------------------------------------------------------
// tick_and_dispatch - the REAL walk. These previously simulated the caller's
// loop by hand, which is precisely why a re-entrancy use-after-free in it went
// unnoticed: the suite passed with the bug shipped. Now the walk under test IS
// the walk that runs.
// ---------------------------------------------------------------------------

TEST_CASE("tick_and_dispatch: dispatches each due timer once with its interval")
{
  TimerTable t;
  uint32_t a = t.add(10, 0);
  uint32_t b = t.add(20, 0);
  std::vector<uint32_t> fired;
  std::vector<uint32_t> intervals;
  t.tick_and_dispatch(20, [&](uint32_t id, uint32_t iv) {
    fired.push_back(id); intervals.push_back(iv);
  });
  REQUIRE_EQ(fired.size(), (size_t)2);
  CHECK_EQ(intervals[0], 10u);
  CHECK_EQ(intervals[1], 20u);
  CHECK(fired[0] == a || fired[1] == a);
  CHECK(fired[0] == b || fired[1] == b);
}

TEST_CASE("tick_and_dispatch: a handler removing ANOTHER due timer stops it firing")
{
  TimerTable t;
  uint32_t a = t.add(10, 0);
  uint32_t b = t.add(10, 0);
  std::vector<uint32_t> fired;
  t.tick_and_dispatch(10, [&](uint32_t id, uint32_t) {
    fired.push_back(id);
    if (id == a) t.remove(b);     // b is later in the same snapshot
  });
  REQUIRE_EQ(fired.size(), (size_t)1);
  CHECK_EQ(fired[0], a);
  CHECK_FALSE(t.is_live(b));
}

TEST_CASE("tick_and_dispatch: a handler can remove itself")
{
  TimerTable t;
  uint32_t id = t.add(10, 0);
  int fires = 0;
  t.tick_and_dispatch(10, [&](uint32_t got, uint32_t) {
    ++fires; CHECK_EQ(got, id); t.remove(id);
  });
  CHECK_EQ(fires, 1);
  t.tick_and_dispatch(1000, [&](uint32_t, uint32_t) { ++fires; });
  CHECK_EQ(fires, 1);            // never again
}

TEST_CASE("tick_and_dispatch: a timer ADDED by a handler waits a full interval")
{
  TimerTable t;
  t.add(10, 0);
  uint32_t added = 0;
  std::vector<uint32_t> fired;
  t.tick_and_dispatch(10, [&](uint32_t id, uint32_t) {
    fired.push_back(id);
    if (!added) added = t.add(10, 10);
  });
  CHECK_EQ(fired.size(), (size_t)1);          // the new timer did NOT fire here
  CHECK(added != 0);
  fired.clear();
  t.tick_and_dispatch(15, [&](uint32_t id, uint32_t) { fired.push_back(id); });
  CHECK_EQ(fired.size(), (size_t)0);          // still waiting at 15
  t.tick_and_dispatch(20, [&](uint32_t id, uint32_t) { fired.push_back(id); });
  CHECK_EQ(fired.size(), (size_t)2);          // both due at 20
}

TEST_CASE("tick_and_dispatch: RE-ENTRANT ticks are suppressed, not nested")
{
  // The bug this pins: every nested pump in the tree (modal dialog, popup menu,
  // client pump_once) services timers, so a handler that opens one re-enters
  // the walk. Nesting re-delivered the same deadlines AND reallocated the
  // container the outer frame was iterating - a use-after-free.
  TimerTable t;
  t.add(10, 0);
  t.add(10, 0);
  int outer = 0, nested = 0;
  t.tick_and_dispatch(10, [&](uint32_t, uint32_t) {
    ++outer;
    CHECK(t.is_ticking());          // we are inside the walk
    // Simulate the handler opening a modal dialog whose pump ticks timers,
    // AND adding timers while nested (which used to realloc the due vector).
    for (int i = 0; i < 8; ++i) t.add(10, 10);
    t.tick_and_dispatch(10, [&](uint32_t, uint32_t) { ++nested; });
  });
  CHECK_EQ(outer, 2);               // both original timers still fired
  CHECK_EQ(nested, 0);              // the nested tick delivered nothing
  CHECK_FALSE(t.is_ticking());      // and the guard cleared afterwards
}

TEST_CASE("tick_and_dispatch: the guard clears even across many ticks")
{
  TimerTable t;
  uint32_t id = t.add(10, 0);
  CHECK(id != 0);
  for (int i = 1; i <= 5; ++i) {
    int fires = 0;
    t.tick_and_dispatch((uint64_t)i * 10, [&](uint32_t, uint32_t) { ++fires; });
    CHECK_EQ(fires, 1);
    CHECK_FALSE(t.is_ticking());
  }
}

TEST_CASE("tick_and_dispatch: a late tick dispatches once, not per missed period")
{
  TimerTable t;
  t.add(10, 0);
  int fires = 0;
  t.tick_and_dispatch(1000, [&](uint32_t, uint32_t) { ++fires; });
  CHECK_EQ(fires, 1);
}

TEST_CASE("tick_and_dispatch: no live timers dispatches nothing")
{
  TimerTable t;
  int fires = 0;
  t.tick_and_dispatch(1000, [&](uint32_t, uint32_t) { ++fires; });
  CHECK_EQ(fires, 0);
}
