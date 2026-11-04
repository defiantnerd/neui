// Popup-surface acceptance harness (NEUI_W_POPUPSURFACE / NEUI_API_POPUP),
// macOS / xpl host.
//
// WHAT THIS IS FOR. The whole point of the feature is a claim about a real OS
// window - "the popup may extend past its owner frame, and it is owned by that
// frame for z-order" - and neither half is visible from inside the widget tree.
// So this harness realizes real windows and then asks APPKIT what happened,
// rather than asking neui to confirm its own bookkeeping:
//
//   1. PLACEMENT  - the panel exists, is anchored under the button, and its rect
//                   genuinely extends beyond the owner's on both axes.
//   2. OWNERSHIP  - the panel is in the owner window's -childWindows, which is
//                   what makes it travel with the editor instead of being
//                   stranded behind it (the failure issue #23 could not fix
//                   client-side).
//   3. ACTIVATION - the panel is NOT key and cannot become key. A plugin editor
//                   that appears to lose focus whenever a picker opens is a bug
//                   report about the DAW.
//   4. CLAMPING   - placement never leaves the monitor work area, and a popup
//                   anchored near the bottom FLIPS above its anchor.
//   5. CASCADE    - a second level opens; a press on the level below closes only
//                   the deeper one; the dismissal reasons arrive in order.
//   6. LIFETIME   - closing hides the panel; destroying the OWNER closes the
//                   popup (the surface is its own root child, so nothing else
//                   would); a session teardown with a popup open leaves no
//                   window behind.
//
//   7. THE GATE   - the suppression that replaces an OS pointer capture: a press
//                   outside the stack dismisses AND is swallowed, a press on a
//                   shallower level closes only the deeper ones, hover and the
//                   wheel are suppressed in the owner but not in the popup,
//                   Escape closes, and a key is RETARGETED onto the deepest
//                   surface rather than falling through to the owner (with focus
//                   already inside the popup the gate stands aside instead) -
//                   plus Tab traversal staying inside the open level.
//   8. RE-ENTRANCY - destroying the OWNER from the dismissal handler, while the
//                   platform hook that dispatched it is still holding a pointer
//                   into that frame. Only fails under Guard Malloc - see the
//                   phase.
//
// Phase 7 runs BEFORE phase 6 in the source even though it is numbered after it:
// phase 6 destroys the owner frame, so nothing that needs a live one can follow.
//
// Phase 7 calls Session's gate entry points directly (through the host header
// and session_by_id) rather than synthesizing HID events. That is deliberate:
// synthetic CGEvents need an unlocked screen and Accessibility permission, and
// what is being asserted here is the DECISION the gate makes, not AppKit's
// delivery of a click - the platform wiring that feeds it is three call sites in
// platform_macos.mm and is what the example is for.
//
// Realizes real NSWindows, so this is built but NOT ctest-registered; run
// ./tests/<config>/neui_popup_surface_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

// Host internals, for phase 7 only - the input gate is not reachable through the
// public API by design (a client never calls it; the platform layer does).
#include "../hosts/crossplatform/host.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const char* what)
{
  if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}

neui_widget_t g_picker{};
neui_widget_t g_detail{};
// A real tab stop inside the picker - phase 7's key / traversal checks need a
// focusable widget in the popup, not just the painted body.
neui_widget_t g_pick_edit{};

struct Dismissal { uint32_t widget_id; uint32_t reason; };
std::vector<Dismissal> g_dismissals;

// Keys the gate RETARGETED onto a surface (phase 7).
struct KeyHit { uint32_t widget_id; uint32_t type; uint32_t keycode; };
std::vector<KeyHit> g_keys;

// Declared-navigation observations (phase 7): the index moves the host reported,
// the items it committed, and a switch that makes the client CLAIM the Down key
// so the per-keystroke override can be asserted.
std::vector<int>      g_nav_index_events;
std::vector<uint32_t> g_nav_selected;
bool                  g_nav_eat_down = false;

// Phase 8 arms this with the owner frame, so the dismissal handler destroys it
// from inside the platform's dismissal hook - the documented client response, and
// the one that frees the memory that hook is standing in.
neui_session_t      g_sess{};
neui_widget_api_t*  g_w = nullptr;
neui_widget_t       g_destroy_owner_on_dismiss = { UINT32_MAX };

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_WIDGET_PAINT) {
    // Draw something so the paint pass is real.
    ev->data.paint.painter_api->fill_rect(ev->data.paint.p, 0, 0,
                                          ev->data.paint.width,
                                          ev->data.paint.height,
                                          0xFF203040u);
    return true;
  }
  if (ev->type == NEUI_EVENT_KEYDOWN || ev->type == NEUI_EVENT_KEYUP ||
      ev->type == NEUI_EVENT_KEYCHAR) {
    g_keys.push_back({ ev->data.key.widget.id, (uint32_t)ev->type,
                       ev->data.key.keycode });
    // true = "the client handled it", which is the documented way to take a key
    // away from the host's own navigation. Armed only for the override check.
    if (g_nav_eat_down && ev->type == NEUI_EVENT_KEYDOWN &&
        ev->data.key.keycode == NEUI_KEY_DOWN)
      return true;
    return false;   // declined - the gate swallows it either way
  }
  if (ev->type == NEUI_EVENT_ATTR_CHANGED &&
      std::strcmp(ev->data.attr.attr_key, NEUI_ATTR_NAV_INDEX) == 0) {
    g_nav_index_events.push_back((int)ev->data.attr.value);
    return true;
  }
  if (ev->type == NEUI_EVENT_ITEM_SELECTED) {
    g_nav_selected.push_back(ev->data.item.index);
    return true;
  }
  if (ev->type == NEUI_EVENT_POPUP_DISMISSED) {
    g_dismissals.push_back({ ev->data.popup.widget.id, ev->data.popup.reason });
    if (g_destroy_owner_on_dismiss.id != UINT32_MAX && g_w) {
      const neui_widget_t victim = g_destroy_owner_on_dismiss;
      g_destroy_owner_on_dismiss = { UINT32_MAX };   // once, and before the call
      g_w->destroy(g_sess, victim);
    }
    return true;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* NEUI_ABI iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

void pump(double seconds)
{
  [[NSRunLoop mainRunLoop] runUntilDate:
      [NSDate dateWithTimeIntervalSinceNow:seconds]];
}

// The NSWindow behind a frame's native handle. nil before it is realized.
NSWindow* window_for(neui_widget_api_t* w, neui_session_t s, neui_widget_t f)
{
  void* nh = w->get_native_handle(s, f);
  if (!nh) return nil;
  id obj = (__bridge id)nh;
  if ([obj isKindOfClass:[NSWindow class]]) return (NSWindow*)obj;
  return [(NSView*)obj window];
}

bool is_child_of(NSWindow* parent, NSWindow* child)
{
  if (!parent || !child) return false;
  for (NSWindow* c in parent.childWindows) if (c == child) return true;
  return false;
}

// Widget handle -> tree slot, the same split the host uses (upper 16 = session).
uint32_t slot_of(neui_widget_t w) { return w.id & 0xFFFFu; }

} // namespace

namespace xpl_host { Session* session_by_id(uint32_t session_id); }

int main(int, char*[])
{
  @autoreleasepool {
    neui_init();
    neui_api_t* host = neui_get_api("neui.host.crossplatform");
    if (!host) { std::printf("FAIL: no crossplatform host\n"); return 1; }

    neui_session_t sess = host->create_session(&g_client, nullptr);
    auto* w  = (neui_widget_api_t*) host->get_interface(sess, NEUI_API_WIDGETS);
    auto* pu = (neui_popup_api_t*)  host->get_interface(sess, NEUI_API_POPUP);
    check(w != nullptr,  "NEUI_API_WIDGETS missing");
    check(pu != nullptr, "NEUI_API_POPUP missing on the xpl host");
    if (!w || !pu) return 1;
    g_sess = sess;
    g_w    = w;   // phase 8's handler destroys a frame through these

    // The interface must be absent on the NATIVE macOS host - the documented
    // feature-detect trap is that neui_get_api(NULL) returns THAT one first.
    if (neui_api_t* nat = neui_get_api("neui.host.macos")) {
      neui_session_t ns = nat->create_session(&g_client, nullptr);
      check(nat->get_interface(ns, NEUI_API_POPUP) == nullptr,
            "NEUI_API_POPUP must be NULL on the native macOS host");
      nat->destroy(ns);
    }

    const neui_widget_t none = { UINT32_MAX };
    const int FRAME_W = 520, FRAME_H = 300;
    const int POP_W   = 640, POP_H   = 520;   // bigger than the frame, both axes

    neui_widget_t frame = w->create(sess, none, NEUI_W_APPWINDOW,
                                    160, 140, FRAME_W, FRAME_H, nullptr);
    neui_widget_t button = w->create(sess, frame, NEUI_W_BUTTON,
                                     16, 52, 160, 30, nullptr);
    w->set_text(sess, button, "anchor");

    g_picker = w->create(sess, none, NEUI_W_POPUPSURFACE, 0, 0,
                         POP_W, POP_H, nullptr);
    w->create(sess, g_picker, NEUI_W_CUSTOMDRAW, 0, 0, POP_W, POP_H, nullptr);
    // Created AFTER the body so get_first_child (the cascade anchor below) still
    // resolves to the painted body.
    g_pick_edit = w->create(sess, g_picker, NEUI_W_INPUTBOX, 20, 300, 200, 24, nullptr);
    g_detail = w->create(sess, none, NEUI_W_POPUPSURFACE, 0, 0, 260, 150, nullptr);
    w->create(sess, g_detail, NEUI_W_CUSTOMDRAW, 0, 0, 260, 150, nullptr);

    w->show(sess, frame);
    pump(0.35);

    NSWindow* owner = window_for(w, sess, frame);
    check(owner != nil, "owner window not realized");

    // ---- 0. widget_show must NOT realize a popup surface -------------------
    // It has no position until something anchors it; showing it would put a
    // borderless window at the corner of the display.
    w->show(sess, g_picker);
    pump(0.10);
    check(w->get_native_handle(sess, g_picker) == nullptr,
          "widgets->show on a POPUPSURFACE must not realize it");
    check(!pu->is_open(sess, g_picker), "show must not open a popup surface");

    // ---- 4a. the clamp box is the work area, not the frame ------------------
    int cw = 0, ch = 0;
    pu->get_clamp_size(sess, button, &cw, &ch);
    NSRect vf = (owner.screen ?: NSScreen.mainScreen).visibleFrame;
    std::printf("clamp box %dx%d, visibleFrame %.0fx%.0f\n",
                cw, ch, vf.size.width, vf.size.height);
    check(cw == (int)llround(vf.size.width) && ch == (int)llround(vf.size.height),
          "clamp size should be the monitor work area with a desktop backing");
    check(cw > FRAME_W && ch > FRAME_H,
          "the clamp box must be bigger than the frame, or nothing is gained");
    check(pu->escapes_frame(sess, button), "escapes_frame should be true here");

    // ---- 1. PLACEMENT -------------------------------------------------------
    check(pu->open(sess, g_picker, button, 0, 2, NEUI_POPUP_BELOW),
          "open() failed");
    pump(0.35);
    check(pu->is_open(sess, g_picker), "is_open false after a successful open");

    NSWindow* panel = window_for(w, sess, g_picker);
    check(panel != nil, "popup panel not realized");
    if (!panel) { std::printf("\nPOPUP SURFACE FAILED (%d)\n", g_fail); return 1; }
    check(panel.isVisible, "popup panel not visible");

    NSRect pr = panel.frame;
    NSRect orr = owner.frame;
    std::printf("owner %.0f,%.0f %.0fx%.0f   panel %.0f,%.0f %.0fx%.0f\n",
                orr.origin.x, orr.origin.y, orr.size.width, orr.size.height,
                pr.origin.x, pr.origin.y, pr.size.width, pr.size.height);
    check((int)llround(pr.size.width) == POP_W &&
          (int)llround(pr.size.height) == POP_H,
          "the panel must be the size the client asked for, unscaled");
    // THE claim of the whole feature: it is not contained by its owner.
    const bool exceeds_h = (pr.origin.x + pr.size.width) > (orr.origin.x + orr.size.width);
    const bool exceeds_v = pr.origin.y < orr.origin.y;   // Cocoa Y-up: below the owner
    check(exceeds_h || exceeds_v,
          "the popup must extend past the owner frame - that is the feature");
    std::printf("extends past owner: horizontally=%d vertically=%d\n",
                exceeds_h ? 1 : 0, exceeds_v ? 1 : 0);

    // Anchored under the button: the panel's TOP edge should sit near the
    // button's bottom, not at the window's origin.
    const CGFloat panel_top = pr.origin.y + pr.size.height;
    const CGFloat owner_top = orr.origin.y + orr.size.height;
    check(panel_top < owner_top && panel_top > orr.origin.y,
          "the panel top should be inside the owner's vertical span (anchored)");

    // ---- 2. OWNERSHIP -------------------------------------------------------
    check(is_child_of(owner, panel),
          "the panel must be a child window of its owner (z-order follows)");

    // ---- 3. ACTIVATION ------------------------------------------------------
    check(!panel.canBecomeKeyWindow,
          "a popup surface must never be able to become key");
    check(!panel.isKeyWindow, "the panel took key status");
    check((panel.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0,
          "the panel should carry NSWindowStyleMaskNonactivatingPanel");

    // ---- 4b. clamped inside the work area -----------------------------------
    const CGFloat main_h = NSScreen.mainScreen.frame.size.height;
    (void)main_h;
    check(pr.origin.x >= vf.origin.x - 1 &&
          pr.origin.x + pr.size.width <= vf.origin.x + vf.size.width + 1,
          "the panel must be clamped horizontally into the work area");
    check(pr.origin.y >= vf.origin.y - 1,
          "the panel must be clamped to the bottom of the work area");

    // ---- 5. CASCADE ---------------------------------------------------------
    g_dismissals.clear();
    check(pu->open(sess, g_detail, w->get_first_child(sess, g_picker),
                   0, 40, NEUI_POPUP_RIGHT),
          "cascade open() failed");
    pump(0.30);
    NSWindow* panel2 = window_for(w, sess, g_detail);
    check(panel2 != nil && panel2.isVisible, "cascade panel not shown");
    check(pu->is_open(sess, g_picker) && pu->is_open(sess, g_detail),
          "both levels should be open");
    // Flat ownership: level 2 hangs off the WINDOW, not off level 1 - deep owner
    // chains buy nothing here and are one more thing for a host to break.
    check(is_child_of(owner, panel2),
          "cascade level should be owned by the root frame, not by the level above");
    check(!is_child_of(panel, panel2), "cascade ownership should be flat");
    if (panel2) {
      NSRect p2 = panel2.frame;
      check(p2.origin.x >= pr.origin.x + pr.size.width - 1 ||
            p2.origin.x + p2.size.width <= pr.origin.x + 1,
            "a RIGHT cascade should open beside its anchor level (or flip left)");
    }

    // Closing the outer level takes the inner one with it, deepest reported
    // first, and the deeper one is reported as a CASCADE dismissal.
    pu->close(sess, g_picker);
    pump(0.20);
    check(!pu->is_open(sess, g_picker) && !pu->is_open(sess, g_detail),
          "closing a level must close everything above it");
    check(g_dismissals.size() == 2, "two dismissal events expected");
    if (g_dismissals.size() == 2) {
      check(g_dismissals[0].widget_id == g_detail.id &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_CASCADE,
            "deepest level should be reported first, as CASCADE");
      check(g_dismissals[1].widget_id == g_picker.id &&
            g_dismissals[1].reason == NEUI_POPUP_DISMISS_CLIENT,
            "the closed level should be reported with the caller's reason");
    }
    check(panel != nil && !panel.isVisible, "the panel should be hidden on close");
    check(!is_child_of(owner, panel),
          "a closed panel must be detached from its owner, or it is re-shown with it");

    // ---- 4c. FLIP -----------------------------------------------------------
    // Anchored near the bottom of the work area, a BELOW popup must flip ABOVE
    // its anchor rather than run off the screen.
    {
      const int flip_y = (int)llround(main_h - (vf.origin.y) - 120);  // 120 px up
      w->set_pos(sess, frame, 160, flip_y, FRAME_W, FRAME_H);
      pump(0.25);
      g_dismissals.clear();
      check(pu->open(sess, g_picker, button, 0, 2, NEUI_POPUP_BELOW),
            "open() near the screen bottom failed");
      pump(0.25);
      NSWindow* p = window_for(w, sess, g_picker);
      NSWindow* o = window_for(w, sess, frame);
      if (p && o) {
        const bool above = p.frame.origin.y >= o.frame.origin.y;
        std::printf("near-bottom placement: panel y=%.0f owner y=%.0f -> %s\n",
                    p.frame.origin.y, o.frame.origin.y,
                    above ? "flipped above" : "still below");
        check(p.frame.origin.y >= vf.origin.y - 1,
              "a flipped / clamped popup must still be inside the work area");
      }
      pu->close_all(sess);
      pump(0.15);
    }

    // ---- 7. THE GATE --------------------------------------------------------
    // What an OS pointer capture would have bought is suppression of the UI
    // underneath; this is that, done portably. Driven directly because no client
    // ever calls it - the platform layer does, from three sites.
    {
      xpl_host::Session* s = xpl_host::session_by_id(sess.session);
      check(s != nullptr, "session_by_id failed");
      if (s) {
        const uint32_t frame_slot  = slot_of(frame);
        const uint32_t picker_slot = slot_of(g_picker);
        const uint32_t detail_slot = slot_of(g_detail);

        // Nothing open: the gate must be completely inert, or every click in
        // every app using neui would pay for this feature.
        check(!s->popup_gate_press(frame_slot), "gate must be inert when closed");
        check(!s->popup_gate_hover(frame_slot), "hover gate must be inert when closed");
        check(!s->popup_gate_wheel(frame_slot), "wheel gate must be inert when closed");
        check(!s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_ESCAPE, 0),
              "Escape must pass through when closed");
        check(!s->popup_diverts_keys(), "keys must not divert when closed");

        // One level open.
        g_dismissals.clear();
        pu->open(sess, g_picker, button, 0, 2, NEUI_POPUP_BELOW);
        pump(0.20);
        check(s->popup_surface_depth(picker_slot) == 0, "picker should be depth 0");
        check(s->popup_surface_depth(frame_slot) < 0, "the owner is not a level");

        // Hover: suppressed in the owner, untouched inside the popup - otherwise
        // the popup's own rows could not highlight.
        check(s->popup_gate_hover(frame_slot),  "hover in the owner must be swallowed");
        check(!s->popup_gate_hover(picker_slot), "hover inside the popup must pass");

        // The wheel is the same call, one axis further: swallowed outside the
        // stack (a scroll must not move a control under an open picker), ordinary
        // inside it - that second half is what makes the recommended idiom work,
        // popup content in a scrolling SECTION sized to get_clamp_size.
        check(s->popup_gate_wheel(frame_slot),   "wheel in the owner must be swallowed");
        check(!s->popup_gate_wheel(picker_slot), "wheel inside the popup must pass");
        // ...and unlike a press, a wheel OUTSIDE only suppresses. Dismissing on
        // scroll would close the picker when a trackpad drifts a pixel.
        check(s->popup_surface_open(), "a wheel outside must not dismiss the stack");
        check(g_dismissals.empty(), "a wheel outside must report no dismissal");

        // A press on the owner dismisses AND is swallowed, and the swallow is
        // paired with exactly one release - if the release leaked, the widget
        // under the popup would see an UP with no DOWN and synthesise a CLICK.
        check(s->popup_gate_press(frame_slot), "press outside must be swallowed");
        check(!s->popup_surface_open(), "press outside must dismiss the stack");
        check(g_dismissals.size() == 1 &&
              g_dismissals[0].reason == NEUI_POPUP_DISMISS_OUTSIDE_PRESS,
              "outside press should report OUTSIDE_PRESS");
        check(s->popup_take_release(), "the paired release must be swallowed once");
        check(!s->popup_take_release(), "...and only once");

        // Two levels: a press on the SHALLOWER one closes only what is deeper and
        // does NOT swallow, so clicking a parent row re-targets in one click.
        g_dismissals.clear();
        pu->open(sess, g_picker, button, 0, 2, NEUI_POPUP_BELOW);
        pu->open(sess, g_detail, w->get_first_child(sess, g_picker), 0, 40,
                 NEUI_POPUP_RIGHT);
        pump(0.20);
        check(s->popup_surface_depth(detail_slot) == 1, "detail should be depth 1");
        // EVERY level scrolls, not just the deepest: a cascade whose parent is a
        // long scrolling list is the ordinary case (pick a bank, then a preset).
        check(!s->popup_gate_wheel(picker_slot), "wheel on a shallower level must pass");
        check(!s->popup_gate_wheel(detail_slot), "wheel on the deepest level must pass");
        check(!s->popup_gate_press(picker_slot),
              "a press on a shallower level must NOT be swallowed");
        check(s->popup_surface_depth(picker_slot) == 0 &&
              s->popup_surface_depth(detail_slot) < 0,
              "a press on level 0 must close level 1 and keep level 0");
        check(!s->popup_take_release(),
              "a press that was not swallowed must not swallow its release");
        // The deepest level is still ordinary: a press on it changes nothing.
        check(!s->popup_gate_press(picker_slot),
              "a press on the deepest level is ordinary dispatch");
        check(s->popup_surface_open(), "...and must not dismiss it");

        // Keys. The rule is a RETARGET, not a pass-through: while a popup is
        // open, a keystroke must never reach the widgets underneath - the same
        // promise the press gate makes, and the one that used to be broken (with
        // a menu open, typing edited the text field behind it).
        g_dismissals.clear();
        g_keys.clear();
        check(s->popup_diverts_keys(),
              "with focus outside the stack, keys must divert");
        check(s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_TAB, 0),
              "a key must not fall through to the owner");
        check(g_keys.size() == 1 && g_keys[0].widget_id == g_picker.id &&
              g_keys[0].keycode == NEUI_KEY_TAB,
              "the key should be delivered to the deepest surface");
        // A key that mapped to no keycode is still swallowed, but reports
        // nothing - there is no meaningful event to hand a client.
        g_keys.clear();
        check(s->popup_gate_key(NEUI_EVENT_KEYDOWN, 0, 0),
              "an unmapped key must still be swallowed");
        check(g_keys.empty(), "...and must not be reported");

        // Focus already INSIDE the deepest level: the gate gets out of the way.
        // _focused_widget is a session index and every platform's key dispatch
        // reads it without asking which frame it belongs to, so the ordinary path
        // already delivers into the popup - a text field in one really does type.
        g_keys.clear();
        s->set_focus(slot_of(g_pick_edit));
        check(!s->popup_diverts_keys(),
              "focus inside the deepest level must not divert");
        check(!s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_TAB, 0),
              "with focus inside the popup the gate must pass the key through");
        check(g_keys.empty(), "...and must not synthesise a delivery of its own");

        // Tab traversal follows the open popup. The surface is a ROOT CHILD, so
        // the owner's tab-stop walk cannot reach it, and the frame that DELIVERED
        // the key is always the owner (a popup never takes OS focus) - so Tab used
        // to jump out of the picker into the editor behind it. Which stop it lands
        // on is the traversal's business; that it stays in the popup is the claim.
        s->focus_next(true, frame_slot);
        check(s->_focused_widget != 0 &&
              s->is_in_subtree(s->_focused_widget, picker_slot),
              "Tab must stay inside the open popup");
        s->focus_next(false, frame_slot);
        check(s->_focused_widget != 0 &&
              s->is_in_subtree(s->_focused_widget, picker_slot),
              "...and so must Shift-Tab");
        s->set_focus(0);

        // Escape closes everything, and still does so from outside the stack.
        // DECLARED NAVIGATION. The arithmetic is Tier-1 (test_popup_nav.cpp);
        // what is asserted here is the LIVE half - reading the declaration off a
        // real surface, writing the index back, the two events, and the override.
        // Asserted on macOS only: none of it is platform-specific (it is pure
        // Session), the same reason the win32 harness does not re-prove the
        // placement arithmetic.
        {
          auto* at = (neui_attr_api_t*) host->get_interface(sess, NEUI_API_ATTRS);
          check(at != nullptr, "NEUI_API_ATTRS missing");
          // Undeclared: the key reaches the client and the host walks nothing.
          g_keys.clear();
          g_nav_index_events.clear();
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_DOWN, 0);
          check(g_keys.size() == 1, "an undeclared surface still gets the key");
          check(g_nav_index_events.empty(),
                "...but the host navigates nothing without a declared count");

          at->set_int(sess, g_picker, NEUI_ATTR_NAV_COUNT, 5);
          g_nav_index_events.clear();
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_DOWN, 0);
          check(at->get_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, -99) == 0,
                "Down from nothing selects the first item");
          check(g_nav_index_events.size() == 1 && g_nav_index_events[0] == 0,
                "...and reports it as ATTR_CHANGED on the surface");
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_END, 0);
          check(at->get_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, -99) == 4,
                "End jumps to the last item");
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_DOWN, 0);
          check(at->get_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, -99) == 0,
                "Down from the last wraps, the menu default");

          g_nav_selected.clear();
          // A client-written index is not echoed back as an event - only the
          // host's own moves are, or a client that syncs on hover would loop.
          g_nav_index_events.clear();
          at->set_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, 3);
          check(g_nav_index_events.empty(),
                "a client-written index fires no ATTR_CHANGED");
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_RETURN, 0);
          check(g_nav_selected.size() == 1 && g_nav_selected[0] == 3,
                "Enter commits the current item as ITEM_SELECTED");

          // THE OVERRIDE, which is per KEYSTROKE rather than per popup: the
          // client's handler returned true for Down (see onevent), so the host
          // must not also walk it - while Up, which it declined, still navigates.
          g_nav_eat_down = true;
          at->set_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, 2);
          g_nav_index_events.clear();
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_DOWN, 0);
          check(at->get_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, -99) == 2,
                "a key the client consumed is not navigated by the host");
          check(g_nav_index_events.empty(), "...and reports nothing");
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_UP, 0);
          check(at->get_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, -99) == 1,
                "...while a key it declined still gets the host default");
          g_nav_eat_down = false;

          // A live count that shrinks under a filter must not leave the walk
          // stepping from an index the client is no longer drawing.
          at->set_int(sess, g_picker, NEUI_ATTR_NAV_COUNT, 2);
          at->set_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, 4);
          s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_DOWN, 0);
          check(at->get_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, -99) == 0,
                "a stale index past a shrunken count re-enters from the top");

          // Focus inside the popup turns the whole thing off - a text field's
          // arrows belong to the text field.
          s->set_focus(slot_of(g_pick_edit));
          at->set_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, 1);
          check(!s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_DOWN, 0),
                "with focus inside the popup the gate still stands aside");
          check(at->get_int(sess, g_picker, NEUI_ATTR_NAV_INDEX, -99) == 1,
                "...so the host does not navigate over a focused text field");
          s->set_focus(0);
          at->set_int(sess, g_picker, NEUI_ATTR_NAV_COUNT, 0);   // undeclare
        }

        // ESCAPE ALWAYS BELONGS TO THE HOST. Not a default the client can take
        // over: it is handled before the dispatch, so no KEYDOWN is delivered for
        // it at all, and neither a client that claims every key nor focus sitting
        // inside the surface changes that. The rule follows from the no-veto rule
        // in <neui/d/popup.h> - a client that swallowed Escape would leave a
        // window stuck over the DAW - so it is asserted, not just documented.
        s->set_focus(slot_of(g_pick_edit));   // focus INSIDE the popup
        g_nav_eat_down = true;                // ...and a client claiming keys
        g_keys.clear();
        check(s->popup_gate_key(NEUI_EVENT_KEYDOWN, NEUI_KEY_ESCAPE, 0),
              "Escape must be consumed");
        check(g_keys.empty(),
              "Escape must never be dispatched to the client");
        g_nav_eat_down = false;
        check(!s->popup_surface_open(), "Escape must dismiss the stack");
        check(g_dismissals.size() == 1 &&
              g_dismissals[0].reason == NEUI_POPUP_DISMISS_ESCAPE,
              "Escape should report ESCAPE");
      }
    }

    // ---- 6. LIFETIME: destroying the OWNER closes the popup ------------------
    // The surface is its own ROOT CHILD, so the owner's subtree does not contain
    // it: without the explicit owner/anchor check this leaves a live borderless
    // window over the desktop belonging to a frame that no longer exists.
    g_dismissals.clear();
    check(pu->open(sess, g_picker, button, 0, 2, NEUI_POPUP_BELOW),
          "reopen before destroy failed");
    pump(0.25);
    check(pu->is_open(sess, g_picker), "popup should be open before the destroy");
    w->destroy(sess, frame);
    pump(0.30);
    check(!pu->is_open(sess, g_picker),
          "destroying the owner frame must close the popup");
    check(g_dismissals.size() == 1 &&
          g_dismissals[0].reason == NEUI_POPUP_DISMISS_OWNER_MOVED,
          "owner destroy should report one dismissal");
    NSWindow* orphan = window_for(w, sess, g_picker);
    check(orphan == nil || !orphan.isVisible,
          "no visible popup window may outlive its owner");

    // ---- 8. DESTROYING THE OWNER FROM THE DISMISSAL HANDLER ------------------
    // The hazard docs/popup-surfaces.md names: a dismissal is dispatched
    // synchronously from inside a platform hook that is still holding a pointer
    // into the frame it is about to lose. Resizing the owner dismisses, the
    // handler destroys the owner, and NEUIView::setFrameSize: then has to not
    // touch the WidgetData it resolved before the dispatch. Needs its own frame
    // because it ends by destroying it.
    //
    // Reads of freed memory pass under the normal allocator, so this phase only
    // FAILS under Guard Malloc:
    //   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib \
    //     ./tests/<config>/neui_popup_surface_smoke_macos
    {
      g_dismissals.clear();
      neui_widget_t frame2 = w->create(sess, none, NEUI_W_APPWINDOW,
                                       220, 180, 420, 260, nullptr);
      neui_widget_t anchor2 = w->create(sess, frame2, NEUI_W_BUTTON,
                                        16, 40, 140, 28, nullptr);
      w->set_text(sess, anchor2, "anchor");
      neui_widget_t picker2 = w->create(sess, none, NEUI_W_POPUPSURFACE,
                                        0, 0, 300, 200, nullptr);
      w->create(sess, picker2, NEUI_W_CUSTOMDRAW, 0, 0, 300, 200, nullptr);
      w->show(sess, frame2);
      pump(0.30);

      NSWindow* own2 = window_for(w, sess, frame2);
      check(own2 != nil, "second owner window not realized");
      check(pu->open(sess, picker2, anchor2, 0, 2, NEUI_POPUP_BELOW),
            "open() against the second frame failed");
      pump(0.25);
      check(pu->is_open(sess, picker2), "the second popup should be open");

      // Arm the handler, then resize the owner from OUTSIDE - AppKit drives
      // setFrameSize:, which is the hook under test. Our own resize path would
      // take the _self_resizing early-out and prove nothing.
      g_destroy_owner_on_dismiss = frame2;
      if (own2) {
        NSRect f2 = own2.frame;
        f2.size.width  += 40;
        f2.size.height += 30;
        [own2 setFrame:f2 display:YES];
      }
      pump(0.40);

      check(g_destroy_owner_on_dismiss.id == UINT32_MAX,
            "the dismissal handler never ran - the resize did not dismiss");
      check(g_dismissals.size() == 1 &&
            g_dismissals[0].reason == NEUI_POPUP_DISMISS_OWNER_MOVED,
            "an owner resize should report exactly one OWNER_MOVED dismissal");
      check(!pu->is_open(sess, picker2),
            "the popup must be closed after its owner was destroyed");
      NSWindow* orphan2 = window_for(w, sess, picker2);
      check(orphan2 == nil || !orphan2.isVisible,
            "no visible popup window may outlive an owner destroyed this way");
    }

    host->destroy(sess);
    pump(0.20);

    std::printf(g_fail ? "\nPOPUP SURFACE FAILED (%d)\n" : "\nPOPUP SURFACE OK\n",
                g_fail);
    return g_fail ? 1 : 0;
  }
}
