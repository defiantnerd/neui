// dnd_example - minimal drop-target demo.
//
// Opens a single window with a CUSTOMDRAW widget that accepts file drops
// (text/uri-list) and plain text. On hover the border highlights; on drop
// the dropped URIs / text are stored and rendered as a list. Pulls into
// CLAUDE.md as the canonical example for Phase D verification.

#include "neui/neui.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef _WIN32
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
  neui_api_t*           neui      = nullptr;
  neui_widget_api_t*    widgets   = nullptr;
  neui_clipboard_api_t* clipboard = nullptr;
  neui_dnd_api_t*       dnd       = nullptr;
  neui_session_t        session   = { 0 };
  uint32_t              win_id    = 0;
  uint32_t              drop_id   = 0;
  bool                  drag_over = false;     // cursor currently over the drop area
  std::vector<std::string> dropped;            // last-dropped items, displayed
};

static void paint_drop(neui_event_paint_t* p, AppState* a)
{
  auto* px = p->painter_api;
  auto* ph = p->p;

  uint32_t bg     = a->drag_over ? 0xff10304a : 0xff203040;
  uint32_t border = a->drag_over ? 0xff60a0ff : 0xff8090a0;
  px->fill_rect(ph, 0, 0, p->width, p->height, bg);
  px->draw_rect(ph, 0.5f, 0.5f, p->width - 1.0f, p->height - 1.0f,
                 2.0f, border);

  const char* label = "Drop files or text here";
  px->draw_text(ph, 16, 12, p->width - 32, 24,
                 label, 14.0f, 0xffffffff);

  float y = 44;
  for (auto& s : a->dropped) {
    px->draw_text(ph, 16, y, p->width - 32, 18,
                   s.c_str(), 12.0f, 0xffd0e0f0);
    y += 18;
    if (y > p->height - 18) break;
  }
}

// Parse a text/uri-list buffer into individual URI strings, dropping
// comments and empty lines. RFC 2483.
static std::vector<std::string> parse_uri_list(const char* data, int len)
{
  std::vector<std::string> out;
  int i = 0;
  while (i < len) {
    int end = i;
    while (end < len && data[end] != '\r' && data[end] != '\n') ++end;
    if (end > i) {
      std::string line(data + i, data + end);
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      if (!line.empty() && line.front() != '#')
        out.push_back(std::move(line));
    }
    while (end < len && (data[end] == '\r' || data[end] == '\n')) ++end;
    i = end;
  }
  return out;
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  auto* a = static_cast<AppState*>(token);
  switch (event->type) {
    case NEUI_EVENT_APP_QUIT:
      return true;

    case NEUI_EVENT_WIDGET_PAINT:
      if (event->data.paint.widget.id == a->drop_id) {
        paint_drop(&event->data.paint, a);
        return true;
      }
      break;

    case NEUI_EVENT_DND_ENTER:
    case NEUI_EVENT_DND_MOVE:
      a->drag_over = true;
      a->widgets->invalidate(a->session, { a->drop_id });
      a->dnd->accept(a->session, NEUI_DND_ACTION_COPY);
      return true;

    case NEUI_EVENT_DND_LEAVE:
      a->drag_over = false;
      a->widgets->invalidate(a->session, { a->drop_id });
      return true;

    case NEUI_EVENT_DND_DROP: {
      a->drag_over = false;
      a->dropped.clear();
      neui_data_item_t item = event->data.dnd.data;
      // Prefer URI list (file drops); fall back to plain text.
      if (a->clipboard->item_has_format(a->session, item, NEUI_MIME_URI_LIST)) {
        int n = a->clipboard->item_get_format(a->session, item,
                                                NEUI_MIME_URI_LIST, nullptr, 0);
        if (n > 0) {
          std::vector<char> buf(static_cast<size_t>(n));
          a->clipboard->item_get_format(a->session, item, NEUI_MIME_URI_LIST,
                                         buf.data(), n);
          a->dropped = parse_uri_list(buf.data(), n);
        }
      } else if (a->clipboard->item_has_format(a->session, item, NEUI_MIME_TEXT)) {
        int n = a->clipboard->item_get_format(a->session, item,
                                                NEUI_MIME_TEXT, nullptr, 0);
        if (n > 0) {
          std::vector<char> buf(static_cast<size_t>(n));
          a->clipboard->item_get_format(a->session, item, NEUI_MIME_TEXT,
                                         buf.data(), n);
          // The text format snapshot includes a trailing null terminator.
          a->dropped.emplace_back(buf.data(),
                                   static_cast<size_t>(n > 0 && buf[n - 1] == 0 ? n - 1 : n));
        }
      }
      dbglog("[dnd_example] dropped %zu items\n", a->dropped.size());
      a->widgets->invalidate(a->session, { a->drop_id });
      a->dnd->accept(a->session, NEUI_DND_ACTION_COPY);
      return true;
    }

    default:
      break;
  }
  return false;
}

static void* NEUI_ABI get_interface(void* token, const char* iface)
{
  (void)token;
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
  if (!host) { dbglog("[dnd_example] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version    = NEUI_VERSION;
  client.get_interface   = get_interface;

  app.session = host->create_session(&client, &app);
  if (!app.session.session) { dbglog("[dnd_example] no session\n"); return 1; }

  app.widgets   = (neui_widget_api_t*)   host->get_interface(app.session, NEUI_API_WIDGETS);
  app.clipboard = (neui_clipboard_api_t*)host->get_interface(app.session, NEUI_API_CLIPBOARD);
  app.dnd       = (neui_dnd_api_t*)      host->get_interface(app.session, NEUI_API_DND);
  if (!app.widgets || !app.clipboard || !app.dnd) {
    dbglog("[dnd_example] missing API\n");
    return 1;
  }

  neui_widget_t win = app.widgets->create(app.session,
                                            neui_widget_t{ UINT32_MAX },
                                            NEUI_W_APPWINDOW,
                                            100, 100, 1024, 720,
                                            "neui DnD example");
  app.win_id = win.id;

  neui_widget_t drop = app.widgets->create(app.session, win, NEUI_W_CUSTOMDRAW,
                                             20, 20, 964, 620, nullptr);
  app.drop_id = drop.id;

  // Mark BOTH the CUSTOMDRAW (xpl widget-precise hit-test) and the
  // window (win32 / macOS native, which target the frame) as drop
  // targets so the same example runs on every host. The xpl walker
  // picks the CUSTOMDRAW (deepest descendant); native hosts pick the
  // window. The event handler treats either widget id the same.
  const char* mimes[] = { NEUI_MIME_URI_LIST, NEUI_MIME_TEXT };
  app.dnd->set_drop_target(app.session, drop, true);
  app.dnd->set_accepted_formats(app.session, drop, mimes, 2);
  app.dnd->set_drop_target(app.session, win, true);
  app.dnd->set_accepted_formats(app.session, win, mimes, 2);

  app.widgets->show(app.session, win);
  host->run(app.session);
  host->destroy(app.session);
  return 0;
}
