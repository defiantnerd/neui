// neui-on-LVGL prototype example + Milestone 3 measurement screen
// (plans/lvgl-host-approach-c.md). A knob-heavy audio panel - the profile
// that dominates embedded render cost: 8 painted rotary KNOBs with live
// value labels inside a SECTION, channel BUTTONs, a Bright CHECKBOX, a
// preset-name INPUTBOX and a description text block.
//
// Runs on the crossplatform host, which the NEUI_WITH_LVGL build pairs with
// the LVGL platform + backend. Console app: LVGL's perf monitor runs in
// LOG_MODE and prints "sysmon: N FPS ..." lines per second.
//
// Measurement hooks:
//   - idle:        just leave the window alone (no sysmon lines = 0 refresh)
//   - knob drag:   drag any knob (or drive synthetic WM_MOUSE* input)
//   - full-screen: press 'S' (or click [Stress]) - every knob value animates
//                  every frame via WIDGET_PREUPDATE, invalidating the whole
//                  panel continuously until toggled off.

#include "neui/neui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const char* k_knob_names[8] = {
  "Gain", "Bass", "Middle", "Treble", "Presence", "Reverb", "Volume", "Master"
};

struct AppState {
  neui_api_t*        neui    = nullptr;
  neui_widget_api_t* widgets = nullptr;
  neui_attr_api_t*   attrs   = nullptr;
  neui_session_t     session = { 0 };

  uint32_t win_id    = 0;
  uint32_t knob_id[8]  = {};
  uint32_t vlabel_id[8] = {};
  uint32_t stress_button_id = 0;
  uint32_t chan_button_id[3] = {};
  uint32_t chan_label_id = 0;

  bool     stress = false;
  uint32_t stress_phase = 0;
};

static void set_knob_value_text(AppState* app, int i, float v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%s %d%%", k_knob_names[i], (int)(v * 100.0f + 0.5f));
  app->widgets->set_text(app->session, { app->vlabel_id[i] }, buf);
}

static bool NEUI_ABI onevent(void* token, neui_event_t* event)
{
  auto* app = static_cast<AppState*>(token);
  if (!app) return false;

  switch (event->type) {

    case NEUI_EVENT_APP_QUIT:
      return true;  // allow close

    case NEUI_EVENT_VALUE_CHANGED: {
      // A knob was dragged - mirror the value into its label (this is the
      // per-widget invalidation path Milestone 2 bounds to two rects).
      uint32_t wid = event->data.value.widget.id;
      for (int i = 0; i < 8; ++i) {
        if (wid == app->knob_id[i]) {
          set_knob_value_text(app, i, event->data.value.value);
          return true;
        }
      }
      return false;
    }

    case NEUI_EVENT_WIDGET_PREUPDATE: {
      // Full-screen stress animation: advance every knob a little each
      // frame. Gated on knob[0] so the phase advances once per paint pass.
      // (PREUPDATE carries the raw tree slot - mask the session half.)
      if (!app->stress ||
          (event->data.preupdate.widget.id & 0xFFFFu) != (app->knob_id[0] & 0xFFFFu))
        return false;
      app->stress_phase++;
      for (int i = 0; i < 8; ++i) {
        float v = 0.5f + 0.45f * sinf((float)app->stress_phase * 0.05f
                                      + (float)i * 0.7f);
        app->attrs->set_float(app->session, { app->knob_id[i] },
                              NEUI_PARAM_VALUE, v);
        set_knob_value_text(app, i, v);
      }
      // Keep the repaint loop alive (the invalidation lands after this
      // paint pass completes).
      app->widgets->invalidate(app->session, { app->win_id });
      return true;
    }

    case NEUI_EVENT_MOUSE_BUTTON_CLICK: {
      uint32_t wid = event->data.mouse.widget.id;
      if (wid == app->stress_button_id) {
        app->stress = !app->stress;
        app->widgets->set_text(app->session, { app->stress_button_id },
                               app->stress ? "Stress: ON" : "Stress: OFF");
        if (app->stress)
          app->widgets->invalidate(app->session, { app->win_id });
        return true;
      }
      for (int c = 0; c < 3; ++c) {
        if (wid == app->chan_button_id[c]) {
          static const char* names[3] = { "Channel: Clean", "Channel: Crunch",
                                          "Channel: Lead" };
          app->widgets->set_text(app->session, { app->chan_label_id }, names[c]);
          return true;
        }
      }
      return false;
    }

    case NEUI_EVENT_KEYDOWN: {
      // 'S' toggles the stress animation from anywhere.
      if (event->data.key.keycode == 'S') {
        app->stress = !app->stress;
        app->widgets->set_text(app->session, { app->stress_button_id },
                               app->stress ? "Stress: ON" : "Stress: OFF");
        if (app->stress)
          app->widgets->invalidate(app->session, { app->win_id });
        return true;
      }
      return false;
    }

    default:
      return false;
  }
}

static void NEUI_ABI ondestroy(void*, neui_widget_t, void*) {}

static neui_widget_client_t widget_client = { NEUI_VERSION, ondestroy, onevent };

static void* NEUI_ABI get_interface(void*, const char* iface)
{
  if (iface && strcmp(iface, NEUI_API_WIDGETS) == 0) return &widget_client;
  return nullptr;
}

static neui_client_t host_client = { NEUI_VERSION, get_interface };

int main()
{
  // Unbuffered stdout so the perf monitor's sysmon lines arrive in real time
  // when redirected to a file / pipe (measurement scripts read them live).
  setvbuf(stdout, nullptr, _IONBF, 0);

  AppState app;

  neui_init();
  app.neui = neui_get_api("neui.host.crossplatform");
  if (!app.neui) {
    fprintf(stderr, "crossplatform host not available\n");
    return 1;
  }

  neui_session_t sess = app.neui->create_session(&host_client, &app);
  app.session = sess;
  app.widgets = (neui_widget_api_t*)app.neui->get_interface(sess, NEUI_API_WIDGETS);
  app.attrs   = (neui_attr_api_t*)  app.neui->get_interface(sess, NEUI_API_ATTRS);
  if (!app.widgets || !app.attrs) return 1;

  // 800x480 logical client - the embedded reference panel size.
  auto win = app.widgets->create(sess, widget_none, NEUI_W_APPWINDOW,
                                 120, 120, 800, 480, nullptr);
  app.win_id = win.id;
  app.widgets->set_text(sess, win, "neui LVGL prototype - amp panel");

  // --- Top band: title, channel buttons, bright checkbox, preset name ------
  auto title = app.widgets->create(sess, win, NEUI_W_LABEL, 12, 10, 240, 22, nullptr);
  app.widgets->set_text(sess, title, "TubeAmp 800 - Edit");

  static const char* chan_names[3] = { "Clean", "Crunch", "Lead" };
  for (int c = 0; c < 3; ++c) {
    auto b = app.widgets->create(sess, win, NEUI_W_BUTTON,
                                 260 + c * 78, 8, 70, 26, nullptr);
    app.widgets->set_text(sess, b, chan_names[c]);
    app.chan_button_id[c] = b.id;
  }

  auto bright = app.widgets->create(sess, win, NEUI_W_CHECKBOX, 508, 12, 80, 20, nullptr);
  app.widgets->set_text(sess, bright, "Bright");

  auto preset = app.widgets->create(sess, win, NEUI_W_INPUTBOX, 600, 8, 188, 26, nullptr);
  app.widgets->set_text(sess, preset, "Lead Solo 4");

  auto chan_label = app.widgets->create(sess, win, NEUI_W_LABEL, 12, 40, 240, 18, nullptr);
  app.widgets->set_text(sess, chan_label, "Channel: Clean");
  app.chan_label_id = chan_label.id;

  auto stress = app.widgets->create(sess, win, NEUI_W_BUTTON, 688, 40, 100, 24, nullptr);
  app.widgets->set_text(sess, stress, "Stress: OFF");
  app.stress_button_id = stress.id;

  // --- Amp section: 8 knobs in two rows, value label under each ------------
  auto section = app.widgets->create(sess, win, NEUI_W_SECTION, 10, 70, 780, 330, nullptr);
  app.widgets->set_text(sess, section, "AMPLIFIER");

  const int kw = 120, kh = 110;        // knob bounds (incl. value-text strip)
  const int cell_w = 780 / 4;
  for (int i = 0; i < 8; ++i) {
    const int col = i % 4, row = i / 4;
    const int cx  = col * cell_w + (cell_w - kw) / 2;
    const int cy  = 10 + row * 150;

    auto k = app.widgets->create(sess, section, NEUI_W_KNOB, cx, cy, kw, kh, nullptr);
    app.knob_id[i] = k.id;
    const float v0 = 0.25f + 0.07f * (float)i;
    app.attrs->set_float(sess, k, NEUI_PARAM_DEFAULT, 0.5f);
    app.attrs->set_float(sess, k, NEUI_PARAM_VALUE,   v0);

    auto vl = app.widgets->create(sess, section, NEUI_W_LABEL,
                                  cx + 10, cy + kh + 2, kw - 20, 16, nullptr);
    app.vlabel_id[i] = vl.id;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d%%", k_knob_names[i], (int)(v0 * 100.0f + 0.5f));
    app.widgets->set_text(sess, vl, buf);
  }

  // --- Bottom verification strip: IMAGE widget + font-size / family checks --
  // The image box is exactly the source's 4:3 aspect (myimage.png is 500x375),
  // so the aspect-preserving fit must fill the box edge to edge - any
  // letterboxing or stretching flags a draw_bitmap scaling bug.
  auto image = app.widgets->create(sess, win, NEUI_W_IMAGE, 12, 402, 96, 72, nullptr);
  {
    char path[MAX_PATH] = "myimage.png";
#ifdef _WIN32
    // Resolve next to the executable so the example works from any CWD.
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      char* slash = strrchr(exe, '\\');
      if (slash) {
        snprintf(path, sizeof(path), "%.*s\\myimage.png",
                 (int)(slash - exe), exe);
      }
    }
#endif
    app.widgets->set_text(sess, image, path);
  }

  // Font-size checks: Segoe UI's cap height is 0.700 em, so the ink height of
  // an all-caps string must be ~0.70x the requested NEUI_ATTR_FONT_SIZE
  // (12 -> ~8.4 px, 16 -> ~11.2, 22 -> ~15.4, 32 -> ~22.4). The last label
  // verifies family resolution (Consolas is visibly monospaced).
  static const struct { int size; int x; int w; } k_font_rows[] = {
    { 12, 150,  70 }, { 16, 230,  80 }, { 22, 320, 100 }, { 32, 430, 130 },
  };
  for (const auto& fr : k_font_rows) {
    auto l = app.widgets->create(sess, win, NEUI_W_LABEL,
                                 fr.x, 466 - (int)(fr.size * 1.35f),
                                 fr.w, (int)(fr.size * 1.35f) + 4, nullptr);
    char buf[24];
    snprintf(buf, sizeof(buf), "HHH %d", fr.size);
    app.widgets->set_text(sess, l, buf);
    app.attrs->set_float(sess, l, NEUI_ATTR_FONT_SIZE, (float)fr.size);
  }
  auto mono = app.widgets->create(sess, win, NEUI_W_LABEL, 580, 444, 200, 24, nullptr);
  app.widgets->set_text(sess, mono, "Consolas iiiWWW");
  app.attrs->set_float(sess, mono, NEUI_ATTR_FONT_SIZE, 16.0f);
  app.attrs->set_string(sess, mono, NEUI_ATTR_FONT_FAMILY, "Consolas");

  app.widgets->show(sess, win);
  bool ok = app.neui->run(sess);
  app.neui->endsession(sess);
  return ok ? 0 : 1;
}
