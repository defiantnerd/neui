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

#include <atomic>
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

// Set from the drag dispatch on the MAIN thread (inside begin_drag's spin) and
// read by the driver thread: proof that the warped motion actually reached the
// target, so the button-release is sent when there is something to drop on
// rather than at a guessed moment. See drive_mouse().
static std::atomic<bool> g_entered{false};

static bool NEUI_ABI onevent(void* /*tok*/, neui_event_t* e)
{
  switch (e->type) {
    case NEUI_EVENT_DND_ENTER:
    case NEUI_EVENT_DND_MOVE:
      g_entered.store(true);
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
//
// Both waits here are on OBSERVABLE STATE rather than on a sleep long enough to
// probably work. This harness used to fail about two runs in five, and both
// races were timing guesses:
//
//   1. begin_drag takes an XGrabPointer on the root window. Motion warped before
//      that grab lands is delivered to whatever is under the pointer instead of
//      to the drag, so the target is never entered and the drop never happens.
//      The drag announces itself by taking ownership of the XdndSelection - and
//      it does so on the line ABOVE the grab, on the same connection, so waiting
//      for the owner and then letting the grab land is both precise and cheap.
//      (Probing with our own XGrabPointer would be more direct but can STEAL the
//      grab from begin_drag, which fails the drag outright - the observation
//      must not perturb what it observes.)
//   2. the release was sent a fixed 200 ms after the last warp. If the enter had
//      not been processed yet, the drag ended over nothing. g_entered is set by
//      the DND_ENTER/DND_MOVE dispatch on the main thread, so the release now
//      waits for the target to have actually been entered.
//
// Every wait is bounded: on timeout the release is sent anyway so begin_drag
// returns and main() reports a real failure instead of hanging.
static bool wait_until(std::chrono::milliseconds limit, bool (*pred)(void*), void* ctx)
{
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred(ctx)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred(ctx);
}

static void drive_mouse()
{
  Display* d = XOpenDisplay(nullptr); if (!d) return;
  Atom NN = XInternAtom(d, "_NET_WM_NAME", False), U8 = XInternAtom(d, "UTF8_STRING", False);
  Window root = DefaultRootWindow(d);

  // (1) wait for the drag to exist, then let its pointer grab land.
  Atom xdnd_sel = XInternAtom(d, "XdndSelection", False);
  struct SelCtx { Display* d; Atom sel; };
  SelCtx sc{ d, xdnd_sel };
  const bool dragging = wait_until(std::chrono::milliseconds(3000),
      [](void* p) {
        auto* c = static_cast<SelCtx*>(p);
        return XGetSelectionOwner(c->d, c->sel) != None;
      }, &sc);
  if (!dragging) printf("DRIVER: no XdndSelection owner - drag never started\n");

  Window w = find_by_name(d, root, NN, U8, g_warp_target);
  if (!w) { printf("DRIVER: window not found\n"); XCloseDisplay(d); return; }
  Window relwin = find_by_name(d, root, NN, U8, "neui dndsrc smoke");  // source frame

  // (2) KEEP MOVING until the target reports it was entered, rather than warping
  // a fixed number of times and hoping the grab was up for one of them. This is
  // the part that makes the harness deterministic: it needs no estimate of when
  // begin_drag's XGrabPointer lands, because a warp that arrives too early is
  // simply followed by another one. A single teleport is one motion event, and
  // losing that one event loses the entire drag - which is what the fixed-sleep
  // version did, intermittently.
  //
  // Two subtleties, both of which cost a debugging round:
  //
  //  - the target centre is recomputed EVERY iteration. The window manager
  //    reparents the frame into its decorations on its own schedule (seconds,
  //    when the machine is busy), and until it does, the window still reports
  //    its pre-reparent position. Computing the centre once, up front, can
  //    therefore aim the warp at where the window used to be - and since the
  //    loop would then keep warping to that same wrong spot, it never recovers.
  //    Re-reading makes it self-correct the moment the WM is done.
  //  - the jiggle alternates two positions because X coalesces a warp to where
  //    the pointer already is into no motion event at all: after a previous run
  //    left the pointer on the pane, warping "to the centre" once is a no-op.
  const auto move_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  bool entered = false;
  while (std::chrono::steady_clock::now() < move_deadline) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, w, &a)) break;
    int cx = 0, cy = 0; Window ch = 0;
    XTranslateCoordinates(d, w, root, a.width / 2, a.height / 2, &cx, &cy, &ch);

    if ((entered = g_entered.load())) break;
    XWarpPointer(d, None, root, 0, 0, 0, 0, cx - 5, cy); XFlush(d);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if ((entered = g_entered.load())) break;
    XWarpPointer(d, None, root, 0, 0, 0, 0, cx,     cy); XFlush(d);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (!entered && !g_entered.load())
    printf("DRIVER: target never entered - releasing anyway\n");

  XWindowAttributes a; XGetWindowAttributes(d, w, &a);
  int cx = 0, cy = 0; Window ch = 0;
  XTranslateCoordinates(d, w, root, a.width / 2, a.height / 2, &cx, &cy, &ch);

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

  // Pump until the window is genuinely ON SCREEN and viewable - not for a fixed
  // 400 ms, and not merely until show() returned.
  //
  // This matters more here than anywhere else in the suite because begin_drag
  // BLOCKS: it spins on its own X connection servicing the drag and never runs
  // the neui event loop, so whatever the window manager had not finished
  // negotiating when the drag starts stays unfinished for the drag's entire
  // lifetime. It cannot recover on its own.
  //
  // Observed directly, and only when this harness ran straight after the others:
  // Weston had reparented our window into a decoration frame still parked at its
  // offscreen placeholder (-32730,-32709) and not yet viewable. The driver then
  // computed the pane centre from that offscreen rect, warped the pointer out
  // there, and the drag quite correctly found no XdndAware window under it for
  // eight seconds. Nothing about the drag-source code was wrong; the test had
  // simply started dragging a window the desktop had not put up yet.
  {
    Display* probe = XOpenDisplay(nullptr);
    Atom pNN = probe ? XInternAtom(probe, "_NET_WM_NAME", False) : 0;
    Atom pU8 = probe ? XInternAtom(probe, "UTF8_STRING", False) : 0;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool ready = false;
    int nudges = 0;
    while (std::chrono::steady_clock::now() < limit) {
      xpl_host::platform_pump_once();
      usleep(10000);
      if (!probe) continue;
      Window wx = find_by_name(probe, DefaultRootWindow(probe), pNN, pU8,
                               "neui dndsrc smoke");
      if (!wx) continue;
      XWindowAttributes at;
      if (!XGetWindowAttributes(probe, wx, &at)) continue;
      if (at.map_state != IsViewable) continue;      // frame still unmapped
      int rx = 0, ry = 0; Window ch = 0;
      XTranslateCoordinates(probe, wx, DefaultRootWindow(probe), 0, 0, &rx, &ry, &ch);
      if (rx < -10000 || ry < -10000) {             // still parked offscreen
        // Nudge: re-assert the position we asked for. Observed on WSLg's Weston
        // after a particular run of the suite - the frame is reparented but left
        // at its offscreen placeholder and simply never placed, while a plain
        // Xlib window mapped in the same moment appears instantly. Re-sending the
        // geometry gives the WM another placement request to act on.
        if (++nudges <= 8) W->set_pos(SESS, win, 100, 100, 400, 300);
        usleep(200000);
        continue;
      }
      ready = true;
      break;
    }
    if (!ready) printf("WARN: window never became viewable on screen\n");
    // A little extra pumping so the XdndAware registration and first paint land.
    for (int i = 0; i < 20; ++i) { xpl_host::platform_pump_once(); usleep(10000); }
    if (probe) XCloseDisplay(probe);
  }

  const char* payload = "internal-drag-payload";
  neui_data_item_t item = CB->create_item(SESS);
  CB->item_set_format(SESS, item, NEUI_MIME_TEXT, payload, (uint32_t)strlen(payload));

  // begin_drag opens with XGrabPointer and gives up - returning 0, with no drag
  // at all - if it cannot have the pointer. Another X client holding a grab is
  // therefore not a drag-source bug but an unrunnable test, and the two are
  // indistinguishable from the outside: the run just reports "no drop" after the
  // driver has spent its whole budget warping a pointer nobody is listening to.
  //
  // A previous harness in the same suite that is still shutting down is exactly
  // such a client, which is what made this fail when run after the others but
  // never on its own. So wait for the pointer to be grabbable before starting.
  // Probing here is safe - it is strictly before our own grab, so unlike inside
  // drive_mouse there is nothing of ours to steal it from.
  if (Display* probe = XOpenDisplay(nullptr)) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool free_ptr = false;
    while (!free_ptr && std::chrono::steady_clock::now() < limit) {
      if (XGrabPointer(probe, DefaultRootWindow(probe), False, ButtonPressMask,
                       GrabModeAsync, GrabModeAsync, None, None,
                       CurrentTime) == GrabSuccess) {
        XUngrabPointer(probe, CurrentTime);
        XSync(probe, False);
        free_ptr = true;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    if (!free_ptr) printf("WARN: another client holds the pointer grab\n");
    XCloseDisplay(probe);
  }

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
