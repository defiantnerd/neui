// overlay_example - a transparent CUSTOMDRAW overlay composited over a
// backdrop CUSTOMDRAW, demonstrating the three host-parity fixes together:
//
//   * Issue 1  - the backdrop's MOUSE_MOVE coordinates are WIDGET-local on
//                every host, so the crosshair tracks the cursor identically
//                on the native win32 host and the crossplatform host.
//   * Issue 2  - NEUI_ATTR_OVERLAY makes the top CUSTOMDRAW composite over the
//                backdrop with per-pixel alpha. On the native win32 host this
//                renders through a DirectComposition visual; the semi-
//                transparent measurement band below proves real per-pixel
//                blending (a colour-key overlay could not do this).
//   * Issue 3  - NEUI_ATTR_INPUT_TRANSPARENT lets the overlay paint at full
//                opacity yet never intercept the pointer, so the mouse falls
//                through to the backdrop that does the tracking.
//
// Move the mouse over the window: the backdrop content stays put, the
// crosshair + readout + translucent band follow the cursor on top of it.

#include "neui/neui.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef WIN32
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOST "neui.host.macos"
#else
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

struct AppState {
  neui_widget_api_t* widgets   = nullptr;
  neui_session_t     sess      = {};
  neui_widget_t      backdrop  = {};
  neui_widget_t      overlay   = {};
  int                mouse_x   = -1;
  int                mouse_y   = -1;
};

// Backdrop: obviously "live content" beneath the overlay - diagonal colour
// bands plus a ring grid, so anything showing through the overlay is clearly
// the backdrop and not the window background.
static void paint_backdrop(neui_event_paint_t* e)
{
  auto* px = e->painter_api;
  auto* ph = e->p;
  px->fill_rect(ph, 0, 0, e->width, e->height, 0xFF161A20);
  const uint32_t bands[] = { 0xFF2B3A55, 0xFF3D5A40, 0xFF6B3B5E, 0xFF7A5A2E };
  float bw = e->width / 12.0f;
  for (int i = 0; i < 12; ++i)
    px->fill_rect(ph, i * bw, 0, bw + 1.0f, e->height, bands[i % 4]);
  for (float r = 40.0f; r < e->width; r += 80.0f) {
    px->begin_path(ph);
    px->arc(ph, e->width * 0.5f, e->height * 0.5f, r, 0.0f, 6.28318530718f);
    px->stroke_path(ph, 1.0f, 0x33FFFFFF);
  }
  px->draw_text(ph, 12, 10, 400, 20, "backdrop content (move the mouse)",
                14.0f, 0xFFE8ECF2);
}

// Overlay: only thin marks + a translucent band. Everything it does not paint
// stays transparent and the backdrop shows through.
static void paint_overlay(neui_event_paint_t* e, AppState* app)
{
  auto* px = e->painter_api;
  auto* ph = e->p;

  // Always-on translucent panel (35% black) - proves per-pixel alpha without
  // needing the mouse: the backdrop's bands show through it, dimmed but not
  // erased. A colour-key overlay could not produce this.
  px->fill_rect(ph, 0, e->height - 40.0f, e->width, 40.0f, 0x59000000);
  px->draw_text(ph, 12, e->height - 30, 700, 20,
                "transparent overlay (35% band proves per-pixel alpha) - move the mouse for a crosshair",
                13.0f, 0xFFFFFFFF);

  if (app->mouse_x < 0)
    return;
  float mx = (float)app->mouse_x;
  float my = (float)app->mouse_y;

  // Translucent measurement band (25% white) - proves per-pixel alpha: the
  // backdrop's colours show through, tinted, not erased.
  px->fill_rect(ph, 0, my - 14.0f, e->width, 28.0f, 0x40FFFFFF);

  // Full-height / full-width crosshair (1px) + centre marker.
  px->fill_rect(ph, mx - 0.5f, 0, 1.0f, e->height, 0xFFFFD54A);
  px->fill_rect(ph, 0, my - 0.5f, e->width, 1.0f, 0xFFFFD54A);
  px->begin_path(ph);
  px->arc(ph, mx, my, 9.0f, 0.0f, 6.28318530718f);
  px->stroke_path(ph, 2.0f, 0xFFFF5A5A);

  char buf[64];
  snprintf(buf, sizeof(buf), "x: %d   y: %d", app->mouse_x, app->mouse_y);
  px->draw_text(ph, mx + 14.0f, my + 12.0f, 160, 18, buf, 13.0f, 0xFFFFFFFF);
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  auto* app = static_cast<AppState*>(token);
  switch (event->type) {
  case NEUI_EVENT_APP_QUIT:
    return true;
  case NEUI_EVENT_WIDGET_PAINT:
    if (event->data.paint.widget.id == app->backdrop.id) {
      paint_backdrop(&event->data.paint); return true;
    }
    if (event->data.paint.widget.id == app->overlay.id) {
      paint_overlay(&event->data.paint, app); return true;
    }
    return false;
  case NEUI_EVENT_MOUSE_MOVE:
    // Gated on the backdrop id: the overlay is input-transparent, so moves
    // arrive here (widget-local, identical on every host). Repaint the
    // overlay so the crosshair follows.
    if (event->data.mouse.widget.id == app->backdrop.id) {
      app->mouse_x = event->data.mouse.x;
      app->mouse_y = event->data.mouse.y;
      if (app->widgets && app->overlay.id)
        app->widgets->invalidate(app->sess, app->overlay);
    }
    return false;
  default:
    return false;
  }
}

static void* NEUI_ABI get_interface(void* /*token*/, const char* iface)
{
  static neui_widget_client_t wc;
  if (!strcmp(iface, NEUI_API_WIDGETS)) {
    wc.neui_version = NEUI_VERSION; wc.ondestroy = nullptr; wc.onevent = on_event;
    return &wc;
  }
  return nullptr;
}

int main(int, char*[])
{
  neui_init();
  neui_api_t* host = neui_get_api(ACTIVE_HOST);
  if (!host) host = neui_get_api(nullptr);
  if (!host) return 1;

  AppState app;
  neui_client_t client;
  client.neui_version = NEUI_VERSION;
  client.get_interface = get_interface;
  neui_session_t sess = host->create_session(&client, &app);
  if (!sess.session) return 1;
  app.sess    = sess;
  app.widgets = (neui_widget_api_t*)host->get_interface(sess, NEUI_API_WIDGETS);
  auto* attrs = (neui_attr_api_t*)host->get_interface(sess, NEUI_API_ATTRS);

  neui_widget_t win = app.widgets->create(sess, neui_widget_t{ UINT32_MAX },
                                          NEUI_W_APPWINDOW, 120, 120, 900, 600, nullptr);
  app.widgets->set_text(sess, win, "neui overlay example - transparent CUSTOMDRAW over content");

  int cx = 0, cy = 0, cw = 900, ch = 600;
  app.widgets->get_client_rect(sess, win, &cx, &cy, &cw, &ch);

  // Backdrop first (so it is beneath in z-order), then the overlay on top.
  app.backdrop = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW, cx, cy, cw, ch, nullptr);
  app.overlay  = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW, cx, cy, cw, ch, nullptr);

  // Make the overlay a transparent, click-through layer.
  attrs->set_int(sess, app.overlay, NEUI_ATTR_OVERLAY,           1);
  attrs->set_int(sess, app.overlay, NEUI_ATTR_INPUT_TRANSPARENT, 1);

  app.widgets->show(sess, win);
  host->run(sess);
  host->destroy(sess);
  return 0;
}
