#include "neui_test.h"

// Paint pacing (hosts/shared/paint_pacer.h): which paints are held back and
// for how long, for a target presenting without vsync.

#include "paint_pacer.h"

using namespace neui_detail;

namespace {
const uint64_t k_ms = 1000000ull;
const uint64_t k_60hz = 16666667ull;
}  // namespace

TEST_CASE("paint pacer: the first paint and forced paints never wait")
{
  PaintPacer p;
  uint64_t wait = 1;
  CHECK_FALSE(p.hold(5 * k_ms, k_60hz, wait));
  CHECK_EQ(wait, 0u);
  p.on_present(5 * k_ms);
  CHECK(p.hold(6 * k_ms, k_60hz, wait));
  p.force();  // a resize: the new size must show at once
  CHECK_FALSE(p.hold(6 * k_ms, k_60hz, wait));
  p.on_present(6 * k_ms);
  CHECK(p.hold(7 * k_ms, k_60hz, wait));  // forcing lasts one paint
}

TEST_CASE("paint pacer: paints within 90% of a refresh after a present wait")
{
  PaintPacer p;
  p.on_present(100 * k_ms);
  uint64_t wait = 0;
  CHECK(p.hold(101 * k_ms, k_60hz, wait));
  CHECK_EQ(wait, (k_60hz - k_60hz / 10) - 1 * k_ms);  // until 15 ms after the present
  CHECK(p.hold(110 * k_ms, k_60hz, wait));
  CHECK_EQ(wait, (k_60hz - k_60hz / 10) - 10 * k_ms);
  // Under 4 ms to wait, which a timer on a 15.6 ms grid would turn into a
  // lost frame: paint now.
  CHECK_FALSE(p.hold(112 * k_ms, k_60hz, wait));
  CHECK_FALSE(p.hold(140 * k_ms, k_60hz, wait));
  CHECK_EQ(wait, 0u);
}

TEST_CASE("paint pacer: a short wait is held within half a refresh")
{
  // 144 Hz: 90% is 6.25 ms, half 3.47 ms. At 3 ms the 3.25 ms wait is short,
  // but painting would make two presents within half a refresh.
  const uint64_t hz144 = 1000000000ull / 144;
  PaintPacer p;
  p.on_present(0);
  uint64_t wait = 0;
  CHECK(p.hold(3 * k_ms, hz144, wait));
  CHECK_EQ(wait, (hz144 - hz144 / 10) - 3 * k_ms);
  CHECK_FALSE(p.hold(4 * k_ms, hz144, wait));
}

TEST_CASE("paint pacer: times are taken at the start of a paint")
{
  // Animation ticks on a 15.6 ms timer grid with paints of 1.7 ms: stamped
  // at the start, every tick paints; stamped at the end, each would look
  // 1.7 ms early.
  PaintPacer p;
  uint64_t wait = 0;
  int paints = 0;
  for (uint64_t tick = 0; tick < 64; ++tick) {
    const uint64_t t = tick * 15625000ull;
    if (p.hold(t, k_60hz, wait)) continue;
    ++paints;
    p.on_present(t);  // the paint itself takes 1.7 ms after t
  }
  CHECK_EQ(paints, 64);
}

TEST_CASE("paint pacer: continuous input paints once per refresh")
{
  // A paint request every millisecond (a 1000 Hz mouse) for one second.
  PaintPacer p;
  int paints = 0;
  uint64_t wait = 0;
  for (uint64_t t = 0; t < 1000; ++t) {
    if (p.hold(t * k_ms, k_60hz, wait)) continue;
    ++paints;
    p.on_present(t * k_ms);
  }
  CHECK(paints >= 60);
  CHECK(paints <= 91);  // at most once per 11 ms (15 ms less the 4 ms not worth holding)
  // On a 144 Hz display the cap follows it.
  PaintPacer q;
  paints = 0;
  for (uint64_t t = 0; t < 1000; ++t) {
    if (q.hold(t * k_ms, 1000000000ull / 144, wait)) continue;
    ++paints;
    q.on_present(t * k_ms);
  }
  CHECK(paints >= 140);
  CHECK(paints <= 289);  // at most once per half refresh: timers cannot do better
}

TEST_CASE("paint pacer: no interval or a clock oddity never stalls")
{
  PaintPacer p;
  p.on_present(50 * k_ms);
  uint64_t wait = 7;
  CHECK_FALSE(p.hold(51 * k_ms, 0, wait));
  CHECK_FALSE(p.hold(40 * k_ms, k_60hz, wait));  // now before the last present
  CHECK_EQ(wait, 0u);
}
