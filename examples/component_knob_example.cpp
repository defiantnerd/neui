// component_knob_example - the declarative "component" format end-to-end.
//
// Loads a knob archetype from components/knob.json (compound visual + behavior
// input + default params), then instantiates four knobs from it - two via the
// one-call widgets->create_from_component, two via widgets->create + set_asset
// (the COMPONENT-aware attach-to-existing path). Per-knob values are set with
// the ordinary attr API. The knob's rotating pointer is an injected SURFACE
// asset resolved by name through neui_component_env_t::resolve_asset, so the
// example ships no binary image. Finally it re-serializes the loaded component
// back to JSON (assets->serialize_component) and logs it - the designer
// round-trip.

#include "neui/neui.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <string>
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
  char buf[2048];
  va_list args; va_start(args, fmt);
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
  neui_attr_api_t*   attrs   = nullptr;
  neui_session_t     session = { 0 };
  neui_asset_t       indicator = asset_none;  // injected pointer surface
  neui_asset_t       knob      = asset_none;  // the loaded component
};

// Paint the knob pointer into an 80x80 surface: a triangle pointing "up"
// (12 o'clock) plus a centre hub. Transparent elsewhere so it composites over
// the knob disc. The component rotates this around its centre by the value.
static void NEUI_ABI paint_indicator(neui_painter* p, neui_painter_api* px,
                                     float w, float h, void* /*user*/)
{
  float cx = w * 0.5f, cy = h * 0.5f;
  px->begin_path(p);
  px->move_to(p, cx, h * 0.12f);
  px->line_to(p, cx - 5.5f, cy);
  px->line_to(p, cx + 5.5f, cy);
  px->close_path(p);
  px->fill_path(p, 0xFFEAF2FF);
  px->begin_path(p);
  px->arc(p, cx, cy, 7.0f, 0.0f, 6.2831853f);
  px->fill_path(p, 0xFF87AAD0);
}

// Resolve the component's "indicator" asset name to the injected surface.
static neui_asset_t NEUI_ABI resolve_asset(void* user, const char* name,
                                           const char* /*hint_path*/)
{
  AppState* a = static_cast<AppState*>(user);
  if (name && strcmp(name, "indicator") == 0) return a->indicator;
  return asset_none;  // fall through to path mode for anything else
}

static bool NEUI_ABI on_event(void* /*token*/, neui_event_t* event)
{
  if (event->type == NEUI_EVENT_APP_QUIT) return true;
  return false;  // the compound + behavior assets handle paint + input
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
  if (!host) { dbglog("[component] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;

  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[component] no session\n"); return 1; }

  app.widgets = (neui_widget_api_t*)host->get_interface(app.session, NEUI_API_WIDGETS);
  app.assets  = (neui_asset_api_t*) host->get_interface(app.session, NEUI_API_ASSETS);
  app.attrs   = (neui_attr_api_t*)  host->get_interface(app.session, NEUI_API_ATTRS);
  if (!app.widgets || !app.assets || !app.attrs) { dbglog("[component] missing API\n"); return 1; }

  // Build the injected indicator surface (the knob's rotating pointer).
  app.indicator = app.assets->create_surface(app.session, 80.0f, 80.0f, 1.0f);
  if (app.indicator.id != asset_none.id)
    app.assets->paint_surface(app.session, app.indicator, 0x00000000,
                              paint_indicator, &app);

  // Load the component archetype once.
  neui_component_env_t env;
  env.base_dir      = nullptr;          // _from_file defaults it to the .json dir
  env.resolve_asset = resolve_asset;    // inject "indicator" -> the surface
  env.user          = &app;
  app.knob = app.assets->create_component_from_file(app.session,
                                                    "components/knob.json", &env);
  if (app.knob.id == asset_none.id)
    dbglog("[component] FAILED to load components/knob.json\n");

  // Designer round-trip: re-serialize the loaded component back to JSON.
  if (app.knob.id != asset_none.id) {
    uint32_t n = app.assets->serialize_component(app.session, app.knob, nullptr, 0, 2);
    std::string buf(n + 1, '\0');
    app.assets->serialize_component(app.session, app.knob, &buf[0], n + 1, 2);
    dbglog("[component] knob.json re-serialized via serialize_component:\n%s\n",
           buf.c_str());
  }

  neui_widget_t win = app.widgets->create(app.session, neui_widget_t{ UINT32_MAX },
                                          NEUI_W_APPWINDOW, 120, 120, 580, 240, nullptr);
  app.widgets->set_text(app.session, win, "neui component (knob) example");

  neui_widget_t lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                          16, 12, 548, 20, nullptr);
  app.widgets->set_text(app.session, lbl,
    "4 knobs from one knob.json - left 2 via create_from_component, right 2 via create + set_asset");

  const char* names[4] = { "Cutoff", "Reso", "Drive", "Mix" };
  float       vals [4] = { 0.30f, 0.60f, 0.50f, 0.80f };
  for (int i = 0; i < 4 && app.knob.id != asset_none.id; ++i) {
    int x = 24 + i * 135;
    neui_widget_t k;
    if (i < 2) {
      // Path A: one call (size from the component default).
      k = app.widgets->create_from_component(app.session, win, app.knob, x, 44, 0, 0);
    } else {
      // Path B: create a CUSTOMDRAW, then attach the component via set_asset.
      k = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW, x, 44, 110, 110, nullptr);
      app.widgets->set_asset(app.session, k, app.knob);
    }
    app.attrs->set_string(app.session, k, "name", names[i]);
    app.attrs->set_float (app.session, k, NEUI_PARAM_VALUE,   vals[i]);
    app.attrs->set_float (app.session, k, NEUI_PARAM_DEFAULT, 0.5f);
  }

  app.widgets->show(app.session, win);
  host->run(app.session);

  if (app.knob.id != asset_none.id)
    app.assets->destroy(app.session, app.knob);       // releases compound + behavior
  if (app.indicator.id != asset_none.id)
    app.assets->destroy(app.session, app.indicator);
  host->destroy(app.session);
  return 0;
}
