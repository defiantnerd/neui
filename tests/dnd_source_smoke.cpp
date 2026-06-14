// Phase: XDND drag-source verification (internal-drag path).
//
// Drives platform_dnd_begin_drag end to end inside ONE process: the main
// thread starts a drag from a neui CUSTOMDRAW that is also a drop target; a
// background thread (its own X connection) warps the pointer over the pane and
// synthesizes the button-release so the blocking drag spin sees motion -> drop.
// The internal path uses Session::dispatch_dnd_* directly (no second app), so
// this exercises offered-atom build, pointer grab, target find, enter/move,
// accept, drop, and the returned action. Linux-only; needs a live X display
// and MOVES THE POINTER, so it is built but not ctest-registered.

#include <neui/neui.h>
#include "platform.h"   // xpl_host::platform_pump_once

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static neui_widget_api_t*    W;
static neui_clipboard_api_t* CB;
static neui_dnd_api_t*       DND;
static neui_session_t        SESS;
static int  g_dropped = 0;
static char g_text[256];

static bool NEUI_ABI onevent(void* /*tok*/, neui_event_t* e)
{
  switch (e->type) {
    case NEUI_EVENT_DND_ENTER:
    case NEUI_EVENT_DND_MOVE:
      DND->accept(SESS, (neui_dnd_action_t)e->data.dnd.suggested_action);
      return true;
    case NEUI_EVENT_DND_DROP: {
      neui_data_item_t it = e->data.dnd.data;
      int n = CB->item_get_format(SESS, it, NEUI_MIME_TEXT, g_text, (int)sizeof g_text - 1);
      if (n > 0) { if (n >= (int)sizeof g_text) n = (int)sizeof g_text - 1; g_text[n] = 0; }
      g_dropped++;
      DND->accept(SESS, (neui_dnd_action_t)e->data.dnd.suggested_action);
      return true;
    }
    default: return false;
  }
}
static neui_widget_client_t WC = { NEUI_VERSION, nullptr, onevent };
static void* NEUI_ABI iface(void*, const char* n)
{ return strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&WC; }
static neui_client_t CL = { NEUI_VERSION, iface };

static Window find_by_name(Display* d, Window r, Atom NN, Atom U8, const char* want)
{
  Atom t; int f; unsigned long n, a; unsigned char* v = 0;
  if (XGetWindowProperty(d, r, NN, 0, 256, False, U8, &t, &f, &n, &a, &v) == Success && v) {
    int m = strstr((char*)v, want) != 0; XFree(v); if (m) return r;
  }
  Window rr, p, *k = 0; unsigned int nk = 0; Window found = 0;
  if (XQueryTree(d, r, &rr, &p, &k, &nk)) {
    for (unsigned i = 0; i < nk && !found; ++i) found = find_by_name(d, k[i], NN, U8, want);
    if (k) XFree(k);
  }
  return found;
}

static const char* g_warp_target = "neui dndsrc smoke";  // overridden in foreign mode

// Background "mouse": warp over the target window centre (real motion -> the
// drag grab), then synthesize a left-button release to the source frame window.
static void drive_mouse()
{
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  Display* d = XOpenDisplay(nullptr); if (!d) return;
  Atom NN = XInternAtom(d, "_NET_WM_NAME", False), U8 = XInternAtom(d, "UTF8_STRING", False);
  Window root = DefaultRootWindow(d);
  Window w = find_by_name(d, root, NN, U8, g_warp_target);
  if (!w) { printf("DRIVER: window not found\n"); XCloseDisplay(d); return; }
  Window relwin = find_by_name(d, root, NN, U8, "neui dndsrc smoke");  // source frame
  XWindowAttributes a; XGetWindowAttributes(d, w, &a);
  int cx, cy; Window ch;
  XTranslateCoordinates(d, w, root, a.width / 2, a.height / 2, &cx, &cy, &ch);
  XWarpPointer(d, None, root, 0, 0, 0, 0, cx - 6, cy); XFlush(d);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  XWarpPointer(d, None, root, 0, 0, 0, 0, cx, cy);     XFlush(d);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  XButtonEvent be; memset(&be, 0, sizeof be);
  be.type = ButtonRelease; be.display = d; be.window = relwin ? relwin : w; be.root = root;
  be.x = a.width / 2; be.y = a.height / 2; be.x_root = cx; be.y_root = cy;
  be.button = Button1; be.same_screen = True; be.state = Button1Mask;
  XSendEvent(d, relwin ? relwin : w, True, ButtonReleaseMask, (XEvent*)&be); XFlush(d);
  XCloseDisplay(d);
}

int main()
{
  neui_init();
  neui_api_t* api = neui_get_api(nullptr);
  if (!api) { printf("no host\n"); return 0; }
  SESS = api->create_session(&CL, nullptr);
  W   = (neui_widget_api_t*)   api->get_interface(SESS, NEUI_API_WIDGETS);
  CB  = (neui_clipboard_api_t*)api->get_interface(SESS, NEUI_API_CLIPBOARD);
  DND = (neui_dnd_api_t*)      api->get_interface(SESS, NEUI_API_DND);
  if (!W || !CB || !DND) { printf("missing api\n"); return 0; }

  // Foreign mode: NEUI_DND_TARGET names an external XdndAware window to drag
  // onto; otherwise we drop onto our own pane (internal path).
  const char* foreign = getenv("NEUI_DND_TARGET");

  neui_widget_t win = W->create(SESS, widget_none, NEUI_W_APPWINDOW, 100, 100, 400, 300, nullptr);
  W->set_text(SESS, win, "neui dndsrc smoke");
  neui_widget_t pane = W->create(SESS, win, NEUI_W_CUSTOMDRAW, 0, 0, 400, 300, nullptr);
  if (!foreign) {
    DND->set_drop_target(SESS, pane, true);
    const char* mimes[] = { NEUI_MIME_TEXT };
    DND->set_accepted_formats(SESS, pane, mimes, 1);
  } else {
    g_warp_target = foreign;   // drag onto the external target instead
  }
  W->show(SESS, win);

  // Pump so the window maps + XdndAware registers before we drag.
  for (int i = 0; i < 40; ++i) { xpl_host::platform_pump_once(); usleep(10000); }

  const char* payload = "internal-drag-payload";
  neui_data_item_t item = CB->create_item(SESS);
  CB->item_set_format(SESS, item, NEUI_MIME_TEXT, payload, (uint32_t)strlen(payload));

  std::thread driver(drive_mouse);
  neui_dnd_action_t act = DND->begin_drag(SESS, pane, item, NEUI_DND_ACTION_COPY);
  driver.join();

  printf("begin_drag returned action=%d\n", (int)act);

  int fail = 0;
  if (foreign) {
    // The external target receives + verifies the payload; we just confirm
    // the negotiated action came back through the XDND handshake.
    printf("foreign drop onto '%s'\n", foreign);
    if (act != NEUI_DND_ACTION_COPY) { printf("FAIL: action != COPY\n"); ++fail; }
  } else {
    printf("dropped=%d text='%s'\n", g_dropped, g_text);
    if (g_dropped < 1)                { printf("FAIL: no drop\n"); ++fail; }
    if (strcmp(g_text, payload) != 0) { printf("FAIL: payload mismatch\n"); ++fail; }
    if (act != NEUI_DND_ACTION_COPY)  { printf("FAIL: action != COPY\n"); ++fail; }
  }
  printf(fail ? "\nDRAG-SOURCE FAILED (%d)\n" : "\nDRAG-SOURCE OK\n", fail);
  return fail ? 1 : 0;
}
