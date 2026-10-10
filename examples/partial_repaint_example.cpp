// partial_repaint_example - simulated DAW automation over a grid of knobs,
// to see and measure partial repaint on the crossplatform (xpl) host.
//
// A 8x6 grid of compound "arc" knobs (CUSTOMDRAW + ARC layers bound to
// NEUI_PARAM_VALUE). The client drives its own loop via pump_once and, every
// ~16 ms, writes NEUI_PARAM_VALUE on a few knobs - the way a plugin mirrors
// host automation. Each write invalidates only that knob, so the xpl host
// (Linux / Cairo, macOS + iOS / CG, win32 / D2D) repaints a handful of rects
// per frame instead of all 48 knobs.
//
// Once a second it prints (and shows in the status line) how long pump_once
// took - the paint happens inside it - so the two modes can be compared:
//
//   ./neui_partial_repaint_example [knobs-per-tick]          partial frames
//   NEUI_DEBUG_FULL_REPAINT=1 ./neui_partial_repaint_example  full frames
//   NEUI_DEBUG_DAMAGE=1 ./neui_partial_repaint_example        tint the damage
//
// With NEUI_DEBUG_DAMAGE the repainted rects flash magenta / cyan; the rest of
// the window must stay intact - stale or missing pixels there are the bug.
//
// Right of the grid an IMAGE (a missing file, so it shows its placeholder)
// slowly rotates via NEUI_ATTR_ROTATION. A rotated IMAGE paints beyond its own
// rect, so this checks that its damage covers the overflow: no smeared trail,
// and the knobs it sweeps over stay intact.

#include "neui/neui.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct AppState
{
  bool quit = false;
};

bool NEUI_ABI on_event(void* token, neui_event_t* e)
{
  if (e->type == NEUI_EVENT_APP_QUIT) {
    static_cast<AppState*>(token)->quit = true;
    return true;
  }
  return false;
}

void* NEUI_ABI get_interface(void*, const char* iface)
{
  static neui_widget_client_t wc;
  if (!std::strcmp(iface, NEUI_API_WIDGETS)) {
    wc.neui_version = NEUI_VERSION; wc.ondestroy = nullptr; wc.onevent = on_event;
    return &wc;
  }
  return nullptr;
}

} // namespace

int main(int argc, char* argv[])
{
  int per_tick = (argc > 1) ? std::atoi(argv[1]) : 4;
  if (per_tick < 1) per_tick = 1;

  neui_init();
  // Partial repaint is an xpl-host feature - always use it here.
  neui_api_t* host = neui_get_api("neui.host.crossplatform");
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

  // Layout: COLS x ROWS knobs of KNOB px with GAP between and MARGIN around,
  // plus a status line below. The window is sized from that content.
  const int COLS = 8, ROWS = 6, KNOB = 80, GAP = 12, MARGIN = 12, STATUS_H = 24;
  const int grid_w = COLS * KNOB + (COLS - 1) * GAP;
  const int grid_h = ROWS * KNOB + (ROWS - 1) * GAP;
  const int IMG_W = 120, IMG_H = 70;   // rotating IMAGE right of the grid
  const int win_w  = MARGIN + grid_w + GAP + IMG_W + MARGIN;
  const int win_h  = MARGIN + grid_h + GAP + STATUS_H + MARGIN;

  neui_widget_t win = widgets->create(sess, neui_widget_t{ UINT32_MAX },
                                      NEUI_W_APPWINDOW, 120, 120, win_w, win_h, nullptr);
  widgets->set_text(sess, win, "neui partial repaint - simulated automation");

  // One shared compound for every knob: a faint track ring plus an active arc
  // whose sweep is bound to NEUI_PARAM_VALUE (per-widget AttrBag).
  neui_asset_t knob = assets->create_compound(sess);
  neui_compound_layer_t track = compound->add_layer(sess, knob, NEUI_COMPOUND_LAYER_ARC, 0);
  compound->set_anchor(sess, knob, track, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
  compound->set_int  (sess, knob, track, "stroke_color", static_cast<int>(0xFF3A3F47u));
  compound->set_float(sess, knob, track, "stroke_width", 4.0f);
  compound->set_float(sess, knob, track, "begin_angle",  -135.0f);
  compound->set_float(sess, knob, track, "end_angle",     135.0f);
  compound->set_float(sess, knob, track, "value",         1.0f);
  neui_compound_layer_t arc = compound->add_layer(sess, knob, NEUI_COMPOUND_LAYER_ARC, 1);
  compound->set_anchor(sess, knob, arc, NEUI_ANCHOR_CENTER, NEUI_ANCHOR_CENTER);
  compound->set_int  (sess, knob, arc, "stroke_color", static_cast<int>(0xFF4E9CF5u));
  compound->set_float(sess, knob, arc, "stroke_width", 9.0f);
  compound->set_int  (sess, knob, arc, "stroke_cap",   1);   // round
  compound->set_float(sess, knob, arc, "begin_angle",  -135.0f);
  compound->set_float(sess, knob, arc, "end_angle",     135.0f);
  compound->bind     (sess, knob, arc, "value", NEUI_PARAM_VALUE, 1.0f, 0.0f);

  std::vector<neui_widget_t> knobs;
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      neui_widget_t w = widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                                        MARGIN + c * (KNOB + GAP), MARGIN + r * (KNOB + GAP),
                                        KNOB, KNOB, nullptr);
      attrs->set_float(sess, w, NEUI_PARAM_VALUE,
                       0.5f + 0.4f * std::sin(static_cast<float>(r * COLS + c)));
      widgets->set_asset(sess, w, knob);
      knobs.push_back(w);
    }
  }

  neui_widget_t spinner = widgets->create(sess, win, NEUI_W_IMAGE,
                                          MARGIN + grid_w + GAP, MARGIN + (grid_h - IMG_H) / 2,
                                          IMG_W, IMG_H, nullptr);
  widgets->set_text(sess, spinner, "no-such-image.png");
  attrs->set_float(sess, spinner, NEUI_ATTR_ROTATION, 0.0f);

  neui_widget_t status = widgets->create(sess, win, NEUI_W_LABEL,
                                         MARGIN, MARGIN + grid_h + GAP, grid_w, STATUS_H, nullptr);
  widgets->set_text(sess, status, "measuring...");
  widgets->show(sess, win);

  using clock = std::chrono::steady_clock;
  const auto start     = clock::now();
  auto       last_tick = start;
  auto       last_report = start;
  double     pump_ms   = 0.0;     // time inside pump_once this second
  int        ticks     = 0;       // automation ticks this second
  unsigned   step      = 0;

  while (!app.quit) {
    const auto p0 = clock::now();
    if (!host->pump_once(sess)) break;
    pump_ms += std::chrono::duration<double, std::milli>(clock::now() - p0).count();

    const auto now = clock::now();
    if (now - last_tick >= std::chrono::milliseconds(16)) {
      last_tick = now;
      ++ticks;
      const float t = std::chrono::duration<float>(now - start).count();
      // A few knobs per tick, walking through the grid so every one moves.
      for (int k = 0; k < per_tick; ++k) {
        const size_t i = (step * 7u + static_cast<unsigned>(k) * 13u) % knobs.size();
        attrs->set_float(sess, knobs[i], NEUI_PARAM_VALUE,
                         0.5f + 0.45f * std::sin(t * 2.0f + static_cast<float>(i)));
      }
      attrs->set_float(sess, spinner, NEUI_ATTR_ROTATION, t * 0.8f);
      ++step;
    }

    if (now - last_report >= std::chrono::seconds(1)) {
      char line[160];
      std::snprintf(line, sizeof(line),
                    "%d knobs/tick, %d ticks/s: %.1f ms/s in pump_once (%.2f ms per tick)",
                    per_tick, ticks, pump_ms, ticks ? pump_ms / ticks : 0.0);
      std::printf("%s\n", line);
      std::fflush(stdout);
      widgets->set_text(sess, status, line);
      last_report = now;
      pump_ms = 0.0;
      ticks   = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  host->destroy(sess);
  return 0;
}
