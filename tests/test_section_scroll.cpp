#include "neui_test.h"

#include "widget_section_scroll.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Scroll state + layout for a section whose content extends past the body
// on both axes: 200x150 body, 600x800 content -> max scroll (400, 650).
static SectionScrollState make_state(SectionScrollAxis axis = SectionScrollAxis::Both)
{
  SectionScrollState st;
  st.axis      = axis;
  st.content_w = 600;
  st.content_h = 800;
  return st;
}

static SectionLayout make_layout()
{
  SectionLayout L;
  L.band_h = 0;
  L.body_x = 0;
  L.body_y = 0;
  L.body_w = 200;
  L.body_h = 150;
  return L;
}

static ScrollWheelInput precise_delta(double dpx)
{
  ScrollWheelInput in;
  in.precise       = true;
  in.phase_changed = true;   // finger-down move
  in.delta_px      = dpx;
  return in;
}

// ---------------------------------------------------------------------------
// section_scroll_max_px / section_scroll_commit
// ---------------------------------------------------------------------------

TEST_CASE("section_scroll_max_px: per-axis content minus body, floored at 0")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();
  CHECK_APPROX(section_scroll_max_px(st, L, false), 650.0);  // 800 - 150
  CHECK_APPROX(section_scroll_max_px(st, L, true),  400.0);  // 600 - 200

  st.content_w = 100;   // fits horizontally
  st.content_h = 100;   // fits vertically
  CHECK_APPROX(section_scroll_max_px(st, L, false), 0.0);
  CHECK_APPROX(section_scroll_max_px(st, L, true),  0.0);
}

TEST_CASE("section_scroll_commit: in-range raw passes through to the model")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  st.kin_v.raw_px = 137.0;
  section_scroll_commit(st, L, false);
  CHECK_EQ(st.scroll_y, 137);
  CHECK_EQ(st.kin_v.last_commit_px, 137);
  CHECK_FALSE(st.kinetic_over_v);

  st.kin_h.raw_px = 42.0;
  section_scroll_commit(st, L, true);
  CHECK_EQ(st.scroll_x, 42);
  CHECK_EQ(st.kin_h.last_commit_px, 42);
  CHECK_FALSE(st.kinetic_over_h);
}

TEST_CASE("section_scroll_commit: overscroll is rubber-damped + flagged")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Pull 100 raw px past the top: display offset is damped (< 100) and
  // negative, and the kinetic-overshoot flag claims the axis.
  st.kin_v.raw_px = -100.0;
  section_scroll_commit(st, L, false);
  CHECK(st.scroll_y < 0);
  CHECK(st.scroll_y > -100);
  CHECK(st.kinetic_over_v);
  CHECK_EQ(st.kin_v.last_commit_px, st.scroll_y);

  // Past the bottom edge symmetric.
  st.kin_v.raw_px = 650.0 + 100.0;
  section_scroll_commit(st, L, false);
  CHECK(st.scroll_y > 650);
  CHECK(st.scroll_y < 750);
  CHECK(st.kinetic_over_v);
}

// ---------------------------------------------------------------------------
// section_scroll_wheel_kinetic
// ---------------------------------------------------------------------------

TEST_CASE("section_scroll_wheel_kinetic: legacy wheel hard-clamps, no bounce")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Negative delta scrolls down (raw increases); positive scrolls up.
  ScrollWheelInput down;   // classic wheel: no precise, no phase, no momentum
  down.delta_px = -60.0;
  ScrollWheelAction a = section_scroll_wheel_kinetic(st, L, down, false);
  CHECK(a.changed);
  CHECK_FALSE(a.start_bounce);
  CHECK_EQ(st.scroll_y, 60);

  ScrollWheelInput up;
  up.delta_px = 10000.0;
  ScrollWheelAction b = section_scroll_wheel_kinetic(st, L, up, false);
  CHECK_EQ(st.scroll_y, 0);
  CHECK_FALSE(b.start_bounce);
  CHECK_FALSE(st.kinetic_over_v);

  ScrollWheelInput down2;
  down2.delta_px = -1e6;
  ScrollWheelAction c = section_scroll_wheel_kinetic(st, L, down2, false);
  CHECK_EQ(st.scroll_y, 650);
  CHECK_FALSE(c.start_bounce);
}

TEST_CASE("section_scroll_wheel_kinetic: precise in-range scroll, both axes independent")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  ScrollWheelAction a = section_scroll_wheel_kinetic(st, L, precise_delta(-50.0), false);
  CHECK(a.changed);
  CHECK_EQ(st.scroll_y, 50);
  CHECK_FALSE(a.start_bounce);

  ScrollWheelAction b = section_scroll_wheel_kinetic(st, L, precise_delta(-30.0), true);
  CHECK(b.changed);
  CHECK_EQ(st.scroll_x, 30);
  CHECK_EQ(st.scroll_y, 50);   // vertical untouched by the horizontal event
}

TEST_CASE("section_scroll_wheel_kinetic: finger-up overscroll starts the bounce")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Drag past the top edge with the finger down: stretches, no bounce yet.
  ScrollWheelAction a = section_scroll_wheel_kinetic(st, L, precise_delta(80.0), false);
  CHECK(a.changed);
  CHECK_FALSE(a.start_bounce);
  CHECK(st.scroll_y < 0);
  CHECK(st.kinetic_over_v);

  // Finger lifts while overscrolled: spring-back starts.
  ScrollWheelInput lift;
  lift.precise     = true;
  lift.phase_ended = true;
  lift.delta_px    = 0.0;
  ScrollWheelAction b = section_scroll_wheel_kinetic(st, L, lift, false);
  CHECK(b.start_bounce);
}

TEST_CASE("section_scroll_wheel_kinetic: external mutation resyncs the integrator")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  section_scroll_wheel_kinetic(st, L, precise_delta(-100.0), false);
  CHECK_EQ(st.scroll_y, 100);

  // Scrollbar drag / API moved the model behind the kinetics' back.
  st.scroll_y = 300;

  section_scroll_wheel_kinetic(st, L, precise_delta(-10.0), false);
  CHECK_EQ(st.scroll_y, 310);   // resynced to 300, then scrolled 10 further
}

// ---------------------------------------------------------------------------
// section_scroll_bounce_step
// ---------------------------------------------------------------------------

TEST_CASE("section_scroll_bounce_step: settles an overscroll back to the edge")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  st.kin_v.raw_px = -40.0;
  section_scroll_commit(st, L, false);
  CHECK(st.scroll_y < 0);
  CHECK(st.kinetic_over_v);

  int guard = 0;
  while (section_scroll_bounce_step(st, L, false) && guard < 1000) ++guard;
  CHECK(guard > 0);             // it actually animated
  CHECK(guard < 1000);          // and converged
  CHECK_EQ(st.scroll_y, 0);
  CHECK_FALSE(st.kinetic_over_v);
}

TEST_CASE("section_scroll_bounce_step: yields to external mutation + drops the overshoot claim")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  st.kin_v.raw_px = -40.0;
  section_scroll_commit(st, L, false);
  CHECK(st.kinetic_over_v);

  // Something else (scrollbar drag) moved the axis since the last commit.
  st.scroll_y = 120;
  bool animating = section_scroll_bounce_step(st, L, false);
  CHECK_FALSE(animating);
  CHECK_EQ(st.scroll_y, 120);          // not clobbered
  CHECK_FALSE(st.kinetic_over_v);      // paint clamp regains authority
}

TEST_CASE("section_scroll_bounce_step: in-range axis is a no-op")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  st.kin_v.raw_px = 100.0;
  section_scroll_commit(st, L, false);
  CHECK_FALSE(section_scroll_bounce_step(st, L, false));
  CHECK_EQ(st.scroll_y, 100);
}

// ---------------------------------------------------------------------------
// clamp_section_scroll_idle
// ---------------------------------------------------------------------------

TEST_CASE("clamp_section_scroll_idle: kinetic overshoot survives the paint clamp")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  st.kin_v.raw_px = -40.0;
  section_scroll_commit(st, L, false);
  int stretched = st.scroll_y;
  CHECK(stretched < 0);

  bool changed = clamp_section_scroll_idle(st, st.content_w, st.content_h,
                                            L.body_w, L.body_h);
  CHECK_FALSE(changed);
  CHECK_EQ(st.scroll_y, stretched);
}

TEST_CASE("clamp_section_scroll_idle: external out-of-range still clamps")
{
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Out of range but NOT kinetics-owned (flag unset): clamps like the
  // plain clamp - e.g. content regenerated smaller while idle.
  st.scroll_y = 5000;
  bool changed = clamp_section_scroll_idle(st, st.content_w, st.content_h,
                                            L.body_w, L.body_h);
  CHECK(changed);
  CHECK_EQ(st.scroll_y, 650);

  // Flag set but the committed value no longer matches the last kinetic
  // commit (external mutation raced in): clamps too.
  st.kin_v.raw_px = -40.0;
  section_scroll_commit(st, L, false);
  st.scroll_y = -300;   // not what the commit wrote
  changed = clamp_section_scroll_idle(st, st.content_w, st.content_h,
                                       L.body_w, L.body_h);
  CHECK(changed);
  CHECK_EQ(st.scroll_y, 0);
}
