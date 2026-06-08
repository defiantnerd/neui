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

TEST_CASE("section_scroll_commit: overscroll passes through + flagged")
{
  // raw_px is the damped display position (rubber damping is applied at
  // input time inside scroll_wheel), so commit is a pure round + flag.
  // The "is the value past the edge?" check still fires kinetic_over_*.
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  st.kin_v.raw_px = -20.0;
  section_scroll_commit(st, L, false);
  CHECK_EQ(st.scroll_y, -20);
  CHECK(st.kinetic_over_v);
  CHECK_EQ(st.kin_v.last_commit_px, -20);

  st.kin_v.raw_px = 650.0 + 30.0;
  section_scroll_commit(st, L, false);
  CHECK_EQ(st.scroll_y, 680);
  CHECK(st.kinetic_over_v);
}

TEST_CASE("section_scroll_wheel_kinetic: reverse-direction input releases overshoot linearly")
{
  // Regression: previously the inverse-rubber map mapped a deep overshoot
  // (raw_px close to the asymptotic limit) to a very large implicit
  // cumulative-input integral. A subsequent small reverse-direction wheel
  // notch was lost in that integral - the user saw the displayed position
  // barely budge and felt like they were wrestling against the spring.
  // With the release-direction shortcut, an input that points back toward
  // the in-range zone subtracts linearly from the displayed position so
  // 1 px of reverse input = 1 px of release. Sustained over-flicks
  // (inputs that EXTEND the overshoot) keep the asymptotic damping.
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Drive into a deep bottom overshoot (raw_px near the asymptotic limit).
  for (int i = 0; i < 50; ++i)
    section_scroll_wheel_kinetic(st, L, precise_delta(-1000.0), false);

  CHECK(st.kinetic_over_v);
  double over_before = st.kin_v.raw_px - 650.0;
  CHECK(over_before > 30.0);   // deep stretch

  // A modest reverse notch (10 px back toward the edge): with the linear
  // release this should reduce the displayed overshoot by exactly 10 px.
  // The pre-fix code would barely move raw_px at all.
  section_scroll_wheel_kinetic(st, L, precise_delta(10.0), false);
  double over_after = st.kin_v.raw_px - 650.0;
  CHECK_APPROX(over_before - over_after, 10.0);

  // A reverse notch large enough to release the whole stretch + push back
  // into the in-range zone lands linearly inside the in-range portion.
  section_scroll_wheel_kinetic(st, L, precise_delta(over_after + 5.0), false);
  CHECK_FALSE(st.kinetic_over_v);
  CHECK_APPROX(st.kin_v.raw_px, 645.0);
}

TEST_CASE("section_scroll_wheel_kinetic: reverse-direction release also works on top edge")
{
  // Same shape on the opposite axis. Drive into a top overshoot (raw_px
  // negative), then a small reverse notch (delta negative = pull down)
  // releases the stretch linearly.
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();
  st.scroll_y = 0;

  for (int i = 0; i < 50; ++i)
    section_scroll_wheel_kinetic(st, L, precise_delta(1000.0), false);

  CHECK(st.kinetic_over_v);
  CHECK(st.kin_v.raw_px < -30.0);
  double over_before = -st.kin_v.raw_px;

  section_scroll_wheel_kinetic(st, L, precise_delta(-10.0), false);
  double over_after = -st.kin_v.raw_px;
  CHECK_APPROX(over_before - over_after, 10.0);
}

TEST_CASE("section_scroll_wheel_kinetic: rubber damping at input keeps raw bounded")
{
  // Regression: previously raw_px integrated input unbounded - heavy
  // wheeling past the edge piled raw_px arbitrarily far past max_px while
  // the visible position saturated. The spring-back then had to crawl all
  // that integrated input back at 29% per tick, making the rubber-band
  // visibly linger long after the visible position had hit its asymptote.
  // With damping-at-input, raw_px IS the damped display position and
  // asymptotes to (max_px + limit) regardless of how much input arrives -
  // so the bounce settle-time is constant in `limit`, not log of input.
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Hammer the bottom edge with absurd cumulative input. body_h = 150 ->
  // limit = min(75, SCROLL_OVERSCROLL_MAX = 60) = 60.
  for (int i = 0; i < 100; ++i)
    section_scroll_wheel_kinetic(st, L, precise_delta(-1000.0), false);

  CHECK(st.kinetic_over_v);
  CHECK(st.scroll_y > 650);
  CHECK(st.kin_v.raw_px - 650.0 < 60.0);   // raw_px did NOT pile up
  CHECK(st.scroll_y - 650 <= 60);           // visible stays at-or-under limit
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

TEST_CASE("section_scroll_wheel_kinetic: finger-throw past edge swallows the follow-up momentum stream")
{
  // Regression: macOS emits phase_ended (not momentum) when a finger lifts
  // after a throw past the edge, then follows with momentumPhase=Began/
  // Changed/Ended events in the same direction. Without arming suppression
  // at phase_ended-overscroll, the first momentum event slips through and
  // pushes raw_px further past the edge for one frame against the
  // already-running spring-back. Visible as a "kick back" before the
  // bounce settles.
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Throw past the top edge with the finger down.
  section_scroll_wheel_kinetic(st, L, precise_delta(80.0), false);
  CHECK(st.kin_v.raw_px < 0.0);
  double raw_at_lift = st.kin_v.raw_px;

  // Finger lifts while overscrolled.
  ScrollWheelInput lift;
  lift.precise     = true;
  lift.phase_ended = true;
  lift.delta_px    = 0.0;
  ScrollWheelAction b = section_scroll_wheel_kinetic(st, L, lift, false);
  CHECK(b.start_bounce);
  CHECK(st.kin_v.suppress_momentum);   // <-- the fix: armed at phase_ended

  // The follow-up momentum stream in the same direction must NOT push
  // raw_px further past the edge - it would fight the spring-back.
  ScrollWheelInput mom;
  mom.precise  = true;
  mom.momentum = true;
  mom.delta_px = 40.0;                  // same direction as the throw
  ScrollWheelAction c = section_scroll_wheel_kinetic(st, L, mom, false);
  CHECK_FALSE(c.changed);
  CHECK_APPROX(st.kin_v.raw_px, raw_at_lift);

  // momentum_ended still clears suppression so the next gesture is free.
  ScrollWheelInput end;
  end.precise        = true;
  end.momentum       = true;
  end.momentum_ended = true;
  end.delta_px       = 0.0;
  section_scroll_wheel_kinetic(st, L, end, false);
  CHECK_FALSE(st.kin_v.suppress_momentum);
}

TEST_CASE("section_scroll_wheel_kinetic: cross-axis wheel on a single-axis section is dropped")
{
  // Regression: previously a horizontal trackpad scroll on a vertical-only
  // section was silently re-aimed at the vertical axis (the per-host
  // handlers had a "horizontal -> vertical-only" fallback alongside the
  // legitimate "vertical -> horizontal-only" classic-wheel fallback). The
  // shared kinetics layer now refuses the call outright when the section's
  // axis mask doesn't include the requested axis, so neither host nor any
  // future caller can bleed input across axes.
  {
    SectionScrollState st = make_state(SectionScrollAxis::Vertical);
    SectionLayout L = make_layout();

    ScrollWheelAction a = section_scroll_wheel_kinetic(st, L, precise_delta(-50.0), true);
    CHECK_FALSE(a.changed);          // no commit
    CHECK_EQ(st.scroll_x, 0);        // horizontal axis untouched
    CHECK_EQ(st.scroll_y, 0);        // vertical axis untouched (the call targeted h)
    CHECK_APPROX(st.kin_h.raw_px, 0.0);
    CHECK_APPROX(st.kin_v.raw_px, 0.0);
  }
  {
    // Symmetric: vertical wheel call on a horizontal-only section is also
    // refused at the kinetics layer. (The per-host handlers still re-route
    // pure-vertical input to the horizontal axis as a classic-wheel-mouse
    // fallback - that re-routing happens BEFORE this entry point, so the
    // call here arrives as axis_h=true and is accepted.)
    SectionScrollState st = make_state(SectionScrollAxis::Horizontal);
    SectionLayout L = make_layout();

    ScrollWheelAction a = section_scroll_wheel_kinetic(st, L, precise_delta(-50.0), false);
    CHECK_FALSE(a.changed);
    CHECK_EQ(st.scroll_x, 0);
    CHECK_EQ(st.scroll_y, 0);
  }
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

TEST_CASE("section_scroll_bounce_step: debounce holds the lerp until the wheel stream goes quiet")
{
  // Regression: previously the bounce lerped on every tick from the moment
  // start_bounce fired - so when wheel events kept arriving (Win32 inertia
  // notches, a touchpad coast-down, or just a user still wheeling past the
  // edge), each event re-extended the rubber and each bounce tick pulled
  // it back, visibly jittering. The quiet-tick gate holds the lerp until
  // the wheel stream has been silent for SCROLL_BOUNCE_DEBOUNCE_TICKS.
  SectionScrollState st = make_state();
  SectionLayout L = make_layout();

  // Wheel past the top edge (positive delta = scroll up = raw goes negative).
  section_scroll_wheel_kinetic(st, L, precise_delta(200.0), false);
  CHECK(st.kinetic_over_v);
  CHECK(st.scroll_y < 0);
  int stretched = st.scroll_y;

  // First SCROLL_BOUNCE_DEBOUNCE_TICKS bounce ticks must NOT move the
  // position - they're the quiet-time window.
  for (int i = 0; i < SCROLL_BOUNCE_DEBOUNCE_TICKS; ++i) {
    bool more = section_scroll_bounce_step(st, L, false);
    CHECK(more);
    CHECK_EQ(st.scroll_y, stretched);   // pinned in place
  }
  // The very next tick lerps.
  CHECK(section_scroll_bounce_step(st, L, false));
  CHECK(st.scroll_y > stretched);   // moved toward the top edge (0)

  // A wheel event arriving mid-bounce re-arms the debounce.
  section_scroll_wheel_kinetic(st, L, precise_delta(10.0), false);
  int re_extended = st.scroll_y;
  for (int i = 0; i < SCROLL_BOUNCE_DEBOUNCE_TICKS; ++i) {
    section_scroll_bounce_step(st, L, false);
    CHECK_EQ(st.scroll_y, re_extended);
  }
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
