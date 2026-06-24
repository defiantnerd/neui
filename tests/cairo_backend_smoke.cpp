// Phase 1 acceptance harness for the Cairo software backend.
//
// Exercises the offscreen render path WITHOUT X11: create an offscreen
// context, draw rect + arc(path) + bitmap + text, read the pixels back, and
// assert a known centre pixel matches the fill colour plus a sane
// measure_text result. Standalone (not part of the header-only neui_tests,
// which deliberately links no backend); registered with ctest on Linux.

#include <neui/neui.h>
#include "../backends/cairo/cairo_backend.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int g_failures = 0;

void check(bool cond, const char* what)
{
  if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
  else       { std::printf("ok:   %s\n", what); }
}

// BGRA8 (premultiplied, top-down) pixel accessors on the read-back buffer.
inline const uint8_t* px(const std::vector<uint8_t>& buf, int w, int x, int y)
{
  return buf.data() + (static_cast<size_t>(y) * w + x) * 4;
}
} // namespace

int main()
{
  neui_render_backend_t* be = neui_cairo_backend::get_backend();
  check(be != nullptr, "get_backend() non-null");
  if (!be) return 1;

  const uint32_t W = 64, H = 64;
  neui_render_ctx_t ctx = be->create_offscreen_context(W, H, 1.0f);
  check(ctx != nullptr, "create_offscreen_context");
  if (!ctx) return 1;

  // --- Draw a frame: black clear, centred red square, an arc, a bitmap. -----
  be->begin_frame(ctx, 0xFF000000u);                  // opaque black
  be->fill_rect(ctx, 16, 16, 32, 32, 0xFFFF0000u);    // opaque red square

  // Path: a filled green quarter-arc wedge in the top-left corner.
  be->begin_path(ctx);
  be->move_to(ctx, 4, 4);
  be->arc(ctx, 4, 4, 6, 0.0f, 1.5707963f);            // 0..pi/2 (clockwise, Y-down)
  be->close_path(ctx);
  be->fill_path(ctx, 0xFF00FF00u);                    // green

  // Bitmap: a 4x4 solid-blue BGRA tile drawn near the bottom-right.
  std::vector<uint8_t> tile(4 * 4 * 4);
  for (int i = 0; i < 4 * 4; ++i) {
    tile[i*4+0] = 255;  // B
    tile[i*4+1] = 0;    // G
    tile[i*4+2] = 0;    // R
    tile[i*4+3] = 255;  // A
  }
  void* bmp = be->create_bitmap(ctx, 4, 4, tile.data(), 1.0f);
  check(bmp != nullptr, "create_bitmap");
  if (bmp) {
    be->draw_bitmap(ctx, bmp, 0, 0, 0, 0, 52, 52, 8, 8, 0xFFFFFFFFu);
  }

  // Text: just exercise the path (no crash); colour correctness is hard to
  // assert pixel-exact, but measure_text below validates font resolution.
  be->draw_text(ctx, 2, 40, 40, 16, "Ag", 12.0f, 0xFFFFFFFFu);

  be->end_frame(ctx);

  // --- Read back + assert. --------------------------------------------------
  std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4, 0xAB);
  bool ok = be->read_pixels_bgra(ctx, out.data());
  check(ok, "read_pixels_bgra");

  // Centre of the red square (32,32): B=0, G=0, R=255, A=255.
  const uint8_t* c = px(out, W, 32, 32);
  check(c[0] == 0 && c[1] == 0 && c[2] == 255 && c[3] == 255,
        "centre pixel is opaque red");

  // A clear-colour corner well away from any draw (60,30): black.
  const uint8_t* k = px(out, W, 60, 30);
  check(k[0] == 0 && k[1] == 0 && k[2] == 0 && k[3] == 255,
        "background pixel is opaque black");

  // Bitmap landed near (55,55): blue (B=255).
  const uint8_t* b = px(out, W, 55, 55);
  check(b[2] == 0 && b[0] == 255, "bitmap pixel is blue");

  // measure_text must return a plausible advance width (font resolution via
  // Fontconfig worked). "Hello" at 14px is well under the surface width.
  float adv = be->measure_text(ctx, "Hello", -1, 14.0f);
  std::printf("measure_text(\"Hello\",14) = %.2f\n", adv);
  check(adv > 0.0f && adv < 200.0f, "measure_text plausible (>0, <200)");

  if (bmp) be->destroy_bitmap(ctx, bmp);

  // --- Path-API extensions: fill-rule, Bézier curves, styled strokes. -------
  const float TWO_PI = 6.28318530717958647692f;

  // Frame A: even-odd ring - two concentric circles cancel to a hole.
  be->begin_frame(ctx, 0xFF000000u);
  be->begin_path(ctx);
  be->set_fill_rule(ctx, NEUI_FILL_RULE_EVENODD);
  be->arc(ctx, 32, 32, 20, 0.0f, TWO_PI);   // outer
  be->arc(ctx, 32, 32, 10, 0.0f, TWO_PI);   // inner -> hole
  be->fill_path(ctx, 0xFF00FF00u);          // green
  be->end_frame(ctx);
  be->read_pixels_bgra(ctx, out.data());
  {
    const uint8_t* hole = px(out, W, 32, 32);          // inside inner radius
    check(hole[1] == 0, "even-odd: ring centre is a hole (not green)");
    const uint8_t* ring = px(out, W, 32, 17);          // between r=10 and r=20
    check(ring[1] > 200, "even-odd: ring band is green");
  }

  // Frame B: cubic + quadratic Bézier closed shape filled - centre is inside.
  be->begin_frame(ctx, 0xFF000000u);
  be->begin_path(ctx);
  be->move_to(ctx, 10, 32);
  be->cubic_to(ctx, 10, 8, 54, 8, 54, 32);   // bulging top (cubic)
  be->quad_to (ctx, 32, 56, 10, 32);         // bottom (quad)
  be->close_path(ctx);
  be->fill_path(ctx, 0xFF00FF00u);
  be->end_frame(ctx);
  be->read_pixels_bgra(ctx, out.data());
  {
    const uint8_t* mid = px(out, W, 32, 32);
    check(mid[1] > 200, "cubic+quad: filled shape covers its interior");
  }

  // Frame C: dashed horizontal stroke. Dash [6 on from x=4, then 12 off]; a
  // butt cap keeps dash ends flush so the gap is unambiguous.
  be->begin_frame(ctx, 0xFF000000u);
  be->begin_path(ctx);
  be->move_to(ctx, 4, 40);
  be->line_to(ctx, 60, 40);
  {
    const float dashes[2] = { 6.0f, 12.0f };
    neui_stroke_style_t style;
    std::memset(&style, 0, sizeof(style));
    style.cap = NEUI_LINE_CAP_BUTT; style.join = NEUI_LINE_JOIN_ROUND;
    style.miter_limit = 4.0f; style.dash_array = dashes; style.dash_count = 2;
    be->stroke_path_styled(ctx, 4.0f, 0xFF0000FFu, &style);   // blue
  }
  be->end_frame(ctx);
  be->read_pixels_bgra(ctx, out.data());
  {
    const uint8_t* on  = px(out, W, 6,  40);   // within first dash [4,10)
    const uint8_t* off = px(out, W, 15, 40);   // within the gap    [10,22)
    check(on[0]  > 200, "dash: pixel on a dash run is blue");
    check(off[0] == 0,  "dash: pixel in a gap is background");
  }

  be->destroy_context(ctx);

  std::printf(g_failures ? "\nSMOKE FAILED (%d)\n" : "\nSMOKE OK\n", g_failures);
  return g_failures ? 1 : 0;
}
