// arc_example - the value-driven arc compound layer (NEUI_COMPOUND_LAYER_ARC).
// Four CUSTOMDRAW widgets, each backed by a compound with a faint full-range
// "track" arc (static value 1.0) plus an "active" arc whose swept fraction is
// BOUND to NEUI_PARAM_VALUE - the same shape asvglib emits for a `dynamic` arc.
//
//   1. circular ring, knob-style 270 deg sweep, polarity min
//   2. elliptical ring (width != height), full 360 deg
//   3. filled wedge / pie, polarity min
//   4. circular ring, polarity center (bipolar) - fills left/right of 12 o'clock
//
// Built imperatively through the public compound API (the same calls the
// component loader drives from JSON).

#include "neui/neui.h"
#include <string.h>

#ifdef WIN32
#define ACTIVE_HOST "neui.host.win32"
#elif defined(__APPLE__)
#define ACTIVE_HOST "neui.host.macos"
#else
#define ACTIVE_HOST "neui.host.crossplatform"
#endif

struct AppState { int unused = 0; };

static bool NEUI_ABI on_event(void*, neui_event_t* e)
{
  return e->type == NEUI_EVENT_APP_QUIT;
}

static void* NEUI_ABI get_interface(void*, const char* iface)
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

  auto* widgets  = (neui_widget_api_t*)   host->get_interface(sess, NEUI_API_WIDGETS);
  auto* attrs    = (neui_attr_api_t*)     host->get_interface(sess, NEUI_API_ATTRS);
  auto* assets   = (neui_asset_api_t*)    host->get_interface(sess, NEUI_API_ASSETS);
  auto* compound = (neui_compound_api_t*) host->get_interface(sess, NEUI_API_COMPOUND);
  if (!widgets || !attrs || !assets || !compound) return 1;

  neui_widget_t win = widgets->create(sess, neui_widget_t{ UINT32_MAX },
                                      NEUI_W_APPWINDOW, 120, 120, 880, 260, nullptr);
  widgets->set_text(sess, win, "neui arc example - value-driven arc / ring / pie layers");

  // Build one widget: a CUSTOMDRAW carrying a track arc + a value-bound active
  // arc. `fill` selects pie vs ring. Returns nothing; attaches the compound.
  auto make_arc = [&](int x, int y, int w, int h, float value,
                      int polarity, float begin_deg, float end_deg,
                      bool pie, uint32_t color) {
    neui_widget_t cw = widgets->create(sess, win, NEUI_W_CUSTOMDRAW, x, y, w, h, nullptr);
    attrs->set_float(sess, cw, NEUI_PARAM_VALUE, value);

    neui_asset_t cs = assets->create_compound(sess);

    // Faint full-range track (static value 1.0) - a thin ring showing the range.
    neui_compound_layer_t track = compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_ARC, 0);
    compound->set_anchor(sess, cs, track, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
    compound->set_int  (sess, cs, track, "stroke_color", static_cast<int>(0xFF3A3F47u));
    compound->set_float(sess, cs, track, "stroke_width", 4.0f);
    compound->set_float(sess, cs, track, "begin_angle",  begin_deg);
    compound->set_float(sess, cs, track, "end_angle",    end_deg);
    compound->set_float(sess, cs, track, "value",        1.0f);

    // Active arc - swept fraction bound to NEUI_PARAM_VALUE.
    neui_compound_layer_t arc = compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_ARC, 1);
    compound->set_anchor(sess, cs, arc, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
    compound->set_float (sess, cs, arc, "begin_angle", begin_deg);
    compound->set_float (sess, cs, arc, "end_angle",   end_deg);
    compound->set_int   (sess, cs, arc, "polarity",    polarity);
    if (pie) {
      compound->set_int(sess, cs, arc, "fill_color", static_cast<int>(color));
    } else {
      compound->set_int  (sess, cs, arc, "stroke_color", static_cast<int>(color));
      compound->set_float(sess, cs, arc, "stroke_width", 12.0f);
      compound->set_int  (sess, cs, arc, "stroke_cap",   1);  // round
    }
    compound->bind(sess, cs, arc, "value", NEUI_PARAM_VALUE, 1.0f, 0.0f);

    widgets->set_asset(sess, cw, cs);
  };

  const int Y = 24, S = 200;
  //        x      y  w    h    value polarity begin   end    pie    color
  make_arc( 20,    Y, S,   S,   0.70f, 0,      -135.f, 135.f, false, 0xFF4E9CF5u); // ring
  make_arc( 240,   Y, S,   140, 0.62f, 0,         0.f, 360.f, false, 0xFF50C878u); // ellipse ring
  make_arc( 460,   Y, S,   S,   0.45f, 0,      -135.f, 135.f, true,  0xFFF59E4Eu); // pie
  make_arc( 660,   Y, S,   S,   0.25f, 1,      -135.f, 135.f, false, 0xFFE05A8Au); // center polarity

  widgets->show(sess, win);
  host->run(sess);
  host->destroy(sess);
  return 0;
}
