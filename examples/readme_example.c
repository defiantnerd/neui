// Minimal example from README.md - input + button + label.
// Click "Submit" and the label below echoes "You have typed <input>".
//
// Source is C99; CMake builds it as C++ (LANGUAGE CXX) because mixing a
// C-only `main` with neui's C++ static libs leaves the C++ runtime
// + static initialisers unmounted on MSVC. The source stays free of
// C-only constructs (no compound literals, explicit (T*)void* casts)
// so it also compiles cleanly as C99 if you'd rather pass /TC.

#include <neui/neui.h>
#include <string.h>
#include <stdio.h>

typedef struct {
  neui_widget_api_t* w;
  uint32_t           session;
  uint32_t           input_id, button_id, label_id;
} App;

static bool onevent(void* token, neui_event_t* e) {
  App* a = (App*)token;
  // APP_QUIT fires when the user closes the appwindow. onevent's return
  // value is the "allow close" flag - true lets the host destroy the
  // window and unwind the run() loop; false cancels the close.
  if (e->type == NEUI_EVENT_APP_QUIT) return true;
  if (e->type == NEUI_EVENT_MOUSE_BUTTON_CLICK
      && e->data.mouse.widget.id == a->button_id) {
    neui_session_t s   = { a->session };
    neui_widget_t  in  = { a->input_id };
    neui_widget_t  lbl = { a->label_id };
    char in_buf[256]   = {0};
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
  App app           = {0};
  neui_init();
  neui_api_t*    api = neui_get_api(NULL);
  neui_session_t s   = api->create_session(&client, &app);
  app.session        = s.session;
  app.w              = (neui_widget_api_t*)api->get_interface(s, NEUI_API_WIDGETS);

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
