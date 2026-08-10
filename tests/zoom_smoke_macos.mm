// UI-zoom acceptance harness (NEUI_ATTR_UI_SCALE), macOS / xpl host.
//
// Asserts the three contracts the zoom feature rests on:
//   1. SIZING   - the native window's content area grows by the zoom while the
//                 frame's logical size and get_client_rect stay unchanged.
//   2. PAINT    - a CUSTOMDRAW callback is handed LOGICAL width/height and a
//                 `scale` that includes the zoom.
//   3. DEVICE   - with NEUI_ATTR_PAINT_DEVICE_PIXELS the same callback instead
//                 receives width/height pre-multiplied by the zoom.
// Plus: the zoom is live (set after show resizes the window), and the frame
// reports METRICS_CHANGED when it changes.
//
// Needs a GUI session (it realizes a real NSWindow), so it is built but not
// ctest-registered; run ./tests/<config>/neui_zoom_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

neui_widget_t g_panel_logical{};
neui_widget_t g_panel_device{};

struct PaintRecord {
  bool  seen   = false;
  float width  = 0;
  float height = 0;
  float scale  = 0;
};

PaintRecord g_rec_logical;
PaintRecord g_rec_device;
int         g_metrics_events = 0;
float       g_last_metrics_scale = 0.0f;

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_WIDGET_PAINT) {
    PaintRecord* r = nullptr;
    if (ev->data.paint.widget.id == g_panel_logical.id) r = &g_rec_logical;
    else if (ev->data.paint.widget.id == g_panel_device.id) r = &g_rec_device;
    if (r) {
      r->seen   = true;
      r->width  = ev->data.paint.width;
      r->height = ev->data.paint.height;
      r->scale  = ev->data.paint.scale;
      // Draw something so the pass isn't optimized into nothing.
      ev->data.paint.painter_api->fill_rect(ev->data.paint.p, 0, 0,
                                            ev->data.paint.width,
                                            ev->data.paint.height,
                                            0xFF204060u);
      return true;
    }
  }
  if (ev->type == NEUI_EVENT_METRICS_CHANGED) {
    ++g_metrics_events;
    g_last_metrics_scale = ev->data.metrics.ui_scale;
    return true;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* NEUI_ABI iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

int g_fail = 0;
void check(bool ok, const char* what)
{
  if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}
bool approx(float a, float b) { return std::fabs(a - b) < 0.51f; }

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];

    neui_init();
    neui_api_t* api = neui_get_api("neui.host.crossplatform");
    if (!api) { std::printf("FAIL: no crossplatform host\n"); return 1; }
    neui_session_t sess = api->create_session(&g_client, nullptr);
    auto* w  = (neui_widget_api_t*)api->get_interface(sess, NEUI_API_WIDGETS);
    auto* at = (neui_attr_api_t*)  api->get_interface(sess, NEUI_API_ATTRS);

    const int LOG_W = 400, LOG_H = 240;
    neui_widget_t frame = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                    80, 80, LOG_W, LOG_H, nullptr);
    g_panel_logical = w->create(sess, frame, NEUI_W_CUSTOMDRAW, 10, 10, 120, 80, nullptr);
    g_panel_device  = w->create(sess, frame, NEUI_W_CUSTOMDRAW, 150, 10, 120, 80, nullptr);
    at->set_int(sess, g_panel_device, NEUI_ATTR_PAINT_DEVICE_PIXELS, 1);

    w->show(sess, frame);
    [[NSRunLoop mainRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.20]];

    // ---- baseline at 100 % --------------------------------------------------
    NSWindow* win = (__bridge NSWindow*)w->get_native_handle(sess, frame);
    check(win != nil, "no native window");
    NSSize c1 = win ? [win contentRectForFrameRect:win.frame].size : NSZeroSize;
    std::printf("zoom 1.0: content %.0fx%.0f (logical %dx%d)\n",
                c1.width, c1.height, LOG_W, LOG_H);
    check(approx((float)c1.width, LOG_W) && approx((float)c1.height, LOG_H),
          "unzoomed content size should equal the logical size");

    const float base_scale = g_rec_logical.scale;
    check(g_rec_logical.seen, "logical panel never painted");
    check(g_rec_device.seen,  "device panel never painted");
    check(approx(g_rec_logical.width, 120) && approx(g_rec_logical.height, 80),
          "logical panel size at 100%");
    // At 100 % zoom the device-pixel panel matches the logical one (the zoom is
    // the only factor this mode multiplies in).
    check(approx(g_rec_device.width, 120) && approx(g_rec_device.height, 80),
          "device panel size at 100%");

    // ---- live change to 200 % ---------------------------------------------
    g_rec_logical = PaintRecord{};
    g_rec_device  = PaintRecord{};
    at->set_float(sess, frame, NEUI_ATTR_UI_SCALE, 2.0f);
    [[NSRunLoop mainRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.30]];

    check(g_metrics_events >= 1, "zoom change should fire METRICS_CHANGED");
    check(approx(g_last_metrics_scale, 2.0f), "METRICS_CHANGED should carry 2.0");

    // 1. SIZING: the native content area doubled...
    NSSize c2 = win ? [win contentRectForFrameRect:win.frame].size : NSZeroSize;
    std::printf("zoom 2.0: content %.0fx%.0f\n", c2.width, c2.height);
    check(approx((float)c2.width, LOG_W * 2) && approx((float)c2.height, LOG_H * 2),
          "zoomed content size should be logical * 2");

    // ...while the client's view of the frame is unchanged.
    int cx = 0, cy = 0, cw = 0, ch = 0;
    w->get_client_rect(sess, frame, &cx, &cy, &cw, &ch);
    std::printf("client rect at zoom 2.0: %dx%d (expect %dx%d)\n",
                cw, ch, LOG_W, LOG_H);
    check(cw == LOG_W && ch == LOG_H,
          "get_client_rect must stay LOGICAL under zoom");

    // 2. PAINT: logical panel keeps logical dimensions, scale reports the zoom.
    std::printf("logical panel: %.0fx%.0f scale %.2f (base %.2f)\n",
                g_rec_logical.width, g_rec_logical.height,
                g_rec_logical.scale, base_scale);
    check(g_rec_logical.seen, "logical panel not repainted after zoom");
    check(approx(g_rec_logical.width, 120) && approx(g_rec_logical.height, 80),
          "logical panel must still get LOGICAL size under zoom");
    check(approx(g_rec_logical.scale, base_scale * 2.0f),
          "logical panel scale must include the zoom");

    // 3. DEVICE: device panel gets pre-multiplied dimensions.
    std::printf("device panel:  %.0fx%.0f scale %.2f\n",
                g_rec_device.width, g_rec_device.height, g_rec_device.scale);
    check(g_rec_device.seen, "device panel not repainted after zoom");
    check(approx(g_rec_device.width, 240) && approx(g_rec_device.height, 160),
          "device-pixel panel must get size * zoom");

    // ---- back to 100 % ----------------------------------------------------
    at->set_float(sess, frame, NEUI_ATTR_UI_SCALE, 1.0f);
    [[NSRunLoop mainRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.20]];
    NSSize c3 = win ? [win contentRectForFrameRect:win.frame].size : NSZeroSize;
    check(approx((float)c3.width, LOG_W) && approx((float)c3.height, LOG_H),
          "resetting zoom to 1.0 should restore the original content size");

    // Out-of-range values clamp rather than break the frame.
    at->set_float(sess, frame, NEUI_ATTR_UI_SCALE, 0.0f);
    check(approx(at->get_float(sess, frame, NEUI_ATTR_UI_SCALE, -1.0f), 0.0f),
          "the raw attr value is stored verbatim");

    w->destroy(sess, frame);
    std::printf(g_fail ? "\nZOOM FAILED (%d)\n" : "\nZOOM OK\n", g_fail);
    return g_fail ? 1 : 0;
  }
}
