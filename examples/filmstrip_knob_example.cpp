// filmstrip_knob_example - a knob whose visual is a frame strip ("filmstrip"
// / "stitchmap"), the convention audio-plugin UIs use for rotary controls.
//
// Exercises the whole filmstrip stack end-to-end:
//   * The strip is generated procedurally into a SURFACE asset: one
//     paint_surface call draws all N knob frames (indicator swept across the
//     value range), one frame per vertical cell.
//   * asset_api->set_frame_layout tags that SURFACE as a 1xN vertical strip.
//   * A COMPOUND asset layer draws the strip with its "frame" prop BOUND to
//     NEUI_PARAM_VALUE (bind scale = N-1), so the knob value scrubs frames -
//     the whole strip uploads to the GPU once and each frame is a sub-rect
//     sample of that single upload.
//   * A BEHAVIOR (DRAG_ROTATIONAL + CONTEXT_RESET) drives the value attr, so
//     dragging the knob animates the strip and right-click resets it.
//
// Drag the knob: the indicator should sweep smoothly; the readout shows the
// value and the currently-displayed frame index.

#include "neui/neui.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <vector>

#ifdef WIN32
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOST "neui.host.macos"
#else
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

static void dbglog(const char* fmt, ...)
{
  char buf[512];
  va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  fputs(buf, stderr);
}

struct AppState
{
  neui_api_t*          neui     = nullptr;
  neui_widget_api_t*   widgets  = nullptr;
  neui_asset_api_t*    assets   = nullptr;
  neui_attr_api_t*     attrs    = nullptr;
  neui_compound_api_t* compound = nullptr;
  neui_behavior_api_t* behavior = nullptr;
  neui_session_t       session  = { 0 };

  neui_asset_t strip      = asset_none;   // the filmstrip SURFACE
  neui_asset_t comp       = asset_none;   // compound (visual)
  neui_asset_t behav      = asset_none;   // behavior (input)
  uint32_t     knob_id    = 0;
  uint32_t     readout_id = 0;

  int   frame_count = 64;
  float cell_px     = 96.0f;              // square cell, logical px
};

// paint_surface callback: draw all `frame_count` knob frames stacked
// vertically, one per cell. The painter here is the same curated API a
// WIDGET_PAINT handler gets. Surface logical size is (cell, cell*N).
static void NEUI_ABI draw_strip(neui_painter_t* p, neui_painter_api_t* api,
                                 float w, float /*h*/, void* user)
{
  auto* a = static_cast<AppState*>(user);
  const int   N    = a->frame_count;
  const float cell = w;                   // surface width == one cell
  const float cx   = cell * 0.5f;
  const float r    = cell * 0.36f;
  const float a0   = -2.3561945f;         // -135 deg (7 o'clock)
  const float a1   =  2.3561945f;         // +135 deg (5 o'clock)

  for (int i = 0; i < N; ++i) {
    const float cy = i * cell + cell * 0.5f;
    const float v  = (N > 1) ? (float)i / (float)(N - 1) : 0.0f;
    const float ang = a0 + v * (a1 - a0); // clockwise from 12 o'clock

    // Body disc.
    api->begin_path(p);
    api->arc(p, cx, cy, r, 0.0f, 6.2831853f);
    api->fill_path(p, 0xFF2B313D);
    // Rim.
    api->begin_path(p);
    api->arc(p, cx, cy, r, 0.0f, 6.2831853f);
    api->stroke_path(p, 2.0f, 0xFF11151C);

    // Value indicator: a line from centre toward the swept angle.
    const float tipx = cx + r * 0.86f * sinf(ang);
    const float tipy = cy - r * 0.86f * cosf(ang);
    api->begin_path(p);
    api->move_to(p, cx, cy);
    api->line_to(p, tipx, tipy);
    api->stroke_path(p, 3.0f, 0xFFE8B23A);

    // Centre hub.
    api->begin_path(p);
    api->arc(p, cx, cy, r * 0.12f, 0.0f, 6.2831853f);
    api->fill_path(p, 0xFFE8B23A);
  }
}

static void update_readout(AppState* a, float value)
{
  if (!a->readout_id) return;
  int frame = (a->frame_count > 1)
    ? (int)lroundf(value * (float)(a->frame_count - 1)) : 0;
  if (frame < 0) frame = 0;
  if (frame > a->frame_count - 1) frame = a->frame_count - 1;
  char buf[96];
  snprintf(buf, sizeof(buf), "value %.3f      frame %d / %d",
           (double)value, frame, a->frame_count - 1);
  a->widgets->set_text(a->session, { a->readout_id }, buf);
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  auto* a = static_cast<AppState*>(token);
  switch (event->type) {
    case NEUI_EVENT_APP_QUIT:
      return true;
    // The behavior asset writes NEUI_PARAM_VALUE and fires ATTR_CHANGED on
    // every drag / wheel / reset; mirror it into the readout label.
    case NEUI_EVENT_ATTR_CHANGED:
      if (event->data.attr.widget.id == a->knob_id) {
        update_readout(a, event->data.attr.value);
        return false;   // let the compound binding repaint too
      }
      break;
    default:
      break;
  }
  return false;   // compound + behavior assets handle paint + input
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
  if (!host) { dbglog("[filmstrip] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;

  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[filmstrip] no session\n"); return 1; }

  app.widgets  = (neui_widget_api_t*)  host->get_interface(app.session, NEUI_API_WIDGETS);
  app.assets   = (neui_asset_api_t*)   host->get_interface(app.session, NEUI_API_ASSETS);
  app.attrs    = (neui_attr_api_t*)    host->get_interface(app.session, NEUI_API_ATTRS);
  app.compound = (neui_compound_api_t*)host->get_interface(app.session, NEUI_API_COMPOUND);
  app.behavior = (neui_behavior_api_t*)host->get_interface(app.session, NEUI_API_BEHAVIOR);
  if (!app.widgets || !app.assets || !app.attrs || !app.compound || !app.behavior) {
    dbglog("[filmstrip] missing API\n"); return 1;
  }

  // 1. Generate the strip into a SURFACE: cell x (cell*N) logical pixels.
  const float strip_w = app.cell_px;
  const float strip_h = app.cell_px * (float)app.frame_count;
  app.strip = app.assets->create_surface(app.session, strip_w, strip_h, 2.0f);
  if (app.strip.id == asset_none.id) {
    dbglog("[filmstrip] surface unsupported (null backend?)\n");
  } else {
    app.assets->paint_surface(app.session, app.strip, 0x00000000, draw_strip, &app);
    // 2. Tag the SURFACE as a 1 x N vertical frame strip.
    if (!app.assets->set_frame_layout(app.session, app.strip, 1,
                                      (uint32_t)app.frame_count, 0))
      dbglog("[filmstrip] set_frame_layout failed\n");
    dbglog("[filmstrip] frame_count = %u\n",
           app.assets->get_frame_count(app.session, app.strip));
  }

  // 3. Compound visual: one asset layer filling the widget, frame bound to value.
  app.comp = app.assets->create_compound(app.session);
  if (app.comp.id != asset_none.id) {
    neui_compound_layer_t L =
      app.compound->add_layer(app.session, app.comp, NEUI_COMPOUND_LAYER_ASSET, 0);
    app.compound->set_anchor(app.session, app.comp, L,
                             NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
    app.compound->set_int(app.session, app.comp, L, "width",  NEUI_COMPOUND_FILL);
    app.compound->set_int(app.session, app.comp, L, "height", NEUI_COMPOUND_FILL);
    app.compound->set_asset(app.session, app.comp, L, "asset", app.strip);
    // value 0..1  ->  frame 0..(N-1)
    app.compound->bind(app.session, app.comp, L, "frame", NEUI_PARAM_VALUE,
                       (float)(app.frame_count - 1), 0.0f);
  }

  // 4. Behavior input: rotational drag on the value + right-click reset.
  app.behav = app.assets->create_behavior(app.session);
  if (app.behav.id != asset_none.id) {
    neui_behavior_handler_t drag =
      app.behavior->add_handler(app.session, app.behav, NEUI_BEHAVIOR_KIND_DRAG_ROTATIONAL);
    app.behavior->set_string(app.session, app.behav, drag, "target", NEUI_PARAM_VALUE);
    app.behavior->set_float (app.session, app.behav, drag, "min", 0.0f);
    app.behavior->set_float (app.session, app.behav, drag, "max", 1.0f);
    app.behavior->add_handler(app.session, app.behav, NEUI_BEHAVIOR_KIND_CONTEXT_RESET);
  }

  // Window + widgets.
  neui_widget_t win = app.widgets->create(app.session, neui_widget_t{ UINT32_MAX },
                                           NEUI_W_APPWINDOW, 120, 120, 360, 320, nullptr);
  app.widgets->set_text(app.session, win, "neui filmstrip knob");

  neui_widget_t hint = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                            20, 16, 320, 22, nullptr);
  app.widgets->set_text(app.session, hint,
                        "Drag the knob to scrub frames - right-click resets.");

  neui_widget_t knob = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                            120, 56, 120, 120, nullptr);
  app.knob_id = knob.id;
  if (app.comp.id  != asset_none.id) app.widgets->set_asset(app.session, knob, app.comp);
  if (app.behav.id != asset_none.id) app.widgets->set_asset(app.session, knob, app.behav);
  app.attrs->set_float(app.session, knob, NEUI_PARAM_DEFAULT, 0.5f);
  app.attrs->set_float(app.session, knob, NEUI_PARAM_VALUE,   0.5f);

  neui_widget_t readout = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                              20, 196, 320, 22, nullptr);
  app.readout_id = readout.id;
  update_readout(&app, 0.5f);

  app.widgets->show(app.session, win);
  host->run(app.session);

  if (app.behav.id != asset_none.id) app.assets->destroy(app.session, app.behav);
  if (app.comp.id  != asset_none.id) app.assets->destroy(app.session, app.comp);
  if (app.strip.id != asset_none.id) app.assets->destroy(app.session, app.strip);
  host->destroy(app.session);
  return 0;
}
