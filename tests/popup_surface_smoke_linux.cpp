// Popup-surface acceptance harness (NEUI_W_POPUPSURFACE / NEUI_API_POPUP),
// Linux / X11 / xpl host.
//
// WHY THIS EXISTS. The Linux backing shipped WRITTEN BUT NEVER COMPILED - it was
// authored on macOS, where this platform does not build, exactly the risk
// CLAUDE.md's cross-platform hygiene note describes. It is the last of the three
// desktop backings to get first evidence. A build alone only proves the API
// calls typecheck; it says nothing about whether the XGrabPointer really lands
// after the map, or whether an override-redirect window keeps the editor's
// focus - the two items docs/popup-surfaces.md names as check-first here.
//
// Like popup_surface_smoke_win32.cpp it is NOT a port of the macOS harness. The
// portable half - placement arithmetic, cascade ownership, dismissal ordering -
// is proven once on macOS and is the same code here. This targets the half X11
// writes for itself, and asks **X11** rather than neui:
//
//   1. WINDOW      - the popup is a real top-level window: a direct child of the
//                    ROOT window, override-redirect, sized exactly as asked
//                    (borderless means no WM decoration inflation), and its rect
//                    genuinely leaves the owner's. That is the whole claim.
//   2. HINT        - _NET_WM_WINDOW_TYPE_POPUP_MENU, the advisory that stops a
//                    compositor animating or shadowing it as a normal window.
//   3. PLACEMENT   - the popup's top-left really is the anchor's bottom-left
//                    plus the requested offset, measured in ROOT coordinates.
//   4. GRAB        - check-first item #1. XGrabPointer must SUCCEED, which it
//                    only does if the map has already landed - hence the XSync
//                    at the end of platform_show_popup_surface, not an XFlush.
//                    Asserted from a SECOND X connection: while the popup is up
//                    that connection must be refused with AlreadyGrabbed, and
//                    once it closes the grab must be released again.
//   5. FOCUS       - check-first item #2. An override-redirect window must not
//                    take the input focus: the editor keeps it across the open,
//                    and the popup's window is never the focus window. This is
//                    the X11 analogue of the WS_EX_NOACTIVATE defect win32 hit.
//   6. PSEUDO-FOCUS - X sends FocusOut/FocusIn pairs around any keyboard grab
//                    taken by ANOTHER client (mode NotifyGrab / NotifyUngrab).
//                    Nobody switched applications, so the stack must survive it;
//                    a REAL focus change must still dismiss. This pair is a
//                    regression guard for a defect this harness found: the
//                    handler acted on every FocusOut, so an idle popup vanished
//                    a few seconds after opening whenever anything, anywhere,
//                    briefly grabbed the keyboard.
//   7. CASCADE     - a second level opens without the first being dismissed
//                    (the PopupPlacingScope regression: placing a level fires
//                    ConfigureNotify for the window being placed, which the
//                    owner-moved hook read as "the owner moved"), and the unwind
//                    is deepest-first.
//   8. OWNER MOVED - moving the OWNER does dismiss, the other half of that hook:
//                    suppressing the placing case must not suppress the real one.
//   9. THE GATE    - the suppression that replaces an OS pointer capture, plus
//                    the keyboard half of it (a key is retargeted onto the
//                    deepest surface rather than falling through to the owner,
//                    and Tab stays inside the open level), driven through
//                    Session directly for the same reason the macOS harness
//                    does: what is asserted is the DECISION, and no client ever
//                    calls it - the platform layer does.
//   9b. WHEEL      - the one gate branch driven through REAL events instead
//                    (XSendEvent from the second connection, which grabs cannot
//                    intercept), because what can break here is X11's routing
//                    rather than the decision: a notch outside the stack must not
//                    reach the widget underneath and must not DISMISS (a wheel is
//                    a core ButtonPress on X11, so an ordering slip in
//                    dispatch_button_press would send it through the press gate),
//                    while a notch aimed at the popup must reach the popup - the
//                    scrolling-SECTION idiom depends on it.
//  10. LIFETIME    - destroying the OWNER must leave no window on the server.
//                    The surface is its own root child, so the owner's subtree
//                    does not contain it: without the explicit owner/anchor
//                    check this strands a live override-redirect window over the
//                    desktop belonging to a frame that no longer exists, with
//                    the pointer grab still held - which on X11 means a desktop
//                    that no longer responds to the mouse.
//
// Realizes real X windows and needs a live display, so this is built but NOT
// ctest-registered (keeps headless `ctest` runs green); run
// ./tests/neui_popup_surface_smoke_linux by hand on an X desktop.

#include <neui/neui.h>
#include <neui/d/keys.h>

// Host internals, for phase 9 only - the input gate is not reachable through the
// public API by design.
#include "../hosts/crossplatform/host.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
const char* g_phase_name = "startup";

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void phase(const char* name)
{
  g_phase_name = name;
  std::printf("\n--- %s\n", name);
}

neui_api_t*        g_api = nullptr;
neui_session_t     g_sess{};
neui_widget_api_t* g_w   = nullptr;
neui_popup_api_t*  g_pu  = nullptr;

struct Dismissal { uint32_t widget_id; uint32_t reason; };
std::vector<Dismissal> g_dismissals;

// Phase 9b: which widget a wheel notch actually landed on. The popup's own child
// is named so "swallowed underneath" and "delivered inside" are counted apart.
uint32_t g_popup_child_id = 0;
int      g_wheels_under   = 0;
int      g_wheels_in_popup = 0;

// A real tab stop inside the picker - phase 9's key / traversal checks need a
// focusable widget in the popup, not just the painted body.
neui_widget_t g_popup_edit{};

// Keys the gate RETARGETED onto a surface.
struct KeyHit { uint32_t widget_id; uint32_t type; uint32_t keycode; };
std::vector<KeyHit> g_keys;

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_WIDGET_PAINT) {
    // Paint something real so the popup's own render target is exercised.
    ev->data.paint.painter_api->fill_rect(ev->data.paint.p, 0, 0,
                                          ev->data.paint.width,
                                          ev->data.paint.height,
                                          0xFF203040u);
    return true;
  }
  if (ev->type == NEUI_EVENT_KEYDOWN || ev->type == NEUI_EVENT_KEYUP ||
      ev->type == NEUI_EVENT_KEYCHAR) {
    g_keys.push_back({ ev->data.key.widget.id,
                       static_cast<uint32_t>(ev->type), ev->data.key.keycode });
    return false;   // not consumed - the gate swallows either way
  }
  if (ev->type == NEUI_EVENT_POPUP_DISMISSED) {
    g_dismissals.push_back({ ev->data.popup.widget.id, ev->data.popup.reason });
    return true;
  }
  if (ev->type == NEUI_EVENT_MOUSE_WHEEL) {
    if (ev->data.wheel.widget.id == g_popup_child_id) ++g_wheels_in_popup;
    else                                              ++g_wheels_under;
    return false;   // don't consume: nothing here should be masked by the harness
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* NEUI_ABI iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

// Widget handle -> tree slot, the same split the host uses.
uint32_t slot_of(neui_widget_t w) { return w.id & 0xFFFFu; }

// ---- watchdog --------------------------------------------------------------
// This harness pumps a real event loop against a real X server. A hang would
// report nothing at all, so an overrun becomes a named failure.
std::atomic<bool> g_finished{false};
std::atomic<long long> g_deadline_ms{0};

long long now_ms()
{
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void watchdog()
{
  while (!g_finished.load()) {
    const long long d = g_deadline_ms.load();
    if (d != 0 && now_ms() > d) {
      std::fprintf(stderr, "\n[FAIL] TIMEOUT in phase: %s\n", g_phase_name);
      std::fflush(nullptr);
      std::_Exit(3);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void pump_for(int ms)
{
  g_deadline_ms.store(now_ms() + ms + 10000);
  const long long end = now_ms() + ms;
  while (now_ms() < end) {
    g_api->pump_once(g_sess);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  g_deadline_ms.store(0);
}

// Pump until `pred` holds, or the deadline passes. Returns pred's final value,
// so the CHECK still does the asserting - this only decides how long to wait.
//
// Every wait in this harness that precedes a positive assertion goes through
// here rather than through a fixed pump_for(). Nothing an X server does is
// synchronous with the call that requested it: mapping a window, moving one,
// and the WM handing over the focus all land when they land. A fixed sleep that
// is long enough on an idle machine is not long enough when the rest of the
// suite just ran, which is exactly how this harness failed - intermittently, and
// only back-to-back with the other tests.
//
// Waiting on the condition is also FASTER in the normal case: it returns as soon
// as the thing happens instead of always burning the worst-case delay.
template <class Pred>
bool pump_until(Pred pred, int limit_ms)
{
  g_deadline_ms.store(now_ms() + limit_ms + 10000);
  const long long end = now_ms() + limit_ms;
  while (now_ms() < end) {
    if (pred()) { g_deadline_ms.store(0); return true; }
    g_api->pump_once(g_sess);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool r = pred();
  g_deadline_ms.store(0);
  return r;
}

// ---- the X side ------------------------------------------------------------
// A SECOND connection to the same display. Two connections are two X clients
// even inside one process, which is what lets this file grab the keyboard "as
// another application" in phase 6 and be refused the pointer in phase 4.
Display* g_xd = nullptr;

struct WinInfo {
  Window window = 0;
  int    x = 0, y = 0;          // root coordinates
  int    w = 0, h = 0;          // physical px
  bool   override_redirect = false;
};

// Every mapped, override-redirect DIRECT CHILD OF ROOT. Being a child of root is
// half the claim: a popup that had been parented into the owner would be a child
// window, could not leave it, and would not show up here at all.
std::vector<WinInfo> override_redirect_children()
{
  std::vector<WinInfo> out;
  Window root = DefaultRootWindow(g_xd), parent = 0, *kids = nullptr;
  unsigned int n = 0;
  if (!XQueryTree(g_xd, root, &root, &parent, &kids, &n)) return out;
  for (unsigned int i = 0; i < n; ++i) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(g_xd, kids[i], &a)) continue;
    if (a.map_state != IsViewable || !a.override_redirect) continue;
    WinInfo wi;
    wi.window = kids[i];
    wi.w = a.width; wi.h = a.height;
    wi.override_redirect = true;
    Window child = 0;
    XTranslateCoordinates(g_xd, kids[i], root, 0, 0, &wi.x, &wi.y, &child);
    out.push_back(wi);
  }
  if (kids) XFree(kids);
  return out;
}

// The override-redirect windows that are OURS: everything present now that was
// not present in the baseline snapshot.
//
// A plain count of override-redirect children is not good enough. The X server
// is shared, and a previous run of this same harness (or any other neui test) is
// still tearing its windows down for a short while after its process exits -
// windows go away when the connection closes, which is asynchronous relative to
// the next process starting. A baseline COUNT therefore drifts downwards
// mid-run, which read as "a popup was left behind" / "a popup never appeared"
// and made this harness fail when run back-to-back with the rest of the suite.
// A set difference only ever asks "what did WE add", which stale teardown
// cannot perturb.
std::vector<WinInfo> new_popups(const std::set<Window>& baseline)
{
  std::vector<WinInfo> out;
  for (auto& wi : override_redirect_children())
    if (!baseline.count(wi.window)) out.push_back(wi);
  return out;
}

std::set<Window> snapshot_or_windows()
{
  std::set<Window> s;
  for (auto& wi : override_redirect_children()) s.insert(wi.window);
  return s;
}

// Wait until the server's override-redirect window set stops changing, so the
// baseline is taken after any previous test's windows have finished going away.
std::set<Window> settled_baseline()
{
  std::set<Window> prev = snapshot_or_windows();
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::set<Window> now = snapshot_or_windows();
    if (now == prev) return now;
    prev = now;
  }
  return prev;
}

// The owner's client window, found by the title neui set on it.
Window find_window_by_name(Window w, const char* name, int depth = 0)
{
  if (depth > 6) return 0;
  char* got = nullptr;
  if (XFetchName(g_xd, w, &got) && got) {
    const bool hit = std::strcmp(got, name) == 0;
    XFree(got);
    if (hit) return w;
  }
  Window root = 0, parent = 0, *kids = nullptr;
  unsigned int n = 0;
  if (!XQueryTree(g_xd, w, &root, &parent, &kids, &n)) return 0;
  Window found = 0;
  for (unsigned int i = 0; i < n && !found; ++i)
    found = find_window_by_name(kids[i], name, depth + 1);
  if (kids) XFree(kids);
  return found;
}

// Post a real core wheel notch (X buttons 4/5) at (x, y) in `target`'s OWN
// coordinates, over the SECOND connection - so it arrives as input from another X
// client, the closest analogue of the win32 harness's PostMessageW. Two properties
// make this the right tool here:
//
//   - XSendEvent bypasses grabs entirely, so it works while the host is holding
//     its XGrabPointer, which is precisely when the gate is interesting;
//   - it targets a WINDOW, which is the thing under test: X delivers a real wheel
//     to whatever is under the pointer, so "outside the stack" and "inside the
//     popup" are two different destination windows, not two coordinates.
//
// It exercises the LEGACY core path (`dispatch_button_press`, buttons 4-7). With
// XI2 compiled in the host drops those buttons once it has seen a real XI2 scroll
// on this server (`g_xi2_scroll_seen`), to avoid double-counting the same physical
// notch. Nothing in this harness scrolls physically, so that stays false - and the
// control check at the top of phase 9b is the guard: if someone brushes a trackpad
// mid-run, it fails there instead of silently proving nothing.
bool send_core_wheel(Window target, int x, int y, unsigned int button)
{
  if (!target) return false;
  int rx = 0, ry = 0;
  Window child = 0;
  XTranslateCoordinates(g_xd, target, DefaultRootWindow(g_xd), x, y,
                        &rx, &ry, &child);
  XEvent ev{};
  ev.type                = ButtonPress;
  ev.xbutton.display     = g_xd;
  ev.xbutton.window      = target;
  ev.xbutton.root        = DefaultRootWindow(g_xd);
  ev.xbutton.subwindow   = None;
  ev.xbutton.time        = CurrentTime;
  ev.xbutton.x           = x;
  ev.xbutton.y           = y;
  ev.xbutton.x_root      = rx;
  ev.xbutton.y_root      = ry;
  ev.xbutton.state       = 0;
  ev.xbutton.button      = button;
  ev.xbutton.same_screen = True;
  const bool ok = XSendEvent(g_xd, target, False, ButtonPressMask, &ev) != 0;
  XFlush(g_xd);
  return ok;
}

bool window_rect(Window w, int* x, int* y, int* cw, int* ch)
{
  XWindowAttributes a;
  if (!w || !XGetWindowAttributes(g_xd, w, &a)) return false;
  Window child = 0;
  XTranslateCoordinates(g_xd, w, DefaultRootWindow(g_xd), 0, 0, x, y, &child);
  *cw = a.width; *ch = a.height;
  return true;
}

std::string window_type_atom(Window w)
{
  Atom wt = XInternAtom(g_xd, "_NET_WM_WINDOW_TYPE", True);
  if (wt == None) return "";
  Atom at = None; int fmt = 0;
  unsigned long ni = 0, ba = 0; unsigned char* data = nullptr;
  std::string out;
  if (XGetWindowProperty(g_xd, w, wt, 0, 4, False, XA_ATOM, &at, &fmt,
                         &ni, &ba, &data) == Success && data) {
    if (ni >= 1) {
      char* an = XGetAtomName(g_xd, reinterpret_cast<Atom*>(data)[0]);
      if (an) { out = an; XFree(an); }
    }
    XFree(data);
  }
  return out;
}

Window current_focus()
{
  Window f = 0; int rev = 0;
  XGetInputFocus(g_xd, &f, &rev);
  return f;
}

// Has the WM reparented this client window into its decoration frame yet?
bool owner_is_reparented(Window w)
{
  Window root = 0, parent = 0, *kids = nullptr;
  unsigned int n = 0;
  if (!XQueryTree(g_xd, w, &root, &parent, &kids, &n)) return false;
  if (kids) XFree(kids);
  return parent != 0 && parent != root;
}

// Wait until the desktop has finished with the owner window, and report where it
// ended up. Everything downstream measures against this origin, so getting it
// early is the difference between asserting neui's placement and asserting a
// number the WM had not decided yet.
//
// The window goes through THREE states before it is usable, and each one on its
// own looks like "done" to a naive check - which is why this needs all three:
//
//   1. NOT YET REPARENTED. The client window is still a direct child of root,
//      sitting exactly where neui asked (160,140). It holds perfectly still
//      there, so a pure "geometry stopped changing" check returns the
//      PRE-reparent origin quite happily. The move into the decoration frame
//      happens later and in one step. Observed to take up to ~3 s here, so the
//      budget must be generous rather than clever.
//   2. REPARENTED BUT PARKED. Weston hands the frame a placeholder position
//      offscreen - (-32730,-32709) was the observed value - before putting it
//      where it belongs. That is also perfectly stable while it lasts, hence the
//      on-screen sanity test.
//   3. SETTLED. Reparented, on-screen, and not moving.
//
// This is not a product bug in any of its states; it is the harness needing to
// start after the desktop has finished. Note the cost of getting it wrong is not
// only a stale origin: a late WM move is a real ConfigureNotify on the owner, and
// neui correctly dismisses the popup stack as OWNER_MOVED - in the middle of a
// phase asserting the stack survives something else entirely.
bool wait_owner_settled(Window w, int* x, int* y, int* cw, int* ch, int limit_ms)
{
  const long long end = now_ms() + limit_ms;
  int px = 0, py = 0, pw = 0, ph = 0;
  int stable = 0;
  bool have_prev = false;

  while (now_ms() < end) {
    int nx = 0, ny = 0, nw = 0, nh = 0;
    const bool got = window_rect(w, &nx, &ny, &nw, &nh);
    // On-screen sanity: rules out the parked placeholder in state 2.
    const bool sane = got && nx > -10000 && ny > -10000;
    if (got && sane && owner_is_reparented(w)) {
      stable = (have_prev && nx == px && ny == py && nw == pw && nh == ph)
                 ? stable + 1 : 0;
      have_prev = true;
    } else {
      stable = 0; have_prev = false;
    }
    if (got) { px = nx; py = ny; pw = nw; ph = nh; }
    if (stable >= 3) break;          // ~360 ms of no movement, reparented, on-screen
    pump_for(120);
  }

  *x = px; *y = py; *cw = pw; *ch = ph;
  return stable >= 3;
}

// Wait until the window manager has actually given the owner the input focus.
//
// Mapping a window does NOT focus it synchronously: the WM decides, on its own
// schedule. Phases 5 and 6 are both statements ABOUT the focus - that opening a
// popup does not move it, and that a foreign keyboard grab does not either - so
// asserting them before the owner has been given the focus in the first place
// tests nothing, and fails rather than passing vacuously, because the focus then
// arrives DURING the phase and neui correctly reads that real change as a
// deactivation.
//
// Keeps pumping while it waits: neui has to process its own FocusIn for the
// session to consider itself focused.
bool wait_for_focus(Window w, int limit_ms)
{
  const long long end = now_ms() + limit_ms;
  while (now_ms() < end) {
    if (current_focus() == w) return true;
    g_api->pump_once(g_sess);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return current_focus() == w;
}

const char* kOwnerTitle = "neui popup surface smoke (linux)";

} // namespace

namespace xpl_host { Session* session_by_id(uint32_t session_id); }

int main(int, char*[])
{
  // Unbuffered: this harness drives a real X server and a crash or a watchdog
  // kill must not take the log of what already passed with it.
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::thread wd(watchdog);

  g_xd = XOpenDisplay(nullptr);
  if (!g_xd) {
    std::printf("FAIL: no X display (this harness needs a live desktop)\n");
    g_finished.store(true); wd.join();
    return 1;
  }

  neui_init();
  g_api = neui_get_api("neui.host.crossplatform");
  if (!g_api) { std::printf("FAIL: no crossplatform host\n"); return 1; }
  g_sess = g_api->create_session(&g_client, nullptr);
  g_w  = (neui_widget_api_t*) g_api->get_interface(g_sess, NEUI_API_WIDGETS);
  g_pu = (neui_popup_api_t*)  g_api->get_interface(g_sess, NEUI_API_POPUP);
  if (!g_w || !g_pu) { std::printf("FAIL: missing widgets/popup interface\n"); return 1; }

  const neui_widget_t none = { UINT32_MAX };

  // A deliberately SMALL owner and a picker larger than it, so "leaves the
  // frame" is unambiguous rather than a few pixels of overhang.
  neui_widget_t frame = g_w->create(g_sess, none, NEUI_W_APPWINDOW,
                                    160, 140, 520, 300, nullptr);
  g_w->set_text(g_sess, frame, kOwnerTitle);
  neui_widget_t button = g_w->create(g_sess, frame, NEUI_W_BUTTON,
                                     16, 52, 160, 30, nullptr);
  g_w->set_text(g_sess, button, "anchor");

  neui_widget_t picker = g_w->create(g_sess, none, NEUI_W_POPUPSURFACE,
                                     0, 0, 640, 520, nullptr);
  neui_widget_t picker_body = g_w->create(g_sess, picker, NEUI_W_CUSTOMDRAW,
                                          0, 0, 640, 520, nullptr);
  g_popup_child_id = picker_body.id;   // phase 9b counts wheels landing here
  // After the body, so get_first_child (the cascade anchor) still resolves to it.
  g_popup_edit = g_w->create(g_sess, picker, NEUI_W_INPUTBOX, 20, 300, 200, 24,
                             nullptr);
  neui_widget_t detail = g_w->create(g_sess, none, NEUI_W_POPUPSURFACE,
                                     0, 0, 260, 150, nullptr);
  g_w->create(g_sess, detail, NEUI_W_CUSTOMDRAW, 0, 0, 260, 150, nullptr);

  g_w->show(g_sess, frame);
  pump_for(600);

  Window owner_win = find_window_by_name(DefaultRootWindow(g_xd), kOwnerTitle);
  int ox = 0, oy = 0, ow = 0, oh = 0;
  // Settled, not merely present: everything below measures against this origin,
  // and a WM that is still placing the window would invalidate it (and dismiss
  // the stack it is about to open). See wait_owner_settled.
  const bool have_owner = owner_win != 0 &&
                          wait_owner_settled(owner_win, &ox, &oy, &ow, &oh, 15000);

  // The device scale, derived from the owner rather than assumed: the whole
  // harness compares physical X coordinates against logical neui ones.
  const double scale = (have_owner && ow > 0) ? (double) ow / 520.0 : 1.0;

  // ---- 1. WINDOW -----------------------------------------------------------
  phase("1. WINDOW - a real override-redirect top-level that leaves the owner");
  check(have_owner, "the owner's X window was found and its geometry settled");
  // Precondition for phases 5 and 6, and a timing hazard rather than a product
  // claim: the WM grants the focus on its own schedule after the map.
  check(wait_for_focus(owner_win, 5000),
        "the WM gave the owner the input focus before the popup phases begin");

  const std::set<Window> baseline = settled_baseline();
  check(g_pu->open(g_sess, picker, button, 0, 2, NEUI_POPUP_BELOW),
        "open(BELOW) returned true");
  pump_until([&]{ return new_popups(baseline).size() == 1; }, 4000);

  std::vector<WinInfo> after = new_popups(baseline);
  check(after.size() == 1,
        "exactly one new override-redirect child of ROOT appeared");

  WinInfo pop{};
  for (auto& wi : after)
    if (wi.w == (int) (640 * scale + 0.5) && wi.h == (int) (520 * scale + 0.5)) pop = wi;

  check(pop.window != 0, "the popup's X window was located by its size");
  check(pop.override_redirect, "override_redirect is set (the WM is out of it)");
  check(pop.w == (int) (640 * scale + 0.5) && pop.h == (int) (520 * scale + 0.5),
        "sized exactly as asked - borderless means no decoration inflation");

  // The claim of the feature: the popup is bigger than, and hangs outside, the
  // window that opened it.
  if (have_owner && pop.window) {
    check(pop.w > ow || pop.h > oh, "the popup is LARGER than its owner frame");
    check(pop.x + pop.w > ox + ow || pop.y + pop.h > oy + oh,
          "the popup's rect genuinely leaves the owner's rect");
  }

  // ---- 2. HINT -------------------------------------------------------------
  phase("2. HINT - _NET_WM_WINDOW_TYPE_POPUP_MENU");
  check(window_type_atom(pop.window) == "_NET_WM_WINDOW_TYPE_POPUP_MENU",
        "the window-type hint is the popup-menu type");

  // ---- 3. PLACEMENT --------------------------------------------------------
  phase("3. PLACEMENT - top-left == anchor bottom-left + offset, in root coords");
  {
    // The anchor is a plain widget, not an X window (the xpl host is a single
    // surface per frame), so its root position is the owner's client origin plus
    // its logical offset, scaled.
    const int anchor_left_root   = ox + (int) (16 * scale + 0.5);
    const int anchor_bottom_root = oy + (int) ((52 + 30 + 2) * scale + 0.5);
    // On mismatch, put the numbers on the record: a placement failure is
    // unreadable without them, and the usual cause is the owner having moved
    // between the measurement and the open rather than bad arithmetic.
    if (std::abs(pop.x - anchor_left_root) > 2 ||
        std::abs(pop.y - anchor_bottom_root) > 2) {
      int now_x = 0, now_y = 0, now_w = 0, now_h = 0;
      window_rect(owner_win, &now_x, &now_y, &now_w, &now_h);
      std::printf("       owner@measure=(%d,%d) owner@now=(%d,%d) scale=%.3f\n"
                  "       popup=(%d,%d)  expected=(%d,%d)\n",
                  ox, oy, now_x, now_y, scale, pop.x, pop.y,
                  anchor_left_root, anchor_bottom_root);
    }
    check(std::abs(pop.x - anchor_left_root) <= 2,
          "popup left edge aligns with the anchor's left edge");
    check(std::abs(pop.y - anchor_bottom_root) <= 2,
          "popup top edge is the anchor's bottom edge + the 2px offset");
  }

  // ---- 4. GRAB -------------------------------------------------------------
  phase("4. GRAB - check-first #1: XGrabPointer landed after the map");
  {
    // From this second connection the pointer must be UNavailable while the
    // popup is up. If platform_show_popup_surface had used XFlush instead of
    // XSync, the host's own grab would have raced the map and failed with
    // GrabNotViewable - and this probe would succeed instead of being refused.
    const int r = XGrabPointer(g_xd, DefaultRootWindow(g_xd), True,
                               ButtonPressMask, GrabModeAsync, GrabModeAsync,
                               None, None, CurrentTime);
    check(r == AlreadyGrabbed,
          "another client is refused the pointer -> the host holds the grab");
    if (r == GrabSuccess) XUngrabPointer(g_xd, CurrentTime);
    XSync(g_xd, False);
  }

  // ---- 5. FOCUS ------------------------------------------------------------
  phase("5. FOCUS - check-first #2: the popup never takes the input focus");
  {
    const Window f = current_focus();
    check(f != pop.window, "the popup's window is not the focus window");
    if (have_owner)
      check(f == owner_win,
            "the owner keeps the keyboard focus across the open");
  }

  // ---- 6. PSEUDO-FOCUS -----------------------------------------------------
  phase("6. PSEUDO-FOCUS - a keyboard grab elsewhere is not a deactivation");
  {
    // X sends FocusOut(mode=NotifyGrab) / FocusIn(mode=NotifyUngrab) to the
    // focused window around ANY keyboard grab taken by another client - a WM
    // keybinding, a screen locker, another app opening its own menu. Acting on
    // those dismissed the stack for no reason; this is the regression guard.
    g_dismissals.clear();
    const int r = XGrabKeyboard(g_xd, DefaultRootWindow(g_xd), True,
                                GrabModeAsync, GrabModeAsync, CurrentTime);
    check(r == GrabSuccess, "the harness could grab the keyboard as another client");
    XSync(g_xd, False);
    pump_for(300);
    XUngrabKeyboard(g_xd, CurrentTime);
    XSync(g_xd, False);
    pump_for(300);
    check(g_pu->is_open(g_sess, picker),
          "the stack SURVIVES another client's keyboard grab");
    check(g_dismissals.empty(), "...and reports no dismissal");
  }

  // ---- 7. CASCADE ----------------------------------------------------------
  phase("7. CASCADE - a second level does not dismiss the first");
  {
    g_dismissals.clear();
    neui_widget_t rows = g_w->get_first_child(g_sess, picker);
    check(g_pu->open(g_sess, detail, rows, 0, 40, NEUI_POPUP_RIGHT),
          "open(RIGHT) for level 1 returned true");
    pump_until([&]{ return new_popups(baseline).size() == 2; }, 4000);
    check(g_pu->is_open(g_sess, picker), "level 0 must still be open");
    check(g_pu->is_open(g_sess, detail), "level 1 must be open");
    check(g_dismissals.empty(),
          "placing a level must not read as 'the owner moved'");

    std::vector<WinInfo> two = new_popups(baseline);
    check(two.size() == 2, "two popup windows are on the server");

    WinInfo lvl1{};
    for (auto& wi : two)
      if (wi.w == (int) (260 * scale + 0.5) && wi.h == (int) (150 * scale + 0.5)) lvl1 = wi;
    check(lvl1.window != 0, "level 1's X window was located");
    if (lvl1.window)
      check(std::abs(lvl1.x - (pop.x + pop.w)) <= 2,
            "level 1 sits at level 0's right edge (RIGHT, no flip needed here)");

    // Unwind: closing the root level takes the deeper one with it.
    g_dismissals.clear();
    g_pu->close(g_sess, picker);
    pump_until([&]{ return new_popups(baseline).empty() && g_dismissals.size() == 2; }, 4000);
    check(!g_pu->is_open(g_sess, picker) && !g_pu->is_open(g_sess, detail),
          "closing level 0 closes the whole stack");
    check(g_dismissals.size() == 2, "both levels report a dismissal");
    if (g_dismissals.size() == 2)
      check(g_dismissals[0].widget_id == detail.id,
            "the DEEPEST level is dismissed first");
    check(new_popups(baseline).empty(),
          "no popup window is left mapped on the server");
  }

  // ---- 8. OWNER MOVED ------------------------------------------------------
  phase("8. OWNER MOVED - the real case the placing-scope must not suppress");
  {
    g_dismissals.clear();
    check(g_pu->open(g_sess, picker, button, 0, 2, NEUI_POPUP_BELOW),
          "reopened for the move test");
    pump_until([&]{ return g_pu->is_open(g_sess, picker); }, 4000);
    g_dismissals.clear();
    g_w->set_pos(g_sess, frame, 220, 200, 520, 300);
    pump_until([&]{ return !g_pu->is_open(g_sess, picker); }, 4000);
    check(!g_pu->is_open(g_sess, picker), "moving the owner dismisses the stack");
    check(g_dismissals.size() == 1 &&
          g_dismissals[0].reason == NEUI_POPUP_DISMISS_OWNER_MOVED,
          "and reports OWNER_MOVED");
  }

  // ---- 9. THE GATE ---------------------------------------------------------
  phase("9. THE GATE - the suppression that replaces a pointer capture");
  {
    xpl_host::Session* s = xpl_host::session_by_id(g_sess.session);
    check(s != nullptr, "session_by_id resolved the live session");
    if (s) {
      const uint32_t frame_slot  = slot_of(frame);
      const uint32_t picker_slot = slot_of(picker);

      check(!s->popup_gate_press(frame_slot), "gate is inert when nothing is open");
      check(!s->popup_gate_hover(frame_slot), "hover gate is inert when closed");
      check(!s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_ESCAPE, 0),
            "Escape passes through when closed");
      check(!s->popup_diverts_keys(), "keys do not divert when closed");

      g_dismissals.clear();
      g_pu->open(g_sess, picker, button, 0, 2, NEUI_POPUP_BELOW);
      pump_until([&]{ return g_pu->is_open(g_sess, picker); }, 4000);
      check(s->popup_surface_depth(picker_slot) == 0, "picker is depth 0");
      check(s->popup_surface_depth(frame_slot) < 0, "the owner is not a level");

      check(s->popup_gate_hover(frame_slot), "hover in the owner is swallowed");
      check(!s->popup_gate_hover(picker_slot), "hover inside the popup passes");

      // A press outside dismisses AND is swallowed, and the swallow is paired
      // with exactly one release - a leaked release would give the widget under
      // the popup an UP with no DOWN and synthesise a click.
      check(s->popup_gate_press(frame_slot), "a press outside is swallowed");
      check(!s->popup_surface_open(), "...and dismisses the stack");
      check(g_dismissals.size() == 1 &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_OUTSIDE_PRESS,
            "...reporting OUTSIDE_PRESS");
      check(s->popup_take_release(), "the paired release is swallowed once");
      check(!s->popup_take_release(), "...and only once");

      // X11's own outside-press route: with owner_events=True a press outside
      // every window of ours is reported against the GRAB window with
      // out-of-bounds coordinates, so it must go through this entry point
      // instead - read through popup_gate_press it would look INSIDE the stack
      // and nothing would ever dismiss.
      g_dismissals.clear();
      g_pu->open(g_sess, picker, button, 0, 2, NEUI_POPUP_BELOW);
      pump_until([&]{ return g_pu->is_open(g_sess, picker); }, 4000);
      check(s->popup_gate_press_outside(),
            "the X11 out-of-bounds press route swallows");
      check(!s->popup_surface_open(), "...and dismisses");
      check(g_dismissals.size() == 1 &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_OUTSIDE_PRESS,
            "...also reporting OUTSIDE_PRESS");

      // Keys. The rule is a RETARGET, not a pass-through: while a popup is open
      // a keystroke must never reach the widgets underneath - the same promise
      // the press gate makes, and the one that used to be broken (with a menu
      // open, typing edited the text field behind it).
      g_dismissals.clear();
      g_keys.clear();
      g_pu->open(g_sess, picker, button, 0, 2, NEUI_POPUP_BELOW);
      pump_until([&]{ return g_pu->is_open(g_sess, picker); }, 4000);
      check(s->popup_diverts_keys(), "with focus outside the stack, keys divert");
      check(s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_TAB, 0),
            "a key does not fall through to the owner");
      check(g_keys.size() == 1 && g_keys[0].widget_id == picker.id &&
            g_keys[0].keycode == NEUI_KEY_TAB,
            "the key is delivered to the deepest surface");
      // A key that mapped to no keycode is still swallowed, but reports nothing -
      // on X11 that is the ordinary case for a dead key or an IME intermediate.
      g_keys.clear();
      check(s->popup_gate_key(NEUI_EVENT_KEYDOWN, 0, 0),
            "an unmapped key is still swallowed");
      check(g_keys.empty(), "...and is not reported");

      // Focus already INSIDE the deepest level: the gate gets out of the way.
      // _focused_widget is a session index and dispatch_key_press reads it
      // without asking which frame it belongs to, so the ordinary path already
      // delivers into the popup even though the override-redirect window never
      // takes the X input focus.
      g_keys.clear();
      s->set_focus(slot_of(g_popup_edit));
      check(!s->popup_diverts_keys(), "focus inside the deepest level does not divert");
      check(!s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_TAB, 0),
            "with focus inside the popup the gate passes the key through");
      check(g_keys.empty(), "...and synthesises no delivery of its own");

      // Tab traversal follows the open popup: the surface is a ROOT CHILD the
      // owner's tab-stop walk cannot reach, and the frame that DELIVERED the key
      // is always the owner, so Tab used to jump out into the editor behind it.
      s->focus_next(true, frame_slot);
      check(s->_focused_widget != 0 &&
            s->is_in_subtree(s->_focused_widget, picker_slot),
            "Tab stays inside the open popup");
      s->focus_next(false, frame_slot);
      check(s->_focused_widget != 0 &&
            s->is_in_subtree(s->_focused_widget, picker_slot),
            "...and so does Shift-Tab");
      s->set_focus(0);

      g_keys.clear();
      check(s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_ESCAPE, 0),
            "Escape is consumed");
      check(!s->popup_surface_open(), "Escape dismisses the stack");
      check(g_dismissals.size() == 1 &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_ESCAPE,
            "...reporting ESCAPE");
    }
  }

  // ---- 9b. WHEEL -----------------------------------------------------------
  phase("9b. WHEEL - suppressed outside the stack, delivered inside it");
  {
    // Driven through REAL events rather than through Session, for the same reason
    // the win32 harness does it that way: the gate's decision is portable and
    // proven once on macOS, while what can break here is X11's own routing - and
    // the two Linux wheel call sites are the kind of thing a Session-level check
    // passes straight through while the bug is live.
    //
    // The Linux-specific hazard is the one asserted below the control: a wheel
    // arrives as a core ButtonPress (buttons 4-7), and `dispatch_button_press`
    // returns for those BEFORE reaching `popup_gate_press`. If that ordering ever
    // inverted, a wheel notch anywhere would read as an outside press and DISMISS
    // the stack - a picker that closes when the user scrolls the list under it.
    const int anchor_x = (int)((16 + 160 / 2) * scale);   // the anchor's centre,
    const int anchor_y = (int)((52 + 30 / 2) * scale);    // in owner-window px

    // CONTROL: with nothing open, the same event must reach the widget under it.
    // Everything below is a claim about suppression, which is meaningless unless
    // delivery works on this server in the first place.
    g_wheels_under = g_wheels_in_popup = 0;
    check(send_core_wheel(owner_win, anchor_x, anchor_y, 5),
          "XSendEvent accepted a core wheel notch on the owner");
    pump_until([&]{ return g_wheels_under > 0; }, 2000);
    std::printf("        control: wheels delivered with nothing open: %d\n",
                g_wheels_under);
    const bool control_ok = g_wheels_under > 0;
    check(control_ok, "a wheel notch reaches the widget underneath when no popup is open");

    if (control_ok) {
      // SUPPRESSED. An ungated notch over a KNOB / SLIDER under the popup is not
      // cosmetic: it fires the whole GESTURE_BEGIN / VALUE_CHANGED / GESTURE_END
      // triple, which in a DAW is an automation write the user never saw.
      g_dismissals.clear();
      g_wheels_under = g_wheels_in_popup = 0;
      check(g_pu->open(g_sess, picker, button, 0, 2, NEUI_POPUP_BELOW),
            "reopened before the wheel checks");
      pump_until([&]{ return g_pu->is_open(g_sess, picker); }, 4000);
      send_core_wheel(owner_win, anchor_x, anchor_y, 5);
      pump_for(250);
      std::printf("        with a popup up, wheels delivered underneath: %d\n",
                  g_wheels_under);
      check(g_wheels_under == 0,
            "a wheel outside the stack did not reach the widget underneath");
      // Suppression, NOT dismissal - and this is the ordering guard described
      // above, since a wheel routed through the press gate would close the stack.
      check(g_pu->is_open(g_sess, picker), "a wheel outside does NOT dismiss");
      check(g_dismissals.empty(), "...and reports no dismissal");

      // DELIVERED. The other half, and the one the recommended idiom depends on:
      // popup content in a scrolling SECTION has to be able to scroll. X delivers
      // to the window under the pointer, so a wheel aimed at the popup arrives
      // against the popup's own window and must pass the gate.
      std::vector<WinInfo> live = new_popups(baseline);
      check(live.size() == 1, "exactly one popup window to aim at");
      if (live.size() == 1) {
        g_wheels_under = g_wheels_in_popup = 0;
        send_core_wheel(live[0].window, live[0].w / 2, live[0].h / 2, 5);
        pump_until([&]{ return g_wheels_in_popup > 0; }, 2000);
        std::printf("        wheels delivered INTO the popup: %d (underneath: %d)\n",
                    g_wheels_in_popup, g_wheels_under);
        check(g_wheels_in_popup > 0,
              "a wheel over the popup reached the popup's own child, not swallowed");
        check(g_wheels_under == 0,
              "...and did not also reach the owner's widget");
        check(g_pu->is_open(g_sess, picker),
              "...and scrolling the popup did not dismiss it");
      }
      g_pu->close_all(g_sess);
      pump_for(150);
    }
  }

  // ---- 10. LIFETIME --------------------------------------------------------
  phase("10. LIFETIME - destroying the owner strands nothing on the server");
  {
    g_dismissals.clear();
    check(g_pu->open(g_sess, picker, button, 0, 2, NEUI_POPUP_BELOW),
          "reopened before the destroy");
    pump_until([&]{ return new_popups(baseline).size() == 1; }, 4000);
    check(g_pu->is_open(g_sess, picker), "open before the destroy");
    check(new_popups(baseline).size() == 1,
          "the popup window is on the server before the destroy");

    g_w->destroy(g_sess, frame);
    pump_until([&]{ return !g_pu->is_open(g_sess, picker) &&
                           new_popups(baseline).empty(); }, 4000);
    check(!g_pu->is_open(g_sess, picker),
          "destroying the owner frame closes the popup");
    check(new_popups(baseline).empty(),
          "no override-redirect window is left behind");

    // And the pointer grab must have been released with it, or the desktop
    // would be left unable to click anything at all.
    const int r = XGrabPointer(g_xd, DefaultRootWindow(g_xd), True,
                               ButtonPressMask, GrabModeAsync, GrabModeAsync,
                               None, None, CurrentTime);
    check(r == GrabSuccess, "the pointer grab was released with the stack");
    if (r == GrabSuccess) XUngrabPointer(g_xd, CurrentTime);
    XSync(g_xd, False);
  }

  std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  g_finished.store(true);
  wd.join();
  // Tear the session down explicitly rather than leaving it to static
  // destruction at process exit: the popup surfaces are still live widgets and
  // their owner frame was destroyed in phase 10, so this is the ~Session path
  // that closes a stack whose owner is already gone.
  g_api->destroy(g_sess);
  XCloseDisplay(g_xd);
  return g_failures ? 1 : 0;
}
