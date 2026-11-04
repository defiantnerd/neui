#include "neui_test.h"

#include "attrs.h"

#include <cstring>
#include <memory>

using namespace neui_detail;

// NOTE: these tests deliberately use custom (non-well-known) keys. AttrBag
// debug-asserts on a kind mismatch for well-known keys (k_well_known_attrs),
// so type-strictness must be probed with keys outside that table.

TEST_CASE("AttrBag: round-trips each kind")
{
  AttrBag bag;
  bag.set_int("x.i", 5);
  bag.set_float("x.f", 1.5f);
  bag.set_string("x.s", "hello");

  CHECK_EQ(bag.get_int("x.i", -1), 5);
  CHECK_APPROX(bag.get_float("x.f", -1.0f), 1.5);
  CHECK_EQ(std::string(bag.get_string("x.s")), std::string("hello"));
}

TEST_CASE("AttrBag: wrong-kind reads return the supplied default")
{
  AttrBag bag;
  bag.set_int("x.i", 5);
  CHECK_APPROX(bag.get_float("x.i", -1.0f), -1.0);   // int read as float -> default
  CHECK(bag.get_string("x.i") == nullptr);           // int read as string -> null

  bag.set_string("x.s", "txt");
  CHECK_EQ(bag.get_int("x.s", -1), -1);
  CHECK_APPROX(bag.get_float("x.s", -2.0f), -2.0);
}

TEST_CASE("AttrBag: has / remove")
{
  AttrBag bag;
  CHECK_FALSE(bag.has("k"));
  bag.set_int("k", 1);
  CHECK(bag.has("k"));
  CHECK(bag.remove("k"));
  CHECK_FALSE(bag.has("k"));
  CHECK_FALSE(bag.remove("k"));   // removing absent key returns false
}

TEST_CASE("attr_as_float: promotes int, reads float, zero, and missing")
{
  AttrBag bag;
  bag.set_int("i", 7);
  bag.set_float("f", 2.5f);
  bag.set_int("z", 0);

  CHECK_APPROX(attr_as_float(&bag, "i", -1.0f), 7.0);   // int promoted
  CHECK_APPROX(attr_as_float(&bag, "f", -1.0f), 2.5);   // float read
  CHECK_APPROX(attr_as_float(&bag, "z", -1.0f), 0.0);   // present zero -> 0, not default
  CHECK_APPROX(attr_as_float(&bag, "absent", 99.0f), 99.0);   // missing -> default
  CHECK_APPROX(attr_as_float(nullptr, "i", 42.0f), 42.0);     // null bag -> default
}

TEST_CASE("ensure_attrs / attrs_readonly: lazy allocation")
{
  std::unique_ptr<AttrBag> slot;
  CHECK(attrs_readonly(slot) == nullptr);   // nothing allocated yet

  AttrBag& b = ensure_attrs(slot);
  b.set_int("k", 1);
  CHECK(slot != nullptr);
  CHECK(attrs_readonly(slot) != nullptr);
  CHECK_EQ(attrs_readonly(slot)->get_int("k", -1), 1);
}

// ---------------------------------------------------------------------------
// UI zoom keys (NEUI_ATTR_UI_SCALE / NEUI_ATTR_PAINT_DEVICE_PIXELS)
// ---------------------------------------------------------------------------

TEST_CASE("ui_scale / paint_device_pixels are registered with the right kinds")
{
  bool saw_scale = false, saw_device = false;
  for (const auto& row : k_well_known_attrs) {
    if (!std::strcmp(row.key, NEUI_ATTR_UI_SCALE)) {
      saw_scale = true;
      CHECK(row.kind == AttrKind::FLOAT);
    }
    if (!std::strcmp(row.key, NEUI_ATTR_PAINT_DEVICE_PIXELS)) {
      saw_device = true;
      CHECK(row.kind == AttrKind::INT);
    }
  }
  CHECK(saw_scale);
  CHECK(saw_device);
}

// The clamp lives on WidgetData::ui_scale() in the xpl host (which the Tier-1
// suite deliberately does not link), so mirror the contract here over the raw
// AttrBag: garbage in must not be able to produce a non-positive or absurd
// scale, because that would divide input coordinates by zero or blow up the
// native window size.
namespace {
  float clamp_ui_scale(const AttrBag& bag)
  {
    float z = bag.get_float(NEUI_ATTR_UI_SCALE, 1.0f);
    if (!(z > 0.0f)) return 1.0f;
    if (z < NEUI_UI_SCALE_MIN) return NEUI_UI_SCALE_MIN;
    if (z > NEUI_UI_SCALE_MAX) return NEUI_UI_SCALE_MAX;
    return z;
  }
}

TEST_CASE("ui_scale clamp: unset is 1.0 and sane values pass through")
{
  AttrBag bag;
  CHECK_APPROX(clamp_ui_scale(bag), 1.0);
  bag.set_float(NEUI_ATTR_UI_SCALE, 1.5f);
  CHECK_APPROX(clamp_ui_scale(bag), 1.5);
  bag.set_float(NEUI_ATTR_UI_SCALE, 2.0f);
  CHECK_APPROX(clamp_ui_scale(bag), 2.0);
}

TEST_CASE("ui_scale clamp: zero / negative fall back to 1.0, not to a degenerate scale")
{
  AttrBag bag;
  bag.set_float(NEUI_ATTR_UI_SCALE, 0.0f);
  CHECK_APPROX(clamp_ui_scale(bag), 1.0);
  bag.set_float(NEUI_ATTR_UI_SCALE, -3.0f);
  CHECK_APPROX(clamp_ui_scale(bag), 1.0);
}

TEST_CASE("ui_scale clamp: out-of-range values pin to the documented bounds")
{
  AttrBag bag;
  bag.set_float(NEUI_ATTR_UI_SCALE, 0.01f);
  CHECK_APPROX(clamp_ui_scale(bag), (double)NEUI_UI_SCALE_MIN);
  bag.set_float(NEUI_ATTR_UI_SCALE, 1000.0f);
  CHECK_APPROX(clamp_ui_scale(bag), (double)NEUI_UI_SCALE_MAX);
}
