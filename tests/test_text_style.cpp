// Tier-1 coverage for the text-field colour attributes (hosts/shared/text_style.h)
// and the font plumbing the INPUTBOX / MULTILINE paint + hit-test paths share
// (hosts/shared/widget_font.h).
//
// The widgets themselves live in the crossplatform host, which this suite does
// not link; what is portable - and where a regression would silently re-theme
// a client's carefully coloured field - is the RESOLUTION: which of the widget's
// attrs wins over the theme palette, and what a font attr turns into on the
// backend's font stack around a measure_text / draw_text pair.

#include "neui_test.h"

#include <cstring>   // strcmp on a const char* - libc++ supplies it transitively, libstdc++ does not
#include <string>
#include <utility>
#include <vector>

#include "text_style.h"
#include "widget_font.h"

using namespace neui_detail;

namespace {

  // The dark and light default palettes differ in every role this header reads,
  // so "unset follows the theme" is checked against both - a resolver that
  // hard-coded one palette would pass on only one of them.
  const Palette& dark()  { return default_dark_palette(); }
  const Palette& light() { return default_light_palette(); }

  // ---- A font-stack backend ------------------------------------------------
  //
  // measure_text answers per char: 7 px in the default font, 11 px in a family
  // named "Wide", plus 1 px per 100 weight above 400. Enough to tell "this
  // measurement went through the pushed font" from "it did not".
  struct FontStackBackend {
    neui_render_backend_t be{};
    std::vector<std::pair<std::string, int>> stack;
    int pushes = 0, pops = 0;
  };
  FontStackBackend* g_fb = nullptr;

  void NEUI_ABI fb_push(neui_render_ctx_t, const char* family, int weight)
  {
    ++g_fb->pushes;
    g_fb->stack.emplace_back(family ? family : "", weight);
  }
  void NEUI_ABI fb_pop(neui_render_ctx_t)
  {
    ++g_fb->pops;
    if (!g_fb->stack.empty()) g_fb->stack.pop_back();
  }
  float NEUI_ABI fb_measure(neui_render_ctx_t, const char* text, int len, float)
  {
    size_t n = (len < 0) ? std::strlen(text) : (size_t)len;
    float per = 7.0f;
    if (!g_fb->stack.empty()) {
      const auto& top = g_fb->stack.back();
      if (top.first == "Wide") per = 11.0f;
      int w = top.second > 0 ? top.second : 400;
      per += (float)(w - 400) / 100.0f;
    }
    return per * (float)n;
  }

  struct FontFixture {
    FontStackBackend fb;
    FontFixture()
    {
      fb.be.push_font    = fb_push;
      fb.be.pop_font     = fb_pop;
      fb.be.measure_text = fb_measure;
      g_fb = &fb;
    }
    ~FontFixture() { g_fb = nullptr; }
    neui_render_backend_t* backend() { return &fb.be; }
  };

  const neui_render_ctx_t k_ctx = (neui_render_ctx_t)0x1234;

} // namespace

// ---------------------------------------------------------------------------
// The attribute rows
// ---------------------------------------------------------------------------

TEST_CASE("text colour keys are registered as INT in k_well_known_attrs")
{
  const char* keys[] = { NEUI_ATTR_TEXT_COLOR, NEUI_ATTR_BORDER_COLOR,
                         NEUI_ATTR_BORDER_FOCUS_COLOR, NEUI_ATTR_SELECTION_COLOR };
  for (const char* k : keys) {
    const AttrKind* kind = well_known_attr_kind(k);
    REQUIRE(kind != nullptr);
    CHECK(*kind == AttrKind::INT);
  }
}

TEST_CASE("text colour keys are distinct and live in the neui.attr namespace")
{
  const char* keys[] = { NEUI_ATTR_TEXT_COLOR, NEUI_ATTR_BORDER_COLOR,
                         NEUI_ATTR_BORDER_FOCUS_COLOR, NEUI_ATTR_SELECTION_COLOR };
  for (size_t i = 0; i < 4; ++i) {
    CHECK(std::strncmp(keys[i], "neui.attr.", 10) == 0);
    for (size_t j = i + 1; j < 4; ++j) CHECK(std::strcmp(keys[i], keys[j]) != 0);
  }
}

// ---------------------------------------------------------------------------
// Unset == the theme, exactly as before the attributes existed
// ---------------------------------------------------------------------------

TEST_CASE("text field style: no attrs follows the theme palette (dark and light)")
{
  for (const Palette* pal : { &dark(), &light() }) {
    AttrBag empty;
    for (const AttrBag* bag : { (const AttrBag*)nullptr, (const AttrBag*)&empty }) {
      TextFieldStyle s = resolve_text_field_style(bag, *pal);
      CHECK_EQ(s.fill,           color(*pal, ColorRole::control_bg));
      CHECK_EQ(s.text,           color(*pal, ColorRole::text_primary));
      CHECK_EQ(s.border,         color(*pal, ColorRole::border));
      CHECK_EQ(s.border_focused, color(*pal, ColorRole::border_focused));
      CHECK_EQ(s.selection,      color(*pal, ColorRole::accent_translucent));
    }
  }
}

TEST_CASE("text field style: attrs unrelated to colour change nothing")
{
  AttrBag bag;
  bag.set_string(NEUI_ATTR_FONT_FAMILY, "Example Sans");
  bag.set_float(NEUI_ATTR_FONT_SIZE, 16.0f);
  bag.set_int(NEUI_ATTR_FONT_WEIGHT, 500);
  bag.set_int(NEUI_ATTR_READONLY, 1);
  TextFieldStyle s = resolve_text_field_style(&bag, dark());
  CHECK_EQ(s.fill, color(dark(), ColorRole::control_bg));
  CHECK_EQ(s.text, color(dark(), ColorRole::text_primary));
}

// ---------------------------------------------------------------------------
// Set == exactly that colour
// ---------------------------------------------------------------------------

TEST_CASE("text field style: each attr overrides only its own role")
{
  // Walk the five attrs one at a time: the others must stay on the theme.
  struct Row { const char* key; uint32_t argb; };
  const Row rows[] = {
    { NEUI_ATTR_BACKGROUND,          0xFF101820u },
    { NEUI_ATTR_TEXT_COLOR,          0xFFE8F0FFu },
    { NEUI_ATTR_BORDER_COLOR,        0xFF304050u },
    { NEUI_ATTR_BORDER_FOCUS_COLOR,  0xFFFFA000u },
    { NEUI_ATTR_SELECTION_COLOR,     0x80336699u },
  };
  const TextFieldStyle theme = resolve_text_field_style(nullptr, light());
  for (int which = 0; which < 5; ++which) {
    AttrBag bag;
    bag.set_int(rows[which].key, (int32_t)rows[which].argb);
    TextFieldStyle s = resolve_text_field_style(&bag, light());
    CHECK_EQ(s.fill,           which == 0 ? rows[0].argb : theme.fill);
    CHECK_EQ(s.text,           which == 1 ? rows[1].argb : theme.text);
    CHECK_EQ(s.border,         which == 2 ? rows[2].argb : theme.border);
    CHECK_EQ(s.border_focused, which == 3 ? rows[3].argb : theme.border_focused);
    CHECK_EQ(s.selection,      which == 4 ? rows[4].argb : theme.selection);
  }
}

TEST_CASE("text field style: a fully styled field takes every colour from the bag")
{
  AttrBag bag;
  bag.set_int(NEUI_ATTR_BACKGROUND,         (int32_t)0xFF14181Cu);
  bag.set_int(NEUI_ATTR_TEXT_COLOR,         (int32_t)0xFFD0D8E0u);
  bag.set_int(NEUI_ATTR_BORDER_COLOR,       (int32_t)0xFF2A3038u);
  bag.set_int(NEUI_ATTR_BORDER_FOCUS_COLOR, (int32_t)0xFF4C9EFFu);
  bag.set_int(NEUI_ATTR_SELECTION_COLOR,    (int32_t)0x604C9EFFu);
  TextFieldStyle s = resolve_text_field_style(&bag, light());
  CHECK_EQ(s.fill,           0xFF14181Cu);
  CHECK_EQ(s.text,           0xFFD0D8E0u);
  CHECK_EQ(s.border,         0xFF2A3038u);
  CHECK_EQ(s.border_focused, 0xFF4C9EFFu);
  CHECK_EQ(s.selection,      0x604C9EFFu);
}

TEST_CASE("text field style: alpha 0 is a colour, not 'unset'")
{
  // 0x00000000 must mean "invisible" (a borderless field), not fall back to
  // the theme - there is no sentinel value, "set" is attrs->has().
  AttrBag bag;
  bag.set_int(NEUI_ATTR_BORDER_COLOR, 0);
  bag.set_int(NEUI_ATTR_BACKGROUND, 0);
  TextFieldStyle s = resolve_text_field_style(&bag, dark());
  CHECK_EQ(s.border, 0u);
  CHECK_EQ(s.fill,   0u);
  CHECK(s.border != color(dark(), ColorRole::border));
}

TEST_CASE("text field style: translucent and high-bit ARGB survive the int round-trip")
{
  // Attributes store int32_t; 0xFFxxxxxx is negative there. The conversion back
  // must be bit-exact or every opaque colour would come out wrong.
  AttrBag bag;
  bag.set_int(NEUI_ATTR_TEXT_COLOR, (int32_t)0xFF8000FFu);
  bag.set_int(NEUI_ATTR_SELECTION_COLOR, (int32_t)0x01020304u);
  TextFieldStyle s = resolve_text_field_style(&bag, dark());
  CHECK_EQ(s.text, 0xFF8000FFu);
  CHECK_EQ(s.selection, 0x01020304u);
}

TEST_CASE("text field style: remove() returns a role to the theme, and it is live")
{
  AttrBag bag;
  bag.set_int(NEUI_ATTR_TEXT_COLOR, (int32_t)0xFFFF0000u);
  CHECK_EQ(resolve_text_field_style(&bag, dark()).text, 0xFFFF0000u);
  bag.set_int(NEUI_ATTR_TEXT_COLOR, (int32_t)0xFF00FF00u);    // overwrite
  CHECK_EQ(resolve_text_field_style(&bag, dark()).text, 0xFF00FF00u);
  CHECK(bag.remove(NEUI_ATTR_TEXT_COLOR));
  CHECK_EQ(resolve_text_field_style(&bag, dark()).text,
           color(dark(), ColorRole::text_primary));
}

TEST_CASE("text field style: an explicit colour wins over either palette")
{
  AttrBag bag;
  bag.set_int(NEUI_ATTR_BACKGROUND, (int32_t)0xFF223344u);
  CHECK_EQ(resolve_text_field_style(&bag, dark()).fill,  0xFF223344u);
  CHECK_EQ(resolve_text_field_style(&bag, light()).fill, 0xFF223344u);
}

// ---------------------------------------------------------------------------
// attr_color_or / resolve_text_color
// ---------------------------------------------------------------------------

TEST_CASE("attr_color_or: a wrong-kind slot falls back instead of becoming a colour")
{
  // Well-known keys assert on a wrong-kind write in debug builds, so probe the
  // release-build behaviour (the slot is stored anyway, reads keep returning the
  // default) with a key outside the table.
  AttrBag bag;
  bag.set_float("x.color", 1.5f);
  CHECK_EQ(attr_color_or(&bag, "x.color", 0xFF123456u), 0xFF123456u);
  bag.set_string("x.color2", "red");
  CHECK_EQ(attr_color_or(&bag, "x.color2", 0xFF123456u), 0xFF123456u);
  bag.set_int("x.color3", (int32_t)0xFFABCDEFu);
  CHECK_EQ(attr_color_or(&bag, "x.color3", 0xFF123456u), 0xFFABCDEFu);
  // An int that happens to equal one of the probe defaults is still a colour.
  bag.set_int("x.zero", 0);
  bag.set_int("x.one", 1);
  CHECK_EQ(attr_color_or(&bag, "x.zero", 0xFF123456u), 0u);
  CHECK_EQ(attr_color_or(&bag, "x.one", 0xFF123456u), 1u);
}

TEST_CASE("resolve_text_color: LABEL / BUTTON read only the text colour")
{
  AttrBag bag;
  CHECK_EQ(resolve_text_color(&bag, dark()), color(dark(), ColorRole::text_primary));
  CHECK_EQ(resolve_text_color(nullptr, light()), color(light(), ColorRole::text_primary));
  bag.set_int(NEUI_ATTR_BACKGROUND, (int32_t)0xFF000000u);   // a fill is not a text colour
  CHECK_EQ(resolve_text_color(&bag, dark()), color(dark(), ColorRole::text_primary));
  bag.set_int(NEUI_ATTR_TEXT_COLOR, (int32_t)0xFFCCDDEEu);
  CHECK_EQ(resolve_text_color(&bag, dark()), 0xFFCCDDEEu);
}

// ---------------------------------------------------------------------------
// Font attrs -> the backend font stack (what INPUTBOX / MULTILINE caret,
// hit-testing, selection and wrap measure through)
// ---------------------------------------------------------------------------

TEST_CASE("read_widget_font: family, weight and size come from the bag")
{
  AttrBag bag;
  bag.set_string(NEUI_ATTR_FONT_FAMILY, "Example Sans");
  bag.set_int(NEUI_ATTR_FONT_WEIGHT, 600);
  bag.set_float(NEUI_ATTR_FONT_SIZE, 15.0f);
  EffectiveFont ef = read_widget_font(&bag, 12.0f);
  CHECK_EQ(ef.family, std::string("Example Sans"));
  CHECK_EQ(ef.weight, 600);
  CHECK_APPROX(ef.size, 15.0);
  CHECK_FALSE(font_is_default(ef));

  EffectiveFont none = read_widget_font(nullptr, 12.0f);
  CHECK(font_is_default(none));
  CHECK_APPROX(none.size, 12.0);
}

TEST_CASE("push_widget_font: the family and weight reach the font stack, balanced")
{
  FontFixture f;
  AttrBag bag;
  bag.set_string(NEUI_ATTR_FONT_FAMILY, "Wide");
  bag.set_int(NEUI_ATTR_FONT_WEIGHT, 700);
  EffectiveFont ef = read_widget_font(&bag, 12.0f);

  push_widget_font(f.backend(), k_ctx, ef);
  REQUIRE_EQ(f.fb.stack.size(), (size_t)1);
  CHECK_EQ(f.fb.stack.back().first, std::string("Wide"));
  CHECK_EQ(f.fb.stack.back().second, 700);
  pop_widget_font(f.backend(), k_ctx, ef);
  CHECK(f.fb.stack.empty());
  CHECK_EQ(f.fb.pushes, f.fb.pops);
}

TEST_CASE("push_widget_font: a widget without font attrs never touches the stack")
{
  FontFixture f;
  EffectiveFont ef = read_widget_font(nullptr, 12.0f);
  push_widget_font(f.backend(), k_ctx, ef);
  pop_widget_font(f.backend(), k_ctx, ef);
  CHECK_EQ(f.fb.pushes, 0);
  CHECK_EQ(f.fb.pops, 0);
}

TEST_CASE("push_widget_font: a weight alone (no family) still selects the weight")
{
  FontFixture f;
  AttrBag bag;
  bag.set_int(NEUI_ATTR_FONT_WEIGHT, 700);
  EffectiveFont ef = read_widget_font(&bag, 12.0f);
  push_widget_font(f.backend(), k_ctx, ef);
  REQUIRE_EQ(f.fb.stack.size(), (size_t)1);
  CHECK_EQ(f.fb.stack.back().first, std::string());   // system family, bold
  CHECK_EQ(f.fb.stack.back().second, 700);
  pop_widget_font(f.backend(), k_ctx, ef);
}

TEST_CASE("caret geometry: a prefix measured inside the push is in the widget's font")
{
  // The INPUTBOX caret x is `pad + measure_text(text, cursor_pos)`, the
  // hit-test walks the same measure_text, and the selection edges are two more
  // of it - all inside one push/pop. If any of them ran OUTSIDE it they would
  // be laid out in the default font while the glyphs are drawn in the widget's.
  FontFixture f;
  AttrBag bag;
  bag.set_string(NEUI_ATTR_FONT_FAMILY, "Wide");
  EffectiveFont ef = read_widget_font(&bag, 12.0f);

  const char* s = "hello";
  const float outside = f.backend()->measure_text(k_ctx, s, 3, ef.size);   // default font
  push_widget_font(f.backend(), k_ctx, ef);
  const float inside  = f.backend()->measure_text(k_ctx, s, 3, ef.size);
  pop_widget_font(f.backend(), k_ctx, ef);
  const float after   = f.backend()->measure_text(k_ctx, s, 3, ef.size);

  CHECK_APPROX(outside, 21.0);     // 3 chars x 7 px
  CHECK_APPROX(inside,  33.0);     // 3 chars x 11 px in "Wide"
  CHECK_APPROX(after,   21.0);     // the pop restored the default
}
