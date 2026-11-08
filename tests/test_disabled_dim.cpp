// Tier-1 coverage for NEUI_ATTR_DISABLED_DIM: the channel decode and the
// "what does this widget get" decision (hosts/shared/disabled_dim.h), and the
// backend wrapper that applies the colour part (hosts/shared/color_dim_backend.h).
//
// The crossplatform host, which applies the dim around a widget's two paint
// passes, is not linked here; tests/disabled_dim_smoke_macos.mm checks the
// pixels it produces.

#include "neui_test.h"

#include <cstdint>
#include <cstring>   // strncmp - libc++ supplies it transitively, libstdc++ does not
#include <vector>

#include "color_dim_backend.h"
#include "disabled_dim.h"

using namespace neui_detail;

namespace {

  // ---- A backend that records what reaches it --------------------------------

  struct Recorder {
    neui_render_ctx_t ctx = nullptr;
    uint32_t argb = 0;            // the colour of the last solid draw
    uint32_t tint = 0;            // the tint of the last draw_bitmap
    std::vector<neui_gradient_stop_t> stops;   // the last gradient's stops
    const neui_stroke_style_t* style = nullptr;
    int calls = 0;
  };
  Recorder g_rec;

  void note(neui_render_ctx_t ctx, uint32_t argb)
  {
    g_rec.ctx = ctx;
    g_rec.argb = argb;
    ++g_rec.calls;
  }
  void note_gradient(neui_render_ctx_t ctx, const neui_gradient_t* grad)
  {
    g_rec.ctx = ctx;
    g_rec.stops.assign(grad->stops, grad->stops + grad->stop_count);
    ++g_rec.calls;
  }

  void NEUI_ABI r_fill_rect(neui_render_ctx_t ctx, float, float, float, float, uint32_t argb)
  { note(ctx, argb); }
  void NEUI_ABI r_draw_rect(neui_render_ctx_t ctx, float, float, float, float, float, uint32_t argb)
  { note(ctx, argb); }
  void NEUI_ABI r_draw_text(neui_render_ctx_t ctx, float, float, float, float, const char*, float,
                            uint32_t argb)
  { note(ctx, argb); }
  void NEUI_ABI r_draw_bitmap(neui_render_ctx_t ctx, void*, float, float, float, float,
                              float, float, float, float, uint32_t tint)
  {
    g_rec.ctx = ctx;
    g_rec.tint = tint;
    ++g_rec.calls;
  }
  void NEUI_ABI r_fill_path(neui_render_ctx_t ctx, uint32_t argb) { note(ctx, argb); }
  void NEUI_ABI r_stroke_path(neui_render_ctx_t ctx, float, uint32_t argb) { note(ctx, argb); }
  void NEUI_ABI r_stroke_path_styled(neui_render_ctx_t ctx, float, uint32_t argb,
                                     const neui_stroke_style_t* style)
  {
    note(ctx, argb);
    g_rec.style = style;
  }
  void NEUI_ABI r_fill_rect_gradient(neui_render_ctx_t ctx, float, float, float, float,
                                     const neui_gradient_t* grad)
  { note_gradient(ctx, grad); }
  void NEUI_ABI r_fill_path_gradient(neui_render_ctx_t ctx, const neui_gradient_t* grad)
  { note_gradient(ctx, grad); }
  void NEUI_ABI r_stroke_path_gradient(neui_render_ctx_t ctx, float, const neui_gradient_t* grad,
                                       const neui_stroke_style_t* style)
  {
    note_gradient(ctx, grad);
    g_rec.style = style;
  }
  void NEUI_ABI r_push_alpha(neui_render_ctx_t, float) {}
  void NEUI_ABI r_begin_path(neui_render_ctx_t) {}
  bool NEUI_ABI r_draw_shader(neui_render_ctx_t, void*, int32_t, int32_t, uint32_t, uint32_t,
                              const float*, uint32_t)
  { return true; }
  void* NEUI_ABI r_create_shader(int, const char*, size_t) { return nullptr; }

  neui_render_backend_t recording_backend()
  {
    neui_render_backend_t be{};
    be.fill_rect            = r_fill_rect;
    be.draw_rect            = r_draw_rect;
    be.draw_text            = r_draw_text;
    be.draw_bitmap          = r_draw_bitmap;
    be.fill_path            = r_fill_path;
    be.stroke_path          = r_stroke_path;
    be.stroke_path_styled   = r_stroke_path_styled;
    be.fill_rect_gradient   = r_fill_rect_gradient;
    be.fill_path_gradient   = r_fill_path_gradient;
    be.stroke_path_gradient = r_stroke_path_gradient;
    be.push_alpha           = r_push_alpha;
    be.begin_path           = r_begin_path;
    be.draw_shader_program  = r_draw_shader;
    be.create_shader_program = r_create_shader;
    return be;
  }

  const neui_render_ctx_t k_target = (neui_render_ctx_t)0x1000;
  const neui_render_ctx_t k_other  = (neui_render_ctx_t)0x2000;

} // namespace

// ---------------------------------------------------------------------------
// The attribute row
// ---------------------------------------------------------------------------

TEST_CASE("disabled_dim: the key is an INT in k_well_known_attrs, in the neui.attr namespace")
{
  const AttrKind* kind = well_known_attr_kind(NEUI_ATTR_DISABLED_DIM);
  REQUIRE(kind != nullptr);
  CHECK(*kind == AttrKind::INT);
  CHECK(std::strncmp(NEUI_ATTR_DISABLED_DIM, "neui.attr.", 10) == 0);
}

// ---------------------------------------------------------------------------
// The decode
// ---------------------------------------------------------------------------

TEST_CASE("disabled_dim: a channel byte is byte / 256, and 0xFF is exactly 1")
{
  CHECK_EQ(dim_channel_factor(0xFF), 1.0f);
  CHECK_EQ(dim_channel_factor(0x80), 0.5f);
  CHECK_EQ(dim_channel_factor(0x00), 0.0f);
  CHECK_EQ(dim_channel_factor(0x40), 0.25f);
  CHECK_EQ(dim_channel_factor(0xFE), 254.0f / 256.0f);
  // Only the low byte counts, so a shifted ARGB can be passed as it is.
  CHECK_EQ(dim_channel_factor(0x12345680u), 0.5f);
  CHECK_EQ(dim_channel_units(0xFF), 256u);
  CHECK_EQ(dim_channel_units(0x80), 128u);
}

TEST_CASE("disabled_dim: the default is today's 50 % opacity, exactly")
{
  CHECK_EQ(k_disabled_dim_default, 0x80FFFFFFu);
  CHECK_EQ(dim_alpha_factor(k_disabled_dim_default), 0.5f);
  CHECK(dim_scales_alpha(k_disabled_dim_default));
  CHECK_FALSE(dim_scales_color(k_disabled_dim_default));
}

TEST_CASE("disabled_dim: which parts a value changes")
{
  CHECK_FALSE(dim_scales_alpha(k_disabled_dim_none));
  CHECK_FALSE(dim_scales_color(k_disabled_dim_none));
  CHECK_FALSE(dim_scales_alpha(0xFF808080u));
  CHECK(dim_scales_color(0xFF808080u));
  CHECK(dim_scales_color(0xFFFFFFFEu));   // one channel below 0xFF is enough
  CHECK(dim_scales_alpha(0x00FFFFFFu));
  CHECK_EQ(dim_alpha_factor(0x00FFFFFFu), 0.0f);
  CHECK_EQ(dim_alpha_factor(0xFF000000u), 1.0f);
}

TEST_CASE("disabled_dim: a colour's R, G and B are scaled per channel, its alpha is kept")
{
  CHECK_EQ(dim_color(0xFFFFFFFFu, 0xFF808080u), 0xFF808080u);        // 255 x 0.5 rounds to 128
  CHECK_EQ(dim_color(0xFFC86432u, 0xFF808080u), 0xFF643219u);        // 200 100 50 -> 100 50 25
  CHECK_EQ(dim_color(0xFFFFFFFFu, 0xFF804000u), 0xFF804000u);        // each channel its own factor
  CHECK_EQ(dim_color(0x40123456u, 0x00FFFFFFu), 0x40123456u);        // colour factors 1: unchanged
  CHECK_EQ(dim_color(0x40FFFFFFu, 0x80808080u), 0x40808080u);        // the A byte is not the colour's
  CHECK_EQ(dim_color(0xFF00FF00u, 0xFF000000u), 0xFF000000u);        // 0 = black
  // 0xFF changes nothing, for every value.
  for (uint32_t v = 0; v < 256; ++v) CHECK_EQ(dim_scale_channel(v, 0xFF), v);
}

// ---------------------------------------------------------------------------
// What a widget gets
// ---------------------------------------------------------------------------

TEST_CASE("disabled_dim: unset and disabled gets the default 0x80FFFFFF")
{
  AttrBag empty;
  CHECK_EQ(widget_disabled_dim(false, nullptr), 0x80FFFFFFu);
  CHECK_EQ(widget_disabled_dim(false, &empty), 0x80FFFFFFu);
  AttrBag other;
  other.set_int(NEUI_ATTR_BACKGROUND, (int32_t)0xFF102030u);   // an unrelated colour attr
  CHECK_EQ(widget_disabled_dim(false, &other), 0x80FFFFFFu);
}

TEST_CASE("disabled_dim: an enabled widget is never dimmed, whatever the attribute says")
{
  AttrBag bag;
  CHECK_EQ(widget_disabled_dim(true, nullptr), k_disabled_dim_none);
  CHECK_EQ(widget_disabled_dim(true, &bag), k_disabled_dim_none);
  bag.set_int(NEUI_ATTR_DISABLED_DIM, (int32_t)0x40808080u);
  CHECK_EQ(widget_disabled_dim(true, &bag), k_disabled_dim_none);
}

TEST_CASE("disabled_dim: a set value is used as it is, read as unsigned")
{
  AttrBag bag;
  bag.set_int(NEUI_ATTR_DISABLED_DIM, (int32_t)0xFF808080u);   // negative as an int32
  CHECK_EQ(widget_disabled_dim(false, &bag), 0xFF808080u);
  bag.set_int(NEUI_ATTR_DISABLED_DIM, (int32_t)0xFFFFFFFFu);   // -1: no dim at all
  CHECK_EQ(widget_disabled_dim(false, &bag), k_disabled_dim_none);
  bag.set_int(NEUI_ATTR_DISABLED_DIM, 0);                      // a real value, not "unset"
  CHECK_EQ(widget_disabled_dim(false, &bag), 0u);
  bag.set_int(NEUI_ATTR_DISABLED_DIM, 1);
  CHECK_EQ(widget_disabled_dim(false, &bag), 1u);
  bag.remove(NEUI_ATTR_DISABLED_DIM);                          // removed: back to the default
  CHECK_EQ(widget_disabled_dim(false, &bag), k_disabled_dim_default);
}

// ---------------------------------------------------------------------------
// ColorDimBackend
// ---------------------------------------------------------------------------

TEST_CASE("ColorDimBackend: every solid colour drawn on its context is scaled")
{
  neui_render_backend_t real = recording_backend();
  const uint32_t dim = 0xFF808080u;
  ColorDimBackend wrap(&real, k_target, dim);
  neui_render_backend_t* be = wrap.backend();
  const uint32_t c = 0xC0C86432u;
  const uint32_t want = 0xC0643219u;   // alpha kept, 200 100 50 halved

  g_rec = Recorder{};
  be->fill_rect(k_target, 0, 0, 1, 1, c);
  CHECK_EQ(g_rec.argb, want);
  be->draw_rect(k_target, 0, 0, 1, 1, 1, c);
  CHECK_EQ(g_rec.argb, want);
  be->draw_text(k_target, 0, 0, 1, 1, "x", 12, c);
  CHECK_EQ(g_rec.argb, want);
  be->fill_path(k_target, c);
  CHECK_EQ(g_rec.argb, want);
  be->stroke_path(k_target, 1, c);
  CHECK_EQ(g_rec.argb, want);
  neui_stroke_style_t style{};
  be->stroke_path_styled(k_target, 1, c, &style);
  CHECK_EQ(g_rec.argb, want);
  CHECK(g_rec.style == &style);
  CHECK_EQ(g_rec.calls, 6);
  CHECK(g_rec.ctx == k_target);
}

TEST_CASE("ColorDimBackend: a bitmap's tint carries the dim, its alpha byte is kept")
{
  neui_render_backend_t real = recording_backend();
  ColorDimBackend wrap(&real, k_target, 0xFF804000u);
  neui_render_backend_t* be = wrap.backend();
  g_rec = Recorder{};
  // "No tint" becomes an opaque tint of the dim's colour, so the backend's
  // multiply runs.
  be->draw_bitmap(k_target, nullptr, 0, 0, 0, 0, 0, 0, 1, 1, 0xFFFFFFFFu);
  CHECK_EQ(g_rec.tint, 0xFF804000u);
  // An existing tint is multiplied, and keeps its own alpha.
  be->draw_bitmap(k_target, nullptr, 0, 0, 0, 0, 0, 0, 1, 1, 0x80FF8080u);
  CHECK_EQ(g_rec.tint, 0x80802000u);
}

TEST_CASE("ColorDimBackend: gradient stops are scaled on a copy")
{
  neui_render_backend_t real = recording_backend();
  ColorDimBackend wrap(&real, k_target, 0xFF808080u);
  neui_render_backend_t* be = wrap.backend();

  const neui_gradient_stop_t stops[3] = {
    { 0.0f, 0xFFFFFFFFu }, { 0.5f, 0x80C86432u }, { 1.0f, 0x00000000u } };
  neui_gradient_t g{};
  g.kind = NEUI_GRADIENT_RADIAL;
  g.stops = stops;
  g.stop_count = 3;
  g.radius = 7.0f;

  g_rec = Recorder{};
  be->fill_rect_gradient(k_target, 0, 0, 1, 1, &g);
  REQUIRE_EQ(g_rec.stops.size(), (size_t)3);
  CHECK_EQ(g_rec.stops[0].argb, 0xFF808080u);
  CHECK_EQ(g_rec.stops[1].argb, 0x80643219u);
  CHECK_EQ(g_rec.stops[2].argb, 0x00000000u);
  CHECK_EQ(g_rec.stops[1].offset, 0.5f);
  // The caller's gradient is untouched.
  CHECK_EQ(stops[0].argb, 0xFFFFFFFFu);
  CHECK(g.stops == stops);

  g_rec = Recorder{};
  be->fill_path_gradient(k_target, &g);
  REQUIRE_EQ(g_rec.stops.size(), (size_t)3);
  CHECK_EQ(g_rec.stops[0].argb, 0xFF808080u);

  neui_stroke_style_t style{};
  g_rec = Recorder{};
  be->stroke_path_gradient(k_target, 2, &g, &style);
  REQUIRE_EQ(g_rec.stops.size(), (size_t)3);
  CHECK_EQ(g_rec.stops[0].argb, 0xFF808080u);
  CHECK(g_rec.style == &style);

  // More stops than fit inline.
  std::vector<neui_gradient_stop_t> many(40);
  for (size_t i = 0; i < many.size(); ++i)
    many[i] = { (float)i / 39.0f, 0xFFFFFFFFu };
  g.stops = many.data();
  g.stop_count = (uint32_t)many.size();
  g_rec = Recorder{};
  be->fill_rect_gradient(k_target, 0, 0, 1, 1, &g);
  REQUIRE_EQ(g_rec.stops.size(), many.size());
  CHECK_EQ(g_rec.stops[0].argb, 0xFF808080u);
  CHECK_EQ(g_rec.stops[39].argb, 0xFF808080u);
  CHECK_EQ(g_rec.stops[39].offset, 1.0f);
}

TEST_CASE("ColorDimBackend: draws on any other context pass through unscaled")
{
  neui_render_backend_t real = recording_backend();
  ColorDimBackend wrap(&real, k_target, 0xFF808080u);
  neui_render_backend_t* be = wrap.backend();
  g_rec = Recorder{};
  be->fill_rect(k_other, 0, 0, 1, 1, 0xFFFFFFFFu);
  CHECK_EQ(g_rec.argb, 0xFFFFFFFFu);
  CHECK(g_rec.ctx == k_other);
  be->draw_bitmap(k_other, nullptr, 0, 0, 0, 0, 0, 0, 1, 1, 0xFFFFFFFFu);
  CHECK_EQ(g_rec.tint, 0xFFFFFFFFu);
  const neui_gradient_stop_t stops[2] = { { 0.0f, 0xFFFFFFFFu }, { 1.0f, 0xFF00FF00u } };
  neui_gradient_t g{};
  g.stops = stops;
  g.stop_count = 2;
  be->fill_rect_gradient(k_other, 0, 0, 1, 1, &g);
  REQUIRE_EQ(g_rec.stops.size(), (size_t)2);
  CHECK_EQ(g_rec.stops[1].argb, 0xFF00FF00u);
}

TEST_CASE("ColorDimBackend: the rest of the vtable is the real backend; GPU shader draws are withheld")
{
  neui_render_backend_t real = recording_backend();
  ColorDimBackend wrap(&real, k_target, 0xFF808080u);
  const neui_render_backend_t* be = wrap.backend();
  CHECK(be != &real);
  // Untouched slots are the real functions, not forwarders.
  CHECK(be->push_alpha == real.push_alpha);
  CHECK(be->begin_path == real.begin_path);
  CHECK(be->create_shader_program == real.create_shader_program);
  // A slot the real backend lacks stays missing.
  CHECK(be->font_metrics == nullptr);
  CHECK(be->measure_text == nullptr);
  // Colour slots are wrapped.
  CHECK(be->fill_rect != real.fill_rect);
  // No GPU draws: the host then renders the shader on the CPU and draws the
  // image through draw_bitmap, which is tinted.
  CHECK(be->draw_shader_program == nullptr);
  CHECK(be->draw_shader_program_textured == nullptr);
}

TEST_CASE("ColorDimBackend: a wrapper without the real slot does not invent one")
{
  neui_render_backend_t real{};
  real.fill_rect = r_fill_rect;
  ColorDimBackend wrap(&real, k_target, 0xFF808080u);
  CHECK(wrap.backend()->fill_rect != nullptr);
  CHECK(wrap.backend()->draw_text == nullptr);
  CHECK(wrap.backend()->fill_rect_gradient == nullptr);
  CHECK(wrap.backend()->draw_bitmap == nullptr);
}

TEST_CASE("ColorDimBackend: a nested wrapper hands back to the outer one")
{
  neui_render_backend_t real = recording_backend();
  ColorDimBackend outer(&real, k_target, 0xFF808080u);
  {
    ColorDimBackend inner(&real, k_other, 0xFF000000u);
    g_rec = Recorder{};
    inner.backend()->fill_rect(k_other, 0, 0, 1, 1, 0xFFFFFFFFu);
    CHECK_EQ(g_rec.argb, 0xFF000000u);
  }
  g_rec = Recorder{};
  outer.backend()->fill_rect(k_target, 0, 0, 1, 1, 0xFFFFFFFFu);
  CHECK_EQ(g_rec.argb, 0xFF808080u);
  outer.backend()->fill_rect(k_other, 0, 0, 1, 1, 0xFFFFFFFFu);
  CHECK_EQ(g_rec.argb, 0xFFFFFFFFu);
}
