// dnd_source_example - drag a payload out of a CUSTOMDRAW.
//
// Layout:
//   left pane (CUSTOMDRAW)  - source. Press + drag past 5 px to call
//                              dnd->begin_drag() with text + uri-list.
//   right pane (CUSTOMDRAW) - drop target. Highlights on hover, prints
//                              the dropped formats on drop.
//   status label             - shows the negotiated action from the
//                              last drag (copy / move / link / cancelled).
//
// Demonstrates both internal drag (left -> right) and external drag
// (left -> Notepad / TextEdit / Finder / Explorer). The drop pane uses
// the same shape as examples/dnd_example.cpp.

#include "neui/neui.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
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

  uint32_t source_id = 0;
  uint32_t drop_id   = 0;
  uint32_t status_id = 0;

  // Drag detection (source pane).
  bool mouse_down = false;
  bool in_drag    = false;
  int  down_x     = 0;
  int  down_y     = 0;

  // Drop pane state.
  bool                       drag_over = false;
  std::vector<std::string>   dropped;
};

static const char* k_payload_text =
  "Hello from neui drag source!";
static const char* k_payload_uri =
  "file:///tmp/neui-demo.txt\r\n";

static void paint_source(neui_event_paint_t* p, AppState* a)
{
  auto* px = p->painter_api;
  auto* ph = p->p;
  uint32_t bg = a->in_drag ? 0xff203050 : 0xff182838;
  px->fill_rect(ph, 0, 0, p->width, p->height, bg);
  px->draw_rect(ph, 0.5f, 0.5f, p->width - 1.0f, p->height - 1.0f,
                 1.0f, 0xff8090a0);
  px->draw_text(ph, 16, 12, p->width - 32, 24,
                 "Drag from here", 14.0f, 0xffffffff);
  px->draw_text(ph, 16, 36, p->width - 32, 18,
                 a->in_drag ? "(dragging...)" : "(press + move past 5 px)",
                 12.0f, 0xff90a0b0);
  px->draw_text(ph, 16, 60, p->width - 32, 18,
                 "Payload: text + file:///tmp/neui-demo.txt",
                 12.0f, 0xff90a0b0);
}

static void paint_drop(neui_event_paint_t* p, AppState* a)
{
  auto* px = p->painter_api;
  auto* ph = p->p;
  uint32_t bg     = a->drag_over ? 0xff10304a : 0xff203040;
  uint32_t border = a->drag_over ? 0xff60a0ff : 0xff8090a0;
  px->fill_rect(ph, 0, 0, p->width, p->height, bg);
  px->draw_rect(ph, 0.5f, 0.5f, p->width - 1.0f, p->height - 1.0f,
                 2.0f, border);
  px->draw_text(ph, 16, 12, p->width - 32, 24,
                 "Drop here", 14.0f, 0xffffffff);
  float y = 44;
  for (auto& s : a->dropped) {
    px->draw_text(ph, 16, y, p->width - 32, 18,
                   s.c_str(), 12.0f, 0xffd0e0f0);
    y += 18;
    if (y > p->height - 18) break;
  }
}

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

static void start_drag(AppState* a)
{
  if (a->in_drag) return;
  a->in_drag = true;
  a->widgets->invalidate(a->session, { a->source_id });

  // Build the payload: plain text + a single-URI file list. The URI is
  // a placeholder - the drop target will receive the string but no
  // actual file is read.
  neui_data_item_t item = a->clipboard->create_item(a->session);
  a->clipboard->item_set_format(a->session, item, NEUI_MIME_TEXT,
                                 k_payload_text,
                                 (int)strlen(k_payload_text) + 1);
  a->clipboard->item_set_format(a->session, item, NEUI_MIME_URI_LIST,
                                 k_payload_uri,
                                 (int)strlen(k_payload_uri));

  // Advertise all three actions so the modifier-aware target (Ctrl=copy,
  // Shift=move, Ctrl+Shift=link) can pick freely.
  uint32_t allowed = NEUI_DND_ACTION_COPY |
                     NEUI_DND_ACTION_MOVE |
                     NEUI_DND_ACTION_LINK;
  neui_dnd_action_t res = a->dnd->begin_drag(a->session,
                                              { a->source_id },
                                              item,
                                              allowed);
  a->clipboard->release(a->session, item);

  const char* msg = "cancelled";
  if      (res == NEUI_DND_ACTION_COPY) msg = "copy";
  else if (res == NEUI_DND_ACTION_MOVE) msg = "move";
  else if (res == NEUI_DND_ACTION_LINK) msg = "link";
  char status[64];
  snprintf(status, sizeof(status), "Drag result: %s", msg);
  a->widgets->set_text(a->session, { a->status_id }, status);
  dbglog("[dnd_source_example] %s\n", status);

  a->in_drag    = false;
  a->mouse_down = false;
  a->widgets->invalidate(a->session, { a->source_id });
}

static bool NEUI_ABI on_event(void* token, neui_event_t* event)
{
  auto* a = static_cast<AppState*>(token);

  // Paint dispatch.
  if (event->type == NEUI_EVENT_WIDGET_PAINT) {
    if (event->data.paint.widget.id == a->source_id) {
      paint_source(&event->data.paint, a);
      return true;
    }
    if (event->data.paint.widget.id == a->drop_id) {
      paint_drop(&event->data.paint, a);
      return true;
    }
    return false;
  }

  if (event->type == NEUI_EVENT_APP_QUIT) return true;

  // Source pane: drag detection.
  if (event->data.mouse.widget.id == a->source_id) {
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      a->mouse_down = true;
      a->down_x = event->data.mouse.x;
      a->down_y = event->data.mouse.y;
      return true;
    }
    if (event->type == NEUI_EVENT_MOUSE_MOVE && a->mouse_down && !a->in_drag) {
      int dx = event->data.mouse.x - a->down_x;
      int dy = event->data.mouse.y - a->down_y;
      if (dx * dx + dy * dy >= 25) {  // 5 px threshold
        start_drag(a);
        return true;
      }
    }
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP) {
      a->mouse_down = false;
      return true;
    }
  }

  // Drop pane: same shape as dnd_example. We accept whatever the
  // framework's modifier-aware `suggested_action` recommends, so the
  // cursor changes between copy / move / link as the user holds
  // Ctrl / Shift / Ctrl+Shift during the drag. We don't actually
  // do anything different on MOVE vs COPY in this demo (the source
  // would normally delete on a confirmed MOVE) - the goal is just
  // visual modifier feedback.
  auto suggested_action = [&]() {
    return static_cast<neui_dnd_action_t>(event->data.dnd.suggested_action);
  };

  switch (event->type) {
    case NEUI_EVENT_DND_ENTER:
    case NEUI_EVENT_DND_MOVE:
      a->drag_over = true;
      a->widgets->invalidate(a->session, { a->drop_id });
      a->dnd->accept(a->session, suggested_action());
      return true;

    case NEUI_EVENT_DND_LEAVE:
      a->drag_over = false;
      a->widgets->invalidate(a->session, { a->drop_id });
      return true;

    case NEUI_EVENT_DND_DROP: {
      a->drag_over = false;
      a->dropped.clear();
      neui_data_item_t it = event->data.dnd.data;
      if (a->clipboard->item_has_format(a->session, it, NEUI_MIME_URI_LIST)) {
        int n = a->clipboard->item_get_format(a->session, it,
                                                NEUI_MIME_URI_LIST, nullptr, 0);
        if (n > 0) {
          std::vector<char> buf(static_cast<size_t>(n));
          a->clipboard->item_get_format(a->session, it, NEUI_MIME_URI_LIST,
                                         buf.data(), n);
          a->dropped = parse_uri_list(buf.data(), n);
        }
      } else if (a->clipboard->item_has_format(a->session, it, NEUI_MIME_TEXT)) {
        int n = a->clipboard->item_get_format(a->session, it,
                                                NEUI_MIME_TEXT, nullptr, 0);
        if (n > 0) {
          std::vector<char> buf(static_cast<size_t>(n));
          a->clipboard->item_get_format(a->session, it, NEUI_MIME_TEXT,
                                         buf.data(), n);
          a->dropped.emplace_back(buf.data(),
                                   static_cast<size_t>(n > 0 && buf[n - 1] == 0 ? n - 1 : n));
        }
      }
      dbglog("[dnd_source_example] dropped %zu items\n", a->dropped.size());
      a->widgets->invalidate(a->session, { a->drop_id });
      a->dnd->accept(a->session, suggested_action());
      return true;
    }

    default: break;
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
  if (!host) { dbglog("[dnd_source_example] no host\n"); return 1; }

  AppState app;
  app.neui = host;

  neui_client_t client;
  client.neui_version  = NEUI_VERSION;
  client.get_interface = get_interface;

  app.session = host->create_session(&client, &app);
  if (!app.session.session) {
    dbglog("[dnd_source_example] no session\n"); return 1;
  }

  app.widgets   = (neui_widget_api_t*)   host->get_interface(app.session, NEUI_API_WIDGETS);
  app.clipboard = (neui_clipboard_api_t*)host->get_interface(app.session, NEUI_API_CLIPBOARD);
  app.dnd       = (neui_dnd_api_t*)      host->get_interface(app.session, NEUI_API_DND);
  if (!app.widgets || !app.clipboard || !app.dnd) {
    dbglog("[dnd_source_example] missing API\n");
    return 1;
  }

  neui_widget_t win = app.widgets->create(app.session,
                                            neui_widget_t{ UINT32_MAX },
                                            NEUI_W_APPWINDOW,
                                            100, 100, 900, 600,
                                            nullptr);
  app.widgets->set_text(app.session, win, "neui drag source example");

  neui_widget_t src  = app.widgets->create(app.session, win,
                                             NEUI_W_CUSTOMDRAW,
                                             20, 20, 410, 480, nullptr);
  neui_widget_t drop = app.widgets->create(app.session, win,
                                             NEUI_W_CUSTOMDRAW,
                                             450, 20, 410, 480, nullptr);
  neui_widget_t lbl  = app.widgets->create(app.session, win,
                                             NEUI_W_LABEL,
                                             20, 520, 840, 20,
                                             nullptr);
  app.widgets->set_text(app.session, lbl, "Drag result: -");
  app.source_id = src.id;
  app.drop_id   = drop.id;
  app.status_id = lbl.id;

  // Only the right pane is a drop target. All three hosts hit-test the
  // widget tree, so DnD events fire on the drop pane only when the cursor is
  // actually over it - the rest of the window receives nothing (no highlight).
  const char* mimes[] = { NEUI_MIME_URI_LIST, NEUI_MIME_TEXT };
  app.dnd->set_drop_target(app.session, drop, true);
  app.dnd->set_accepted_formats(app.session, drop, mimes, 2);

  app.widgets->show(app.session, win);
  host->run(app.session);
  host->destroy(app.session);
  return 0;
}
