// Popup-surface acceptance harness (NEUI_W_POPUPSURFACE / NEUI_API_POPUP),
// win32 / xpl host.
//
// WHY THIS EXISTS. The win32 backing shipped WRITTEN BUT NEVER COMPILED - it was
// authored on macOS, where this platform does not build, exactly the risk
// CLAUDE.md's cross-platform hygiene note describes. A build alone only proves
// the API calls typecheck; it says nothing about whether WS_EX_NOACTIVATE really
// keeps the editor's focus or whether the owner re-own works. So this is the
// counterpart to popup_surface_smoke_macos.mm, and like it, it realizes real
// windows and then asks WIN32 what happened rather than asking neui to confirm
// its own bookkeeping.
//
// It is NOT a port. The portable half - placement arithmetic, cascade
// ownership, dismissal ordering, the input gate - is proven once on macOS and is
// the same code here; re-asserting it buys little. What this file targets is the
// half win32 writes for itself, which is everything the macOS harness could not
// have caught:
//
//   1. WINDOW      - the popup is a real WS_POPUP top-level window, NOT a child
//                    HWND, sized as asked, and its rect genuinely leaves the
//                    owner's. That is the whole claim of the feature.
//   2. OWNERSHIP   - GetWindow(GW_OWNER) is the owner's GA_ROOT. This is win32's
//                    -childWindows: it is what buys z-order-follows-owner and
//                    hide-on-owner-minimize from the OS rather than from us.
//   3. RE-OWN      - re-opening against a DIFFERENT frame moves the ownership.
//                    docs/popup-surfaces.md names the GWLP_HWNDPARENT re-own as
//                    a check-first item, and it is the one call in the file with
//                    no macOS equivalent (addChildWindow: is idempotent, this is
//                    not).
//   4. ACTIVATION  - the documented promise a DAW cares about: opening a picker,
//                    and then CLICKING it, must not move the foreground window
//                    or the thread's focus off the editor. Asserted around real
//                    posted input, not from the style bits alone.
//   5. STYLES      - WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, no WS_CHILD. The
//                    toolwindow bit is why the picker is not in the taskbar and
//                    not in the Alt+Tab list.
//   6. CLAMP/FLIP  - get_clamp_size agrees with the monitor work area converted
//                    to logical px (the /scale in platform_get_work_area is easy
//                    to get backwards, and a 150 % display is where it shows),
//                    and a popup anchored near the bottom stays on screen.
//   7. CASCADE     - a second level opens without the first being dismissed.
//                    That is the PopupPlacingScope regression: creating a level
//                    fires WM_MOVE / WM_SIZE for the window being placed, which
//                    the owner-moved hook read as "the owner moved" and used to
//                    close the level below it.
//   8. WM_MOVE     - moving the OWNER does dismiss, which is the other half of
//                    the same hook: suppressing the placing case must not have
//                    suppressed the real one.
//   9. WM_ACTIVATEAPP - win32's entire outside-press watch, driven directly. The
//                    message is the only notification we get for a press we do
//                    not own, so a typo in that WndProc branch would mean a
//                    picker that never closes when the user clicks the DAW.
//  10. THE GATE    - the suppression that replaces an OS pointer capture,
//                    driven through Session (no client ever calls it - the
//                    platform layer does, from five sites in platform_win32.cpp).
//  11. LIFETIME    - closing hides the HWND; destroying the OWNER destroys the
//                    popup window. The surface is its own root child, so the
//                    owner's subtree does not contain it: without the explicit
//                    owner/anchor check this leaves a live borderless window
//                    over the desktop belonging to a frame that no longer
//                    exists, with the watch still running.
//
// Phase 10 calls Session's gate entry points directly, for the same reason the
// macOS harness does: what is asserted is the DECISION the gate makes, and the
// win32 wiring that feeds it is five call sites in the WndProc which phases 4,
// 8 and 9 already drive with real messages.
//
// Realizes real HWNDs and posts real input, so this is built but NOT
// ctest-registered; run tests/<config>/neui_popup_surface_smoke_win32.exe by
// hand. A console target on purpose - the checks report on stdout.

// NOMINMAX before windows.h: host.h pulls in hosts/shared/*.h, which use
// std::min / std::max, and the SDK's min/max macros turn those into syntax
// errors at the :: - the same reason the host TUs define it.
#define NOMINMAX
#include <windows.h>

#include <neui/neui.h>

// Host internals, for phase 10 only - the input gate is not reachable through
// the public API by design.
#include "../hosts/crossplatform/host.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
neui_widget_api_t* g_w = nullptr;

struct Dismissal { uint32_t widget_id; uint32_t reason; };
std::vector<Dismissal> g_dismissals;

// Clicks observed by the client, so "the dismissing press was swallowed" is an
// observed fact rather than an inference from the gate's return value.
int g_clicks_under = 0;
neui_widget_t g_anchor{};

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_WIDGET_PAINT) {
    // Paint something so the frame's paint pass is real - a popup surface that
    // only ever gets an empty paint would not exercise its own render target.
    ev->data.paint.painter_api->fill_rect(ev->data.paint.p, 0, 0,
                                          ev->data.paint.width,
                                          ev->data.paint.height,
                                          0xFF203040u);
    return true;
  }
  if (ev->type == NEUI_EVENT_POPUP_DISMISSED) {
    g_dismissals.push_back({ ev->data.popup.widget.id, ev->data.popup.reason });
    return true;
  }
  if (ev->type == NEUI_EVENT_MOUSE_BUTTON_CLICK &&
      ev->data.mouse.widget.id == g_anchor.id)
    ++g_clicks_under;
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* NEUI_ABI iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

// ---- watchdog --------------------------------------------------------------
// This harness posts real input and pumps a real message loop. A hang reports
// nothing at all, so an overrun is turned into a named failure.
std::atomic<bool> g_finished{false};
std::atomic<unsigned long long> g_deadline{0};

void watchdog()
{
  for (;;) {
    if (g_finished.load()) return;
    const unsigned long long d = g_deadline.load();
    if (d != 0 && GetTickCount64() > d) {
      std::fprintf(stderr, "\n[FAIL] TIMEOUT in phase: %s\n", g_phase_name);
      std::fflush(nullptr);
      std::_Exit(3);
    }
    Sleep(50);
  }
}

void pump_for(int ms)
{
  g_deadline.store(GetTickCount64() + (unsigned long long)ms + 10000);
  const unsigned long long end = GetTickCount64() + (unsigned long long)ms;
  while (GetTickCount64() < end) {
    g_api->pump_once(g_sess);
    Sleep(5);
  }
  g_deadline.store(0);
}

HWND hwnd_of(neui_widget_t f)
{
  return static_cast<HWND>(g_w->get_native_handle(g_sess, f));
}

RECT rect_of(HWND h)
{
  RECT r = {};
  if (h) GetWindowRect(h, &r);
  return r;
}

void print_rect(const char* label, RECT r)
{
  std::printf("        %s: %ld,%ld %ldx%ld\n", label, r.left, r.top,
              r.right - r.left, r.bottom - r.top);
}

float scale_of(HWND h)
{
  UINT dpi = h ? GetDpiForWindow(h) : 96;
  if (dpi == 0) dpi = 96;
  return (float)dpi / 96.0f;
}

// Widget handle -> tree slot, the same split the host uses (upper 16 = session).
uint32_t slot_of(neui_widget_t w) { return w.id & 0xFFFFu; }

} // namespace

namespace xpl_host { Session* session_by_id(uint32_t session_id); }

int main()
{
  // Unbuffered: the harness can be aborted by its own watchdog, and a buffered
  // stdout would take the progress log - the only thing that says WHICH check
  // was running - down with it.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::thread(watchdog).detach();

  neui_init();
  g_api = neui_get_api("neui.host.crossplatform");
  if (!g_api) { std::printf("FAIL: no crossplatform host\n"); return 1; }

  g_sess = g_api->create_session(&g_client, nullptr);
  g_w      = (neui_widget_api_t*) g_api->get_interface(g_sess, NEUI_API_WIDGETS);
  auto* pu = (neui_popup_api_t*)  g_api->get_interface(g_sess, NEUI_API_POPUP);
  const neui_session_t sess = g_sess;

  phase("0. interface presence");
  check(g_w != nullptr,  "NEUI_API_WIDGETS present");
  check(pu != nullptr,   "NEUI_API_POPUP present on the xpl host");
  if (!g_w || !pu) return 1;
  // The documented feature-detect trap: on win32 neui_get_api(NULL) returns the
  // NATIVE host first, and NEUI_API_POPUP is xpl-only.
  if (neui_api_t* nat = neui_get_api("neui.host.win32")) {
    neui_session_t ns = nat->create_session(&g_client, nullptr);
    check(nat->get_interface(ns, NEUI_API_POPUP) == nullptr,
          "NEUI_API_POPUP is NULL on the native win32 host");
    nat->destroy(ns);
  }

  const int FRAME_W = 520, FRAME_H = 300;
  const int POP_W   = 640, POP_H   = 520;   // bigger than the frame on both axes

  neui_widget_t frame = g_w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                    160, 140, FRAME_W, FRAME_H, nullptr);
  g_w->set_text(sess, frame, "neui popup owner");
  neui_widget_t button = g_w->create(sess, frame, NEUI_W_BUTTON,
                                     16, 52, 160, 30, nullptr);
  g_w->set_text(sess, button, "anchor");
  g_anchor = button;

  // A SECOND frame, for the re-own check in phase 3.
  neui_widget_t frame2 = g_w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                     760, 140, 360, 240, nullptr);
  g_w->set_text(sess, frame2, "neui popup owner B");
  neui_widget_t button2 = g_w->create(sess, frame2, NEUI_W_BUTTON,
                                      16, 40, 160, 30, nullptr);
  g_w->set_text(sess, button2, "anchor B");

  neui_widget_t picker = g_w->create(sess, widget_none, NEUI_W_POPUPSURFACE,
                                     0, 0, POP_W, POP_H, nullptr);
  g_w->create(sess, picker, NEUI_W_CUSTOMDRAW, 0, 0, POP_W, POP_H, nullptr);
  neui_widget_t detail = g_w->create(sess, widget_none, NEUI_W_POPUPSURFACE,
                                     0, 0, 260, 150, nullptr);
  g_w->create(sess, detail, NEUI_W_CUSTOMDRAW, 0, 0, 260, 150, nullptr);

  g_w->show(sess, frame);
  g_w->show(sess, frame2);
  pump_for(300);

  HWND owner = hwnd_of(frame);
  check(owner != nullptr && IsWindow(owner), "owner HWND realized");
  if (!owner) return 1;

  // Bring our own window forward so the activation checks in phase 4 mean
  // something. Best-effort: SetForegroundWindow is refused when another process
  // owns the foreground, so the phase reports what it actually observed.
  SetForegroundWindow(owner);
  pump_for(250);

  phase("0b. widgets->show must not realize a popup surface");
  // It has no position until something anchors it; showing it would put a
  // borderless window at the corner of the display.
  g_w->show(sess, picker);
  pump_for(100);
  check(g_w->get_native_handle(sess, picker) == nullptr,
        "show() on a POPUPSURFACE does not realize it");
  check(!pu->is_open(sess, picker), "show() does not open it");

  phase("6a. clamp box is the monitor work area, in logical px");
  {
    int cw = 0, ch = 0;
    pu->get_clamp_size(sess, button, &cw, &ch);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const float s = scale_of(owner);
    const int want_w = (int)((float)(mi.rcWork.right - mi.rcWork.left) / s);
    const int want_h = (int)((float)(mi.rcWork.bottom - mi.rcWork.top) / s);
    std::printf("        clamp %dx%d, work area %ldx%ld phys, scale %.2f\n",
                cw, ch, mi.rcWork.right - mi.rcWork.left,
                mi.rcWork.bottom - mi.rcWork.top, (double)s);
    // The /scale in platform_get_work_area is the easy thing to get backwards,
    // and multiplying instead would pass every other check in this file while
    // letting a popup be placed off-screen on a 150 % display.
    check(cw == want_w && ch == want_h,
          "clamp size is the work area converted to logical px");
    check(cw > FRAME_W && ch > FRAME_H,
          "the clamp box is bigger than the frame, or nothing is gained");
    check(pu->escapes_frame(sess, button), "escapes_frame is true here");
  }

  // Captured BEFORE the first open, so phase 4 asserts the promise ("opening a
  // picker does not move the editor's focus") rather than a snapshot taken after
  // the damage was already done.
  const HWND fg_pre    = GetForegroundWindow();
  const HWND focus_pre = GetFocus();
  const HWND active_pre = GetActiveWindow();

  phase("1. WINDOW: a real top-level popup that leaves its owner");
  check(pu->open(sess, picker, button, 0, 2, NEUI_POPUP_BELOW), "open() succeeded");
  pump_for(300);
  check(pu->is_open(sess, picker), "is_open after a successful open");

  HWND panel = hwnd_of(picker);
  check(panel != nullptr && IsWindow(panel), "popup HWND realized");
  if (!panel) { std::printf("\nPOPUP SURFACE FAILED (%d)\n", g_failures); return 1; }
  check(IsWindowVisible(panel) != 0, "popup window is visible");

  RECT pr = rect_of(panel), orr = rect_of(owner);
  print_rect("owner", orr);
  print_rect("panel", pr);
  {
    const float s = scale_of(panel);
    const int want_w = (int)((float)POP_W * s + 0.5f);
    const int want_h = (int)((float)POP_H * s + 0.5f);
    // Borderless WS_POPUP: client rect == window rect, so the window IS the
    // size the client asked for. A stray AdjustWindowRectEx here would show up
    // as the popup being a title bar taller than requested.
    check(std::abs((int)(pr.right - pr.left) - want_w) <= 2 &&
          std::abs((int)(pr.bottom - pr.top) - want_h) <= 2,
          "the popup window is exactly the requested size, scaled by DPI");
  }
  // THE claim of the whole feature: it is not contained by its owner.
  {
    const bool exceeds_h = pr.right  > orr.right;
    const bool exceeds_v = pr.bottom > orr.bottom;
    std::printf("        extends past owner: horizontally=%d vertically=%d\n",
                exceeds_h ? 1 : 0, exceeds_v ? 1 : 0);
    check(exceeds_h || exceeds_v,
          "the popup extends past the owner frame - that is the feature");
    // Anchored under the button, so the popup's TOP edge is inside the owner's
    // vertical span rather than at the window origin.
    check(pr.top > orr.top && pr.top < orr.bottom,
          "the popup top is anchored inside the owner's vertical span");
  }

  phase("2. OWNERSHIP: GW_OWNER is the owner's GA_ROOT");
  {
    HWND got  = GetWindow(panel, GW_OWNER);
    HWND want = GetAncestor(owner, GA_ROOT);
    std::printf("        GW_OWNER=%p  GA_ROOT(owner)=%p\n", (void*)got, (void*)want);
    // This is what buys z-order-follows-owner and hide-on-owner-minimize from
    // the OS. GetParent() would report the same HWND for an owned popup, so it
    // cannot distinguish owner from parent - GetWindow(GW_OWNER) can.
    check(got == want, "the popup is OWNED by the owner frame's root window");
    check(GetAncestor(panel, GA_PARENT) == GetDesktopWindow() ||
          GetAncestor(panel, GA_PARENT) == nullptr,
          "the popup is a top-level window, not a child of the frame");
  }

  phase("5. STYLES: non-activating tool window, never a child");
  {
    const LONG_PTR st = GetWindowLongPtrW(panel, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
    std::printf("        style=0x%08llX exstyle=0x%08llX\n",
                (unsigned long long)st, (unsigned long long)ex);
    check((st & WS_POPUP) != 0, "WS_POPUP set");
    check((st & WS_CHILD) == 0, "WS_CHILD clear - it is not inside the frame");
    check((st & (WS_CAPTION | WS_THICKFRAME)) == 0, "borderless: no caption, no frame");
    check((ex & WS_EX_NOACTIVATE) != 0, "WS_EX_NOACTIVATE set");
    check((ex & WS_EX_TOOLWINDOW) != 0,
          "WS_EX_TOOLWINDOW set - out of the taskbar and Alt+Tab");
  }

  phase("4. ACTIVATION: the editor keeps focus, even when the popup is clicked");
  {
    // WS_EX_NOACTIVATE only stops Windows from activating a window that is
    // CLICKED. It says nothing about a programmatic SetFocus - and SetFocus on a
    // top-level window activates it too - so the style bits are NOT the promise
    // and asserting them (phase 5) is not enough. These four are the promise.
    std::printf("        panel=%p owner=%p\n", (void*)panel, (void*)owner);
    std::printf("        foreground: before open=%p now=%p\n",
                (void*)fg_pre, (void*)GetForegroundWindow());
    std::printf("        thread focus: before open=%p now=%p\n",
                (void*)focus_pre, (void*)GetFocus());
    check(GetForegroundWindow() != panel, "the popup did not take the foreground on open");
    check(GetActiveWindow() != panel, "the popup did not become the active window");
    check(GetFocus() != panel, "the popup did not take the thread's keyboard focus");
    check(GetFocus() == focus_pre,
          "opening the popup left the editor's keyboard focus exactly where it was");

    // Then CLICK it - the gesture a user actually makes, and the one the
    // extended style is meant to cover.
    const HWND fg_before = GetForegroundWindow();
    LPARAM lp = MAKELPARAM(20, 20);
    PostMessageW(panel, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(panel, WM_LBUTTONUP, 0, lp);
    pump_for(250);
    std::printf("        foreground after click=%p\n", (void*)GetForegroundWindow());
    check(GetForegroundWindow() == fg_before,
          "clicking the popup did not move the foreground window");
    check(GetFocus() == focus_pre,
          "clicking the popup did not move the thread's keyboard focus");
    check(GetActiveWindow() == active_pre || GetActiveWindow() != panel,
          "clicking the popup did not make it the active window");
    check(pu->is_open(sess, picker), "a click INSIDE the popup does not dismiss it");
  }

  phase("6b. clamped inside the work area");
  {
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r = rect_of(panel);
    check(r.left >= mi.rcWork.left - 2 && r.right <= mi.rcWork.right + 2,
          "the popup is clamped horizontally into the work area");
    check(r.top >= mi.rcWork.top - 2 && r.bottom <= mi.rcWork.bottom + 2,
          "the popup is clamped vertically into the work area");
  }

  phase("7. CASCADE: a second level does not dismiss the first");
  {
    g_dismissals.clear();
    check(pu->open(sess, detail, g_w->get_first_child(sess, picker), 0, 40,
                   NEUI_POPUP_RIGHT), "cascade open() succeeded");
    pump_for(250);
    HWND panel2 = hwnd_of(detail);
    check(panel2 != nullptr && IsWindowVisible(panel2), "cascade window shown");
    // The PopupPlacingScope regression, and the reason this phase exists on
    // win32 specifically: placing level 2 fires WM_MOVE / WM_SIZE, which the
    // owner-moved hook used to read as "the owner moved" and close level 1.
    check(g_dismissals.empty(),
          "opening a cascade level dismisses NOTHING (PopupPlacingScope)");
    check(pu->is_open(sess, picker) && pu->is_open(sess, detail),
          "both levels are open");
    if (panel2) {
      // Flat ownership: level 2 hangs off the WINDOW, not off level 1.
      check(GetWindow(panel2, GW_OWNER) == GetAncestor(owner, GA_ROOT),
            "the cascade level is owned by the root frame, not by the level above");
      check(GetWindow(panel2, GW_OWNER) != panel, "cascade ownership is flat");
    }

    // Closing the outer level takes the inner one with it, deepest reported
    // first, and the deeper one is reported as a CASCADE dismissal.
    g_dismissals.clear();
    pu->close(sess, picker);
    pump_for(200);
    check(!pu->is_open(sess, picker) && !pu->is_open(sess, detail),
          "closing a level closes everything above it");
    check(g_dismissals.size() == 2, "two dismissals reported");
    if (g_dismissals.size() == 2) {
      check(g_dismissals[0].widget_id == detail.id &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_CASCADE,
            "the deepest level is reported first, as CASCADE");
      check(g_dismissals[1].widget_id == picker.id &&
            g_dismissals[1].reason == NEUI_POPUP_DISMISS_CLIENT,
            "the closed level is reported with the caller's reason");
    }
    check(!IsWindowVisible(panel), "the popup window is hidden on close");
  }

  phase("3. RE-OWN: re-opening against a different frame moves ownership");
  {
    // GWLP_HWNDPARENT is the one call in platform_win32.cpp with no macOS
    // counterpart (addChildWindow: is idempotent; this is a live re-parent of
    // the OWNER slot), and it only runs on the second open.
    HWND owner_b = hwnd_of(frame2);
    check(owner_b != nullptr, "second owner HWND realized");
    check(pu->open(sess, picker, button2, 0, 2, NEUI_POPUP_BELOW),
          "open() against the second frame succeeded");
    pump_for(250);
    HWND got = GetWindow(hwnd_of(picker), GW_OWNER);
    std::printf("        GW_OWNER now=%p  (frame A=%p, frame B=%p)\n",
                (void*)got, (void*)GetAncestor(owner, GA_ROOT),
                (void*)GetAncestor(owner_b, GA_ROOT));
    check(got == GetAncestor(owner_b, GA_ROOT),
          "the popup is re-owned by the frame it was re-opened against");
    // Placed against frame B now, so it must have moved there.
    RECT r = rect_of(hwnd_of(picker)), rb = rect_of(owner_b);
    check(r.left >= rb.left - 4,
          "the popup moved to the second frame's anchor");
    pu->close_all(sess);
    pump_for(150);
  }

  phase("8. WM_MOVE: moving the OWNER dismisses");
  {
    g_dismissals.clear();
    check(pu->open(sess, picker, button, 0, 2, NEUI_POPUP_BELOW), "open() for the move test");
    pump_for(200);
    check(pu->is_open(sess, picker), "open before the move");
    // The other half of the PopupPlacingScope hook: suppressing the placing
    // case must not have suppressed the real one.
    g_w->set_pos(sess, frame, 200, 180, FRAME_W, FRAME_H);
    pump_for(250);
    check(!pu->is_open(sess, picker), "moving the owner dismissed the popup");
    check(g_dismissals.size() == 1 &&
          g_dismissals[0].reason == NEUI_POPUP_DISMISS_OWNER_MOVED,
          "the move is reported as OWNER_MOVED");
  }

  phase("9. WM_ACTIVATEAPP: win32's entire outside-press watch");
  {
    // This message is the ONLY notification win32 gives us for a press we do not
    // own (the documented gap being a press in the DAW's own UI, which raises no
    // activation change at all). It is driven directly because making another
    // application come forward is not something a harness can arrange reliably.
    g_dismissals.clear();
    check(pu->open(sess, picker, button, 0, 2, NEUI_POPUP_BELOW), "open() for the deactivate test");
    pump_for(200);
    check(pu->is_open(sess, picker), "open before the deactivation");

    // wParam TRUE is "we are being activated" and must NOT dismiss - a popup
    // that closed when its own app came forward would close on every open.
    SendMessageW(owner, WM_ACTIVATEAPP, TRUE, 0);
    pump_for(100);
    check(pu->is_open(sess, picker),
          "WM_ACTIVATEAPP(TRUE) does not dismiss - we are the one being activated");

    SendMessageW(owner, WM_ACTIVATEAPP, FALSE, 0);
    pump_for(150);
    check(!pu->is_open(sess, picker),
          "WM_ACTIVATEAPP(FALSE) dismisses the stack");
    check(g_dismissals.size() == 1 &&
          g_dismissals[0].reason == NEUI_POPUP_DISMISS_DEACTIVATED,
          "app deactivation is reported as DEACTIVATED");
  }

  phase("10. THE GATE: suppression of the UI underneath");
  {
    xpl_host::Session* s = xpl_host::session_by_id(sess.session);
    check(s != nullptr, "session_by_id resolved");
    if (s) {
      const uint32_t frame_slot  = slot_of(frame);
      const uint32_t picker_slot = slot_of(picker);
      const uint32_t detail_slot = slot_of(detail);

      // Nothing open: the gate must be completely inert, or every click in every
      // app using neui pays for this feature.
      check(!s->popup_gate_press(frame_slot), "gate inert when nothing is open");
      check(!s->popup_gate_hover(frame_slot), "hover gate inert when nothing is open");
      check(!s->popup_gate_key(NEUI_KEY_ESCAPE), "Escape passes through when closed");

      g_dismissals.clear();
      pu->open(sess, picker, button, 0, 2, NEUI_POPUP_BELOW);
      pump_for(200);
      check(s->popup_surface_depth(picker_slot) == 0, "picker is depth 0");
      check(s->popup_surface_depth(frame_slot) < 0, "the owner is not a level");

      // Hover: suppressed in the owner, untouched inside the popup - otherwise
      // the popup's own rows could not highlight.
      check(s->popup_gate_hover(frame_slot),   "hover in the owner is swallowed");
      check(!s->popup_gate_hover(picker_slot), "hover inside the popup passes");

      // A press on the owner dismisses AND is swallowed, paired with exactly one
      // release - a leaked release makes the widget under the popup see an UP
      // with no DOWN and synthesise a CLICK.
      check(s->popup_gate_press(frame_slot), "a press outside is swallowed");
      check(!s->popup_surface_open(), "a press outside dismisses the stack");
      check(g_dismissals.size() == 1 &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_OUTSIDE_PRESS,
            "an outside press is reported as OUTSIDE_PRESS");
      check(s->popup_take_release(), "the paired release is swallowed once");
      check(!s->popup_take_release(), "...and only once");

      // Two levels: a press on the SHALLOWER one closes only what is deeper and
      // does NOT swallow, so clicking a parent row re-targets in one click.
      pu->open(sess, picker, button, 0, 2, NEUI_POPUP_BELOW);
      pu->open(sess, detail, g_w->get_first_child(sess, picker), 0, 40,
               NEUI_POPUP_RIGHT);
      pump_for(200);
      check(s->popup_surface_depth(detail_slot) == 1, "detail is depth 1");
      check(!s->popup_gate_press(picker_slot),
            "a press on a shallower level is NOT swallowed");
      check(s->popup_surface_depth(picker_slot) == 0 &&
            s->popup_surface_depth(detail_slot) < 0,
            "a press on level 0 closes level 1 and keeps level 0");
      check(!s->popup_take_release(),
            "a press that was not swallowed does not swallow its release");
      check(!s->popup_gate_press(picker_slot),
            "a press on the deepest level is ordinary dispatch");
      check(s->popup_surface_open(), "...and does not dismiss it");

      // Escape closes everything; any other key passes through.
      g_dismissals.clear();
      check(!s->popup_gate_key(NEUI_KEY_TAB), "only Escape is consumed");
      check(s->popup_gate_key(NEUI_KEY_ESCAPE), "Escape is consumed");
      check(!s->popup_surface_open(), "Escape dismisses the stack");
      check(g_dismissals.size() == 1 &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_ESCAPE,
            "Escape is reported as ESCAPE");
    }
  }

  phase("4b. the dismissing press is swallowed end-to-end");
  {
    // Phase 10 asserts the gate's decision; this asserts the WndProc wiring that
    // feeds it, through real posted input: a press on the OWNER while a popup is
    // up must close the popup and NOT reach the button underneath.
    g_clicks_under = 0;
    pu->open(sess, picker, button, 0, 2, NEUI_POPUP_BELOW);
    pump_for(200);
    check(pu->is_open(sess, picker), "open before the outside press");

    // Aim at the anchor button's centre, in the owner's client coordinates.
    const float s = scale_of(owner);
    const int bx = (int)((16 + 160 / 2) * s), by = (int)((52 + 30 / 2) * s);
    LPARAM lp = MAKELPARAM(bx, by);
    PostMessageW(owner, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(owner, WM_LBUTTONUP, 0, lp);
    pump_for(300);
    check(!pu->is_open(sess, picker), "the outside press dismissed the popup");
    std::printf("        clicks delivered to the widget underneath: %d\n",
                g_clicks_under);
    check(g_clicks_under == 0,
          "the dismissing press did NOT also actuate the widget underneath");
  }

  phase("11. LIFETIME: destroying the owner takes the popup with it");
  {
    g_dismissals.clear();
    check(pu->open(sess, picker, button, 0, 2, NEUI_POPUP_BELOW),
          "reopen before the destroy");
    pump_for(250);
    check(pu->is_open(sess, picker), "open before the destroy");
    HWND doomed = hwnd_of(picker);

    // The surface is its own ROOT CHILD, so the owner's subtree does not contain
    // it: without the explicit owner/anchor check this leaves a live borderless
    // window over the desktop belonging to a frame that no longer exists, with
    // the outside-press watch still running.
    g_w->destroy(sess, frame);
    pump_for(350);
    check(!pu->is_open(sess, picker), "destroying the owner closed the popup");
    check(g_dismissals.size() >= 1 &&
          g_dismissals[0].reason == NEUI_POPUP_DISMISS_OWNER_MOVED,
          "the owner destroy is reported as a dismissal");
    check(doomed == nullptr || !IsWindow(doomed) || !IsWindowVisible(doomed),
          "no visible popup window outlives its owner");
  }

  phase("11b. session teardown with a popup open leaves no window");
  {
    // frame2 is still alive; open against it, then tear the session down without
    // closing. ~Session must close the stack, next to the relative-pointer and
    // cursor releases, for the same reason those are there.
    check(pu->open(sess, picker, button2, 0, 2, NEUI_POPUP_BELOW),
          "open against the surviving frame");
    pump_for(250);
    HWND leaked = hwnd_of(picker);
    check(leaked != nullptr && IsWindowVisible(leaked), "open before teardown");
    g_api->destroy(sess);
    pump_for(150);
    check(leaked == nullptr || !IsWindow(leaked) || !IsWindowVisible(leaked),
          "session teardown leaves no visible popup window behind");
  }

  g_finished.store(true);
  std::printf(g_failures ? "\nPOPUP SURFACE/WIN32 FAILED (%d)\n"
                         : "\nPOPUP SURFACE/WIN32 OK\n", g_failures);
  return g_failures ? 1 : 0;
}
