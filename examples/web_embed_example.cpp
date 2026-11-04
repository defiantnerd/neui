// web_embed_example - a neui app embedded in a web page (plans/wasm-host.md
// Stage 7). Built for the web only, as an ES module that
// examples/web/neui-web.js mounts into an element of the page; see
// examples/web/embed.html for the host page.
//
// Two shapes from one program:
//   (no arguments)        an APPWINDOW that fills the element it is mounted in
//                         (or keeps its create() size); main runs run(), so the
//                         code after it runs when the page unmounts the app.
//   --plug=<selector>     a PLUGWINDOW embedded through NEUI_API_EMBED
//                         set_parent into the element the CSS selector names.
//                         As in a DAW, main returns without calling run(); the
//                         host's service loop runs the frame.
//   --name=<text>         what the app calls itself in its messages.
//
// Page interop through NEUI_API_WEB: turning the knob or pressing the button
// posts a JSON message to the page; a message from the page shows in the
// app; so do the page being hidden or shown, and the frame's size.

#include <neui/neui.h>
#include <stdio.h>
#include <string.h>
#include <string>

struct AppState
{
  neui_api_t*        api     = nullptr;
  neui_widget_api_t* widgets = nullptr;
  neui_attr_api_t*   attrs   = nullptr;
  neui_web_api_t*    web     = nullptr;
  neui_session_t     session = { 0 };
  neui_widget_t      frame   = { UINT32_MAX };
  neui_widget_t      title   = { UINT32_MAX };
  neui_widget_t      knob    = { UINT32_MAX };
  neui_widget_t      button  = { UINT32_MAX };
  neui_widget_t      inbox   = { UINT32_MAX };
  neui_widget_t      status  = { UINT32_MAX };
  std::string        name    = "app";
  int                pings   = 0;
};

static void post(AppState* a, const std::string& json)
{
  if (a->web) a->web->post_message(a->session, json.c_str(), (uint32_t)json.size());
}

static void set_status(AppState* a)
{
  char buf[160];
  const char* page = (a->web && a->web->page_state(a->session) == NEUI_WEB_PAGE_HIDDEN)
                     ? "hidden" : "visible";
  const char* flavor = !a->web ? "-"
                     : a->web->flavor(a->session) == NEUI_WEB_FLAVOR_JSPI ? "JSPI" : "Asyncify";
  int x = 0, y = 0, w = 0, h = 0;
  a->widgets->get_client_rect(a->session, a->frame, &x, &y, &w, &h);
  snprintf(buf, sizeof buf, "%d x %d  -  page %s  -  %s", w, h, page, flavor);
  a->widgets->set_text(a->session, a->status, buf);
}

static bool NEUI_ABI on_event(void* token, neui_event_t* e)
{
  auto* a = static_cast<AppState*>(token);
  switch (e->type) {
    case NEUI_EVENT_APP_QUIT:
      return true;
    case NEUI_EVENT_VALUE_CHANGED:
      if (e->data.value.widget.id == a->knob.id) {
        char buf[96];
        snprintf(buf, sizeof buf, "{\"app\":\"%s\",\"value\":%.3f}", a->name.c_str(),
                 (double)e->data.value.value);
        post(a, buf);
      }
      break;
    case NEUI_EVENT_MOUSE_BUTTON_CLICK:
      if (e->data.mouse.widget.id == a->button.id) {
        char buf[96];
        snprintf(buf, sizeof buf, "{\"app\":\"%s\",\"ping\":%d}", a->name.c_str(), ++a->pings);
        post(a, buf);
        return true;
      }
      break;
    case NEUI_EVENT_WEB_MESSAGE: {
      std::string text = "Page: ";
      text.append(e->data.web.data, e->data.web.length);
      a->widgets->set_text(a->session, a->inbox, text.c_str());
      break;
    }
    case NEUI_EVENT_WEB_PAGE:
      set_status(a);
      break;
    case NEUI_EVENT_RESIZE:
      if (e->data.resize.widget.id == a->frame.id) set_status(a);
      break;
    default:
      break;
  }
  return false;
}

static neui_widget_client_t g_wclient = { NEUI_VERSION, nullptr, on_event };
static void* NEUI_ABI client_iface(void* /*token*/, const char* name)
{
  return strcmp(name, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wclient;
}
static neui_client_t g_client = { NEUI_VERSION, client_iface };

// Static: in the PLUGWINDOW shape main returns while the app keeps running.
static AppState g_app;

int main(int argc, char** argv)
{
  std::string plug;
  for (int i = 1; i < argc; ++i) {
    if (!strncmp(argv[i], "--plug=", 7)) plug = argv[i] + 7;
    else if (!strncmp(argv[i], "--name=", 7)) g_app.name = argv[i] + 7;
  }
  AppState* a = &g_app;

  neui_init();
  a->api = neui_get_api(nullptr);
  if (!a->api) return 1;
  a->session = a->api->create_session(&g_client, a);
  a->widgets = (neui_widget_api_t*)a->api->get_interface(a->session, NEUI_API_WIDGETS);
  a->attrs   = (neui_attr_api_t*)a->api->get_interface(a->session, NEUI_API_ATTRS);
  a->web     = (neui_web_api_t*)a->api->get_interface(a->session, NEUI_API_WEB);
  if (!a->widgets || !a->attrs) return 1;

  const bool plugged = !plug.empty();
  a->frame = a->widgets->create(a->session, neui_widget_t{ UINT32_MAX },
                                plugged ? NEUI_W_PLUGWINDOW : NEUI_W_APPWINDOW,
                                100, 100, 360, 220, nullptr);
  a->widgets->set_text(a->session, a->frame, "neui embed example");
  // Follow the page's theme live (app.setTheme, prefers-color-scheme).
  a->attrs->set_int(a->session, a->frame, NEUI_ATTR_FOLLOW_SYSTEM_THEME, 1);
  if (plugged) {
    auto* embed = (neui_embed_api_t*)a->api->get_interface(a->session, NEUI_API_EMBED);
    if (embed) embed->set_parent(a->session, a->frame, (void*)plug.c_str());
  }

  std::string heading = "neui app \"" + a->name + "\"";
  a->title  = a->widgets->create(a->session, a->frame, NEUI_W_LABEL, 12, 10, 330, 20, nullptr);
  a->widgets->set_text(a->session, a->title, heading.c_str());
  a->knob   = a->widgets->create(a->session, a->frame, NEUI_W_KNOB, 12, 40, 72, 72, nullptr);
  a->attrs->set_float(a->session, a->knob, NEUI_PARAM_VALUE, 0.5f);
  a->button = a->widgets->create(a->session, a->frame, NEUI_W_BUTTON, 100, 62, 120, 26, nullptr);
  a->widgets->set_text(a->session, a->button, "Ping the page");
  a->inbox  = a->widgets->create(a->session, a->frame, NEUI_W_LABEL, 12, 126, 330, 20, nullptr);
  a->widgets->set_text(a->session, a->inbox, "Page: (nothing yet)");
  a->status = a->widgets->create(a->session, a->frame, NEUI_W_LABEL, 12, 154, 330, 20, nullptr);

  a->widgets->show(a->session, a->frame);
  set_status(a);
  post(a, "{\"app\":\"" + a->name + "\",\"ready\":true}");

  if (plugged) return 0;   // embedded: the host runs the frame, as in a DAW

  a->api->run(a->session);
  // The page unmounted the app (or its window was closed): clean up as on
  // the desktop.
  a->api->destroy(a->session);
  fprintf(stderr, "[web_embed_example] %s: main returned\n", a->name.c_str());
  return 0;
}
