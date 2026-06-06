#include "neui_test.h"

#include "compound.h"
#include "painter.h"

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

// ---------------------------------------------------------------------------
// Rect layer setters
// ---------------------------------------------------------------------------

TEST_CASE("apply_set_int routes rect colour props to rect fields only")
{
  CompoundLayer L; L.kind = NEUI_COMPOUND_LAYER_RECT;
  CHECK(apply_set_int(L, "fill_color",   (int)0xFF112233));
  CHECK(apply_set_int(L, "stroke_color", (int)0xFFAABBCC));
  CHECK_EQ((unsigned)L.fill_color,   0xFF112233u);
  CHECK_EQ((unsigned)L.stroke_color, 0xFFAABBCCu);

  // On a non-rect layer the same prop names are unrecognised (and inert).
  CompoundLayer T; T.kind = NEUI_COMPOUND_LAYER_TEXT;
  CHECK_FALSE(apply_set_int(T, "fill_color", (int)0xFF000000));
  CHECK_EQ((unsigned)T.fill_color, 0u);
}

TEST_CASE("apply_set_float: rect stroke_width / corner_radius clamp negatives to 0")
{
  CompoundLayer L; L.kind = NEUI_COMPOUND_LAYER_RECT;
  CHECK(apply_set_float(L, "stroke_width",  2.0f));
  CHECK(apply_set_float(L, "corner_radius", 6.0f));
  CHECK_APPROX(L.stroke_width,  2.0);
  CHECK_APPROX(L.corner_radius, 6.0);

  // Negative inputs are clamped (the painter would draw nothing useful with
  // them and we do not want to surprise the renderer).
  CHECK(apply_set_float(L, "stroke_width",  -1.0f));
  CHECK(apply_set_float(L, "corner_radius", -3.0f));
  CHECK_APPROX(L.stroke_width,  0.0);
  CHECK_APPROX(L.corner_radius, 0.0);
}

// ---------------------------------------------------------------------------
// Asset-layer tint
// ---------------------------------------------------------------------------

TEST_CASE("apply_set_int: \"tint\" lands on asset layers only")
{
  CompoundLayer A; A.kind = NEUI_COMPOUND_LAYER_ASSET;
  CHECK(apply_set_int(A, "tint", (int)0xFF40C0FF));
  CHECK_EQ((unsigned)A.tint, 0xFF40C0FFu);

  // Rect / text layers don't carry a tint slot - return false (inert).
  CompoundLayer R; R.kind = NEUI_COMPOUND_LAYER_RECT;
  CHECK_FALSE(apply_set_int(R, "tint", (int)0xFF000000));
  CHECK_EQ((unsigned)A.tint, 0xFF40C0FFu);  // A is unchanged
}

TEST_CASE("premultiply_tint: 0xFFFFFFFF passthrough leaves pixels intact")
{
  uint8_t src[8] = { 10, 20, 30, 40,  50, 60, 70, 80 };
  uint8_t dst[8] = { 0 };
  premultiply_tint(src, dst, 2, 1, 0xFFFFFFFFu);
  for (int i = 0; i < 8; ++i) CHECK_EQ((int)dst[i], (int)src[i]);
}

TEST_CASE("premultiply_tint: pure red tint clears green + blue, keeps alpha")
{
  // Premultiplied white-opaque -> after red tint should become red-opaque.
  uint8_t src[4] = { 255, 255, 255, 255 };   // BGRA
  uint8_t dst[4] = { 0 };
  premultiply_tint(src, dst, 1, 1, 0xFFFF0000u);  // A=FF, R=FF, G=00, B=00
  CHECK_EQ((int)dst[0],   0);   // B
  CHECK_EQ((int)dst[1],   0);   // G
  CHECK_EQ((int)dst[2], 255);   // R
  CHECK_EQ((int)dst[3], 255);   // A
}

TEST_CASE("premultiply_tint: half-alpha tint scales the alpha channel")
{
  uint8_t src[4] = { 255, 255, 255, 255 };
  uint8_t dst[4] = { 0 };
  premultiply_tint(src, dst, 1, 1, 0x80FFFFFFu);  // A=80
  CHECK_EQ((int)dst[3], (255 * 0x80) / 255);
}

// ---------------------------------------------------------------------------
// Path layer
// ---------------------------------------------------------------------------

TEST_CASE("apply_set_path: replaces the layer's command list on PATH layers")
{
  CompoundLayer L; L.kind = NEUI_COMPOUND_LAYER_PATH;
  neui_path_cmd_t cmds[3] = {
    { NEUI_PATH_CMD_MOVE_TO, { 0, 0, 0, 0, 0 } },
    { NEUI_PATH_CMD_LINE_TO, { 10, 10, 0, 0, 0 } },
    { NEUI_PATH_CMD_CLOSE,   { 0, 0, 0, 0, 0 } },
  };
  apply_set_path(L, cmds, 3);
  REQUIRE(L.path_cmds.size() == 3);
  CHECK_EQ((int)L.path_cmds[0].kind, (int)NEUI_PATH_CMD_MOVE_TO);
  CHECK_EQ((int)L.path_cmds[1].kind, (int)NEUI_PATH_CMD_LINE_TO);
  CHECK_APPROX(L.path_cmds[1].args[0], 10.0);
  CHECK_APPROX(L.path_cmds[1].args[1], 10.0);
  CHECK_EQ((int)L.path_cmds[2].kind, (int)NEUI_PATH_CMD_CLOSE);

  // Subsequent calls replace, not append.
  neui_path_cmd_t replacement[1] = { { NEUI_PATH_CMD_MOVE_TO, { 5, 5, 0, 0, 0 } } };
  apply_set_path(L, replacement, 1);
  CHECK_EQ((int)L.path_cmds.size(), 1);

  // NULL / zero count clears.
  apply_set_path(L, nullptr, 0);
  CHECK(L.path_cmds.empty());
}

TEST_CASE("apply_set_path: silently no-ops on non-PATH layers")
{
  CompoundLayer R; R.kind = NEUI_COMPOUND_LAYER_RECT;
  neui_path_cmd_t cmds[1] = { { NEUI_PATH_CMD_MOVE_TO, { 1, 2, 0, 0, 0 } } };
  apply_set_path(R, cmds, 1);
  CHECK(R.path_cmds.empty());
}

TEST_CASE("apply_set_int: PATH layers accept fill_color / stroke_color")
{
  CompoundLayer P; P.kind = NEUI_COMPOUND_LAYER_PATH;
  CHECK(apply_set_int(P, "fill_color",   (int)0xFF112233));
  CHECK(apply_set_int(P, "stroke_color", (int)0xFFAABBCC));
  CHECK_EQ((unsigned)P.fill_color,   0xFF112233u);
  CHECK_EQ((unsigned)P.stroke_color, 0xFFAABBCCu);
}

TEST_CASE("apply_set_float: PATH layers accept stroke_width but not corner_radius")
{
  CompoundLayer P; P.kind = NEUI_COMPOUND_LAYER_PATH;
  CHECK(apply_set_float(P, "stroke_width", 2.5f));
  CHECK_APPROX(P.stroke_width, 2.5);
  // corner_radius is rect-only - returns false (unrecognised) on PATH.
  CHECK_FALSE(apply_set_float(P, "corner_radius", 4.0f));
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
