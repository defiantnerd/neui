#include "neui_test.h"

// The session animation clock (hosts/shared/anim_clock.h): frame time,
// marking, the fps-gated tick, dropping widgets that stopped painting, pause
// and reduced motion, and the timer interval it asks for.

#include "anim_clock.h"

#include <utility>
#include <vector>

using namespace neui_detail;

namespace {

uint64_t g_now = 0;
uint64_t fake_now() { return g_now; }

const uint64_t k_ms = 1000000ull;

using Calls = std::vector<std::pair<uint32_t, uint32_t>>;

Calls tick(AnimClock& c)
{
  Calls out;
  c.tick([&](uint32_t w, uint32_t o) { out.emplace_back(w, o); });
  return out;
}

// Paint `widget` the way a host does: reads the clock, then marks.
double paint(AnimClock& c, uint32_t widget, uint32_t owner)
{
  c.begin_widget();
  const double t = c.time();
  if (c.end_widget()) c.mark(widget, owner);
  return t;
}

}  // namespace

TEST_CASE("anim clock: time is running seconds; a paint that reads it is flagged")
{
  g_now = 1000 * k_ms;
  AnimClock c(&fake_now);
  g_now += 250 * k_ms;
  c.begin_widget();
  CHECK_FALSE(c.end_widget());
  c.begin_widget();
  CHECK_APPROX(c.time(), 0.25);
  CHECK(c.end_widget());
  CHECK_FALSE(c.end_widget());  // consumed
  CHECK(c.empty());
}

TEST_CASE("anim clock: the timer runs only for owners with marked widgets")
{
  g_now = 0;
  AnimClock c(&fake_now);
  CHECK_EQ(c.heartbeat_ms(1), 0u);
  paint(c, 10, 1);
  CHECK_EQ(c.heartbeat_ms(1), 16u);  // 30 fps: half a frame
  CHECK_EQ(c.heartbeat_ms(2), 0u);
  c.set_fps(10);
  CHECK_EQ(c.heartbeat_ms(1), 50u);
  c.set_fps(1000);
  CHECK_EQ(c.fps(), AnimClock::k_max_fps);
  CHECK_EQ(c.heartbeat_ms(1), 8u);
  c.set_fps(0);
  CHECK_EQ(c.fps(), 1u);
}

TEST_CASE("anim clock: frames are gated by the fps; widgets of a frame agree")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 10, 1);
  paint(c, 11, 1);
  g_now = 16 * k_ms;
  CHECK(tick(c).empty());  // 33.3 ms not reached (16 + 8 slack < 33.3)
  g_now = 32 * k_ms;
  Calls a = tick(c);  // within half a heartbeat of the due time
  CHECK_EQ(a.size(), 2u);
  // Both repaint in this frame and read one time.
  const double t1 = paint(c, 10, 1);
  g_now = 40 * k_ms;
  const double t2 = paint(c, 11, 1);
  CHECK_APPROX(t1, 0.032);
  CHECK(t1 == t2);
  g_now = 48 * k_ms;
  CHECK(tick(c).empty());
  g_now = 64 * k_ms;
  CHECK_EQ(tick(c).size(), 2u);
}

TEST_CASE("anim clock: a widget that misses two frames running is dropped")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 10, 1);
  paint(c, 20, 2);
  g_now = 40 * k_ms;
  CHECK_EQ(tick(c).size(), 2u);
  paint(c, 10, 1);  // 20 does not paint (hidden, or its paint is late)
  g_now = 80 * k_ms;
  CHECK_EQ(tick(c).size(), 2u);  // one miss: invalidated again
  paint(c, 10, 1);
  g_now = 120 * k_ms;
  Calls b = tick(c);  // the second miss
  REQUIRE(b.size() == 1u);
  CHECK_EQ(b[0].first, 10u);
  CHECK_EQ(c.size(), 1u);
  CHECK_EQ(c.heartbeat_ms(2), 0u);  // owner 2 stops its timer
  // A later paint brings it back.
  paint(c, 20, 2);
  CHECK(c.heartbeat_ms(2) > 0u);
}

TEST_CASE("anim clock: a stall does not cause a burst of frames")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 1, 0);
  g_now = 2000 * k_ms;  // two seconds without a timer event
  CHECK_EQ(tick(c).size(), 1u);
  paint(c, 1, 0);
  g_now += 16 * k_ms;
  CHECK(tick(c).empty());
  g_now += 17 * k_ms;
  CHECK_EQ(tick(c).size(), 1u);
}

TEST_CASE("anim clock: pause holds the time, stops the timer and keeps the widgets")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 1, 0);
  g_now = 1000 * k_ms;
  c.set_paused(true);
  CHECK_EQ(c.heartbeat_ms(0), 0u);
  g_now = 5000 * k_ms;
  CHECK(tick(c).empty());
  CHECK_EQ(c.size(), 1u);
  CHECK_APPROX(paint(c, 1, 0), 1.0);
  c.set_paused(false);
  g_now = 5500 * k_ms;
  CHECK_APPROX(paint(c, 1, 0), 1.0);  // the frame time until the next frame
  Calls r = tick(c);  // resuming makes the next timer event a frame
  CHECK_EQ(r.size(), 1u);
  CHECK_APPROX(paint(c, 1, 0), 1.5);  // no jump: the paused 4 s do not count
}

TEST_CASE("anim clock: reduced motion holds the time and polls slowly")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 1, 3);
  g_now = 100 * k_ms;
  c.set_reduced_motion(true);
  CHECK(c.reduced_motion());
  CHECK_FALSE(c.running());
  CHECK_EQ(c.heartbeat_ms(3), AnimClock::k_poll_ms);
  g_now = 900 * k_ms;
  CHECK(tick(c).empty());
  CHECK_EQ(c.size(), 1u);  // nothing dropped while held
  c.set_reduced_motion(false);
  CHECK_EQ(c.heartbeat_ms(3), 16u);
  CHECK_EQ(tick(c).size(), 1u);
  CHECK_APPROX(paint(c, 1, 3), 0.1);
}

TEST_CASE("anim clock: a late paint resets the misses")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 5, 0);
  for (int frame = 1; frame <= 6; ++frame) {
    g_now = uint64_t(frame) * 40 * k_ms;
    CHECK_EQ(tick(c).size(), 1u);
    if (frame % 2 == 0) paint(c, 5, 0);  // paints every other frame
  }
  CHECK_EQ(c.size(), 1u);
}

TEST_CASE("anim clock: a destroyed owner's widgets go, and the time follows the clock")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 1, 7);
  paint(c, 2, 8);
  g_now = 40 * k_ms;
  CHECK_EQ(tick(c).size(), 2u);
  c.forget_owner(7);
  CHECK_FALSE(c.has_owner(7));
  CHECK(c.has_owner(8));
  c.forget_owner(8);
  CHECK(c.empty());
  g_now = 900 * k_ms;  // nothing ticks any more
  CHECK_APPROX(paint(c, 3, 9), 0.9);  // not the 40 ms frame
}

TEST_CASE("anim clock: re-marking a widget moves it to its new owner")
{
  g_now = 0;
  AnimClock c(&fake_now);
  paint(c, 7, 1);
  paint(c, 7, 2);
  CHECK_EQ(c.size(), 1u);
  CHECK_FALSE(c.has_owner(1));
  CHECK(c.has_owner(2));
  uint32_t seen = 0;
  c.for_each([&](uint32_t w, uint32_t o) { seen = w * 100 + o; });
  CHECK_EQ(seen, 702u);
}
