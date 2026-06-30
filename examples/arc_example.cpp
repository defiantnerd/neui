// arc_example - the value-driven compound layers that "render `value` of me".
// Top row: the arc layer (NEUI_COMPOUND_LAYER_ARC). Bottom row: the same
// value-driven model on a PATH stroke trim (§A) and a RECT fill bar (§B).
// Every widget pairs a faint full-range "track" (static value 1.0) with an
// "active" layer whose fraction is BOUND to NEUI_PARAM_VALUE - the shape
// asvglib emits for a `dynamic` arc / line / rect.
//
//   Arcs (top):
//     1. circular ring, knob-style 270 deg sweep, polarity min
//     2. elliptical ring (width != height), full 360 deg
//     3. filled wedge / pie, polarity min
//     4. circular ring, polarity center (bipolar) - fills left/right of 12 o'clock
//   Trim + bar (bottom):
//     5. horizontal line, stroke-trimmed from the start (polarity min)       §A
//     6. horizontal line, stroke-trimmed from the centre (polarity center)   §A
//     7. horizontal rect bar, fill from the left edge (polarity min)         §B
//     8. vertical   rect bar, fill from the bottom edge (polarity max)       §B
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
                                      NEUI_W_APPWINDOW, 120, 120, 880, 420, nullptr);
  widgets->set_text(sess, win, "neui arc example - value-driven arc / line-trim / rect-bar layers");

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

  // §A - value-driven PATH stroke trim. A horizontal line spanning the widget;
  // a faint full-length track plus an active stroke trimmed to NEUI_PARAM_VALUE
  // of the path's arc length, anchored by `polarity`. PATH coords are
  // layer-local (the layer fills the widget by default), so the line runs from
  // (0, h/2) to (w, h/2).
  auto make_line = [&](int x, int y, int w, int h, float value,
                       int polarity, uint32_t color) {
    neui_widget_t cw = widgets->create(sess, win, NEUI_W_CUSTOMDRAW, x, y, w, h, nullptr);
    attrs->set_float(sess, cw, NEUI_PARAM_VALUE, value);

    neui_asset_t cs = assets->create_compound(sess);
    neui_path_cmd_t line[2] = {};
    line[0].kind = NEUI_PATH_CMD_MOVE_TO; line[0].args[0] = 0.0f;
    line[0].args[1] = static_cast<float>(h) * 0.5f;
    line[1].kind = NEUI_PATH_CMD_LINE_TO; line[1].args[0] = static_cast<float>(w);
    line[1].args[1] = static_cast<float>(h) * 0.5f;

    // Faint full-length track (static value 1.0 = whole path, untrimmed).
    neui_compound_layer_t track = compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_PATH, 0);
    compound->set_anchor(sess, cs, track, NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT);
    compound->set_path (sess, cs, track, line, 2);
    compound->set_int  (sess, cs, track, "stroke_color", static_cast<int>(0xFF3A3F47u));
    compound->set_float(sess, cs, track, "stroke_width", 6.0f);
    compound->set_int  (sess, cs, track, "stroke_cap",   1);  // round

    // Active stroke - trimmed fraction bound to NEUI_PARAM_VALUE.
    neui_compound_layer_t active = compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_PATH, 1);
    compound->set_anchor(sess, cs, active, NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT);
    compound->set_path  (sess, cs, active, line, 2);
    compound->set_int   (sess, cs, active, "stroke_color", static_cast<int>(color));
    compound->set_float (sess, cs, active, "stroke_width", 12.0f);
    compound->set_int   (sess, cs, active, "stroke_cap",   1);  // round
    compound->set_int   (sess, cs, active, "polarity",     polarity);
    compound->bind      (sess, cs, active, "value", NEUI_PARAM_VALUE, 1.0f, 0.0f);

    widgets->set_asset(sess, cw, cs);
  };

  // §B - value-driven RECT fill (the linear bar / level meter). A faint track
  // rect (static full fill) plus a meter rect whose fill fraction is bound to
  // NEUI_PARAM_VALUE along `orientation`, anchored by `polarity`; the meter's
  // own stroke outlines the full rect as a border.
  auto make_bar = [&](int x, int y, int w, int h, float value,
                      int orientation, int polarity, uint32_t color) {
    neui_widget_t cw = widgets->create(sess, win, NEUI_W_CUSTOMDRAW, x, y, w, h, nullptr);
    attrs->set_float(sess, cw, NEUI_PARAM_VALUE, value);

    neui_asset_t cs = assets->create_compound(sess);

    // Faint track (static value 1.0 = full fill).
    neui_compound_layer_t track = compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_RECT, 0);
    compound->set_anchor(sess, cs, track, NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT);
    compound->set_int  (sess, cs, track, "fill_color",    static_cast<int>(0xFF2A2E34u));
    compound->set_float(sess, cs, track, "corner_radius", 5.0f);

    // Meter - value-driven fill + a full-rect border track.
    neui_compound_layer_t bar = compound->add_layer(sess, cs, NEUI_COMPOUND_LAYER_RECT, 1);
    compound->set_anchor(sess, cs, bar, NEUI_ANCHOR_TOP_LEFT, NEUI_ANCHOR_TOP_LEFT);
    compound->set_int  (sess, cs, bar, "fill_color",    static_cast<int>(color));
    compound->set_int  (sess, cs, bar, "stroke_color",  static_cast<int>(0xFF3A3F47u));
    compound->set_float(sess, cs, bar, "stroke_width",  2.0f);
    compound->set_float(sess, cs, bar, "corner_radius", 5.0f);
    compound->set_int  (sess, cs, bar, "orientation",   orientation);
    compound->set_int  (sess, cs, bar, "polarity",      polarity);
    compound->bind     (sess, cs, bar, "value", NEUI_PARAM_VALUE, 1.0f, 0.0f);

    widgets->set_asset(sess, cw, cs);
  };

  const int Y = 24, S = 200;
  //        x      y  w    h    value polarity begin   end    pie    color
  make_arc( 20,    Y, S,   S,   0.70f, 0,      -135.f, 135.f, false, 0xFF4E9CF5u); // ring
  make_arc( 240,   Y, S,   140, 0.62f, 0,         0.f, 360.f, false, 0xFF50C878u); // ellipse ring
  make_arc( 460,   Y, S,   S,   0.45f, 0,      -135.f, 135.f, true,  0xFFF59E4Eu); // pie
  make_arc( 660,   Y, S,   S,   0.25f, 1,      -135.f, 135.f, false, 0xFFE05A8Au); // center polarity

  // Bottom row: §A trimmed lines (left) + §B rect bars (right).
  const int Y2 = 250;
  //         x    y         w    h   value polarity  color
  make_line( 20,  Y2,       400, 40, 0.70f, 0,       0xFF4E9CF5u); // trim from start
  make_line( 20,  Y2 + 60,  400, 40, 0.50f, 1,       0xFFE05A8Au); // trim from centre
  //         x    y         w    h   value orient polarity  color
  make_bar ( 460, Y2,       380, 40, 0.70f, 0,     0,       0xFF50C878u); // horizontal, from left
  make_bar ( 600, Y2 + 60,  40,  90, 0.55f, 1,     2,       0xFFF59E4Eu); // vertical, from bottom

  widgets->show(sess, win);
  host->run(sess);
  host->destroy(sess);
  return 0;
}
