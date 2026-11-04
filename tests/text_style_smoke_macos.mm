// Text-field styling acceptance harness (NEUI_ATTR_TEXT_COLOR / _BORDER_COLOR /
// _BORDER_FOCUS_COLOR / _SELECTION_COLOR / _BACKGROUND and NEUI_ATTR_FONT_FAMILY
// on INPUTBOX + MULTILINE), macOS.
//
// The Tier-1 suite covers the attribute resolution; what only a real backend can
// answer is whether the PIXELS follow it, and whether the caret and the glyphs
// agree. This realizes real windows, snapshots their content with
// -cacheDisplayInRect:, and reads the pixels back:
//
//   crossplatform host + CoreGraphics backend
//     1. COLOURS   fill, unfocused border, text colour, focused border and the
//                  selection tint are what the attrs say; an unstyled field next
//                  to it is not.
//     2. FONT      a named family changes the glyph extent (it is drawn in that
//                  family), a weight selects the heavier face, an unknown family
//                  falls back to the system font, and
//                  - when NEUI_TEST_FONT_FILE names a .ttf/.otf - a family the
//                  client REGISTERED through the asset API resolves too.
//     3. CARET     Home then N x Right puts the caret after exactly N glyphs of a
//                  monospaced family (Courier New, 7.2 px advance at 12 px vs
//                  ~3.3 px for the system font's 'i'), i.e. measure_text and
//                  draw_text agree on the font. Shift+End paints the selection
//                  colour over the glyphs.
//     4. WRAP      a MULTILINE with soft wrap re-flows when the FAMILY changes at
//                  an unchanged size (the wrap cache is keyed on family and
//                  weight, not only size).
//     5. ON SCREEN the same attributes written LIVE after a full frame (so the
//        frame repaints only that widget's damage rect) show up in what the
//        window server holds - read back with screencapture, one window by id.
//   macOS native host
//     6. NSTextField / NSTextView take NEUI_ATTR_TEXT_COLOR and
//        NEUI_ATTR_BACKGROUND, and a live remove() puts the system look back.
//
// Needs a window server (it realizes real NSWindows), so it is built but not
// ctest-registered; run ./tests/<config>/neui_text_style_smoke_macos manually.
// Optional: NEUI_TEST_FONT_FILE=/path/to/font.ttf adds the registered-family check.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "ok:  " : "FAIL:", what);
  if (!ok) ++g_fail;
}

bool NEUI_ABI onevent(void*, neui_event_t*) { return false; }
neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* NEUI_ABI iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

void pump(double s = 0.15)
{
  [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:s]];
}

// ---- pixels ---------------------------------------------------------------

struct Px { int r, g, b; };

Px argb_px(uint32_t c) { return { (int)((c >> 16) & 0xFF), (int)((c >> 8) & 0xFF), (int)(c & 0xFF) }; }
int dist(Px a, Px b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b); }

struct Shot {
  NSBitmapImageRep* rep = nil;
  float scale = 1.0f;
  int   oy = 0;            // device rows above the content (title bar, in a window capture)

  // Device pixel at (dx, dy). Opaque window content: alpha is not looked at.
  Px dev(int dx, int dy) const
  {
    const uint8_t* d = rep.bitmapData;
    const NSInteger bpp = rep.bitsPerPixel / 8, bpr = rep.bytesPerRow;
    const uint8_t* p = d + (dy + oy) * bpr + dx * bpp;
    // Default NSBitmapImageRep layout is R,G,B(,A); AlphaFirst flips it.
    if (rep.bitmapFormat & NSBitmapFormatAlphaFirst) return { p[1], p[2], p[3] };
    return { p[0], p[1], p[2] };
  }
  // Logical-point sample (centre of the point's device pixel block).
  Px at(float lx, float ly) const
  { return dev((int)std::floor((lx + 0.5f) * scale), (int)std::floor((ly + 0.5f) * scale)); }
};

Shot capture(NSView* v)
{
  Shot s;
  s.rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
  [v cacheDisplayInRect:v.bounds toBitmapImageRep:s.rep];
  s.scale = (float)s.rep.pixelsWide / (float)v.bounds.size.width;
  return s;
}

// What the window server really holds for `win`: the window alone, by id (never
// the screen), through screencapture. Unlike capture() - which asks the view to
// draw EVERYTHING into a bitmap - this is the pixels the frame left on screen,
// so a live attribute change that only queued a partial repaint is visible here
// only if its damage rect was right. Returns an empty Shot (rep == nil) when
// screencapture is unavailable (no screen-capture permission).
Shot capture_onscreen(NSWindow* win)
{
  Shot s;
  [win displayIfNeeded];
  pump(0.3);
  NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"neui_text_style_%d.png", (int)getpid()]];
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
  s.oy = (int)std::lround((win.frame.size.height - content.size.height) * s.scale);
  return s;
}

struct Box { float x, y, w, h; };

// Is this pixel "ink": clearly nearer the text colour than the fill?
bool is_ink(Px p, Px fill, Px text) { return dist(p, text) + 30 < dist(p, fill) && dist(p, text) < 150; }

// Per device column of `r` (interior only: 2 logical px in from the border):
// how many device rows are ink.
std::vector<int> ink_rows_per_col(const Shot& s, Box r, uint32_t fill, uint32_t text,
                                  int* col0_out, int* rows_total)
{
  const int x0 = (int)std::ceil((r.x + 2) * s.scale), x1 = (int)std::floor((r.x + r.w - 2) * s.scale);
  const int y0 = (int)std::ceil((r.y + 1) * s.scale), y1 = (int)std::floor((r.y + r.h - 1) * s.scale);
  *col0_out = x0;
  *rows_total = y1 - y0;
  std::vector<int> out;
  const Px f = argb_px(fill), t = argb_px(text);
  for (int x = x0; x < x1; ++x) {
    int n = 0;
    for (int y = y0; y < y1; ++y) if (is_ink(s.dev(x, y), f, t)) ++n;
    out.push_back(n);
  }
  return out;
}

// Rightmost logical x (relative to the field's left edge) that holds any ink.
float ink_right(const Shot& s, Box r, uint32_t fill, uint32_t text)
{
  int c0, rows;
  auto cols = ink_rows_per_col(s, r, fill, text, &c0, &rows);
  int last = -1;
  for (size_t i = 0; i < cols.size(); ++i) if (cols[i] > 0) last = (int)i;
  return last < 0 ? -1.0f : ((c0 + last + 1) / s.scale) - r.x;
}

// Caret = a column whose ink covers >= 80% of the interior height (the glyphs of
// the test strings are far shorter). Returns the caret's left edge relative to
// the field (logical px) or -1; `glyph_runs_left` counts the glyph runs wholly
// to its left.
struct CaretInfo { float x = -1; int glyph_runs_left = 0; int glyph_runs = 0; };
CaretInfo find_caret(const Shot& s, Box r, uint32_t fill, uint32_t text)
{
  int c0, rows;
  auto cols = ink_rows_per_col(s, r, fill, text, &c0, &rows);
  CaretInfo ci;
  int caret_first = -1;
  for (size_t i = 0; i < cols.size(); ++i)
    if (cols[i] * 10 >= rows * 8) { caret_first = (int)i; break; }
  if (caret_first < 0) return ci;
  ci.x = (c0 + caret_first) / s.scale - r.x;
  bool in_run = false;
  for (size_t i = 0; i < cols.size(); ++i) {
    const bool is_caret = cols[i] * 10 >= rows * 8;
    const bool is_glyph = cols[i] > 0 && !is_caret;
    if (is_glyph && !in_run) {
      ++ci.glyph_runs;
      if ((int)i < caret_first) ++ci.glyph_runs_left;
    }
    in_run = is_glyph;
  }
  return ci;
}

// Number of pixels in the field interior within `tol` of `argb`.
int count_near(const Shot& s, Box r, uint32_t argb, int tol)
{
  const Px want = argb_px(argb);
  const int x0 = (int)std::ceil((r.x + 2) * s.scale), x1 = (int)std::floor((r.x + r.w - 2) * s.scale);
  const int y0 = (int)std::ceil((r.y + 1) * s.scale), y1 = (int)std::floor((r.y + r.h - 1) * s.scale);
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) if (dist(s.dev(x, y), want) <= tol) ++n;
  return n;
}

uint32_t blend(uint32_t over, uint32_t under);

// Number of interior pixels that read as ink (nearer the text colour than the fill).
int count_ink(const Shot& s, Box r, uint32_t fill, uint32_t text)
{
  const Px f = argb_px(fill), t = argb_px(text);
  const int x0 = (int)std::ceil((r.x + 2) * s.scale), x1 = (int)std::floor((r.x + r.w - 2) * s.scale);
  const int y0 = (int)std::ceil((r.y + 1) * s.scale), y1 = (int)std::floor((r.y + r.h - 1) * s.scale);
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) if (is_ink(s.dev(x, y), f, t)) ++n;
  return n;
}

// Does the field's outline carry `argb`? CoreGraphics strokes a 1 px line CENTRED
// on the rect edge, so at 1x it lands as half of two device pixels - one over the
// field's fill, one over what lies outside it - and only at 2x as one full pixel.
// Accept the full colour or either 50% blend.
bool outline_has(const Shot& s, Box r, uint32_t argb, uint32_t inside, uint32_t outside, int tol)
{
  const Px want[3] = { argb_px(argb),
                       argb_px(blend((argb & 0x00FFFFFFu) | 0x80000000u, inside)),
                       argb_px(blend((argb & 0x00FFFFFFu) | 0x80000000u, outside)) };
  const int x0 = (int)std::floor(r.x * s.scale), x1 = (int)std::ceil((r.x + r.w) * s.scale);
  const int y0 = (int)std::floor(r.y * s.scale), y1 = (int)std::ceil((r.y + r.h) * s.scale);
  const int mid_y = (y0 + y1) / 2;
  const int mid_x = (x0 + x1) / 2;
  const int band = (int)std::ceil(2 * s.scale);
  auto hit = [&](Px p) { for (const Px& w : want) if (dist(p, w) <= tol) return true; return false; };
  for (int d = -band; d < band; ++d) {
    if (hit(s.dev(x0 + d, mid_y))) return true;       // left edge
    if (hit(s.dev(x1 - 1 - d, mid_y))) return true;   // right edge
    if (hit(s.dev(mid_x, y0 + d))) return true;       // top edge
    if (hit(s.dev(mid_x, y1 - 1 - d))) return true;   // bottom edge
  }
  return false;
}

uint32_t blend(uint32_t over, uint32_t under)
{
  const float a = ((over >> 24) & 0xFF) / 255.0f;
  auto ch = [&](int sh) {
    return (uint32_t)std::lround(((over >> sh) & 0xFF) * a + ((under >> sh) & 0xFF) * (1 - a));
  };
  return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// ---- synthetic input (straight into the xpl view, like AppKit would) ------

NSView* content_view(NSWindow* w) { return w.contentView; }

void key(NSWindow* win, unsigned short code, unichar ch, NSEventModifierFlags mods = 0)
{
  NSString* s = [NSString stringWithCharacters:&ch length:1];
  NSEvent* ev = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                            modifierFlags:mods timestamp:0 windowNumber:win.windowNumber
                                  context:nil characters:s charactersIgnoringModifiers:s
                                isARepeat:NO keyCode:code];
  [content_view(win) keyDown:ev];
}
constexpr unsigned short kHome = 115, kEnd = 119, kRight = 124;
constexpr unichar uHome = 0xF729, uEnd = 0xF72B, uRight = 0xF703;

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];
    neui_init();

    const uint32_t FRAME_BG = 0xFF202428, FILL = 0xFF102030, TEXT = 0xFFFFC040,
                   BORDER = 0xFFD040C0, FOCUS = 0xFF40E060, SEL = 0x80FF0000;
    const char* kI = "iiiiiiiiiiiiiiii";          // 16 x 'i': narrow in the system font, 7.2 px in Courier
    const char* kMix = "Hamburgefonstiv 0123456789";

    // ======================= crossplatform host =============================
    neui_api_t* api = neui_get_api("neui.host.crossplatform");
    if (!api) { std::printf("FAIL: no crossplatform host\n"); return 1; }
    neui_session_t sess = api->create_session(&g_client, nullptr);
    auto* w  = (neui_widget_api_t*)api->get_interface(sess, NEUI_API_WIDGETS);
    auto* at = (neui_attr_api_t*)  api->get_interface(sess, NEUI_API_ATTRS);
    auto* as = (neui_asset_api_t*) api->get_interface(sess, NEUI_API_ASSETS);

    char reg_family[256] = { 0 };
    neui_asset_t reg_font = asset_none;
    if (const char* path = std::getenv("NEUI_TEST_FONT_FILE")) {
      reg_font = as->create_font_from_file(sess, path);
      if (reg_font.id != asset_none.id) as->get_font_family(sess, reg_font, reg_family, sizeof(reg_family));
      std::printf("registered %s -> family \"%s\"\n", path, reg_family);
    }

    neui_widget_t frame = w->create(sess, widget_none, NEUI_W_APPWINDOW, 80, 60, 420, 520, nullptr);
    at->set_int(sess, frame, NEUI_ATTR_BACKGROUND, (int32_t)FRAME_BG);

    auto field = [&](float x, float y, float fw, float fh, const char* text, bool styled) {
      neui_widget_t f = w->create(sess, frame, NEUI_W_INPUTBOX, (int)x, (int)y, (int)fw, (int)fh, nullptr);
      w->set_text(sess, f, text);
      if (styled) {
        at->set_int(sess, f, NEUI_ATTR_BACKGROUND,         (int32_t)FILL);
        at->set_int(sess, f, NEUI_ATTR_TEXT_COLOR,         (int32_t)TEXT);
        at->set_int(sess, f, NEUI_ATTR_BORDER_COLOR,       (int32_t)BORDER);
        at->set_int(sess, f, NEUI_ATTR_BORDER_FOCUS_COLOR, (int32_t)FOCUS);
        at->set_int(sess, f, NEUI_ATTR_SELECTION_COLOR,    (int32_t)SEL);
      }
      return f;
    };
    const Box rCourier = { 10, 10, 380, 28 }, rPlain = { 10, 48, 380, 28 },
               rSystem = { 10, 86, 380, 28 };
    neui_widget_t courier = field(rCourier.x, rCourier.y, rCourier.w, rCourier.h, kI, true);
    neui_widget_t plain = field(rPlain.x, rPlain.y, rPlain.w, rPlain.h, kI, false);   // unstyled control
    field(rSystem.x,  rSystem.y,  rSystem.w,  rSystem.h,  kI, true);    // styled, system family
    at->set_string(sess, courier, NEUI_ATTR_FONT_FAMILY, "Courier New");

    // Family fields on the mixed string: system / unknown / registered.
    const Box rMixSys = { 10, 124, 380, 28 }, rMixUnk = { 10, 162, 380, 28 },
               rMixReg = { 10, 200, 380, 28 };
    neui_widget_t mix_sys = field(rMixSys.x, rMixSys.y, rMixSys.w, rMixSys.h, kMix, true);
    neui_widget_t mix_unk = field(rMixUnk.x, rMixUnk.y, rMixUnk.w, rMixUnk.h, kMix, true);
    neui_widget_t mix_reg = field(rMixReg.x, rMixReg.y, rMixReg.w, rMixReg.h, kMix, true);
    (void)mix_sys;
    at->set_string(sess, mix_unk, NEUI_ATTR_FONT_FAMILY, "No Such Family 12345");
    if (reg_family[0]) at->set_string(sess, mix_reg, NEUI_ATTR_FONT_FAMILY, reg_family);

    // An unstyled LABEL, for the on-screen phase.
    const Box rLabel = { 10, 486, 380, 22 };
    neui_widget_t label = w->create(sess, frame, NEUI_W_LABEL, (int)rLabel.x, (int)rLabel.y,
                                    (int)rLabel.w, (int)rLabel.h, nullptr);
    w->set_text(sess, label, "A LABEL with its own text colour");

    // Weight: the same family and text at 400 and at 700.
    const Box rW400 = { 10, 410, 380, 28 }, rW700 = { 10, 448, 380, 28 };
    neui_widget_t w400 = field(rW400.x, rW400.y, rW400.w, rW400.h, kMix, true);
    neui_widget_t w700 = field(rW700.x, rW700.y, rW700.w, rW700.h, kMix, true);
    at->set_string(sess, w400, NEUI_ATTR_FONT_FAMILY, "Courier New");
    at->set_string(sess, w700, NEUI_ATTR_FONT_FAMILY, "Courier New");
    at->set_int(sess, w700, NEUI_ATTR_FONT_WEIGHT, 700);

    // MULTILINE, soft-wrapped, system family first.
    const Box rMl = { 10, 240, 380, 160 };
    neui_widget_t ml = w->create(sess, frame, NEUI_W_MULTILINE, (int)rMl.x, (int)rMl.y, (int)rMl.w, (int)rMl.h, nullptr);
    std::string para;
    for (int i = 0; i < 20; ++i) para += "wander quiet ";   // 260 chars: ~5 rows in the system font, ~6-7 in Courier New
    w->set_text(sess, ml, para.c_str());
    at->set_int(sess, ml, NEUI_ATTR_LINE_WRAP,      1);
    at->set_int(sess, ml, NEUI_ATTR_BACKGROUND,     (int32_t)FILL);
    at->set_int(sess, ml, NEUI_ATTR_TEXT_COLOR,     (int32_t)TEXT);
    at->set_int(sess, ml, NEUI_ATTR_BORDER_COLOR,   (int32_t)BORDER);

    w->show(sess, frame);
    pump();
    NSWindow* win = (__bridge NSWindow*)w->get_native_handle(sess, frame);
    check(win != nil, "xpl: native window exists");
    if (!win) return 1;
    NSView* view = content_view(win);

    // ---- 1. colours ---------------------------------------------------------
    Shot s = capture(view);
    std::printf("backing scale %.1f\n", s.scale);
    auto fill_at = [&](Box r) { return s.at(r.x + r.w - 12, r.y + r.h / 2); };
    check(dist(fill_at(rCourier), argb_px(FILL)) <= 12, "styled INPUTBOX fill is NEUI_ATTR_BACKGROUND");
    check(dist(fill_at(rPlain),   argb_px(FILL)) > 40,  "unstyled INPUTBOX keeps the theme fill");
    check(outline_has(s, rCourier, BORDER, FILL, FRAME_BG, 24), "styled INPUTBOX outline is NEUI_ATTR_BORDER_COLOR");
    check(!outline_has(s, rPlain, BORDER, FILL, FRAME_BG, 24),  "unstyled INPUTBOX outline is not the styled colour");
    check(count_near(s, rCourier, TEXT, 40) > 8, "styled INPUTBOX text is NEUI_ATTR_TEXT_COLOR");
    check(count_near(s, rPlain, TEXT, 40) == 0,  "unstyled INPUTBOX text is not the styled colour");

    // ---- 2. font family -----------------------------------------------------
    const float ext_courier = ink_right(s, rCourier, FILL, TEXT);
    const float ext_system  = ink_right(s, rSystem,  FILL, TEXT);
    std::printf("ink right edge: Courier New %.1f px, system %.1f px (16 x 'i')\n", ext_courier, ext_system);
    check(ext_courier > ext_system + 30.0f, "family 'Courier New' is drawn (glyphs span far past the system font's)");
    check(std::fabs(ext_courier - (4 + 15 * 7.2f + 5.5f)) < 5.0f, "Courier New advance is ~7.2 px/char");

    const float e_sys = ink_right(s, rMixSys, FILL, TEXT);
    const float e_unk = ink_right(s, rMixUnk, FILL, TEXT);
    std::printf("mixed text: system %.1f, unknown family %.1f\n", e_sys, e_unk);
    check(std::fabs(e_sys - e_unk) < 1.0f, "unknown family falls back to the system font");
    if (reg_family[0]) {
      const float e_reg = ink_right(s, rMixReg, FILL, TEXT);
      std::printf("mixed text: registered \"%s\" %.1f\n", reg_family, e_reg);
      check(std::fabs(e_reg - e_sys) > 2.0f, "a family registered through the asset API is drawn in INPUTBOX");
    } else {
      std::printf("skip: no NEUI_TEST_FONT_FILE, registered-family check not run\n");
    }

    const int ink400 = count_ink(s, rW400, FILL, TEXT), ink700 = count_ink(s, rW700, FILL, TEXT);
    std::printf("ink pixels, Courier New: weight 400 %d, weight 700 %d\n", ink400, ink700);
    check(ink400 > 20 && ink700 > ink400 * 11 / 10, "NEUI_ATTR_FONT_WEIGHT 700 draws the bold face (heavier strokes)");

    // ---- 3. caret vs glyphs, focus border, selection ------------------------
    w->set_focus(sess, courier);
    pump(0.05);
    key(win, kHome, uHome);
    for (int i = 0; i < 5; ++i) key(win, kRight, uRight);
    s = capture(view);
    check(outline_has(s, rCourier, FOCUS, FILL, FRAME_BG, 24), "focused outline is NEUI_ATTR_BORDER_FOCUS_COLOR");
    CaretInfo ci = find_caret(s, rCourier, FILL, TEXT);
    std::printf("caret after Home + 5 x Right: x=%.1f, glyph runs left %d of %d\n",
                ci.x, ci.glyph_runs_left, ci.glyph_runs);
    check(ci.x > 0, "a caret is painted in NEUI_ATTR_TEXT_COLOR");
    check(ci.glyph_runs_left == 5, "caret sits after exactly 5 glyphs: caret and glyphs share one font");
    check(std::fabs(ci.x - (4 + 5 * 7.2f)) < 1.6f, "caret x = pad + 5 x the family's advance");

    key(win, kEnd, uEnd);
    s = capture(view);
    ci = find_caret(s, rCourier, FILL, TEXT);
    check(ci.glyph_runs_left == 16, "End puts the caret after all 16 glyphs");

    key(win, kHome, uHome);
    key(win, kEnd, uEnd, NSEventModifierFlagShift);
    s = capture(view);
    const uint32_t tint = blend(SEL, FILL);
    check(count_near(s, rCourier, tint, 20) > 200, "selection is painted in NEUI_ATTR_SELECTION_COLOR");

    // ---- 4. wrap cache follows the family -----------------------------------
    auto wrapped_rows = [&](const Shot& sh) {
      int rows = 0;
      const Px f = argb_px(FILL), t = argb_px(TEXT);
      for (int r = 0; r < 8; ++r) {
        const int y0 = (int)std::floor((rMl.y + 2 + 18 * r + 1) * sh.scale);
        const int y1 = (int)std::floor((rMl.y + 2 + 18 * (r + 1) - 1) * sh.scale);
        bool ink = false;
        for (int y = y0; y < y1 && !ink; ++y)
          for (int x = (int)((rMl.x + 5) * sh.scale); x < (int)((rMl.x + rMl.w - 20) * sh.scale); ++x)
            if (is_ink(sh.dev(x, y), f, t)) { ink = true; break; }
        if (ink) ++rows;
      }
      return rows;
    };
    s = capture(view);
    check(count_near(s, rMl, TEXT, 40) > 50, "styled MULTILINE text is NEUI_ATTR_TEXT_COLOR");
    check(dist(s.at(rMl.x + rMl.w - 30, rMl.y + 4), argb_px(FILL)) <= 12, "styled MULTILINE fill is NEUI_ATTR_BACKGROUND");
    check(outline_has(s, rMl, BORDER, FILL, FRAME_BG, 24), "styled MULTILINE outline is NEUI_ATTR_BORDER_COLOR");
    const int rows_sys = wrapped_rows(s);
    at->set_string(sess, ml, NEUI_ATTR_FONT_FAMILY, "Courier New");   // same size, wider family
    s = capture(view);
    const int rows_courier = wrapped_rows(s);
    std::printf("MULTILINE wrapped rows: system %d, Courier New %d\n", rows_sys, rows_courier);
    check(rows_sys >= 2 && rows_courier > rows_sys, "MULTILINE re-wraps when only the family changes");

    // ---- live remove() returns to the theme ---------------------------------
    at->remove(sess, courier, NEUI_ATTR_BACKGROUND);
    at->remove(sess, courier, NEUI_ATTR_BORDER_FOCUS_COLOR);
    s = capture(view);
    check(dist(fill_at(rCourier), argb_px(FILL)) > 40, "remove(BACKGROUND) returns the fill to the theme");
    check(!outline_has(s, rCourier, FOCUS, FILL, FRAME_BG, 24), "remove(BORDER_FOCUS_COLOR) returns the focus outline to the theme");

    // ---- 5. the same attributes through the real display path -----------------
    // Everything above read a full-view snapshot. A frame normally repaints only
    // the widget whose attribute changed (its damage rect), so write the attrs
    // LIVE, after a full frame is up, and read what the window server holds.
    {
      [view setNeedsDisplay:YES];
      Shot before = capture_onscreen(win);
      if (!before.rep) {
        std::printf("skip: screencapture unavailable, on-screen (partial repaint) phase not run\n");
      } else {
        const uint32_t FILL2 = 0xFF305020, TEXT2 = 0xFFF0F060, BORDER2 = 0xFFE05050, LBL2 = 0xFFE040E0;
        const Px frame_px = argb_px(FRAME_BG);
        check(dist(before.at(rPlain.x + rPlain.w - 12, rPlain.y + rPlain.h / 2), frame_px) > 40,
              "on-screen: the unstyled field starts on the theme (not the frame colour)");
        at->set_int(sess, plain, NEUI_ATTR_BACKGROUND,   (int32_t)FILL2);
        at->set_int(sess, plain, NEUI_ATTR_TEXT_COLOR,   (int32_t)TEXT2);
        at->set_int(sess, plain, NEUI_ATTR_BORDER_COLOR, (int32_t)BORDER2);
        at->set_int(sess, label, NEUI_ATTR_TEXT_COLOR,   (int32_t)LBL2);
        Shot after = capture_onscreen(win);
        check(after.rep != nil, "on-screen: second capture");
        if (after.rep) {
          check(dist(after.at(rPlain.x + rPlain.w - 12, rPlain.y + rPlain.h / 2), argb_px(FILL2)) <= 24,
                "on-screen: a live BACKGROUND write repaints the field");
          check(count_ink(after, rPlain, FILL2, TEXT2) > 8, "on-screen: a live TEXT_COLOR write repaints the text");
          check(outline_has(after, rPlain, BORDER2, FILL2, FRAME_BG, 40), "on-screen: a live BORDER_COLOR write repaints the outline");
          int lbl_ink = 0;
          const Px lb = argb_px(LBL2), back = after.at(rLabel.x + rLabel.w - 6, rLabel.y + 2);
          for (int y = (int)(rLabel.y * after.scale); y < (int)((rLabel.y + rLabel.h) * after.scale); ++y)
            for (int x = (int)(rLabel.x * after.scale); x < (int)((rLabel.x + 200) * after.scale); ++x)
              if (is_ink(after.dev(x, y), back, lb)) ++lbl_ink;
          check(lbl_ink > 8, "on-screen: a live LABEL TEXT_COLOR write repaints the label");
          // A neighbour nobody touched must not have changed.
          check(dist(after.at(rSystem.x + rSystem.w - 12, rSystem.y + rSystem.h / 2),
                     before.at(rSystem.x + rSystem.w - 12, rSystem.y + rSystem.h / 2)) <= 6
                && count_ink(after, rSystem, FILL, TEXT) == count_ink(before, rSystem, FILL, TEXT),
                "on-screen: an untouched neighbour field is unchanged");
        }
        // And back: removing the attrs live returns the field to the theme.
        at->remove(sess, plain, NEUI_ATTR_BACKGROUND);
        at->remove(sess, plain, NEUI_ATTR_TEXT_COLOR);
        at->remove(sess, plain, NEUI_ATTR_BORDER_COLOR);
        Shot gone = capture_onscreen(win);
        if (gone.rep)
          check(dist(gone.at(rPlain.x + rPlain.w - 12, rPlain.y + rPlain.h / 2), argb_px(FILL2)) > 40,
                "on-screen: a live remove() returns the field to the theme");
      }
    }

    if (reg_font.id != asset_none.id) as->destroy(sess, reg_font);

    // ======================= macOS native host ===============================
    neui_api_t* nat = neui_get_api("neui.host.macos");
    if (!nat) { std::printf("skip: no macOS native host\n"); }
    else {
      neui_session_t ns = nat->create_session(&g_client, nullptr);
      auto* nw = (neui_widget_api_t*)nat->get_interface(ns, NEUI_API_WIDGETS);
      auto* na = (neui_attr_api_t*)  nat->get_interface(ns, NEUI_API_ATTRS);
      neui_widget_t nf = nw->create(ns, widget_none, NEUI_W_APPWINDOW, 560, 60, 360, 300, nullptr);
      const Box nIn = { 10, 10, 330, 28 }, nMl = { 10, 60, 330, 100 }, nLbl = { 10, 180, 330, 24 };
      neui_widget_t ni = nw->create(ns, nf, NEUI_W_INPUTBOX,  (int)nIn.x,  (int)nIn.y,  (int)nIn.w,  (int)nIn.h,  nullptr);
      neui_widget_t nm = nw->create(ns, nf, NEUI_W_MULTILINE, (int)nMl.x,  (int)nMl.y,  (int)nMl.w,  (int)nMl.h,  nullptr);
      neui_widget_t nl = nw->create(ns, nf, NEUI_W_LABEL,     (int)nLbl.x, (int)nLbl.y, (int)nLbl.w, (int)nLbl.h, nullptr);
      nw->set_text(ns, ni, "native field");
      nw->set_text(ns, nm, "native multi\nline field");
      nw->set_text(ns, nl, "native label");
      na->set_int(ns, ni, NEUI_ATTR_BACKGROUND, (int32_t)FILL);
      na->set_int(ns, ni, NEUI_ATTR_TEXT_COLOR, (int32_t)TEXT);
      na->set_int(ns, nm, NEUI_ATTR_BACKGROUND, (int32_t)FILL);
      na->set_int(ns, nm, NEUI_ATTR_TEXT_COLOR, (int32_t)TEXT);
      na->set_int(ns, nl, NEUI_ATTR_TEXT_COLOR, (int32_t)TEXT);
      nw->show(ns, nf);
      pump(0.3);
      NSWindow* nwin = (__bridge NSWindow*)nw->get_native_handle(ns, nf);
      if (!nwin) { check(false, "native: window exists"); }
      else {
        NSView* nv = nwin.contentView;
        Shot ns_ = capture(nv);
        check(dist(ns_.at(nIn.x + nIn.w - 14, nIn.y + nIn.h / 2), argb_px(FILL)) <= 12,
              "native INPUTBOX fill is NEUI_ATTR_BACKGROUND");
        check(count_near(ns_, nIn, TEXT, 40) > 8, "native INPUTBOX text is NEUI_ATTR_TEXT_COLOR");
        check(dist(ns_.at(nMl.x + nMl.w - 30, nMl.y + nMl.h - 10), argb_px(FILL)) <= 12,
              "native MULTILINE fill is NEUI_ATTR_BACKGROUND");
        check(count_ink(ns_, nMl, FILL, TEXT) > 8, "native MULTILINE text is NEUI_ATTR_TEXT_COLOR");
        {
          // LABEL has no fill: its backdrop is the window's, so measure ink against that.
          const Px back = ns_.at(nLbl.x + nLbl.w - 10, nLbl.y + 2);
          int n = 0;
          const int x0 = (int)(nLbl.x * ns_.scale), x1 = (int)((nLbl.x + 120) * ns_.scale);
          const int y0 = (int)(nLbl.y * ns_.scale), y1 = (int)((nLbl.y + nLbl.h) * ns_.scale);
          for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
              if (dist(ns_.dev(x, y), argb_px(TEXT)) <= 40 && dist(ns_.dev(x, y), back) > 40) ++n;
          check(n > 8, "native LABEL text is NEUI_ATTR_TEXT_COLOR");
        }
        na->remove(ns, ni, NEUI_ATTR_BACKGROUND);
        na->remove(ns, ni, NEUI_ATTR_TEXT_COLOR);
        ns_ = capture(nv);
        check(dist(ns_.at(nIn.x + nIn.w - 14, nIn.y + nIn.h / 2), argb_px(FILL)) > 40,
              "native: remove(BACKGROUND) puts the system fill back");
        check(count_near(ns_, nIn, TEXT, 40) == 0, "native: remove(TEXT_COLOR) puts the system text colour back");
      }
      nat->destroy(ns);
    }

    api->destroy(sess);
  }
  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
