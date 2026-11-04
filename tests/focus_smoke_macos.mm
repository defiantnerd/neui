// Focus-traversal acceptance harness (xpl host, macOS).
//
// What this exists to pin down: TAB TRAVERSAL IS PER FRAME.
//
// Session::focus_next used to collect tab stops from every root child of the
// widget tree - i.e. from every FRAME in the session - and cycle through the
// concatenation. With two windows open, tabbing off the last control of window A
// moved the LOGICAL focus into a control of window B while the OS keyboard focus
// stayed with A: subsequent keystrokes went to a widget in a window the user was
// not looking at, and A appeared to lose its caret for no reason. That is wrong
// independently of accessibility, and it is exactly the kind of defect that no
// single-window test can see - which is why both frames here are real NSWindows.
//
// The checks:
//   1. PER-FRAME     - Tab from the last control of frame A wraps to A's first
//                      control; it never lands in frame B.
//   2. WRAPAROUND    - forward traversal cycles within the frame.
//   3. REVERSE       - Shift+Tab walks backwards, also staying in-frame.
//   4. INDEPENDENCE  - driving Tab at frame B cycles B's controls only, so the
//                      fix is a real partition rather than "always frame A".
//   5. COORDINATES   - a mouse event over a chip-bearing SECTION's child still
//                      reports the right widget with the right widget-local
//                      coordinates. This guards the child_origin_of refactor
//                      that split the origin arithmetic out of the paint walk:
//                      if the shared helper and the paint walk ever disagree,
//                      section children are the first thing to break.
//
//   10-12. THE FIXES' OWN EDGE CASES - a dialog with no tab stop (9b), a
//                      user-closed dialog (9c), hiding a CONTAINER that holds
//                      focus (11), and a client that destroys a widget from
//                      inside a focus event (12).
//
//   6-9. FOCUS IS NEVER LEFT SOMEWHERE UNREACHABLE - the focused widget being
//                      destroyed (6), hidden (7), carried off screen by a tab
//                      switch (8), or blocked by a modal dialog (9) must all move
//                      focus. Each was wrong on its own terms (a dead keyboard,
//                      or keystrokes landing on something invisible) before it
//                      was an accessibility problem; check 9 proves it by TYPING
//                      into the dialog and asserting the owner's field is
//                      untouched.
//
// Focus is read from NEUI_EVENT_WIDGET_FOCUS, and keys / clicks are posted as
// real NSEvents into -keyDown: / -mouseDown:, so the whole production path runs
// and the harness cannot pass by agreeing with its own bookkeeping.
//
// Needs a GUI session (it realizes two real NSWindows), so it is built but not
// ctest-registered; run ./tests/<config>/neui_focus_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

// Last widget to receive focus, from NEUI_EVENT_WIDGET_FOCUS(focused=true).
uint32_t g_focused = 0;
// Focus LOSS, so checks 6-9 can tell "focus moved away" from "nothing happened".
uint32_t g_unfocused = 0;

// Check 10: a client that mutates the tree from a focus-lost handler. The
// framework dispatches that event from inside widget_destroy / widget_hide, so
// everything those functions touch afterwards has to survive it.
neui_session_t       g_sess{};
neui_widget_api_t*   g_w = nullptr;
uint32_t             g_reentrant_on = 0;   // fire when THIS widget loses focus
neui_widget_t        g_reentrant_victim{};
bool                 g_reentrant_fired = false;
// Last mouse event's widget + widget-local coords, for check 5.
uint32_t g_mouse_widget = 0;
int      g_mouse_x = -1, g_mouse_y = -1;

// neui_widget_t is a struct around an id, so keep the handles AND compare on
// .id - the event payloads carry ids.
struct Ids {
  neui_widget_t a_win{}, a1{}, a2{}, a3{};
  neui_widget_t b_win{}, b1{}, b2{};
  neui_widget_t c_win{};
  neui_widget_t sect{};
  // Checks 6-9: focus must not be left pointing at something the user cannot
  // see or reach.
  neui_widget_t doomed{};              // destroyed while focused
  neui_widget_t hideme{};              // hidden while focused
  neui_widget_t tabview{}, page1{}, page2{}, tabfield{}, page1_ctl{};
  neui_widget_t holder{}, held1{}, held2{};
  neui_widget_t d_win{}, d_tabs{}, d_p1{}, d_p2{}, d_c1{}, d_c2{};
  neui_widget_t dlg{}, dlg_btn{}, owner_field{};
} g;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

const char* name_of(uint32_t id)
{
  if (id == g.a1.id)   return "A.button1";
  if (id == g.a2.id)   return "A.button2";
  if (id == g.a3.id)   return "A.section_child";
  if (id == g.b1.id)   return "B.button1";
  if (id == g.b2.id)   return "B.button2";
  if (id == g.sect.id) return "A.section";
  if (id == g.doomed.id)      return "C.doomed";
  if (id == g.hideme.id)      return "C.hideme";
  if (id == g.tabfield.id)    return "C.page2_field";
  if (id == g.page1_ctl.id)   return "C.page1_field";
  if (id == g.held1.id)       return "C.held1";
  if (id == g.held2.id)       return "C.held2";
  if (id == g.d_c1.id)        return "D.page1_ctl";
  if (id == g.d_c2.id)        return "D.page2_ctl";
  if (id == g.dlg_btn.id)     return "dialog.button";
  if (id == g.owner_field.id) return "C.owner_field";
  if (id == 0)         return "<none>";
  return "<other>";
}

void check_focus(neui_widget_t want_w, const char* what)
{
  const uint32_t want = want_w.id;
  bool ok = (g_focused == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        want focus on %s, got %s\n",
                name_of(want), name_of(g_focused));
    ++g_failures;
  }
}

// True when `id` belongs to frame B - the assertion that matters for check 1.
bool in_frame_b(uint32_t id) { return id == g.b1.id || id == g.b2.id; }

bool onevent(void*, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_WIDGET_FOCUS:
      if (ev->data.focus.focused) g_focused = ev->data.focus.widget.id;
      else {
        g_unfocused = ev->data.focus.widget.id;
        // Check 10: destroy a DIFFERENT widget from inside the focus-lost
        // callback - the "this field lost focus, tear the panel down" handler
        // any client might write.
        if (g_reentrant_on != 0 && g_unfocused == g_reentrant_on && g_w) {
          g_reentrant_on = 0;              // once
          g_reentrant_fired = true;
          g_w->destroy(g_sess, g_reentrant_victim);
        }
      }
      break;
    case NEUI_EVENT_MOUSE_BUTTON_DOWN:
      g_mouse_widget = ev->data.mouse.widget.id;
      g_mouse_x      = ev->data.mouse.x;
      g_mouse_y      = ev->data.mouse.y;
      break;
    default: break;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

// The NSWindow whose title matches - the frames are titled so the harness can
// tell them apart without reaching into host internals.
NSWindow* window_titled(NSString* title)
{
  for (NSWindow* w in [NSApp windows])
    if ([[w title] isEqualToString:title]) return w;
  return nil;
}

void post_tab(NSWindow* win, bool shift)
{
  NSView* v = [win contentView];
  if (!v) return;
  NSEvent* ev = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                 location:NSZeroPoint
                            modifierFlags:(shift ? NSEventModifierFlagShift : 0)
                                timestamp:0
                             windowNumber:[win windowNumber]
                                  context:nil
                               characters:@"\t"
              charactersIgnoringModifiers:@"\t"
                                isARepeat:NO
                                  keyCode:48 /* kVK_Tab */];
  [v keyDown:ev];
}

// Type one character through the production path (-keyDown: -> insertText:), so
// "where do keystrokes actually LAND" is an observable fact rather than an
// inference from what the framework says about focus.
void type_char(NSWindow* win, NSString* ch)
{
  NSView* v = [win contentView];
  if (!v) return;
  NSEvent* ev = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                 location:NSZeroPoint
                            modifierFlags:0
                                timestamp:0
                             windowNumber:[win windowNumber]
                                  context:nil
                               characters:ch
              charactersIgnoringModifiers:ch
                                isARepeat:NO
                                  keyCode:0 /* kVK_ANSI_A */];
  [v keyDown:ev];
}

void pump_briefly()
{
  [[NSRunLoop mainRunLoop] runUntilDate:
      [NSDate dateWithTimeIntervalSinceNow:0.15]];
}

void click_in(NSWindow* win, float lx, float ly)
{
  NSView* v = [win contentView];
  if (!v) return;
  // locationInWindow is y-up from the bottom-left; the view is isFlipped:YES.
  const CGFloat h = [v bounds].size.height;
  NSEvent* ev = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                   location:NSMakePoint(lx, h - ly)
                              modifierFlags:0
                                  timestamp:0
                               windowNumber:[win windowNumber]
                                    context:nil
                                eventNumber:0
                                 clickCount:1
                                   pressure:1.0];
  [v mouseDown:ev];
}

} // namespace

int main()
{
  @autoreleasepool {
    [NSApplication sharedApplication];

    neui_init();
    neui_api_t* api = neui_get_api("neui.host.crossplatform");
    if (!api) { std::printf("FAIL: no crossplatform host\n"); return 1; }

    void* app = nullptr;
    neui_session_t sess = api->create_session(&g_client, &app);
    auto* w = (neui_widget_api_t*)api->get_interface(sess, NEUI_API_WIDGETS);
    g_sess = sess; g_w = w;
    auto* attrs = (neui_attr_api_t*)api->get_interface(sess, NEUI_API_ATTRS);
    auto* tabs  = (neui_tabs_api_t*)api->get_interface(sess, NEUI_API_TABS);
    if (!w || !attrs || !tabs) {
      std::printf("FAIL: no widgets/attrs/tabs interface\n"); return 1;
    }

    // ---- frame A: two buttons + a chip-bearing SECTION with one button -----
    // Sized from the content (300x230 client) with margins, per the house rule.
    g.a_win = w->create(sess, widget_none, NEUI_W_APPWINDOW, 80, 120, 300, 230, nullptr);
    w->set_text(sess, g.a_win, "focus A");
    g.a1 = w->create(sess, g.a_win, NEUI_W_BUTTON, 12, 12, 120, 26, nullptr);
    w->set_text(sess, g.a1, "A one");
    g.a2 = w->create(sess, g.a_win, NEUI_W_BUTTON, 12, 48, 120, 26, nullptr);
    w->set_text(sess, g.a2, "A two");
    // A section WITH a header chip: its children's (x, y) is body-relative, so
    // the child sits below the chip band. This is the case the shared
    // child_origin_of helper has to get right.
    g.sect = w->create(sess, g.a_win, NEUI_W_SECTION, 12, 90, 260, 120, nullptr);
    w->set_text(sess, g.sect, "sect");
    g.a3 = w->create(sess, g.sect, NEUI_W_BUTTON, 10, 10, 120, 26, nullptr);
    w->set_text(sess, g.a3, "A three");
    w->show(sess, g.a_win);

    // ---- frame B: two buttons ---------------------------------------------
    g.b_win = w->create(sess, widget_none, NEUI_W_APPWINDOW, 420, 120, 240, 120, nullptr);
    w->set_text(sess, g.b_win, "focus B");
    g.b1 = w->create(sess, g.b_win, NEUI_W_BUTTON, 12, 12, 120, 26, nullptr);
    w->set_text(sess, g.b1, "B one");
    g.b2 = w->create(sess, g.b_win, NEUI_W_BUTTON, 12, 48, 120, 26, nullptr);
    w->set_text(sess, g.b2, "B two");
    w->show(sess, g.b_win);

    // ---- frame C: the material for checks 6-9 ------------------------------
    // A SEPARATE frame on purpose: checks 1-4 assert exact wrap targets from
    // frames A and B, so adding tab stops to either would silently invalidate
    // them (it did, the first time round).
    g.c_win = w->create(sess, widget_none, NEUI_W_APPWINDOW, 80, 400, 300, 250, nullptr);
    w->set_text(sess, g.c_win, "focus C");
    g.doomed = w->create(sess, g.c_win, NEUI_W_BUTTON, 12, 12, 120, 26, nullptr);
    w->set_text(sess, g.doomed, "doomed");
    g.hideme = w->create(sess, g.c_win, NEUI_W_BUTTON, 12, 48, 120, 26, nullptr);
    w->set_text(sess, g.hideme, "hideme");
    g.owner_field = w->create(sess, g.c_win, NEUI_W_INPUTBOX, 150, 12, 120, 22, nullptr);
    // A TABVIEW whose SECOND page holds a tab stop: switching away from page 2
    // makes it invisible, and the control inside must not keep focus.
    g.tabview = w->create(sess, g.c_win, NEUI_W_TABVIEW, 12, 84, 260, 140, nullptr);
    g.page1 = w->create(sess, g.tabview, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
    w->set_text(sess, g.page1, "one");
    g.page2 = w->create(sess, g.tabview, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
    w->set_text(sess, g.page2, "two");
    g.tabfield = w->create(sess, g.page2, NEUI_W_BUTTON, 10, 10, 100, 26, nullptr);
    w->set_text(sess, g.tabfield, "in page 2");
    // Page 1 gets a control too, so a FORWARD switch (0 -> 1) is testable and not
    // just the backward one - the two used to behave differently.
    g.page1_ctl = w->create(sess, g.page1, NEUI_W_BUTTON, 10, 10, 100, 26, nullptr);
    w->set_text(sess, g.page1_ctl, "in page 1");
    // Checks 11-12: a CONTAINER holding focus, and a re-entrancy victim.
    g.holder = w->create(sess, g.c_win, NEUI_W_SECTION, 150, 48, 120, 60, nullptr);
    attrs->set_string(sess, g.holder, NEUI_ATTR_ALIGN_TEXT, "none");
    g.held1 = w->create(sess, g.holder, NEUI_W_BUTTON, 6, 4, 100, 22, nullptr);
    w->set_text(sess, g.held1, "held one");
    g.held2 = w->create(sess, g.holder, NEUI_W_BUTTON, 6, 30, 100, 22, nullptr);
    w->set_text(sess, g.held2, "held two");
    w->show(sess, g.c_win);

    // ---- frame D: a TABVIEW-ONLY window -----------------------------------
    // Its only tab stops live INSIDE the pages, which is what makes the forward
    // tab switch discriminating: moving focus mid-loop (while the incoming page
    // is still invisible) finds no stop at all and clears focus - a dead keyboard
    // on every forward switch. With the move after the loop, the incoming page is
    // visible and its control takes the focus.
    g.d_win = w->create(sess, widget_none, NEUI_W_APPWINDOW, 420, 400, 240, 180, nullptr);
    w->set_text(sess, g.d_win, "focus D");
    g.d_tabs = w->create(sess, g.d_win, NEUI_W_TABVIEW, 12, 12, 210, 150, nullptr);
    // A TABVIEW is itself a tab stop by default, which would leave the frame with
    // a stop OUTSIDE the pages and mask the very thing this frame is for: focus
    // would land on the strip either way. Take it out of the traversal so the
    // pages' controls really are the only stops.
    w->set_tab_stop(sess, g.d_tabs, false);
    g.d_p1 = w->create(sess, g.d_tabs, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
    w->set_text(sess, g.d_p1, "p1");
    g.d_c1 = w->create(sess, g.d_p1, NEUI_W_BUTTON, 10, 10, 100, 26, nullptr);
    w->set_text(sess, g.d_c1, "d one");
    g.d_p2 = w->create(sess, g.d_tabs, NEUI_W_TABPAGE, 0, 0, 0, 0, nullptr);
    w->set_text(sess, g.d_p2, "p2");
    g.d_c2 = w->create(sess, g.d_p2, NEUI_W_BUTTON, 10, 10, 100, 26, nullptr);
    w->set_text(sess, g.d_c2, "d two");
    w->show(sess, g.d_win);

    // Let both frames realize + paint once (the section body layout the
    // coordinate check depends on is produced by the paint pass).
    [[NSRunLoop mainRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.30]];

    NSWindow* wa = window_titled(@"focus A");
    NSWindow* wb = window_titled(@"focus B");
    NSWindow* wc = window_titled(@"focus C");
    NSWindow* wdw = window_titled(@"focus D");
    check(wa != nil && wb != nil && wc != nil && wdw != nil,
          "all four frames realized as NSWindows");
    if (!wa || !wb || !wc || !wdw) { std::printf("\nFOCUS FAILED (setup)\n"); return 1; }

    // ---- 1/2. forward traversal stays inside frame A -----------------------
    // First Tab with nothing focused picks frame A's first tab stop.
    post_tab(wa, false);
    check_focus(g.a1, "first Tab focuses A's first control");

    post_tab(wa, false);
    check_focus(g.a2, "Tab advances within frame A");

    post_tab(wa, false);
    check_focus(g.a3, "Tab reaches the section's child");

    // The one that used to fail: wrapping past A's last control.
    post_tab(wa, false);
    check_focus(g.a1, "Tab wraps to A's first control, does NOT enter frame B");
    check(!in_frame_b(g_focused), "focus never crossed into frame B");

    // A few more laps - a partition bug can also show up only after wrapping.
    bool escaped = false;
    for (int i = 0; i < 8; ++i) {
      post_tab(wa, false);
      if (in_frame_b(g_focused)) { escaped = true; break; }
    }
    check(!escaped, "8 further Tabs in frame A never reach frame B");

    // ---- 3. reverse traversal ---------------------------------------------
    // Land on a known control first so the reverse step is deterministic.
    // Bounded: an empty tab-stop list (the failure mode this file exists to
    // catch) would make an unbounded loop HANG instead of reporting.
    for (int i = 0; i < 16 && g_focused != g.a1.id; ++i) post_tab(wa, false);
    check(g_focused == g.a1.id, "reached A's first control within 16 Tabs");
    post_tab(wa, true);
    check_focus(g.a3, "Shift+Tab walks backwards within frame A");
    check(!in_frame_b(g_focused), "reverse traversal stays in frame A");

    // ---- 4. frame B cycles independently ----------------------------------
    // Click a control in B so the focused widget - and therefore the frame the
    // traversal is scoped to - moves there.
    click_in(wb, 20.0f, 20.0f);
    check_focus(g.b1, "clicking in frame B focuses B's control");
    post_tab(wb, false);
    check_focus(g.b2, "Tab advances within frame B");
    post_tab(wb, false);
    check_focus(g.b1, "Tab wraps within frame B, does not re-enter A");

    // ---- 5. section-child coordinates (guards child_origin_of) ------------
    // The section is at (12, 90) in frame A and carries a chip, so its child at
    // body-relative (10, 10) sits at frame-local (12+10, 90+band+10). Rather
    // than hard-coding the band height, assert what the contract promises: the
    // event lands on the CHILD and its widget-local coords are inside the
    // child's 120x26 box, near where we clicked. A stale abs_x/abs_y (the
    // failure mode if the two walks disagree) reports the section itself, or
    // coords offset by the whole band.
    // Probe down the column and record the FIRST frame-local y at which the
    // child answers - that y is the child's top edge, which is the number that
    // actually encodes the body offset.
    //
    // Why the top edge and not just "the child was hit": hit-testing and
    // widget-local conversion both derive from the same abs cache, so a
    // CONSISTENT origin error keeps them agreeing with each other and any
    // "coords are inside the box" assertion passes anyway. The band offset only
    // moves y, so y is the axis that can detect it.
    //
    // The section is at frame-local (12, 90) and its child at body-relative
    // (10, 10), so with the band honoured the child's top edge is 90 + band + 10
    // (122 here, band = 22).
    //
    // VERIFIED DISCRIMINATING: dropping the body offset from child_origin_of's
    // ABS output (leaving the paint translate intact) makes this section report
    // first hit at y=112 with child-local (8, 12) instead of y=122 with (8, 0),
    // and the "y == 0 local" assertion below is the one that fires. The
    // mechanism is the section's body CLIP: it still starts at 90+band, so
    // clicks above it are rejected and the first accepted row lands 12 px into
    // a child whose hit rect has slid up. The `> 100` check alone does NOT
    // catch that case - it is here for the grosser failure where the child's
    // rect collapses onto the section origin.
    int first_hit_y = -1;
    for (int probe_y = 90; probe_y <= 200 && first_hit_y < 0; ++probe_y) {
      g_mouse_widget = 0; g_mouse_x = g_mouse_y = -1;
      click_in(wa, 30.0f, (float)probe_y);
      if (g_mouse_widget == g.a3.id) first_hit_y = probe_y;
    }
    check(first_hit_y >= 0, "a click inside the section's child reports the CHILD");
    if (first_hit_y >= 0) {
      std::printf("        child top edge at frame-local y = %d"
                  " (band-less layout would be 100)\n", first_hit_y);
      check(first_hit_y > 100,
            "child sits BELOW the chip band (body offset applied, not dropped)");
      // Sanity bound: a plausible chip band, so a wildly wrong offset also fails.
      check(first_hit_y <= 130, "child top edge is within one chip band of 100");
      // x: the child starts at frame-local 22 (section 12 + body-relative 10),
      // so a click at 30 must report ~8 widget-local.
      std::printf("        child-local coords at first hit = (%d, %d)\n",
                  g_mouse_x, g_mouse_y);
      check(g_mouse_x >= 6 && g_mouse_x <= 10,
            "widget-local x matches the section body offset (expected ~8)");
      check(g_mouse_y == 0, "first hit is the child's top row (y == 0 local)");
    }

    // ---- 6-9. FOCUS MUST NOT POINT AT SOMETHING UNREACHABLE ---------------
    //
    // Four ways the framework used to leave _focused_widget on a widget the user
    // can neither see nor reach. All four are wrong on their own terms - a dead
    // keyboard, or keystrokes landing somewhere invisible - and all four also
    // make an accessibility provider contradict itself, because the node it
    // would report as focused is pruned from the tree (invisible) or gone.

    // 6. DESTROY the focused widget. The old behaviour left _focused_widget
    //    pointing at a freed tree slot: keystrokes went nowhere, and the next
    //    widget created into that recycled slot silently inherited focus with no
    //    focus event ever fired - so a control the user never touched was
    //    "focused" as far as the framework and any AT were concerned.
    w->set_focus(sess, g.doomed);
    check_focus(g.doomed, "6 the doomed button takes focus");
    g_focused = 0; g_unfocused = 0;
    w->destroy(sess, g.doomed);
    check(g_unfocused == g.doomed.id,
          "6 destroying the focused widget fires focus-lost for it");
    // Also try to put focus back with the now-STALE handle, the way a client
    // holding a widget_t across a destroy would. The slot is empty at this point,
    // so this must be refused outright - accepting it re-arms the same bug from
    // the API side, and the typing probe below would catch it.
    w->set_focus(sess, g.doomed);

    // A widget created into the recycled slot must NOT come up focused. Checking
    // for the absence of a focus EVENT proves nothing here - the bug is that
    // _focused_widget silently keeps naming the slot, and no event is fired
    // either way. So make it an INPUTBOX and TYPE: with focus left dangling the
    // characters land in a field the user never selected.
    neui_widget_t recycled = w->create(sess, g.c_win, NEUI_W_INPUTBOX,
                                       12, 12, 120, 22, nullptr);
    // The check only means anything if the slot really was recycled (the low 16
    // bits of a widget id ARE the tree slot), so assert that rather than assume.
    check((recycled.id & 0xffff) == (g.doomed.id & 0xffff),
          "6 the new widget did take the destroyed widget's tree slot");
    type_char(wc, @"z");
    {
      char buf[64] = {0};
      w->get_text(sess, recycled, buf, (int)sizeof(buf));
      check(std::strlen(buf) == 0,
            "6 a widget created into the freed slot does NOT inherit focus");
    }
    // And Tab still works afterwards - a dangling focus used to make the first
    // Tab a no-op.
    post_tab(wc, false);
    check(g_focused != 0 && g_focused != recycled.id,
          "6 Tab still moves focus after the focused widget was destroyed");

    // 7. HIDE the focused widget. It still exists, so focus should move to the
    //    next tab stop in the same frame rather than sit on something invisible.
    w->set_focus(sess, g.hideme);
    check_focus(g.hideme, "7 the hideme button takes focus");
    g_focused = 0; g_unfocused = 0;
    w->hide(sess, g.hideme);
    check(g_unfocused == g.hideme.id,
          "7 hiding the focused widget fires focus-lost for it");
    check(g_focused != 0 && g_focused != g.hideme.id,
          "7 ...and focus moves to another tab stop in the same frame");

    // 8. DESELECT the tab page containing focus. apply_page_geometry sets the
    //    page invisible; focus must leave with it. Before, the user typed into a
    //    control on a page that was no longer on screen.
    // Page 2 has to be the SELECTED page first, or set_selected(0) is a no-op and
    // the whole check passes without anything happening - which is exactly what
    // it did on the first run.
    tabs->set_selected(sess, g.tabview, 1);
    pump_briefly();
    w->set_focus(sess, g.tabfield);
    check_focus(g.tabfield, "8 a control inside the selected tab page takes focus");
    g_focused = 0; g_unfocused = 0;
    tabs->set_selected(sess, g.tabview, 0);   // page 2 goes off screen
    pump_briefly();
    check(g_unfocused == g.tabfield.id,
          "8 selecting another tab fires focus-lost for the hidden control");
    check(g_focused != 0 && g_focused != g.tabfield.id,
          "8 ...and focus moves to a control that IS on screen");
    // Tab must not be able to reach it either, now that its page is hidden.
    g_focused = 0;
    bool reached_hidden = false;
    for (int i = 0; i < 10; ++i) {
      post_tab(wc, false);
      if (g_focused == g.tabfield.id) { reached_hidden = true; break; }
    }
    check(!reached_hidden,
          "8 Tab never lands on a control inside an unselected tab page");

    // 8b The FORWARD switch, which used to behave differently from the backward
    //    one: pages update their visibility in index order, so moving focus
    //    mid-loop meant the newly selected page was still invisible going
    //    forwards (focus left the tabview, or was cleared outright when the
    //    frame's only stops were inside pages) but already visible going
    //    backwards. The move now happens after the loop, with every page's
    //    visibility final.
    w->set_focus(sess, g.page1_ctl);
    check_focus(g.page1_ctl, "8b a control inside page 1 takes focus");
    g_focused = 0; g_unfocused = 0;
    tabs->set_selected(sess, g.tabview, 1);      // forward: page 1 -> page 2
    pump_briefly();
    check(g_unfocused == g.page1_ctl.id,
          "8b a forward switch fires focus-lost for the hidden control");
    check(g_focused != 0 && g_focused != g.page1_ctl.id,
          "8b ...and focus is NOT cleared, it moves to a visible control");
    tabs->set_selected(sess, g.tabview, 0);
    pump_briefly();

    // 8c The same forward switch in a window whose ONLY tab stops are inside the
    //    pages. Moving focus mid-loop finds nothing to move to (the incoming page
    //    is still invisible at that point) and clears focus outright - a dead
    //    keyboard. After the loop, the incoming page is visible and its control
    //    takes it, which is also what a user expects from a tab switch.
    w->set_focus(sess, g.d_c1);
    check_focus(g.d_c1, "8c frame D's page-1 control takes focus");
    g_focused = 0; g_unfocused = 0;
    tabs->set_selected(sess, g.d_tabs, 1);
    pump_briefly();
    check(g_unfocused == g.d_c1.id, "8c the forward switch releases page 1");
    check_focus(g.d_c2,
                "8c focus lands on the INCOMING page's control, not nowhere");

    // 9. A MODAL DIALOG must take focus off its input-blocked owner. Focus is
    //    session-global and -keyDown: routes by it, not by which window the key
    //    arrived at - so with focus left in the owner, typing INTO THE DIALOG
    //    edited a field in the window the dialog had just blocked.
    w->set_focus(sess, g.owner_field);
    check_focus(g.owner_field, "9 the owner's text field takes focus");
    g.dlg = w->create(sess, widget_none, NEUI_W_DIALOG, 200, 200, 200, 100, nullptr);
    w->set_text(sess, g.dlg, "focus dlg");
    g.dlg_btn = w->create(sess, g.dlg, NEUI_W_INPUTBOX, 12, 12, 120, 22, nullptr);
    w->set_owner(sess, g.dlg, g.c_win);
    g_focused = 0; g_unfocused = 0;
    // A modal dialog's show() BLOCKS in a nested OS pump until the dialog is
    // destroyed (that is the documented design), so everything this check needs
    // has to run from inside that pump. The timer fires once, does the work, and
    // destroys the dialog - which is what lets show() return.
    __block int  dlg_focus_ok   = -1;
    __block int  dlg_typing_ok  = -1;
    __block bool dlg_window_ok  = false;
    [NSTimer scheduledTimerWithTimeInterval:0.30 repeats:NO
             block:^(NSTimer*) {
      dlg_focus_ok = (g_focused == g.dlg_btn.id) ? 1 : 0;
      NSWindow* wd = window_titled(@"focus dlg");
      dlg_window_ok = (wd != nil);
      if (wd) {
        // The load-bearing part, asserted POSITIVELY: the keystroke must land in
        // the DIALOG's field, not merely fail to reach the owner's. "Not the
        // owner" alone also passes when focus is cleared and the key goes
        // nowhere, which would hide a half-fixed dialog.
        type_char(wd, @"x");
        char owner_buf[64] = {0}, dlg_buf[64] = {0};
        w->get_text(sess, g.owner_field, owner_buf, (int)sizeof(owner_buf));
        w->get_text(sess, g.dlg_btn, dlg_buf, (int)sizeof(dlg_buf));
        dlg_typing_ok = (std::strlen(owner_buf) == 0 &&
                         std::strcmp(dlg_buf, "x") == 0) ? 1 : 0;
      }
      w->destroy(sess, g.dlg);      // ends the modal pump
    }];
    w->show(sess, g.dlg);           // blocks until the timer's destroy
    check(dlg_window_ok, "9 the dialog realized as an NSWindow");
    check(dlg_focus_ok == 1,
          "9 showing a modal dialog focuses a control INSIDE the dialog");
    check(dlg_typing_ok == 1,
          "9 typing in the dialog does NOT edit the blocked owner's field");
    pump_briefly();
    check_focus(g.owner_field,
                "9 closing the dialog gives focus back to where it was");

    // 9b A DIALOG WITH NO TAB STOP. focus_next does nothing when a frame has no
    //    stops, so the dialog took no focus at all and typing at it still edited
    //    the blocked owner - the very defect check 9 is about, surviving in the
    //    message-box shape (a dialog of plain LABELs). Clearing is the honest
    //    fallback: no control in there can hold focus.
    w->set_focus(sess, g.owner_field);
    neui_widget_t dlg2 = w->create(sess, widget_none, NEUI_W_DIALOG,
                                   260, 260, 200, 90, nullptr);
    w->set_text(sess, dlg2, "focus dlg2");
    neui_widget_t dlg2_lbl = w->create(sess, dlg2, NEUI_W_LABEL,
                                       12, 12, 160, 20, nullptr);
    w->set_text(sess, dlg2_lbl, "no tab stops here");
    w->set_owner(sess, dlg2, g.c_win);
    __block int dlg2_typing_ok = -1;
    [NSTimer scheduledTimerWithTimeInterval:0.30 repeats:NO
             block:^(NSTimer*) {
      NSWindow* w2 = window_titled(@"focus dlg2");
      if (w2) {
        type_char(w2, @"y");
        char buf[64] = {0};
        w->get_text(sess, g.owner_field, buf, (int)sizeof(buf));
        dlg2_typing_ok = (std::strlen(buf) == 0) ? 1 : 0;
      }
      w->destroy(sess, dlg2);
    }];
    w->show(sess, dlg2);
    check(dlg2_typing_ok == 1,
          "9b a dialog with NO tab stop still takes focus off its blocked owner");

    // 9c THE USER closing the dialog window (rather than the client destroying
    //    the widget) unwinds the pump with no widget_destroy running at all, so
    //    the restore has to live in the platform teardown path too. Without it
    //    focus stays on a control inside a CLOSED window - a dead keyboard.
    w->set_focus(sess, g.owner_field);
    neui_widget_t dlg3 = w->create(sess, widget_none, NEUI_W_DIALOG,
                                   300, 300, 200, 90, nullptr);
    w->set_text(sess, dlg3, "focus dlg3");
    neui_widget_t dlg3_btn = w->create(sess, dlg3, NEUI_W_BUTTON,
                                       12, 12, 120, 26, nullptr);
    w->set_text(sess, dlg3_btn, "dlg3 ok");
    w->set_owner(sess, dlg3, g.c_win);
    [NSTimer scheduledTimerWithTimeInterval:0.30 repeats:NO
             block:^(NSTimer*) {
      NSWindow* w3 = window_titled(@"focus dlg3");
      if (w3) [w3 close];        // the USER route: no widget_destroy at all
    }];
    w->show(sess, dlg3);
    pump_briefly();
    check_focus(g.owner_field,
                "9c a USER-closed dialog also restores the owner's focus");
    w->destroy(sess, dlg3);

    // 9d THE SAVED FOCUS CAN GO STALE. prev_focus is a tree slot, and slots are
    //    recycled - so if the widget that held focus is destroyed while the dialog
    //    is up (a client's timer doing UI work is a supported pattern) and a new
    //    widget takes its slot, restoring by slot alone hands focus to a control
    //    the user never touched. The save is stamped with the widget's instance
    //    id and the restore checks it, so a recycled slot is refused instead.
    w->set_focus(sess, g.owner_field);
    neui_widget_t dlg4 = w->create(sess, widget_none, NEUI_W_DIALOG,
                                   340, 340, 200, 90, nullptr);
    w->set_text(sess, dlg4, "focus dlg4");
    neui_widget_t dlg4_btn = w->create(sess, dlg4, NEUI_W_BUTTON,
                                       12, 12, 120, 26, nullptr);
    w->set_text(sess, dlg4_btn, "dlg4 ok");
    w->set_owner(sess, dlg4, g.c_win);
    __block neui_widget_t squatter{};
    [NSTimer scheduledTimerWithTimeInterval:0.30 repeats:NO
             block:^(NSTimer*) {
      // Destroy the widget whose focus the dialog saved, then create another one
      // into its freed slot.
      w->destroy(sess, g.owner_field);
      squatter = w->create(sess, g.c_win, NEUI_W_INPUTBOX, 150, 12, 120, 22, nullptr);
      w->destroy(sess, dlg4);
    }];
    w->show(sess, dlg4);
    pump_briefly();
    check((squatter.id & 0xffff) == (g.owner_field.id & 0xffff),
          "9d the new widget took the saved-focus widget's tree slot");
    type_char(wc, @"q");
    {
      char buf[64] = {0};
      w->get_text(sess, squatter, buf, (int)sizeof(buf));
      check(std::strlen(buf) == 0,
            "9d a recycled slot is NOT mistaken for the saved focus widget");
    }

    // 11 HIDING A CONTAINER that holds focus. focus_leave_subtree ran BEFORE the
    //    container went invisible, so collect_tab_stops still saw its children:
    //    focus was handed to a SIBLING INSIDE the container being hidden - a real
    //    focus-gained event (and an auto-scroll) for a control about to vanish -
    //    and only then cleared. The visible end state was "focus cleared", which
    //    contradicts both the comment and the commit message.
    w->set_focus(sess, g.held1);
    check_focus(g.held1, "11 a control inside the container takes focus");
    g_focused = 0; g_unfocused = 0;
    w->hide(sess, g.holder);
    pump_briefly();
    check(g_unfocused == g.held1.id,
          "11 hiding the container fires focus-lost for the focused child");
    check(g_focused != g.held2.id,
          "11 ...and focus is NEVER handed to a sibling inside it");
    check(g_focused != 0,
          "11 ...it moves to a tab stop OUTSIDE the hidden container");

    // 12 A CLIENT THAT MUTATES THE TREE FROM A FOCUS EVENT. widget_destroy
    //    dispatches focus-lost and then goes on dereferencing the slot it is
    //    tearing down, so a handler that destroys anything is a use-after-free
    //    waiting to happen. Reaching the end of this check IS the assertion.
    // The victim is the trigger's own PARENT - the "this field lost focus, tear
    // down the panel it lives in" handler. That is what makes it a use-after-
    // free rather than a harmless sibling destroy: the panel takes the trigger's
    // tree slot with it, and widget_destroy carries on dereferencing that slot
    // after the callback returns.
    neui_widget_t victim = w->create(sess, g.c_win, NEUI_W_SECTION,
                                     150, 120, 120, 60, nullptr);
    attrs->set_string(sess, victim, NEUI_ATTR_ALIGN_TEXT, "none");
    neui_widget_t trigger = w->create(sess, victim, NEUI_W_BUTTON,
                                      6, 4, 100, 22, nullptr);
    w->set_text(sess, trigger, "trigger");
    w->set_focus(sess, trigger);
    g_reentrant_victim = victim;
    g_reentrant_on     = trigger.id;
    g_reentrant_fired  = false;
    w->destroy(sess, trigger);       // fires focus-lost -> client destroys victim
    pump_briefly();
    check(g_reentrant_fired,
          "12 the client's focus-lost handler really ran during the destroy");
    check(true, "12 ...and destroying its PARENT from it did not corrupt the tree");

    w->destroy(sess, g.d_win);
    w->destroy(sess, g.c_win);
    w->destroy(sess, g.b_win);
    w->destroy(sess, g.a_win);

    std::printf(g_failures ? "\nFOCUS FAILED (%d)\n" : "\nFOCUS OK\n", g_failures);
    return g_failures ? 1 : 0;
  }
}
