// font_loading_example - registers a client-supplied font (a file a plugin
// would ship in its resource folder) so its family is usable for text
// rendering WITHOUT installing it system-wide, then proves the family reaches
// BOTH text seams:
//
//   * Native-control prong: a BUTTON / INPUTBOX whose text is drawn by the
//     platform control class (win32 HFONT / macOS NSFont) picks up the family
//     via NEUI_ATTR_FONT_FAMILY.
//   * Painted prong: a CUSTOMDRAW widget draws text with the family via
//     painter->push_font (the same path used by every neui-backend-* draw).
//
// The font binds to NEUI_ASSET_KIND_FONT: assets->create_font_from_file
// returns an ordinary neui_asset_t that owns the registration lifetime; the
// client references the font by its family-name string thereafter, exactly
// like a system font. get_font_family discovers the internal family name.
//
// Bundling: a real plugin ships a .ttf next to its binary and loads it by a
// path relative to that binary. Since this repo ships no font binary, the
// example walks a candidate list - a bundled fonts/ file first, then a
// distinctive platform system font as a fallback so the demo always renders
// SOMETHING different from the default. Drop a .ttf at examples/fonts/ (and
// it is copied next to the executable by CMake) to exercise the true
// bundled-resource path.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // plain fopen for the portable in-memory smoke
#endif

#include "neui/neui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
  neui_attr_api_t*   attrs   = nullptr;
  neui_session_t     session = { 0 };

  uint32_t           canvas_id = 0;

  neui_asset_t       font   = asset_none;
  char               family[256] = { 0 };  // resolved family name; "" if none
};

// Candidate font files: bundled first, then a distinctive platform fallback.
static const char* const k_font_candidates[] = {
  "fonts/DemoFont.ttf",          // bundled (drop your own .ttf here)
  "DemoFont.ttf",
#ifdef _WIN32
  "C:\\Windows\\Fonts\\comic.ttf",   // Comic Sans - obviously not the default
  "C:\\Windows\\Fonts\\consolab.ttf",
#elif defined(__APPLE__)
  "/System/Library/Fonts/Supplemental/Comic Sans MS.ttf",
  "/System/Library/Fonts/Supplemental/Georgia.ttf",
  "/System/Library/Fonts/Menlo.ttc",
#else
  "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
  "/usr/share/fonts/dejavu/DejaVuSerif.ttf",
#endif
};

// Secondary smoke of the in-memory primitive: read the same file's bytes and
// register them via create_font, log the resolved family, then release that
// handle (the file-form registration keeps the family live). Proves the bytes
// path end-to-end without changing what the UI displays.
static void smoke_in_memory(AppState* a, const char* path)
{
  FILE* fp = fopen(path, "rb");
  if (!fp) return;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (n <= 0) { fclose(fp); return; }
  uint8_t* buf = (uint8_t*)malloc((size_t)n);
  if (!buf) { fclose(fp); return; }
  size_t got = fread(buf, 1, (size_t)n, fp);
  fclose(fp);
  if (got == (size_t)n) {
    neui_asset_t mem = a->assets->create_font(a->session, buf, (uint32_t)n);
    if (mem.id != asset_none.id) {
      char fam[256] = { 0 };
      a->assets->get_font_family(a->session, mem, fam, sizeof(fam));
      dbglog("[font_loading] create_font (in-memory) -> family \"%s\"\n", fam);
      a->assets->destroy(a->session, mem);
    } else {
      dbglog("[font_loading] create_font (in-memory) returned asset_none\n");
    }
  }
  free(buf);
}

static void load_font(AppState* a)
{
  for (const char* path : k_font_candidates) {
    neui_asset_t f = a->assets->create_font_from_file(a->session, path);
    if (f.id != asset_none.id) {
      a->font = f;
      a->assets->get_font_family(a->session, f, a->family, sizeof(a->family));
      dbglog("[font_loading] registered \"%s\" -> family \"%s\"\n",
             path, a->family);
      smoke_in_memory(a, path);
      return;
    }
  }
  dbglog("[font_loading] no candidate font could be registered "
         "(null backend? missing files?) - using host default.\n");
}

static void paint_canvas(neui_event_paint_t* p, AppState* a)
{
  auto* px = p->painter_api;
  auto* ph = p->p;

  px->fill_rect(ph, 0, 0, p->width, p->height, 0xFF1A2028);
  px->draw_rect(ph, 0.5f, 0.5f, p->width - 1.0f, p->height - 1.0f, 1.0f, 0xFF304050);

  // Reference line in the host default font.
  px->draw_text(ph, 16, 14, p->width - 32, 24,
                "CUSTOMDRAW painted text - host default font",
                16.0f, 0xFF90A0B0);

  // The same string in the registered family, via push_font (painted prong).
  if (a->family[0]) {
    px->push_font(ph, a->family, 0);     // weight 0 = Normal
    px->draw_text(ph, 16, 48, p->width - 32, 36,
                  "CUSTOMDRAW painted text - registered font",
                  26.0f, 0xFFE8F0FF);
    // And a bold weight, proving weight selection on the same family.
    px->push_font(ph, a->family, 700);
    px->draw_text(ph, 16, 92, p->width - 32, 32,
                  "...and the same family at weight 700 (Bold)",
                  20.0f, 0xFFC8D4E4);
    px->pop_font(ph);
    px->pop_font(ph);
  } else {
    px->draw_text(ph, 16, 48, p->width - 32, 24,
                  "(no font registered - nothing extra to show)",
                  16.0f, 0xFFE08080);
  }
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
  if (!host) { dbglog("[font_loading] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;

  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[font_loading] no session\n"); return 1; }

  app.widgets = (neui_widget_api_t*)host->get_interface(app.session, NEUI_API_WIDGETS);
  app.assets  = (neui_asset_api_t*) host->get_interface(app.session, NEUI_API_ASSETS);
  app.attrs   = (neui_attr_api_t*)  host->get_interface(app.session, NEUI_API_ATTRS);
  if (!app.widgets || !app.assets || !app.attrs) {
    dbglog("[font_loading] missing API\n"); return 1;
  }

  load_font(&app);

  neui_widget_t win = app.widgets->create(app.session,
                                          neui_widget_t{ UINT32_MAX },
                                          NEUI_W_APPWINDOW,
                                          120, 120, 760, 460, nullptr);
  app.widgets->set_text(app.session, win, "neui font-loading example");

  // Status label naming the registered family.
  char status[320];
  if (app.family[0])
    snprintf(status, sizeof(status), "Registered family: \"%s\"", app.family);
  else
    snprintf(status, sizeof(status), "No client font registered (host default in use)");
  neui_widget_t lbl = app.widgets->create(app.session, win, NEUI_W_LABEL,
                                          16, 16, 720, 22, nullptr);
  app.widgets->set_text(app.session, lbl, status);

  // Native-control prong: a BUTTON + INPUTBOX in the registered family.
  neui_widget_t btn = app.widgets->create(app.session, win, NEUI_W_BUTTON,
                                          16, 52, 360, 40, nullptr);
  app.widgets->set_text(app.session, btn, "Native BUTTON in the loaded font");

  neui_widget_t input = app.widgets->create(app.session, win, NEUI_W_INPUTBOX,
                                            16, 100, 360, 34, nullptr);
  app.widgets->set_text(app.session, input, "Editable INPUTBOX in the loaded font");

  if (app.family[0]) {
    app.attrs->set_string(app.session, btn,   NEUI_ATTR_FONT_FAMILY, app.family);
    app.attrs->set_float (app.session, btn,   NEUI_ATTR_FONT_SIZE,   18.0f);
    app.attrs->set_string(app.session, input, NEUI_ATTR_FONT_FAMILY, app.family);
    app.attrs->set_float (app.session, input, NEUI_ATTR_FONT_SIZE,   16.0f);
  }

  // Painted prong: a CUSTOMDRAW canvas that push_fonts the same family.
  neui_widget_t canvas = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                             16, 150, 720, 280, nullptr);
  app.canvas_id = canvas.id;

  app.widgets->show(app.session, win);
  host->run(app.session);

  if (app.font.id != asset_none.id)
    app.assets->destroy(app.session, app.font);  // unregisters the family
  host->destroy(app.session);
  return 0;
}
