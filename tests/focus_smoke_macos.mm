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
// Last mouse event's widget + widget-local coords, for check 5.
uint32_t g_mouse_widget = 0;
int      g_mouse_x = -1, g_mouse_y = -1;

// neui_widget_t is a struct around an id, so keep the handles AND compare on
// .id - the event payloads carry ids.
struct Ids {
  neui_widget_t a_win{}, a1{}, a2{}, a3{};
  neui_widget_t b_win{}, b1{}, b2{};
  neui_widget_t sect{};
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
    auto* attrs = (neui_attr_api_t*)api->get_interface(sess, NEUI_API_ATTRS);
    if (!w || !attrs) { std::printf("FAIL: no widgets/attrs interface\n"); return 1; }

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

    // Let both frames realize + paint once (the section body layout the
    // coordinate check depends on is produced by the paint pass).
    [[NSRunLoop mainRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.30]];

    NSWindow* wa = window_titled(@"focus A");
    NSWindow* wb = window_titled(@"focus B");
    check(wa != nil && wb != nil, "both frames realized as NSWindows");
    if (!wa || !wb) { std::printf("\nFOCUS FAILED (setup)\n"); return 1; }

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

    w->destroy(sess, g.b_win);
    w->destroy(sess, g.a_win);

    std::printf(g_failures ? "\nFOCUS FAILED (%d)\n" : "\nFOCUS OK\n", g_failures);
    return g_failures ? 1 : 0;
  }
}
