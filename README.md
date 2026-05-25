# neui

a modern and free UI framework - in the making

## key features:

The goal is to develop a modern and free UI framework that is portable and cross platform and easy to use. It should benefit from existing platforms and behave platform specific to minimize the feeling of alienisation then comes along with approaches like web-based frameworks like electron etc.
This includes using fonts, colors and sizes that are defined from the target system.

* cross platform
* C based interface
* strict separation of client and host
* extensible

## targets

* windows
* macOS
* linux (not implemented yet)
* crossplatform for all of the above
* embedded

## how to use

Minimal example - a window with an input field, a Submit button, and a label below. `neui_init()` registers every host the linked neuilib was built with; `neui_get_api(NULL)` returns the first registered (native where one exists, xpl elsewhere). The callback fires on every event with `emit_events` set; BUTTON / INPUTBOX auto-enable, so we just filter on widget id + event type and pull the input text into the label.

```c
#include <neui/neui.h>
#include <string.h>
#include <stdio.h>

// neui_session_t has a const id field and can't be copy-assigned, so we
// stash the raw uint32_t and reconstruct the handle at every call site.
typedef struct {
  neui_widget_api_t* w;
  uint32_t           session;
  uint32_t           input_id, button_id, label_id;
} App;

static bool onevent(void* token, neui_event_t* e) {
  App* a = (App*)token;
  // APP_QUIT fires when the user closes the appwindow; returning true
  // lets the host destroy the window and unwind run().
  if (e->type == NEUI_EVENT_APP_QUIT) return true;
  if (e->type == NEUI_EVENT_MOUSE_BUTTON_CLICK
      && e->data.mouse.widget.id == a->button_id) {
    neui_session_t s   = { a->session };
    neui_widget_t  in  = { a->input_id };
    neui_widget_t  lbl = { a->label_id };
    char in_buf[256] = {0};
    char out_buf[320];
    a->w->get_text(s, in, in_buf, sizeof in_buf);
    snprintf(out_buf, sizeof out_buf, "You have typed %s", in_buf);
    a->w->set_text(s, lbl, out_buf);
  }
  return false;
}

static neui_widget_client_t wclient = { NEUI_VERSION, NULL, onevent };
static void* iface(void* t, const char* n) {
  return strcmp(n, NEUI_API_WIDGETS) ? NULL : (void*)&wclient;
}
static neui_client_t client = { NEUI_VERSION, iface };

int main(void) {
  App app = {0};
  neui_init();
  neui_api_t*    api = neui_get_api(NULL);
  neui_session_t s   = api->create_session(&client, &app);
  app.session = s.session;
  app.w = (neui_widget_api_t*)api->get_interface(s, NEUI_API_WIDGETS);

  neui_widget_t win = app.w->create(s, widget_none,
                                      NEUI_W_APPWINDOW, 100, 100, 360, 120, NULL);
  neui_widget_t in  = app.w->create(s, win, NEUI_W_INPUTBOX,  10, 10, 240, 24, NULL);
  neui_widget_t btn = app.w->create(s, win, NEUI_W_BUTTON,   260, 10,  80, 24, NULL);
  neui_widget_t lbl = app.w->create(s, win, NEUI_W_LABEL,     10, 50, 330, 24, NULL);
  app.input_id  = in.id;
  app.button_id = btn.id;
  app.label_id  = lbl.id;
  app.w->set_text(s, btn, "Submit");

  app.w->show(s, win);
  return api->run(s) ? 0 : 1;
}
```

This source is wired up in CMake as `readme_example` so you can build + run it directly: `cmake --build out/build --config Debug --target readme_example`.

The C language is chosen for the interface, internal structures are C/C++ or Objective-C.
Since all access is provided via one symbol, this can either be a dynamically loaded library or compiled statically.

Access to the lib is provided over one single symbol that provides access to all further features, structured in a modular way.
For simple applications you just need a few lines to present and interact with the UI system, but you can also access clipboard,
graphics, content etc.

## license

MIT
