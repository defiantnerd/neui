// In-frame menubar verification (Linux/X11). Builds an APPWINDOW with a
// menubar (File with a nested submenu, Edit with built-in-command + shortcut
// bindings), then from a background thread drives synthetic clicks + key
// accelerators at the frame window and asserts which menu item activated.
//
// Coverage:
//   - keyboard accelerators -> dispatch_menu_event routing (Ctrl+Z, Ctrl+S)
//   - click a top-level label -> dropdown opens -> click a leaf -> activation
//   - click a submenu row -> cascade opens -> click a nested leaf (deeper than
//     one level)
//
// Needs a live X display and a cooperative (non-tiling) WM, so it is built but
// NOT ctest-registered; run ./tests/neui_menubar_smoke manually on a desktop.

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
static neui_tree_api_t*   T;
static neui_session_t     SESS;
static neui_widget_t      WIN, MB;

// Item ids we assert against, captured at build time.
static neui_item_t IT_NEW, IT_OPEN, IT_SAVE, IT_RECENT, IT_DOC1, IT_UNDO;

static uint32_t g_last_activated = 0;   // neui item id of the last TREE_ITEM_ACTIVATED

static bool NEUI_ABI onevent(void*, neui_event_t* e)
{
  if (e->type == NEUI_EVENT_TREE_ITEM_ACTIVATED)
    g_last_activated = e->data.tree.item.id;
  return e->type == NEUI_EVENT_APP_QUIT;
}
static neui_widget_client_t WC = { NEUI_VERSION, nullptr, onevent };
static void* NEUI_ABI iface(void*, const char* n)
{ return strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&WC; }
static neui_client_t CL = { NEUI_VERSION, iface };

// ---- X helpers ------------------------------------------------------------

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

static Display* X = nullptr;
static Window   FRAME = 0;
static double   SCALE = 1.0;   // physical px per logical px (from window width)

static void pump(int iterations)
{
  for (int i = 0; i < iterations; ++i) { xpl_host::platform_pump_once(); usleep(8000); }
}

// Synthetic left click at logical (lx, ly) in the frame window.
static void click(int lx, int ly)
{
  int px = (int)(lx * SCALE + 0.5), py = (int)(ly * SCALE + 0.5);
  XButtonEvent be; memset(&be, 0, sizeof be);
  be.display = X; be.window = FRAME; be.root = DefaultRootWindow(X);
  be.same_screen = True; be.x = px; be.y = py; be.button = Button1;
  be.time = CurrentTime;
  be.type = ButtonPress;   XSendEvent(X, FRAME, True, ButtonPressMask,   (XEvent*)&be);
  be.type = ButtonRelease; XSendEvent(X, FRAME, True, ButtonReleaseMask, (XEvent*)&be);
  XFlush(X);
  pump(12);
}

// Synthetic key (with modifier state) to the frame window.
static void key(KeySym ks, unsigned int state)
{
  XKeyEvent ke; memset(&ke, 0, sizeof ke);
  ke.display = X; ke.window = FRAME; ke.root = DefaultRootWindow(X);
  ke.same_screen = True; ke.x = ke.y = 5; ke.time = CurrentTime;
  ke.keycode = XKeysymToKeycode(X, ks); ke.state = state;
  ke.type = KeyPress;   XSendEvent(X, FRAME, True, KeyPressMask,   (XEvent*)&ke);
  ke.type = KeyRelease; XSendEvent(X, FRAME, True, KeyReleaseMask, (XEvent*)&ke);
  XFlush(X);
  pump(12);
}

static int g_fail = 0;
static void check(const char* what, uint32_t got, neui_item_t want)
{
  if (got == want.id) printf("ok:   %s -> item %u\n", what, got);
  else { printf("FAIL: %s -> got item %u want %u\n", what, got, want.id); ++g_fail; }
}
static void check_int(const char* what, int got, int want)
{
  if (got == want) printf("ok:   %s -> %d\n", what, got);
  else { printf("FAIL: %s -> got %d want %d\n", what, got, want); ++g_fail; }
}

int main()
{
  neui_init();
  neui_api_t* api = neui_get_api(nullptr);
  if (!api) { printf("no host\n"); return 0; }
  SESS = api->create_session(&CL, nullptr);
  W = (neui_widget_api_t*)api->get_interface(SESS, NEUI_API_WIDGETS);
  T = (neui_tree_api_t*)  api->get_interface(SESS, NEUI_API_TREE);
  if (!W || !T) { printf("missing api\n"); return 0; }

  WIN = W->create(SESS, widget_none, NEUI_W_APPWINDOW, 200, 200, 500, 360, nullptr);
  W->set_text(SESS, WIN, "menubar smoke");

  MB = W->create(SESS, WIN, NEUI_W_MENUBAR, 0, 0, 0, 0, nullptr);
  neui_item_t file = T->add(SESS, MB, tree_item_root, "File", nullptr);
  IT_NEW    = T->add(SESS, MB, file, "New",  (void*)1);
  IT_OPEN   = T->add(SESS, MB, file, "Open", (void*)2);
  IT_RECENT = T->add(SESS, MB, file, "Recent", nullptr);     // submenu (has children)
  IT_DOC1   = T->add(SESS, MB, IT_RECENT, "doc1.txt", (void*)31);
              T->add(SESS, MB, IT_RECENT, "doc2.txt", (void*)32);
              T->add(SESS, MB, file, "-", nullptr);
  IT_SAVE   = T->add(SESS, MB, file, "Save", (void*)4);
  T->set_shortcut(SESS, MB, IT_SAVE, NEUI_KMOD_CTRL, NEUI_KEY_S);

  neui_item_t edit = T->add(SESS, MB, tree_item_root, "Edit", nullptr);
  IT_UNDO = T->add(SESS, MB, edit, "Undo", (void*)10);
  T->set_shortcut(SESS, MB, IT_UNDO, NEUI_KMOD_CTRL, NEUI_KEY_Z);
  T->set_menu_cmd(SESS, MB, IT_UNDO, NEUI_CMD_UNDO);  // no focused widget -> falls through to ACTIVATED

  W->show(SESS, WIN);
  pump(40);

  // Public client-area query: with a menubar the content area starts at the
  // band bottom (24 px) and is that much shorter than the full frame.
  {
    int crx, cry, crw, crh, sw, sh;
    W->get_client_rect(SESS, WIN, &crx, &cry, &crw, &crh);
    W->get_size(SESS, WIN, &sw, &sh);
    check_int("client_rect.x", crx, 0);
    check_int("client_rect.y (band height)", cry, 24);
    check_int("client_rect.w", crw, sw);
    check_int("client_rect.h", crh, sh - 24);
  }

  // Exercise the toast path (anchors below the band, clipped to client area):
  // just make sure it paints a few frames without crashing.
  if (auto* N = (neui_notify_api_t*)api->get_interface(SESS, NEUI_API_NOTIFY)) {
    N->toast(SESS, WIN, "toast below the menubar band");
    pump(20);
  }

  // Locate the frame window + derive the logical->physical scale.
  X = XOpenDisplay(nullptr);
  if (!X) { printf("no display\n"); return 0; }
  Atom NN = XInternAtom(X, "_NET_WM_NAME", False), U8 = XInternAtom(X, "UTF8_STRING", False);
  for (int i = 0; i < 300 && !FRAME; ++i) {
    FRAME = find_by_name(X, DefaultRootWindow(X), NN, U8, "menubar smoke");
    if (!FRAME) usleep(10000);
  }
  if (!FRAME) { printf("frame window not found\n"); return 0; }
  XWindowAttributes wa;
  if (XGetWindowAttributes(X, FRAME, &wa) && wa.width > 0) SCALE = wa.width / 500.0;
  std::this_thread::sleep_for(std::chrono::milliseconds(150));  // map + settle
  pump(10);

  // --- accelerators (DPI-independent) ---
  g_last_activated = 0;
  key(XK_z, ControlMask);
  check("Ctrl+Z accelerator", g_last_activated, IT_UNDO);

  g_last_activated = 0;
  key(XK_s, ControlMask);
  check("Ctrl+S accelerator", g_last_activated, IT_SAVE);

  // --- click open + leaf pick ---
  // Band: "File" label starts at x=0 (band y in [0,24)). Dropdown column 0
  // anchors at x=0, y=24; rows start 4 px in, each 22 px tall ("New" first).
  g_last_activated = 0;
  click(10, 12);   // open File
  click(20, 38);   // "New" row (24 + 4 .. 24 + 4 + 22)
  check("click File>New", g_last_activated, IT_NEW);

  // --- cascade: File > Recent > doc1 (deeper than one level) ---
  // "Recent" is the 3rd row: y = 24 + (4 + 22 + 22) .. ; col 0 width clamps to
  // POPUP_MIN_W (140), so the submenu column anchors near x=140.
  g_last_activated = 0;
  click(10, 12);    // open File
  click(20, 83);    // "Recent" submenu row (24 + 4 + 44 + 11)
  click(200, 87);   // "doc1.txt" in the cascade column (right of col 0, y=24+44+4+11)
  check("click File>Recent>doc1", g_last_activated, IT_DOC1);

  printf(g_fail ? "\nMENUBAR FAILED (%d)\n" : "\nMENUBAR OK\n", g_fail);
  if (X) XCloseDisplay(X);
  return g_fail ? 1 : 0;
}
