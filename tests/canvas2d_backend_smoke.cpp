// Acceptance harness for the Canvas 2D web backend (plans/wasm-host.md) - the
// web twin of cairo_backend_smoke.cpp, with the same checks plus the ones the
// canvas makes interesting: the premultiplied round trip through ImageData,
// the bitmap tint and gradients.
//
// Needs a browser (Canvas 2D is a DOM API), so it is built as a page and run
// headless by tests/web/run_page.mjs, which waits for the "SMOKE OK" /
// "SMOKE FAILED" line. Not registered with ctest (node has no canvas).

#include <neui/neui.h>
#include "../backends/canvas2d/canvas2d_backend.h"

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
  neui_render_backend_t* be = neui_canvas2d_backend::get_backend();
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

  // measure_text must return a plausible advance width (the browser resolved
  // the system UI font). "Hello" at 14px is well under the surface width.
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

  // --- Partial repaint (set_frame_damage). ------------------------------------
  // The surface now holds a complete frame (Frame C), so damage is accepted:
  // the next frame clears + paints only inside it, and every pixel outside
  // keeps Frame C's content (the blue dash at (6,40)).
  {
    const float dmg[4] = { 0.0f, 0.0f, 16.0f, 16.0f };
    check(be->set_frame_damage && be->set_frame_damage(ctx, dmg, 1),
          "damage: accepted once the surface holds a frame");
    be->begin_frame(ctx, 0xFFFF0000u);                 // red clear - damage only
    be->fill_rect(ctx, 4, 4, 56, 56, 0xFF00FF00u);     // green - clipped to damage
    be->end_frame(ctx);
    be->read_pixels_bgra(ctx, out.data());
    const uint8_t* inside  = px(out, W, 8, 8);
    const uint8_t* edge    = px(out, W, 1, 1);         // in damage, outside the fill
    const uint8_t* kept    = px(out, W, 6, 40);        // Frame C dash
    const uint8_t* beyond  = px(out, W, 30, 30);       // under the fill, outside damage
    check(inside[1] > 200 && inside[2] == 0, "damage: inside is repainted");
    check(edge[2] > 200 && edge[1] == 0,     "damage: clear is clipped to the damage");
    check(kept[0] > 200 && kept[1] == 0,     "damage: outside keeps the previous frame");
    check(beyond[1] == 0,                    "damage: draws outside the damage are clipped");

    // The damage is consumed by that one frame - the next frame is full.
    be->begin_frame(ctx, 0xFF000000u);
    be->end_frame(ctx);
    be->read_pixels_bgra(ctx, out.data());
    check(px(out, W, 6, 40)[0] == 0, "damage: consumed by one frame (next is full)");

    // count 0 = full frame, and reports false (nothing partial to cull for).
    check(!be->set_frame_damage(ctx, nullptr, 0), "damage: count 0 reports a full frame");
  }
  {
    // A fresh surface has no previous frame to keep - damage is refused, so
    // the host knows not to cull.
    neui_render_ctx_t fresh = be->create_offscreen_context(W, H, 1.0f);
    const float dmg[4] = { 0.0f, 0.0f, 8.0f, 8.0f };
    check(fresh && be->set_frame_damage && !be->set_frame_damage(fresh, dmg, 1),
          "damage: refused before the surface holds a frame");
    if (fresh) be->destroy_context(fresh);
  }

  // --- The path survives other draws (renderer.h: valid until begin_path). --
  // fill_rect, push_clip and the like make their own geometry; they must not
  // fill or clip to the path, nor drop it.
  be->begin_frame(ctx, 0xFF000000u);
  be->begin_path(ctx);
  be->move_to(ctx, 8, 8);
  be->line_to(ctx, 40, 8);
  be->line_to(ctx, 8, 40);                             // an open triangle
  be->stroke_path(ctx, 2.0f, 0xFF0000FFu);             // blue, stroked only
  be->fill_rect(ctx, 48, 48, 8, 8, 0xFF00FF00u);       // green, elsewhere
  be->end_frame(ctx);
  be->read_pixels_bgra(ctx, out.data());
  check(px(out, W, 16, 16)[1] == 0, "path: a later fill_rect does not fill the path");
  check(px(out, W, 52, 52)[1] > 200, "path: the fill_rect itself draws");

  be->begin_frame(ctx, 0xFF000000u);
  be->begin_path(ctx);
  be->move_to(ctx, 8, 8);
  be->line_to(ctx, 40, 8);
  be->line_to(ctx, 8, 40);
  be->stroke_path(ctx, 2.0f, 0xFF0000FFu);
  be->push_clip(ctx, 48, 48, 8, 8);
  be->fill_rect(ctx, 0, 0, 64, 64, 0xFF00FF00u);       // green, clipped
  be->pop_clip(ctx);
  be->fill_path(ctx, 0xFFFF0000u);                     // red: the path is still there
  be->end_frame(ctx);
  be->read_pixels_bgra(ctx, out.data());
  {
    const uint8_t* in_path = px(out, W, 16, 16);
    check(in_path[2] > 200 && in_path[1] == 0, "path: kept across push_clip, then filled");
    check(px(out, W, 52, 52)[1] > 200, "path: the clip is the rect");
    check(px(out, W, 30, 30)[1] == 0 && px(out, W, 30, 30)[2] == 0,
          "path: the clip does not take in the path");
  }


  // --- Canvas specifics. ------------------------------------------------------
  // Premultiplied BGRA -> straight RGBA (ImageData) -> premultiplied again:
  // exact for opaque pixels, within a step or two for translucent ones.
  {
    std::vector<uint8_t> half(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
      half[i*4+0] = 64; half[i*4+1] = 32; half[i*4+2] = 0; half[i*4+3] = 128;  // premult
    }
    void* hb = be->create_bitmap(ctx, 4, 4, half.data(), 1.0f);
    be->begin_frame(ctx, 0x00000000u);                   // transparent clear
    if (hb) be->draw_bitmap(ctx, hb, 0, 0, 0, 0, 0, 0, 16, 16, 0xFFFFFFFFu);
    be->end_frame(ctx);
    be->read_pixels_bgra(ctx, out.data());
    const uint8_t* q = px(out, W, 8, 8);
    std::printf("round trip: %d %d %d %d\n", q[0], q[1], q[2], q[3]);
    auto near = [](int a, int b) { return a - b <= 2 && b - a <= 2; };
    check(near(q[0], 64) && near(q[1], 32) && q[2] == 0 && q[3] == 128,
          "premultiplied round trip within 2 steps");
    check(px(out, W, 40, 40)[3] == 0, "transparent clear stays transparent");

    // Tint: a white bitmap tinted red draws red, alpha kept.
    std::vector<uint8_t> white(4 * 4 * 4, 255);
    void* wb = be->create_bitmap(ctx, 4, 4, white.data(), 1.0f);
    be->begin_frame(ctx, 0xFF000000u);
    if (wb) be->draw_bitmap(ctx, wb, 0, 0, 0, 0, 0, 0, 16, 16, 0xFFFF0000u);
    be->end_frame(ctx);
    be->read_pixels_bgra(ctx, out.data());
    const uint8_t* t = px(out, W, 8, 8);
    check(t[2] > 240 && t[1] < 16 && t[0] < 16, "tint multiplies the bitmap (white -> red)");
    if (hb) be->destroy_bitmap(ctx, hb);
    if (wb) be->destroy_bitmap(ctx, wb);
  }
  {
    // Linear gradient, black -> white left to right: dark left, light right.
    const neui_gradient_stop_t stops[2] = { { 0.0f, 0xFF000000u }, { 1.0f, 0xFFFFFFFFu } };
    neui_gradient_t g;
    std::memset(&g, 0, sizeof(g));
    g.kind = NEUI_GRADIENT_LINEAR; g.stops = stops; g.stop_count = 2;
    g.start_x = 0; g.start_y = 0; g.end_x = 64; g.end_y = 0;
    be->begin_frame(ctx, 0xFF000000u);
    be->fill_rect_gradient(ctx, 0, 0, 64, 64, &g);
    be->end_frame(ctx);
    be->read_pixels_bgra(ctx, out.data());
    check(px(out, W, 4, 32)[1] < 40 && px(out, W, 60, 32)[1] > 215,
          "linear gradient runs dark -> light");
  }
  {
    // A scaled off-screen context: logical draws land at scale x device px.
    neui_render_ctx_t hi = be->create_offscreen_context(W, H, 2.0f);
    check(hi != nullptr, "create_offscreen_context at scale 2");
    if (hi) {
      be->begin_frame(hi, 0xFF000000u);
      be->fill_rect(hi, 8, 8, 8, 8, 0xFFFF0000u);         // device 16..32
      float m[6] = {0, 0, 0, 0, 0, 0};
      be->get_device_transform(hi, m);
      be->end_frame(hi);
      check(m[0] == 2.0f && m[3] == 2.0f && m[4] == 0.0f, "device transform carries the scale");
      be->read_pixels_bgra(hi, out.data());
      check(px(out, W, 24, 24)[2] == 255 && px(out, W, 12, 12)[2] == 0,
            "scale 2: a logical rect covers twice the device pixels");
      be->destroy_context(hi);
    }
  }

  be->destroy_context(ctx);

  std::printf(g_failures ? "\nSMOKE FAILED (%d)\n" : "\nSMOKE OK\n", g_failures);
  return g_failures ? 1 : 0;
}
