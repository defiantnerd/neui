#include "neui_test.h"

#include "compound.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// Template parsing + rendering
// ---------------------------------------------------------------------------

static std::string render(const char* tmpl, const AttrBag* bag)
{
  return render_template(parse_template(tmpl), bag);
}

TEST_CASE("parse_template: plain literal is one literal segment")
{
  auto segs = parse_template("hello world");
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].is_literal);
  CHECK_EQ(segs[0].text, std::string("hello world"));
}

TEST_CASE("parse_template: {key} becomes a key segment")
{
  auto segs = parse_template("a {key} b");
  REQUIRE(segs.size() == 3);
  CHECK(segs[0].is_literal);
  CHECK_FALSE(segs[1].is_literal);
  CHECK_EQ(segs[1].text, std::string("key"));
  CHECK(segs[2].is_literal);
}

TEST_CASE("parse_template: {{ and }} collapse to literal braces")
{
  auto segs = parse_template("{{x}}");
  std::string joined;
  for (auto& s : segs) { CHECK(s.is_literal); joined += s.text; }
  CHECK_EQ(joined, std::string("{x}"));
}

TEST_CASE("parse_template: unclosed brace falls through as literal")
{
  auto segs = parse_template("a {unterminated");
  std::string joined;
  for (auto& s : segs) { CHECK(s.is_literal); joined += s.text; }
  CHECK_EQ(joined, std::string("a {unterminated"));
}

TEST_CASE("render_template: substitutes string / int / float attrs")
{
  AttrBag bag;
  bag.set_string("label", "Gain");
  bag.set_int("count", 42);
  bag.set_float("ratio", 1.5f);
  CHECK_EQ(render("{label}: {count} @ {ratio}", &bag),
           std::string("Gain: 42 @ 1.5"));
}

TEST_CASE("render_template: missing key yields empty, exact-zero yields 0")
{
  AttrBag bag;
  bag.set_int("zero", 0);
  CHECK_EQ(render("[{absent}]", &bag), std::string("[]"));
  CHECK_EQ(render("{zero}", &bag), std::string("0"));
}

TEST_CASE("render_template: null bag emits literals only")
{
  CHECK_EQ(render("a{key}b", nullptr), std::string("ab"));
}

// ---------------------------------------------------------------------------
// Anchor geometry
// ---------------------------------------------------------------------------

TEST_CASE("anchor_point resolves the 9 anchors within a 100x40 rect")
{
  float x, y;
  anchor_point(NEUI_ANCHOR_TOP_LEFT, 100, 40, x, y);     CHECK_APPROX(x, 0);  CHECK_APPROX(y, 0);
  anchor_point(NEUI_ANCHOR_CENTER, 100, 40, x, y);       CHECK_APPROX(x, 50); CHECK_APPROX(y, 20);
  anchor_point(NEUI_ANCHOR_BOTTOM_RIGHT, 100, 40, x, y); CHECK_APPROX(x, 100);CHECK_APPROX(y, 40);
  anchor_point(NEUI_ANCHOR_TOP_RIGHT, 100, 40, x, y);    CHECK_APPROX(x, 100);CHECK_APPROX(y, 0);
  anchor_point(NEUI_ANCHOR_BOTTOM_LEFT, 100, 40, x, y);  CHECK_APPROX(x, 0);  CHECK_APPROX(y, 40);
}

TEST_CASE("resolve_layer_rect: top-left anchors apply offset verbatim")
{
  LayerRect r = resolve_layer_rect(200, 100,
                                   NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT,
                                   5, 7, 30, 20);
  CHECK_APPROX(r.x, 5);
  CHECK_APPROX(r.y, 7);
  CHECK_APPROX(r.w, 30);
  CHECK_APPROX(r.h, 20);
}

TEST_CASE("resolve_layer_rect: center-in-center centres the layer")
{
  LayerRect r = resolve_layer_rect(200, 100,
                                   NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER,
                                   0, 0, 40, 20);
  CHECK_APPROX(r.x, 200.0 / 2 - 40.0 / 2);   // 80
  CHECK_APPROX(r.y, 100.0 / 2 - 20.0 / 2);   // 40
}

TEST_CASE("resolve_layer_rect: bottom-right pins the layer to the corner")
{
  LayerRect r = resolve_layer_rect(200, 100,
                                   NEUI_ANCHOR_BOTTOM_RIGHT, NEUI_ANCHOR_BOTTOM_RIGHT,
                                   0, 0, 40, 20);
  CHECK_APPROX(r.x, 200 - 40);
  CHECK_APPROX(r.y, 100 - 20);
}

// ---------------------------------------------------------------------------
// Bindings + effective values
// ---------------------------------------------------------------------------

TEST_CASE("round_to_int rounds half away from zero")
{
  CHECK_EQ(round_to_int(2.4f), 2);
  CHECK_EQ(round_to_int(2.5f), 3);
  CHECK_EQ(round_to_int(-2.5f), -3);
  CHECK_EQ(round_to_int(-2.4f), -2);
}

TEST_CASE("eval_binding_float: scale*x + offset over the attr value")
{
  AttrBag bag;
  bag.set_float("v", 0.5f);
  CompoundBinding b;
  b.attr_key = "v";
  b.scale = 360.0f;
  b.offset = 10.0f;
  CHECK_APPROX(eval_binding_float(b, &bag), 190.0);   // 0.5*360 + 10

  // Int attrs are promoted by attr_as_float.
  AttrBag ibag;
  ibag.set_int("n", 3);
  CompoundBinding bi;
  bi.attr_key = "n";
  bi.scale = 2.0f;
  CHECK_APPROX(eval_binding_float(bi, &ibag), 6.0);
}

TEST_CASE("effective_int/float: static value unless a binding overrides")
{
  AttrBag bag;
  bag.set_float("v", 0.25f);
  CompoundLayer L;
  // Unbound prop returns the static field value.
  CHECK_EQ(effective_int(L, "offset_x", 17, &bag), 17);
  CHECK_APPROX(effective_float(L, "rotation", 1.0f, &bag), 1.0);

  // Bound prop returns the transformed attr value.
  CompoundBinding b; b.attr_key = "v"; b.scale = 100.0f; b.offset = 0.0f;
  L.bindings["offset_x"] = b;
  CHECK_EQ(effective_int(L, "offset_x", 17, &bag), 25);   // round(0.25*100)
}

TEST_CASE("eval_binding_asset: missing or zero id -> asset_none, else handle")
{
  AttrBag bag;
  CompoundBinding b; b.attr_key = "img"; b.is_asset = true;
  CHECK(eval_binding_asset(b, &bag).id == asset_none.id);   // missing key

  bag.set_int("img", 0);
  CHECK(eval_binding_asset(b, &bag).id == asset_none.id);   // explicit 0

  bag.set_int("img", 1234);
  CHECK_EQ((unsigned)eval_binding_asset(b, &bag).id, 1234u);
}
