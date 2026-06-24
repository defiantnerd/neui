// surface_example - renders content into an off-screen SURFACE asset
// and displays it through a NEUI_W_CUSTOMDRAW widget that calls
// painter->draw_asset. Demonstrates that paint_surface is repeatable
// (the "Randomise" button re-renders the same surface and the next
// frame picks up the new pixels via the cache-drop path).
//
// Verifies all three storage tiers in one demo:
//   * SURFACE asset created via asset_api->create_surface.
//   * Repeated paint_surface calls re-rendering the same handle.
//   * CUSTOMDRAW widget drawing the surface via painter->draw_asset
//     (same code path that draws any bitmap asset).

#include "neui/neui.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef WIN32
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOST "neui.host.macos"
#else
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

static void dbglog(const char* fmt, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
#ifdef _WIN32
  OutputDebugStringA(buf);
#endif
  fputs(buf, stderr);
}

struct AppState
{
  neui_api_t*        neui    = nullptr;
  neui_widget_api_t* widgets = nullptr;
  neui_asset_api_t*  assets  = nullptr;
  neui_session_t     session = { 0 };

  uint32_t           win_id     = 0;
  uint32_t           canvas_id  = 0;
  uint32_t           randomise_id = 0;
  uint32_t           rerender_id  = 0;
  uint32_t           gradient_chk = 0;

  neui_asset_t       surface = asset_none;
  std::vector<float> bars;   // 24 normalised heights, drawn into the surface
  int                paint_seq = 0;
  bool               use_gradient = false;  // toggled by the checkbox
};

// Client paint callback - same painter_api / shape as WIDGET_PAINT, just
// targeting the surface's off-screen ctx. Renders a 24-bar bar-chart
// against the current `bars`.
static void NEUI_ABI draw_diag(neui_painter_t*     p,
                                  neui_painter_api_t* api,
                                  float               w,
                                  float               h,
                                  void*               user)
{
  auto* a = static_cast<AppState*>(user);

  if (a->use_gradient) {
    // Radial backdrop: a soft glow centred near the top, demonstrating
    // NEUI_GRADIENT_RADIAL. The 0.0 stop sits at the centre, the 1.0 stop
    // on the circle of `radius`, with CLAMP filling the corners.
    neui_gradient_stop_t bg_stops[2] = {
      { 0.0f, 0xFF243044 },
      { 1.0f, 0xFF0A0E14 },
    };
    neui_gradient_t bg{};
    bg.kind       = NEUI_GRADIENT_RADIAL;
    bg.stops      = bg_stops;
    bg.stop_count = 2;
    bg.extend     = NEUI_GRADIENT_EXTEND_CLAMP;
    bg.start_x    = w * 0.5f;   // centre
    bg.start_y    = h * 0.25f;
    bg.end_x      = w * 0.5f;   // focal == centre -> plain concentric radial
    bg.end_y      = h * 0.25f;
    bg.radius     = (w > h ? w : h) * 0.75f;
    api->fill_rect_gradient(p, 0, 0, w, h, &bg);
  } else {
    api->fill_rect(p, 0, 0, w, h, 0xFF101820);
  }
  api->draw_rect(p, 0.5f, 0.5f, w - 1.0f, h - 1.0f, 1.0f, 0xFF304050);

  const float pad     = 12.0f;
  const float top     = 24.0f;
  const float chart_w = w - pad * 2.0f;
  const float chart_h = h - top - pad;
  const float bar_gap = 2.0f;
  const int   n       = static_cast<int>(a->bars.size());
  const float bar_w   = (chart_w - bar_gap * (n - 1)) / static_cast<float>(n);

  // One vertical gradient defined over the whole chart height: warm orange at
  // the top, cool blue at the floor. Every bar samples the SAME axis so the
  // colour at a given height is consistent across bars - each bar is a window
  // onto this shared linear gradient (NEUI_GRADIENT_LINEAR).
  neui_gradient_stop_t bar_stops[3] = {
    { 0.0f, 0xFFFF8030 },   // top of chart - warm
    { 0.5f, 0xFFB060C0 },   // mid - violet
    { 1.0f, 0xFF40C0FF },   // floor - cool
  };
  neui_gradient_t bar_grad{};
  bar_grad.kind       = NEUI_GRADIENT_LINEAR;
  bar_grad.stops      = bar_stops;
  bar_grad.stop_count = 3;
  bar_grad.extend     = NEUI_GRADIENT_EXTEND_CLAMP;
  bar_grad.start_x    = 0.0f;  bar_grad.start_y = top;
  bar_grad.end_x      = 0.0f;  bar_grad.end_y   = top + chart_h;

  for (int i = 0; i < n; ++i) {
    float v  = a->bars[i];
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    float bh = v * chart_h;
    float bx = pad + i * (bar_w + bar_gap);
    float by = top + (chart_h - bh);
    if (a->use_gradient) {
      api->fill_rect_gradient(p, bx, by, bar_w, bh, &bar_grad);
    } else {
      // Colour ramp from cool blue at the floor to warm orange at the top.
      uint8_t r = static_cast<uint8_t>(64  + 191 * v);
      uint8_t g = static_cast<uint8_t>(128 + 32  * v);
      uint8_t b = static_cast<uint8_t>(224 - 160 * v);
      uint32_t argb = 0xFF000000 | (r << 16) | (g << 8) | b;
      api->fill_rect(p, bx, by, bar_w, bh, argb);
    }
  }

  char title[80];
  snprintf(title, sizeof(title), "Diagnostic surface  (paint #%d, %s)",
            a->paint_seq, a->use_gradient ? "gradient" : "solid");
  api->draw_text(p, pad, 4, w - pad * 2.0f, 20,
                  title, 13.0f, 0xFFE0E8F0);
}

static void randomise_bars(AppState* a)
{
  for (auto& v : a->bars)
    v = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
}

static void repaint_surface(AppState* a)
{
  if (a->surface.id == asset_none.id) return;
  a->paint_seq++;
  // Clear to fully transparent so the canvas widget's own background
  // shows through any rounded corners we might paint later. The chart
  // itself fills its rect opaquely, so for now this just keeps the
  // semantics clean.
  a->assets->paint_surface(a->session, a->surface, 0x00000000,
                            draw_diag, a);
  a->widgets->invalidate(a->session, { a->canvas_id });
}

static void paint_canvas(neui_event_paint_t* p, AppState* a)
{
  auto* px = p->painter_api;
  auto* ph = p->p;

  // Backdrop so it's obvious where the SURFACE sits inside the widget.
  px->fill_rect(ph, 0, 0, p->width, p->height, 0xFF202830);

  if (a->surface.id == asset_none.id) {
    px->draw_text(ph, 12, 12, p->width - 24, 20,
                   "(surface not available - null backend?)",
                   13.0f, 0xFFE08080);
    return;
  }

  // Logical surface size is whatever we passed to create_surface; the
  // scale parameter affects backing-pixel density, not logical dims.
  float sw = 0, sh = 0;
  a->assets->get_size(a->session, a->surface, &sw, &sh);

  // Centre the surface in the canvas widget.
  float dx = (p->width  - sw) * 0.5f;
  float dy = (p->height - sh) * 0.5f;
  px->draw_asset(ph, a->surface, dx, dy, sw, sh);
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  auto* a = static_cast<AppState*>(token);
  switch (event->type) {
    case NEUI_EVENT_APP_QUIT:
      return true;

    case NEUI_EVENT_WIDGET_PAINT:
      if (event->data.paint.widget.id == a->canvas_id) {
        paint_canvas(&event->data.paint, a);
        return true;
      }
      break;

    case NEUI_EVENT_MOUSE_BUTTON_CLICK: {
      uint32_t wid = event->data.mouse.widget.id;
      if (wid == a->randomise_id) {
        randomise_bars(a);
        repaint_surface(a);
        return true;
      }
      if (wid == a->rerender_id) {
        // Re-render without changing data, proving the cache-drop path
        // works regardless of whether the pixels actually changed.
        repaint_surface(a);
        return true;
      }
      break;
    }

    case NEUI_EVENT_CHECKBOX_CHANGED: {
      if (event->data.checkbox.widget.id == a->gradient_chk) {
        a->use_gradient = (event->data.checkbox.state == NEUI_CHECK_CHECKED);
        repaint_surface(a);
        return true;
      }
      break;
    }

    default:
      break;
  }
  return false;
}

static void* NEUI_ABI get_interface(void* /*token*/, const char* iface)
{
  static neui_widget_client_t widget_client;
  if (!strcmp(iface, NEUI_API_WIDGETS)) {
    widget_client.neui_version = NEUI_VERSION;
    widget_client.ondestroy    = nullptr;
    widget_client.onevent      = on_event;
    return &widget_client;
  }
  return nullptr;
}

int main(int /*argc*/, char* /*argv*/[])
{
  neui_init();
  neui_api_t* host = neui_get_api(ACTIVE_HOST);
  if (!host) host = neui_get_api(nullptr);
  if (!host) { dbglog("[surface_example] no host\n"); return 1; }

  AppState app;
  app.neui = host;
  app.bars.assign(24, 0.0f);
  randomise_bars(&app);

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;

  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[surface_example] no session\n"); return 1; }

  app.widgets = (neui_widget_api_t*)host->get_interface(app.session, NEUI_API_WIDGETS);
  app.assets  = (neui_asset_api_t*) host->get_interface(app.session, NEUI_API_ASSETS);
  if (!app.widgets || !app.assets) {
    dbglog("[surface_example] missing API\n"); return 1;
  }

  // Build a SURFACE at 2.0 scale so the bars stay crisp on HiDPI
  // displays. On a 1x display the backend downsamples at draw time.
  app.surface = app.assets->create_surface(app.session, 360.0f, 220.0f, 2.0f);
  if (app.surface.id == asset_none.id)
    dbglog("[surface_example] surface unsupported (null backend?)\n");

  neui_widget_t win = app.widgets->create(app.session,
                                            neui_widget_t{ UINT32_MAX },
                                            NEUI_W_APPWINDOW,
                                            100, 100, 760, 480, nullptr);
  app.widgets->set_text(app.session, win, "neui SURFACE example");
  app.win_id = win.id;

  neui_widget_t canvas = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                               16, 16, 720, 380, nullptr);
  app.canvas_id = canvas.id;

  neui_widget_t btn_rand = app.widgets->create(app.session, win, NEUI_W_BUTTON,
                                                  16, 412, 160, 28, nullptr);
  app.widgets->set_text(app.session, btn_rand, "Randomise");
  app.randomise_id = btn_rand.id;

  neui_widget_t btn_re = app.widgets->create(app.session, win, NEUI_W_BUTTON,
                                                188, 412, 160, 28, nullptr);
  app.widgets->set_text(app.session, btn_re, "Re-render (no change)");
  app.rerender_id = btn_re.id;

  // Toggle between solid fills and the new gradient fills.
  neui_widget_t chk_grad = app.widgets->create(app.session, win, NEUI_W_CHECKBOX,
                                                 372, 416, 200, 22, nullptr);
  app.widgets->set_text(app.session, chk_grad, "Gradient fill");
  app.gradient_chk = chk_grad.id;

  // First render so the canvas shows something before the user clicks.
  repaint_surface(&app);

  app.widgets->show(app.session, win);
  host->run(app.session);

  if (app.surface.id != asset_none.id)
    app.assets->destroy(app.session, app.surface);

  host->destroy(app.session);
  return 0;
}
