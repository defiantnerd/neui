// Disabled-dim acceptance harness (NEUI_ATTR_DISABLED_DIM), macOS, crossplatform
// host + CoreGraphics backend.
//
// The Tier-1 suite covers the decode, the decision and the colour-scaling
// backend wrapper; what only a real backend can answer is whether the PIXELS of
// a disabled widget follow them. This realizes a real window, snapshots it with
// -cacheDisplayInRect: and reads the pixels back.
//
// The widget under test is a CUSTOMDRAW whose compound has five solid cells: a
// rect layer at z = -1 (painted by paint()), a rect layer at z = 0 (painted by
// paint_after_children(), which the 50 % dim used to miss), a bitmap layer at
// z = 0 (the dim reaches it through draw_bitmap's tint), a gradient rect at
// z = 0 and a shader layer at z = 0 (its CPU image is a bitmap, tinted too). It
// has an enabled CUSTOMDRAW child, and the frame has an untouched neighbour.
//
//   1. FULL FRAMES   enabled = full colour; disabled with the attribute unset =
//                    every cell halfway to the background; 0xFF808080 = the
//                    colour halved and opaque; 0x80808080 = both; 0xFFFFFFFF =
//                    unchanged while disabled. The child keeps its colour (the
//                    dim is per widget, not per subtree).
//   2. PARTIAL       after a full frame, a live set_enabled / attribute write
//                    repaints only the widget's damage rect: the neighbour gets
//                    no WIDGET_PREUPDATE (it was culled) while the widget gets
//                    one, the widget's rect drawn on its own (a partial frame)
//                    shows the dim, and - where screencapture is allowed - so do
//                    the pixels the window server holds.
//
// Needs a window server (it realizes a real NSWindow), so it is built but not
// ctest-registered; run ./tests/<config>/neui_disabled_dim_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "ok:  " : "FAIL:", what);
  if (!ok) ++g_fail;
}

// WIDGET_PREUPDATE per widget id: a widget gets one each time a frame paints
// it, and none when a partial frame culls it.
uint32_t g_watch[2] = { 0, 0 };
int      g_preupdates[2] = { 0, 0 };

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_WIDGET_PREUPDATE)
    for (int i = 0; i < 2; ++i)
      if (ev->data.preupdate.widget.id == g_watch[i]) ++g_preupdates[i];
  return false;
}
neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* NEUI_ABI iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

void pump(double s = 0.15)
{
  [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:s]];
}

// ---- colours ---------------------------------------------------------------

struct Px { int r, g, b; };

Px argb_px(uint32_t c) { return { (int)((c >> 16) & 0xFF), (int)((c >> 8) & 0xFF), (int)(c & 0xFF) }; }
int dist(Px a, Px b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b); }

// What the dim does to an opaque colour drawn over `bg`: R, G and B scaled by
// byte / 256 (0xFF = 1), then composited at the A factor.
uint32_t dimmed(uint32_t c, uint32_t dim, uint32_t bg)
{
  auto units = [](uint32_t b) { b &= 0xFF; return b == 0xFF ? 256u : b; };
  const double a = units(dim >> 24) / 256.0;
  auto ch = [&](int sh) {
    const double col = (double)(((((c >> sh) & 0xFF) * units(dim >> sh)) + 128u) >> 8);
    return (uint32_t)std::lround(col * a + ((bg >> sh) & 0xFF) * (1.0 - a));
  };
  return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// ---- pixels ----------------------------------------------------------------

struct Shot {
  NSBitmapImageRep* rep = nil;
  float scale = 1.0f;
  float ox = 0, oy = 0;    // logical origin of the shot in the view
  int   rows_above = 0;    // device rows above the content (title bar, in a window capture)

  Px dev(int dx, int dy) const
  {
    const uint8_t* d = rep.bitmapData;
    const NSInteger bpp = rep.bitsPerPixel / 8, bpr = rep.bytesPerRow;
    const uint8_t* p = d + (dy + rows_above) * bpr + dx * bpp;
    if (rep.bitmapFormat & NSBitmapFormatAlphaFirst) return { p[1], p[2], p[3] };
    return { p[0], p[1], p[2] };
  }
  // The pixel at view point (lx, ly).
  Px at(float lx, float ly) const
  {
    return dev((int)std::floor((lx - ox + 0.5f) * scale), (int)std::floor((ly - oy + 0.5f) * scale));
  }
};

// The view drawn into a bitmap, `r` only. A rect smaller than the view reaches
// drawRect: as such, so the frame paints it as a partial frame.
Shot capture(NSView* v, NSRect r)
{
  Shot s;
  s.rep = [v bitmapImageRepForCachingDisplayInRect:r];
  [v cacheDisplayInRect:r toBitmapImageRep:s.rep];
  s.scale = (float)s.rep.pixelsWide / (float)r.size.width;
  s.ox = (float)r.origin.x;
  s.oy = (float)r.origin.y;
  return s;
}
Shot capture(NSView* v) { return capture(v, v.bounds); }

// What the window server holds for `win` (the window alone, by id), through
// screencapture: the pixels the frame left on screen, so a live change that
// only queued a partial repaint shows here only if its damage was right. An
// empty Shot when screencapture is unavailable (no screen-capture permission).
Shot capture_onscreen(NSWindow* win)
{
  Shot s;
  [win displayIfNeeded];
  pump(0.3);
  NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"neui_disabled_dim_%d.png", (int)getpid()]];
  NSString* cmd = [NSString stringWithFormat:@"/usr/sbin/screencapture -x -o -l %ld '%@' 2>/dev/null",
                   (long)win.windowNumber, path];
  const int rc = std::system(cmd.UTF8String);
  NSBitmapImageRep* rep = nil;
  if (rc == 0) {
    NSImageRep* any = [NSBitmapImageRep imageRepWithContentsOfFile:path];
    if ([any isKindOfClass:[NSBitmapImageRep class]]) rep = (NSBitmapImageRep*)any;
  }
  [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
  if (!rep) return s;
  s.rep = rep;
  s.scale = (float)rep.pixelsWide / (float)win.frame.size.width;
  const NSRect content = [win contentRectForFrameRect:win.frame];
  s.rows_above = (int)std::lround((win.frame.size.height - content.size.height) * s.scale);
  return s;
}

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];
    neui_init();

    // Colours chosen so "half opacity" ((c + bg) / 2) and "half colour" (c / 2)
    // are far apart in every cell.
    const uint32_t BG = 0xFF3060A0;
    const uint32_t RECT_BELOW = 0xFFF0C040, RECT_ABOVE = 0xFF40E0A0,
                   BITMAP = 0xFFE05080, GRADIENT = 0xFFA0A0F0, SHADER = 0xFF00CC66,
                   CHILD = 0xFF80F040, NEIGHBOUR = 0xFFC8C8C8;

    neui_api_t* api = neui_get_api("neui.host.crossplatform");
    if (!api) { std::printf("FAIL: no crossplatform host\n"); return 1; }
    neui_session_t sess = api->create_session(&g_client, nullptr);
    auto* w  = (neui_widget_api_t*)  api->get_interface(sess, NEUI_API_WIDGETS);
    auto* at = (neui_attr_api_t*)    api->get_interface(sess, NEUI_API_ATTRS);
    auto* as = (neui_asset_api_t*)   api->get_interface(sess, NEUI_API_ASSETS);
    auto* cp = (neui_compound_api_t*)api->get_interface(sess, NEUI_API_COMPOUND);
    if (!w || !at || !as || !cp) { std::printf("FAIL: missing interfaces\n"); return 1; }

    neui_widget_t frame = w->create(sess, widget_none, NEUI_W_APPWINDOW, 80, 80, 540, 200, nullptr);
    at->set_int(sess, frame, NEUI_ATTR_BACKGROUND, (int32_t)BG);

    // ---- the widget under test ------------------------------------------------
    const NSRect rWidget = NSMakeRect(20, 20, 500, 60);
    neui_widget_t cd = w->create(sess, frame, NEUI_W_CUSTOMDRAW, (int)rWidget.origin.x,
                                 (int)rWidget.origin.y, (int)rWidget.size.width,
                                 (int)rWidget.size.height, nullptr);
    neui_asset_t comp = as->create_compound(sess);
    auto cell = [&](neui_compound_layer_kind_t kind, int z, int x) {
      neui_compound_layer_t l = cp->add_layer(sess, comp, kind, z);
      cp->set_anchor(sess, comp, l, NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT);
      cp->set_int(sess, comp, l, "offset_x", x);
      cp->set_int(sess, comp, l, "width", 70);
      cp->set_int(sess, comp, l, "height", 60);
      return l;
    };
    neui_compound_layer_t below = cell(NEUI_COMPOUND_LAYER_RECT, -1, 0);
    cp->set_int(sess, comp, below, "fill_color", (int)RECT_BELOW);
    neui_compound_layer_t above = cell(NEUI_COMPOUND_LAYER_RECT, 0, 80);
    cp->set_int(sess, comp, above, "fill_color", (int)RECT_ABOVE);
    // A solid opaque bitmap (BGRA, premultiplied).
    std::vector<uint8_t> px(70 * 60 * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i + 0] = BITMAP & 0xFF;
      px[i + 1] = (BITMAP >> 8) & 0xFF;
      px[i + 2] = (BITMAP >> 16) & 0xFF;
      px[i + 3] = 0xFF;
    }
    neui_asset_t bmp = as->create_bitmap(sess, 70, 60, px.data(), 1.0f);
    check(bmp.id != asset_none.id, "bitmap asset created");
    neui_compound_layer_t bitmap = cell(NEUI_COMPOUND_LAYER_ASSET, 0, 160);
    cp->set_asset(sess, comp, bitmap, "asset", bmp);
    neui_compound_layer_t grad = cell(NEUI_COMPOUND_LAYER_RECT, 0, 240);
    const neui_gradient_stop_t stops[2] = { { 0.0f, GRADIENT }, { 1.0f, GRADIENT } };
    neui_gradient_t g{};
    g.kind = NEUI_GRADIENT_LINEAR;
    g.stops = stops;
    g.stop_count = 2;
    g.end_x = 1.0f;
    cp->set_gradient(sess, comp, grad, &g);
    // A shader layer: its image is made on the CPU and drawn as a bitmap, so the
    // dim reaches it through the tint as well. Straight 0, 0.8, 0.4 = 00CC66.
    auto* sh = (neui_shader_api_t*)api->get_interface(sess, NEUI_API_SHADER);
    neui_asset_t shader = asset_none;
    if (sh) {
      const char* src = "vec4 main(vec2 p) { return vec4(0.0, 0.8, 0.4, 1.0); }";
      char err[256] = { 0 };
      shader = sh->create(sess, src, (uint32_t)std::strlen(src), "solid.nsl", err, sizeof(err));
      if (shader.id == asset_none.id) std::printf("shader: %s\n", err);
      check(shader.id != asset_none.id, "shader asset created");
      neui_compound_layer_t shl = cell(NEUI_COMPOUND_LAYER_SHADER, 0, 320);
      cp->set_asset(sess, comp, shl, "shader", shader);
    } else {
      std::printf("skip: no NEUI_API_SHADER, the shader cell is not checked\n");
    }
    w->set_asset(sess, cd, comp);

    // An enabled child, beside the layers (the z >= 0 layers paint over children).
    neui_widget_t child = w->create(sess, cd, NEUI_W_CUSTOMDRAW, 410, 0, 80, 60, nullptr);
    neui_asset_t child_comp = as->create_compound(sess);
    neui_compound_layer_t cl = cp->add_layer(sess, child_comp, NEUI_COMPOUND_LAYER_RECT, 0);
    cp->set_int(sess, child_comp, cl, "fill_color", (int)CHILD);
    w->set_asset(sess, child, child_comp);

    // An untouched neighbour.
    const NSRect rNeighbour = NSMakeRect(20, 110, 500, 60);
    neui_widget_t nb = w->create(sess, frame, NEUI_W_CUSTOMDRAW, (int)rNeighbour.origin.x,
                                 (int)rNeighbour.origin.y, (int)rNeighbour.size.width,
                                 (int)rNeighbour.size.height, nullptr);
    neui_asset_t nb_comp = as->create_compound(sess);
    neui_compound_layer_t nl = cp->add_layer(sess, nb_comp, NEUI_COMPOUND_LAYER_RECT, 0);
    cp->set_int(sess, nb_comp, nl, "fill_color", (int)NEIGHBOUR);
    w->set_asset(sess, nb, nb_comp);

    g_watch[0] = cd.id;
    g_watch[1] = nb.id;

    w->show(sess, frame);
    pump();
    NSWindow* win = (__bridge NSWindow*)w->get_native_handle(sess, frame);
    check(win != nil, "native window exists");
    if (!win) return 1;
    NSView* view = win.contentView;

    // Cell centres, in view points.
    struct Cell { const char* name; float x; uint32_t color; };
    const float cy = (float)(rWidget.origin.y + 30);
    std::vector<Cell> cells = {
      { "z = -1 rect",       (float)rWidget.origin.x + 35,  RECT_BELOW },
      { "z = 0 rect",        (float)rWidget.origin.x + 115, RECT_ABOVE },
      { "z = 0 bitmap",      (float)rWidget.origin.x + 195, BITMAP },
      { "z = 0 gradient",    (float)rWidget.origin.x + 275, GRADIENT },
    };
    if (shader.id != asset_none.id)
      cells.push_back({ "z = 0 shader", (float)rWidget.origin.x + 355, SHADER });
    const float child_x = (float)rWidget.origin.x + 450;
    const float nb_x = (float)rNeighbour.origin.x + 250, nb_y = (float)rNeighbour.origin.y + 30;
    const int tol = 9;

    // Every cell of the widget at what `dim` makes of its colour; the child and
    // the neighbour at theirs. `dim` = 0xFFFFFFFF expects full colour.
    auto expect = [&](const Shot& s, uint32_t dim, const char* label, bool with_neighbour) {
      for (const Cell& c : cells) {
        const uint32_t want = dimmed(c.color, dim, BG);
        const Px got = s.at(c.x, cy);
        const bool ok = dist(got, argb_px(want)) <= tol;
        char what[200];
        std::snprintf(what, sizeof(what), "%s: %s is %06X (got %02X%02X%02X)", label, c.name,
                      want & 0xFFFFFF, got.r, got.g, got.b);
        check(ok, what);
      }
      char what[200];
      const Px got = s.at(child_x, cy);
      std::snprintf(what, sizeof(what), "%s: the enabled child keeps its colour (got %02X%02X%02X)",
                    label, got.r, got.g, got.b);
      check(dist(got, argb_px(CHILD)) <= tol, what);
      if (with_neighbour) {
        std::snprintf(what, sizeof(what), "%s: the neighbour is untouched", label);
        check(dist(s.at(nb_x, nb_y), argb_px(NEIGHBOUR)) <= tol, what);
      }
    };

    // ---- 1. full frames ---------------------------------------------------------
    std::printf("-- full frames\n");
    Shot s = capture(view);
    std::printf("backing scale %.1f\n", s.scale);
    expect(s, 0xFFFFFFFFu, "enabled", true);

    w->set_enabled(sess, cd, false);
    expect(capture(view), 0x80FFFFFFu, "disabled, unset (default 0x80FFFFFF)", true);

    at->set_int(sess, cd, NEUI_ATTR_DISABLED_DIM, (int32_t)0xFF808080u);
    expect(capture(view), 0xFF808080u, "disabled, 0xFF808080 (half colour, opaque)", true);

    at->set_int(sess, cd, NEUI_ATTR_DISABLED_DIM, (int32_t)0x80808080u);
    expect(capture(view), 0x80808080u, "disabled, 0x80808080 (half colour, half opacity)", true);

    at->set_int(sess, cd, NEUI_ATTR_DISABLED_DIM, (int32_t)0xFFFFFFFFu);
    expect(capture(view), 0xFFFFFFFFu, "disabled, 0xFFFFFFFF (no dim)", true);

    at->set_int(sess, cd, NEUI_ATTR_DISABLED_DIM, (int32_t)0xFF808080u);
    w->set_enabled(sess, cd, true);
    expect(capture(view), 0xFFFFFFFFu, "enabled, 0xFF808080 set (enabled is never dimmed)", true);
    at->remove(sess, cd, NEUI_ATTR_DISABLED_DIM);

    // ---- 2. live changes after a full frame: the partial-repaint path -----------
    std::printf("-- live changes after a full frame (partial repaint)\n");
    const bool onscreen_ok = capture_onscreen(win).rep != nil;
    if (!onscreen_ok)
      std::printf("skip: screencapture unavailable, the window-server reads are not run\n");

    auto live = [&](const char* label, uint32_t want_dim, auto change) {
      // A full frame first.
      [view setNeedsDisplay:YES];
      [view displayIfNeeded];
      pump(0.05);
      g_preupdates[0] = g_preupdates[1] = 0;
      change();
      [view displayIfNeeded];   // AppKit draws only what was invalidated
      pump(0.05);
      char what[200];
      std::snprintf(what, sizeof(what),
                    "%s: repainted through a partial frame (widget %d, neighbour %d WIDGET_PREUPDATE)",
                    label, g_preupdates[0], g_preupdates[1]);
      check(g_preupdates[0] >= 1 && g_preupdates[1] == 0, what);

      // The widget's rect alone, which drawRect: gets as a partial frame.
      g_preupdates[0] = g_preupdates[1] = 0;
      Shot part = capture(view, rWidget);
      std::snprintf(what, sizeof(what), "%s: the widget's rect drawn alone is a partial frame", label);
      check(g_preupdates[0] >= 1 && g_preupdates[1] == 0, what);
      char lbl[160];
      std::snprintf(lbl, sizeof(lbl), "%s, partial", label);
      expect(part, want_dim, lbl, false);

      if (onscreen_ok) {
        Shot screen = capture_onscreen(win);
        std::snprintf(lbl, sizeof(lbl), "%s, on screen", label);
        if (screen.rep) expect(screen, want_dim, lbl, true);
        else check(false, "on-screen capture");
      }
    };

    live("live set_enabled(false)", 0x80FFFFFFu, [&] { w->set_enabled(sess, cd, false); });
    live("live dim 0xFF808080", 0xFF808080u,
         [&] { at->set_int(sess, cd, NEUI_ATTR_DISABLED_DIM, (int32_t)0xFF808080u); });
    live("live dim 0xFFFFFFFF", 0xFFFFFFFFu,
         [&] { at->set_int(sess, cd, NEUI_ATTR_DISABLED_DIM, (int32_t)0xFFFFFFFFu); });
    live("live remove(dim)", 0x80FFFFFFu,
         [&] { at->remove(sess, cd, NEUI_ATTR_DISABLED_DIM); });
    live("live set_enabled(true)", 0xFFFFFFFFu, [&] { w->set_enabled(sess, cd, true); });

    w->set_asset(sess, cd, asset_none);
    as->destroy(sess, bmp);
    if (shader.id != asset_none.id) as->destroy(sess, shader);
    api->destroy(sess);
  }
  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
