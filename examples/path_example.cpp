// path_example - the extended path API: cubic/quadratic Bézier curves,
// even-odd fill-rule, and styled strokes (round cap/join + dashes). A single
// CUSTOMDRAW widget paints three demos via the painter path API directly
// (the same calls an SVG renderer would issue without flattening curves).

#include "neui/neui.h"
#include <string.h>
#include <stdio.h>

#ifdef WIN32
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOST "neui.host.macos"
#else
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

static const float kTwoPi = 6.28318530718f;

struct AppState {
  neui_widget_api_t* widgets = nullptr;
};

static void paint_canvas(neui_event_paint_t* e)
{
  auto* px = e->painter_api;
  auto* ph = e->p;

  px->fill_rect(ph, 0, 0, e->width, e->height, 0xFF1E232B);

  // --- 1. Cubic + quadratic Bézier filled leaf -----------------------------
  const float x1 = 70.0f;
  px->draw_text(ph, x1 - 40, 8, 180, 18, "cubic + quad fill", 13.0f, 0xFFB8C0CC);
  px->begin_path(ph);
  px->move_to(ph, x1, 50.0f);
  px->cubic_to(ph, x1 + 70, 30,  x1 + 70, 110, x1, 130);   // right side (cubic)
  px->quad_to (ph, x1 - 55, 90,  x1, 50.0f);               // left side (quad)
  px->close_path(ph);
  px->fill_path(ph, 0xFF4E9CF5);
  px->stroke_path(ph, 2.0f, 0xFF1B5FB0);

  // --- 2. Even-odd ring (two same-direction circles -> hole) ---------------
  const float cx2 = 300.0f, cy2 = 90.0f;
  px->draw_text(ph, cx2 - 70, 8, 180, 18, "even-odd ring", 13.0f, 0xFFB8C0CC);
  px->begin_path(ph);
  px->set_fill_rule(ph, NEUI_FILL_RULE_EVENODD);  // before the first verb
  px->arc(ph, cx2, cy2, 52.0f, 0.0f, kTwoPi);     // outer
  px->arc(ph, cx2, cy2, 30.0f, 0.0f, kTwoPi);     // inner -> cancels -> hole
  px->fill_path(ph, 0xFFF59E4E);

  // --- 3. Dashed, round-cap/round-join styled stroke -----------------------
  const float x3 = 470.0f;
  px->draw_text(ph, x3 - 10, 8, 200, 18, "dashed round stroke", 13.0f, 0xFFB8C0CC);
  const float dashes[2] = { 14.0f, 9.0f };
  neui_stroke_style_t style;
  memset(&style, 0, sizeof(style));
  style.cap         = NEUI_LINE_CAP_ROUND;
  style.join        = NEUI_LINE_JOIN_ROUND;
  style.miter_limit = 4.0f;
  style.dash_array  = dashes;
  style.dash_count  = 2;
  style.dash_offset = 0.0f;
  px->begin_path(ph);
  px->move_to(ph, x3, 110.0f);
  px->cubic_to(ph, x3 + 50, 20, x3 + 110, 150, x3 + 150, 70);
  px->stroke_path_styled(ph, 5.0f, 0xFF50C878, &style);
}

static bool NEUI_ABI on_event(void* /*token*/, neui_event_t* event)
{
  if (event->type == NEUI_EVENT_APP_QUIT) return true;
  if (event->type == NEUI_EVENT_WIDGET_PAINT) { paint_canvas(&event->data.paint); return true; }
  return false;
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
  neui_client_t client; client.neui_version = NEUI_VERSION; client.get_interface = get_interface;
  neui_session_t sess = host->create_session(&client, &app);
  if (!sess.session) return 1;
  app.widgets = (neui_widget_api_t*)host->get_interface(sess, NEUI_API_WIDGETS);

  neui_widget_t win = app.widgets->create(sess, neui_widget_t{ UINT32_MAX },
                                          NEUI_W_APPWINDOW, 120, 120, 960, 360, nullptr);
  app.widgets->set_text(sess, win, "neui path example - curves, even-odd, dashes");
  neui_widget_t canvas = app.widgets->create(sess, win, NEUI_W_CUSTOMDRAW, 0, 0, 960, 360, nullptr);
  (void)canvas;

  app.widgets->show(sess, win);
  host->run(sess);
  host->destroy(sess);
  return 0;
}
