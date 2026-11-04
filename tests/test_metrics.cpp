#include "neui_test.h"

#include "metrics.h"

// The NEUI_API_METRICS seams. Two hosts for one platform can be linked into the
// same binary (both iOS hosts are), so the seams are registries: each host adds
// its implementation, and safe_area asks each until one claims the frame.
// Assigning a single slot instead made the last-registered host win and report
// zeros for every frame the other one owned.

using namespace neui_detail;

namespace {
  // Two fake hosts, each owning one frame id.
  bool fake_host_a(neui_session_t, neui_widget_t frame, int* l, int* t, int* r, int* b)
  {
    if (frame.id != 0xA) return false;
    if (l) *l = 1;
    if (t) *t = 2;
    if (r) *r = 3;
    if (b) *b = 4;
    return true;
  }
  bool fake_host_b(neui_session_t, neui_widget_t frame, int* l, int* t, int* r, int* b)
  {
    if (frame.id != 0xB) return false;
    if (l) *l = 5;
    if (t) *t = 6;
    if (r) *r = 7;
    if (b) *b = 8;
    return true;
  }
}

TEST_CASE("metrics: safe_area asks every host, not just the last registered")
{
  metrics_add_safe_area_seam(&fake_host_a);
  metrics_add_safe_area_seam(&fake_host_b);

  int l = -1, t = -1, r = -1, b = -1;

  // The FIRST-registered host still answers for its own frame. This is the
  // regression: with an assigned slot, host B would have been asked instead and
  // reported zeros.
  metrics_safe_area(neui_session_t{ 0 }, neui_widget_t{ 0xA }, &l, &t, &r, &b);
  CHECK_EQ(l, 1); CHECK_EQ(t, 2); CHECK_EQ(r, 3); CHECK_EQ(b, 4);

  metrics_safe_area(neui_session_t{ 0 }, neui_widget_t{ 0xB }, &l, &t, &r, &b);
  CHECK_EQ(l, 5); CHECK_EQ(t, 6); CHECK_EQ(r, 7); CHECK_EQ(b, 8);

  // A frame no host claims falls back to the desktop default: zero insets.
  metrics_safe_area(neui_session_t{ 0 }, neui_widget_t{ 0xC }, &l, &t, &r, &b);
  CHECK_EQ(l, 0); CHECK_EQ(t, 0); CHECK_EQ(r, 0); CHECK_EQ(b, 0);

  // Out pointers may be NULL - the public header says so.
  metrics_safe_area(neui_session_t{ 0 }, neui_widget_t{ 0xA }, nullptr, &t, nullptr, nullptr);
  CHECK_EQ(t, 2);
}

TEST_CASE("metrics: adding the same seam twice is a no-op")
{
  // register_host() is documented idempotent, so a repeat must not grow the
  // list (and must not make a claim-check run twice).
  const size_t before = metrics_safe_area_seams().size();
  metrics_add_safe_area_seam(&fake_host_a);
  metrics_add_safe_area_seam(&fake_host_a);
  CHECK_EQ((int)metrics_safe_area_seams().size(), (int)before);

  metrics_add_safe_area_seam(nullptr);   // and null is ignored, not stored
  CHECK_EQ((int)metrics_safe_area_seams().size(), (int)before);
}

TEST_CASE("metrics: the desktop defaults are the framework's own sizes at scale 1")
{
  // painted_ui_scale() is 1.0 on every desktop host, so these are exact.
  CHECK_APPROX(metrics_ui_scale(neui_session_t{ 0 }), 1.0);
  CHECK_EQ(metrics_metric(neui_session_t{ 0 }, NEUI_METRIC_CONTROL_HEIGHT),
           METRIC_BASE_CONTROL_HEIGHT);
  CHECK_EQ(metrics_metric(neui_session_t{ 0 }, NEUI_METRIC_MARGIN), METRIC_BASE_MARGIN);
  CHECK_EQ(metrics_metric(neui_session_t{ 0 }, NEUI_METRIC_SPACING), METRIC_BASE_SPACING);
  CHECK_EQ(metrics_metric(neui_session_t{ 0 }, NEUI_METRIC_BODY_FONT_SIZE), 12);
  CHECK_EQ(metrics_metric(neui_session_t{ 0 }, (neui_metric_t)9999), 0);  // unknown => 0
}

TEST_CASE("metrics: the default text measure counts codepoints, not bytes")
{
  const neui_session_t s = { 0 };
  CHECK_EQ(metrics_measure_text_default(s, nullptr, nullptr, 10.0f, 0), 0);
  CHECK_EQ(metrics_measure_text_default(s, "", nullptr, 10.0f, 0), 0);
  // 4 glyphs * 10px * 0.5 = 20.
  CHECK_EQ(metrics_measure_text_default(s, "abcd", nullptr, 10.0f, 0), 20);
  // "ä" is two bytes but one glyph, so it must not measure as two.
  CHECK_EQ(metrics_measure_text_default(s, "\xc3\xa4", nullptr, 10.0f, 0), 5);
}
