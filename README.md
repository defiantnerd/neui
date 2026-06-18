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

* windows (implemented)
* macOS (implemented)
* linux (implemented, X11 + Cairo via the crossplatform host)
* iOS / iPadOS (implemented, native UIKit host + crossplatform host)
* crossplatform for all of the above
* embedded (DAW plugin windows, X11 host run-loop integration on linux)

## how to use

Minimal example - a window with an input field, a Submit button, and a label below. `neui_init()` registers every host the linked neuilib was built with; `neui_get_api(NULL)` returns the first registered (native where one exists, xpl elsewhere). The callback fires on every event with `emit_events` set; BUTTON / INPUTBOX auto-enable, so we just filter on widget id + event type and pull the input text into the label.

```c
#include <neui/neui.h>
#include <string.h>
#include <stdio.h>

// Session + widget handles are plain id structs; store by value,
// treat as read-only.
typedef struct {
  neui_widget_api_t* w;
  neui_session_t     s;
  neui_widget_t      input, button, label;
} App;

static bool onevent(void* token, neui_event_t* e) {
  App* a = (App*)token;
  // APP_QUIT fires when the user closes the appwindow; returning true
  // lets the host destroy the window and unwind run().
  if (e->type == NEUI_EVENT_APP_QUIT) return true;
  if (e->type == NEUI_EVENT_MOUSE_BUTTON_CLICK
      && e->data.mouse.widget.id == a->button.id) {
    char in_buf[256] = {0};
    char out_buf[320];
    a->w->get_text(a->s, a->input, in_buf, sizeof in_buf);
    snprintf(out_buf, sizeof out_buf, "You have typed %s", in_buf);
    a->w->set_text(a->s, a->label, out_buf);
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
  neui_api_t* api = neui_get_api(NULL);
  app.s = api->create_session(&client, &app);
  app.w = (neui_widget_api_t*)api->get_interface(app.s, NEUI_API_WIDGETS);

  neui_widget_t win = app.w->create(app.s, widget_none,
                                      NEUI_W_APPWINDOW, 100, 100, 360, 120, NULL);
  app.input  = app.w->create(app.s, win, NEUI_W_INPUTBOX,  10, 10, 240, 24, NULL);
  app.button = app.w->create(app.s, win, NEUI_W_BUTTON,   260, 10,  80, 24, NULL);
  app.label  = app.w->create(app.s, win, NEUI_W_LABEL,     10, 50, 330, 24, NULL);
  app.w->set_text(app.s, app.button, "Submit");

  app.w->show(app.s, win);
  return api->run(app.s) ? 0 : 1;
}
```

This source is wired up in CMake as `readme_example` so you can build + run it directly: `cmake --build out/build --config Debug --target readme_example`.

The C language is chosen for the interface, internal structures are C/C++ or Objective-C.
Since all access is provided via one symbol, this can either be a dynamically loaded library or compiled statically.

Access to the lib is provided over one single symbol that provides access to all further features, structured in a modular way.
For simple applications you just need a few lines to present and interact with the UI system, but you can also access clipboard,
graphics, content etc.

## tests and examples

The repo ships a header-only unit suite (`neui_tests`, run via `ctest`) and a set
of example programs (`neui_example`, `readme_example`, and others).

These are built only when neui is the top-level CMake project. When you pull neui
into your own build with `add_subdirectory()`, both default to off so your tree
stays lean. Flip them on explicitly if you need them:

```
-DNEUI_BUILD_TESTS=ON
-DNEUI_BUILD_EXAMPLES=ON
```

## license

MIT
