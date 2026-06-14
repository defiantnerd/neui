// Message-box verification: open modal boxes and dismiss them from a
// background thread (find the box by its caption, send Enter/Esc), checking
// the returned NEUI_ID_*. Linux-only; needs a live X display.

#include <neui/neui.h>
#include "platform.h"   // xpl_host::platform_pump_once

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

static neui_widget_api_t* W;
static neui_notify_api_t* N;
static neui_session_t     SESS;
static neui_widget_t      WIN;

static bool NEUI_ABI onevent(void*, neui_event_t* e)
{ return e->type == NEUI_EVENT_APP_QUIT; }
static neui_widget_client_t WC = { NEUI_VERSION, nullptr, onevent };
static void* NEUI_ABI iface(void*, const char* n)
{ return strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&WC; }
static neui_client_t CL = { NEUI_VERSION, iface };

static Window find_by_name(Display* d, Window r, Atom NN, Atom U8, const char* want)
{
  Atom t; int f; unsigned long n, a; unsigned char* v = 0;
  if (XGetWindowProperty(d, r, NN, 0, 256, False, U8, &t, &f, &n, &a, &v) == Success && v) {
    int m = !strcmp((char*)v, want); XFree(v); if (m) return r;
  }
  Window rr, p, *k = 0; unsigned int nk = 0; Window found = 0;
  if (XQueryTree(d, r, &rr, &p, &k, &nk)) {
    for (unsigned i = 0; i < nk && !found; ++i) found = find_by_name(d, k[i], NN, U8, want);
    if (k) XFree(k);
  }
  return found;
}

// Wait for the box titled `caption`, then send `ks` (Enter / Escape) to it.
static void dismiss(const char* caption, KeySym ks)
{
  Display* d = XOpenDisplay(nullptr); if (!d) return;
  Atom NN = XInternAtom(d, "_NET_WM_NAME", False), U8 = XInternAtom(d, "UTF8_STRING", False);
  Window root = DefaultRootWindow(d), w = 0;
  for (int i = 0; i < 300 && !w; ++i) { w = find_by_name(d, root, NN, U8, caption); if (!w) usleep(10000); }
  if (!w) { printf("DISMISS: '%s' not found\n", caption); XCloseDisplay(d); return; }
  std::this_thread::sleep_for(std::chrono::milliseconds(120));  // let it map + focus
  XKeyEvent ke; memset(&ke, 0, sizeof ke);
  ke.display = d; ke.window = w; ke.root = root; ke.time = CurrentTime;
  ke.x = ke.y = 5; ke.same_screen = True; ke.keycode = XKeysymToKeycode(d, ks); ke.state = 0;
  ke.type = KeyPress;   XSendEvent(d, w, True, KeyPressMask,   (XEvent*)&ke);
  ke.type = KeyRelease; XSendEvent(d, w, True, KeyReleaseMask, (XEvent*)&ke);
  XFlush(d); XCloseDisplay(d);
}

static int g_fail = 0;
static void check(const char* what, int got, int want)
{
  if (got == want) printf("ok:   %s -> %d\n", what, got);
  else { printf("FAIL: %s -> got %d want %d\n", what, got, want); ++g_fail; }
}

static int box(const char* text, const char* caption, uint32_t flags, KeySym ks)
{
  std::thread t(dismiss, caption, ks);
  int r = N->message_box(SESS, WIN, text, caption, flags);
  t.join();
  return r;
}

int main()
{
  neui_init();
  neui_api_t* api = neui_get_api(nullptr);
  if (!api) { printf("no host\n"); return 0; }
  SESS = api->create_session(&CL, nullptr);
  W = (neui_widget_api_t*)api->get_interface(SESS, NEUI_API_WIDGETS);
  N = (neui_notify_api_t*)api->get_interface(SESS, NEUI_API_NOTIFY);
  if (!W || !N) { printf("missing api\n"); return 0; }

  WIN = W->create(SESS, widget_none, NEUI_W_APPWINDOW, 200, 200, 500, 360, nullptr);
  W->set_text(SESS, WIN, "notify smoke owner");
  W->show(SESS, WIN);
  for (int i = 0; i < 40; ++i) { xpl_host::platform_pump_once(); usleep(10000); }

  check("OK + Enter",
        box("This is an information box.", "MBInfo",
            NEUI_MB_OK | NEUI_MB_ICONINFORMATION, XK_Return),
        NEUI_ID_OK);
  check("YESNO defbutton2 + Enter",
        box("Discard your changes?", "MBQuestion",
            NEUI_MB_YESNO | NEUI_MB_DEFBUTTON2 | NEUI_MB_ICONQUESTION, XK_Return),
        NEUI_ID_NO);
  check("OKCANCEL + Esc",
        box("A warning, with a longer body that should wrap across a couple of "
            "lines to exercise the word-wrap layout path.", "MBWarn",
            NEUI_MB_OKCANCEL | NEUI_MB_ICONWARNING, XK_Escape),
        NEUI_ID_CANCEL);

  printf(g_fail ? "\nNOTIFY FAILED (%d)\n" : "\nNOTIFY OK\n", g_fail);
  return g_fail ? 1 : 0;
}
