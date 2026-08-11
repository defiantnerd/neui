// macOS NSAccessibility PROVIDER acceptance harness (xpl host).
//
// tests/test_a11y_tree.cpp covers the portable model on fabricated rows;
// tests/a11y_tree_smoke_macos.mm covers the host adapter that produces those
// rows from a live widget tree. This covers the third layer: the provider that
// publishes the tree to the OS. It deliberately talks to the frame's NSView
// through the REAL NSAccessibility protocol - accessibilityChildren,
// accessibilityHitTest:, accessibilityFocusedUIElement, accessibilityRole /
// Label / Value / Frame, accessibilityPerform* - and never through the adapter
// API, because "the provider agrees with the adapter" is not the property that
// matters. What matters is that what VoiceOver would ask for is what it gets.
//
// The two checks that earn their place:
//
//   SCREEN GEOMETRY (check 3). An element's accessibilityFrame is in SCREEN
//   points with a bottom-left origin; nodes are frame-local logical px with a
//   top-left origin. Every other assertion here would still pass with that flip
//   inverted, so the reported rect is cross-validated by converting it back to
//   view coordinates, POSTING A REAL CLICK at its centre, and asserting the
//   production hit-test resolves the same widget. Two independent paths, one
//   answer - the same discipline the adapter harness uses, one layer up. A
//   provider that gets this wrong sends a magnifier or a switch-control cursor
//   somewhere the user cannot click.
//
//   THE IN-PAINT GUARD (check 12). ensure_abs_positions can force a synchronous
//   paint, and a client can reach the accessibility path from a WIDGET_PAINT
//   handler - so it must refuse while a paint is on the stack rather than
//   re-enter paint_frame on a live render context. Asserted by querying from
//   inside a real WIDGET_PAINT and requiring an empty answer, then requiring the
//   next query outside the paint to succeed.
//
// The rest, each a way the provider could be wrong with the model and adapter
// both correct:
//    1  PUBLISHED    - the view has children at all; a BUTTON is AXButton.
//    2  NAMES        - labelled_by names the input; the label itself is gone.
//    3  GEOMETRY     - see above.
//    4  HIT TEST     - a screen point resolves to the same element. 4b (a
//                      scrolled-away child) and 4c (an open combobox's drop
//                      rows) are the cases where our hit test and AppKit's
//                      fallback DISAGREE - see 4c for why the plain case alone
//                      proves nothing.
//    5  FOCUS        - per FRAME: frame A reports its focused element, B nil.
//    6  PRESS        - AT press on a BUTTON reaches the client as a CLICK.
//    7  INCREMENT    - AT increment on a KNOB fires GESTURE_BEGIN / VALUE_CHANGED
//                      / GESTURE_END and moves the value, like an arrow key.
//    8  VALUE TEXT   - a declared range announces "-27", not "50 %".
//    9  NUMBER VALUE - checkbox / tri-state values are 0 / 1 / 2, not strings.
//   10  SUB-ELEMENTS - list rows are AXRow, grid cells AXCell, headers say
//                      "column header"; a row press selects that row.
//   11  IDENTITY     - the same node returns the SAME element object across
//                      rebuilds (VoiceOver holds references), and a destroyed
//                      widget's element answers "gone", not another widget.
//   12  IN-PAINT     - see above; 12b is the provider-level consequence (a
//                      refused rebuild keeps the last good tree instead of
//                      reporting an empty window).
//   13  DISABLED     - a disabled control refuses the press action.
//   14  SECURE       - a password field carries the secure subrole and no value.
//   15  IS_ACTIVE    - false until something queries, true afterwards.
//
// Realizes two real NSWindows, so built but NOT ctest-registered; run
// ./tests/<config>/neui_a11y_provider_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include "a11y_adapter.h"   // check 12 only: the in-paint query has no other door

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_eq_int(long got, long want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) { std::printf("        want %ld, got %ld\n", want, got); ++g_failures; }
}

void check_eq_str(const std::string& got, const char* want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        want \"%s\", got \"%s\"\n", want, got.c_str());
    ++g_failures;
  }
}

// ---- client-side observation ------------------------------------------------

uint32_t g_click_widget  = 0;
uint32_t g_mouse_widget  = 0;
int      g_mouse_x = -1, g_mouse_y = -1;
uint32_t g_value_widget  = 0;
float    g_value         = -1.0f;
int      g_gesture_begin = 0, g_gesture_end = 0;
uint32_t g_item_widget   = 0;
int      g_item_index    = -1;

// Check 12: set from inside a real WIDGET_PAINT.
neui_widget_t g_paint_probe_frame = widget_none;
int  g_in_paint_node_count = -1;      // -1 = the probe never ran
bool g_probe_done          = false;

// Check 12b: a PROVIDER query from inside a paint, armed only once a tree has
// already been built - which is the situation that matters (an AT querying while
// the window happens to be painting must not be told the window is empty).
NSView* g_probe2_view   = nil;
bool    g_probe2_armed  = false;
bool    g_probe2_done   = false;
int     g_probe2_count  = -1;

bool onevent(void*, neui_event_t* ev)
{
  switch (ev->type) {
    case NEUI_EVENT_MOUSE_BUTTON_CLICK:
      g_click_widget = ev->data.mouse.widget.id;
      break;
    case NEUI_EVENT_MOUSE_BUTTON_DOWN:
      g_mouse_widget = ev->data.mouse.widget.id;
      g_mouse_x      = ev->data.mouse.x;
      g_mouse_y      = ev->data.mouse.y;
      break;
    case NEUI_EVENT_VALUE_CHANGED:
      g_value_widget = ev->data.value.widget.id;
      g_value        = ev->data.value.value;
      break;
    case NEUI_EVENT_GESTURE_BEGIN: ++g_gesture_begin; break;
    case NEUI_EVENT_GESTURE_END:   ++g_gesture_end;   break;
    case NEUI_EVENT_ITEM_SELECTED:
      g_item_widget = ev->data.item.widget.id;
      g_item_index  = (int)ev->data.item.index;
      break;
    case NEUI_EVENT_WIDGET_PAINT:
      // The in-paint guard probe. Runs once: this is the only place a client can
      // legitimately be executing while paint_frame is on the stack.
      if (!g_probe_done && g_paint_probe_frame.id != widget_none.id) {
        g_probe_done = true;
        auto nodes = xpl_host::a11y_build_tree_for_frame(g_paint_probe_frame);
        g_in_paint_node_count = (int)nodes.size();
      }
      if (g_probe2_armed && !g_probe2_done && g_probe2_view) {
        g_probe2_done  = true;
        g_probe2_count = (int)[g_probe2_view accessibilityChildren].count;
      }
      break;
    default: break;
  }
  return false;
}

neui_widget_client_t g_wc = { NEUI_VERSION, nullptr, onevent };
void* iface(void*, const char* n)
{ return std::strcmp(n, NEUI_API_WIDGETS) ? nullptr : (void*)&g_wc; }
neui_client_t g_client = { NEUI_VERSION, iface };

// ---- AppKit helpers --------------------------------------------------------

NSWindow* window_titled(NSString* title)
{
  for (NSWindow* win in [NSApp windows])
    if ([[win title] isEqualToString:title]) return win;
  return nil;
}

// Every accessibility query in this harness goes through the frame's content
// view, which is what the OS asks about a neui window's contents.
NSView* view_of(NSWindow* win) { return [win contentView]; }

std::string str_of(id v)
{
  if (![v isKindOfClass:[NSString class]]) return std::string();
  return std::string([(NSString*)v UTF8String]);
}

// Depth-first search of the published tree for the element whose label matches.
id element_labelled(id root, NSString* label)
{
  NSArray* kids = [root accessibilityChildren];
  for (id k in kids) {
    NSString* l = [k accessibilityLabel];
    if (l && [l isEqualToString:label]) return k;
    id deeper = element_labelled(k, label);
    if (deeper) return deeper;
  }
  return nil;
}

// First element (depth-first) with the given role, optionally under a parent
// whose label matches `under` - enough to find a list row or a grid cell.
id element_with_role(id root, NSString* role)
{
  NSArray* kids = [root accessibilityChildren];
  for (id k in kids) {
    if ([[k accessibilityRole] isEqualToString:role]) return k;
    id deeper = element_with_role(k, role);
    if (deeper) return deeper;
  }
  return nil;
}

int count_role(id root, NSString* role)
{
  int n = 0;
  for (id k in [root accessibilityChildren]) {
    if ([[k accessibilityRole] isEqualToString:role]) ++n;
    n += count_role(k, role);
  }
  return n;
}

void click_view_local(NSWindow* win, CGFloat lx, CGFloat ly)
{
  NSView* v = view_of(win);
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

// kVK_* keycodes run through mac_keycode_to_neui in the platform layer, so this
// drives the production key path rather than poking widget state.
void post_key(NSWindow* win, unsigned short kvk)
{
  NSView* v = view_of(win);
  if (!v) return;
  NSEvent* ev = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                 location:NSZeroPoint
                            modifierFlags:0
                                timestamp:0
                             windowNumber:[win windowNumber]
                                  context:nil
                               characters:@""
              charactersIgnoringModifiers:@""
                                isARepeat:NO
                                  keyCode:kvk];
  [v keyDown:ev];
}

void pump(double seconds)
{
  [[NSRunLoop mainRunLoop] runUntilDate:
      [NSDate dateWithTimeIntervalSinceNow:seconds]];
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
    auto* w     = (neui_widget_api_t*) api->get_interface(sess, NEUI_API_WIDGETS);
    auto* attrs = (neui_attr_api_t*)   api->get_interface(sess, NEUI_API_ATTRS);
    auto* items = (neui_items_api_t*)  api->get_interface(sess, NEUI_API_ITEMS);
    auto* grid  = (neui_grid_api_t*)   api->get_interface(sess, NEUI_API_GRID);
    auto* a11y  = (neui_a11y_api_t*)   api->get_interface(sess, NEUI_API_A11Y);
    if (!w || !attrs || !items || !grid || !a11y) {
      std::printf("FAIL: missing interface\n"); return 1;
    }

    // 15a: nothing has queried yet.
    check(!a11y->is_active(sess), "15 is_active is false before any AT query");

    // ---------------------------------------------------------------------
    // Frame A. Content max extent 240 x 300 plus a margin, per the house rule.
    neui_widget_t fa = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                 60, 100, 260, 356, nullptr);
    w->set_text(sess, fa, "prov A");

    neui_widget_t lbl = w->create(sess, fa, NEUI_W_LABEL, 12, 12, 80, 20, nullptr);
    w->set_text(sess, lbl, "Cutoff");
    neui_widget_t inp = w->create(sess, fa, NEUI_W_INPUTBOX, 100, 12, 140, 22, nullptr);
    a11y->set_labelled_by(sess, inp, lbl);

    neui_widget_t knob = w->create(sess, fa, NEUI_W_KNOB, 12, 44, 60, 60, nullptr);
    attrs->set_float(sess, knob, NEUI_PARAM_VALUE, 0.5f);
    a11y->set_value_range(sess, knob, -60.0f, 6.0f, 0.0f);
    a11y->set_name(sess, knob, "Cutoff knob");

    neui_widget_t btn = w->create(sess, fa, NEUI_W_BUTTON, 100, 76, 100, 28, nullptr);
    w->set_text(sess, btn, "Save");

    neui_widget_t dis = w->create(sess, fa, NEUI_W_BUTTON, 100, 110, 100, 28, nullptr);
    w->set_text(sess, dis, "Nope");
    w->set_enabled(sess, dis, false);

    neui_widget_t chk = w->create(sess, fa, NEUI_W_CHECKBOX, 12, 148, 120, 22, nullptr);
    w->set_text(sess, chk, "Mono");
    neui_widget_t chk3 = w->create(sess, fa, NEUI_W_CHECKBOX3, 12, 174, 120, 22, nullptr);
    w->set_text(sess, chk3, "Tri");
    w->set_check(sess, chk3, NEUI_CHECK_INDETERMINATE);

    neui_widget_t pwd = w->create(sess, fa, NEUI_W_INPUTBOX, 140, 148, 100, 22, nullptr);
    attrs->set_int(sess, pwd, NEUI_ATTR_PASSWORD, 1);
    w->set_text(sess, pwd, "hunter2");
    a11y->set_name(sess, pwd, "Passphrase");

    neui_widget_t list = w->create(sess, fa, NEUI_W_LISTBOX, 12, 202, 228, 60, nullptr);
    for (int i = 0; i < 20; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "item %d", i);
      items->add(sess, list, buf, nullptr);
    }

    neui_widget_t gr = w->create(sess, fa, NEUI_W_GRID, 12, 270, 228, 74, nullptr);
    grid->add_column(sess, gr, "Name", 110);
    grid->add_column(sess, gr, "Val",  100);
    for (int r = 0; r < 8; ++r) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "row%d", r);
      const char* vals[2] = { buf, "42" };
      grid->add_row(sess, gr, vals);
    }

    // Check 12's probe surface. Zero-size would be dropped from the tree, so it
    // is a real (small) CUSTOMDRAW; its WIDGET_PAINT runs the in-paint query.
    neui_widget_t cd = w->create(sess, fa, NEUI_W_CUSTOMDRAW, 210, 44, 30, 24, nullptr);
    a11y->set_role(sess, cd, NEUI_A11Y_ROLE_NONE);   // keep it out of the tree
    g_paint_probe_frame = fa;

    w->show(sess, fa);

    // ---------------------------------------------------------------------
    // Frame B - exists only so focus reporting can be shown to be PER FRAME.
    neui_widget_t fb = w->create(sess, widget_none, NEUI_W_APPWINDOW,
                                 360, 100, 230, 290, nullptr);
    w->set_text(sess, fb, "prov B");
    neui_widget_t bbtn = w->create(sess, fb, NEUI_W_BUTTON, 12, 12, 100, 28, nullptr);
    w->set_text(sess, bbtn, "Bee");

    // Check 4b's material: a scrolling SECTION whose later children fall below
    // the fold. They stay in the tree (a focused control that scrolls away must
    // remain reachable) but they are NOT at their reported position on screen.
    neui_widget_t scroller = w->create(sess, fb, NEUI_W_SECTION,
                                      12, 52, 200, 60, nullptr);
    attrs->set_string(sess, scroller, NEUI_ATTR_SCROLL_MODE, "vertical");
    attrs->set_string(sess, scroller, NEUI_ATTR_ALIGN_TEXT, "none");
    for (int i = 0; i < 6; ++i) {
      neui_widget_t b = w->create(sess, scroller, NEUI_W_BUTTON, 8, 6 + i * 34,
                                  120, 26, nullptr);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "s%d", i);
      w->set_text(sess, b, buf);
    }
    // Check 4c: an open COMBOBOX paints its rows OUTSIDE its own rect, which is
    // the one hit-test case AppKit's hierarchical fallback cannot reach.
    neui_widget_t combo = w->create(sess, fb, NEUI_W_COMBOBOX,
                                   12, 124, 150, 22, nullptr);
    items->add(sess, combo, "alpha", nullptr);
    items->add(sess, combo, "beta",  nullptr);
    items->add(sess, combo, "gamma", nullptr);
    items->set_selected(sess, combo, 0);

    w->show(sess, fb);

    pump(0.4);

    NSWindow* wa = window_titled(@"prov A");
    NSWindow* wb = window_titled(@"prov B");
    if (!wa || !wb) { std::printf("FAIL: windows not found\n"); return 1; }
    NSView* va = view_of(wa);
    NSView* vb = view_of(wb);

    // ---------------------------------------------------------------------
    // 1  PUBLISHED
    NSArray* top = [va accessibilityChildren];
    check(top.count > 0, "1  the frame's view publishes accessibility children");
    check_eq_str(str_of([va accessibilityRole]), "AXGroup",
                 "1  the view itself is a group, not a second window");

    id e_btn = element_labelled(va, @"Save");
    check(e_btn != nil, "1  the BUTTON is published");
    if (e_btn)
      check_eq_str(str_of([e_btn accessibilityRole]), "AXButton",
                   "1  BUTTON maps to AXButton");

    // 15b: the query above is the first one.
    check(a11y->is_active(sess), "15 is_active is true after the first AT query");

    // ---------------------------------------------------------------------
    // 2  NAMES - the LABEL names the INPUTBOX and is itself dropped.
    id e_inp = element_labelled(va, @"Cutoff");
    check(e_inp != nil, "2  the INPUTBOX carries the label's text as its name");
    if (e_inp)
      check_eq_str(str_of([e_inp accessibilityRole]), "AXTextField",
                   "2  ...and it is the text field, not the label");
    // The consumed LABEL must not also be published as static text, or an AT
    // reads "Cutoff" twice.
    check_eq_int(count_role(va, @"AXStaticText"), 0,
                 "2  the consumed LABEL is not published as static text");

    // ---------------------------------------------------------------------
    // 3  SCREEN GEOMETRY, cross-validated by a real click. This is the check
    //    that catches an inverted Y-flip; everything else would still pass.
    if (e_btn) {
      NSRect scr = [e_btn accessibilityFrame];
      check(scr.size.width > 0 && scr.size.height > 0,
            "3  the element reports a non-empty screen rect");
      NSPoint centre = NSMakePoint(NSMidX(scr), NSMidY(scr));
      // screen -> window -> view, the reverse of what the provider did.
      NSPoint in_win = [wa convertRectFromScreen:
                            NSMakeRect(centre.x, centre.y, 1, 1)].origin;
      NSPoint in_view = [va convertPoint:in_win fromView:nil];
      g_mouse_widget = 0; g_mouse_x = -1; g_mouse_y = -1;
      // click_view_local flips y itself, so hand it the view-local top-down y.
      click_view_local(wa, in_view.x, in_view.y);
      check(g_mouse_widget == btn.id,
            "3  a click at the reported screen centre hits the reported widget");
      // 100x28 button: the centre is (50, 14) widget-local, +/- rounding.
      check(g_mouse_x >= 48 && g_mouse_x <= 52 &&
            g_mouse_y >= 12 && g_mouse_y <= 16,
            "3  ...and lands at the widget's centre in widget-local px");
    }

    // ---------------------------------------------------------------------
    // 4  HIT TEST - the same screen point resolves to the same element.
    if (e_btn) {
      NSRect scr = [e_btn accessibilityFrame];
      id hit = [va accessibilityHitTest:NSMakePoint(NSMidX(scr), NSMidY(scr))];
      check(hit == e_btn, "4  accessibilityHitTest: returns that same element");
      // A point far outside the frame must not resolve to a widget.
      id miss = [va accessibilityHitTest:NSMakePoint(scr.origin.x - 4000.0,
                                                     scr.origin.y)];
      check(miss != e_btn, "4  a point outside the frame does not hit it");
    }

    // 4b THE CHECK THAT ACTUALLY TESTS OUR HIT TEST. The plain case above does
    //    NOT: when mac_a11y_hit_test returns nil the view falls back to
    //    [super accessibilityHitTest:], and AppKit finds the element by walking
    //    the children we publish and reading their accessibilityFrames - so the
    //    provider's own hit test could be entirely dead and check 4 would still
    //    pass (verified by mutation). The OFFSCREEN rule is where the two paths
    //    DISAGREE: a scrolled-away child is published with its real rect, and
    //    AppKit's frame-based walk happily returns it, while ours skips it
    //    because that is not what is drawn there. An AT pointing a magnifier or
    //    a switch-control cursor at a clipped-away control is the bug this
    //    catches.
    id e_off = element_labelled(vb, @"s5");
    check(e_off != nil, "4b a child scrolled below a section's fold stays in the tree");
    if (e_off) {
      NSRect off = [e_off accessibilityFrame];
      id hit_off = [vb accessibilityHitTest:NSMakePoint(NSMidX(off), NSMidY(off))];
      check(hit_off != e_off,
            "4b ...but hit-testing its rect does NOT resolve to it (offscreen)");
    }

    // 4c THE OTHER DISAGREEMENT, and the one that proves our hit test carries
    //    weight: an open COMBOBOX's drop rows are painted OUTSIDE the collapsed
    //    bar's rect. AppKit's fallback walks the hierarchy and will not descend
    //    into a child whose own frame does not contain the point, so it can
    //    never reach those rows; the flat node hit test can. Without this, an AT
    //    cannot point at the list the user is looking at.
    click_view_local(wb, 12.0f + 75.0f, 124.0f + 11.0f);   // open the drop list
    pump(0.15);
    {
      id e_combo = element_with_role(vb, @"AXPopUpButton");
      check(e_combo != nil, "4c the COMBOBOX is published as AXPopUpButton");
      id e_row = e_combo ? element_labelled(e_combo, @"beta") : nil;
      check(e_row != nil, "4c an open drop row is published");
      if (e_row) {
        NSRect rr = [e_row accessibilityFrame];
        NSRect cr = e_combo ? [e_combo accessibilityFrame] : NSZeroRect;
        check(!NSContainsRect(cr, rr),
              "4c ...and it sits outside the collapsed bar's own rect");
        id hit_row = [vb accessibilityHitTest:NSMakePoint(NSMidX(rr), NSMidY(rr))];
        check(hit_row == e_row,
              "4c hit-testing an open drop row resolves to that row");
      }
    }
    // An open combo swallows every click in its frame, so close it before the
    // focus checks below (learned the hard way in the adapter harness).
    post_key(wb, 0x35 /* kVK_Escape */);
    pump(0.1);

    // ---------------------------------------------------------------------
    // 5  FOCUS, PER FRAME. Click the button in A to focus it.
    click_view_local(wa, 150, 90);
    pump(0.1);
    id focused_a = [va accessibilityFocusedUIElement];
    check(focused_a == e_btn, "5  frame A reports its own focused element");
    if (focused_a) check([focused_a isAccessibilityFocused],
                         "5  ...and that element says it is focused");
    // B's view must not answer with A's element. NSView's own fallback answers
    // with the view itself, which is the honest "focus is not in me".
    id focused_b = [vb accessibilityFocusedUIElement];
    check(focused_b != e_btn,
          "5  frame B does NOT report frame A's focused element");

    // ---------------------------------------------------------------------
    // 6  PRESS reaches the client as a real CLICK.
    if (e_btn) {
      g_click_widget = 0;
      check([e_btn isAccessibilitySelectorAllowed:@selector(accessibilityPerformPress)],
            "6  a BUTTON offers the press action");
      check([e_btn accessibilityPerformPress], "6  press is performed");
      check(g_click_widget == btn.id,
            "6  ...and arrives as MOUSE_BUTTON_CLICK on that button");
    }

    // ---------------------------------------------------------------------
    // 7  INCREMENT on the KNOB - the arrow-key path, gestures included.
    id e_knob = element_labelled(va, @"Cutoff knob");
    check(e_knob != nil, "7  the KNOB is published under its declared name");
    if (e_knob) {
      check_eq_str(str_of([e_knob accessibilityRole]), "AXSlider",
                   "7  KNOB maps to AXSlider");
      check_eq_str(str_of([e_knob accessibilityRoleDescription]), "knob",
                   "7  ...but its role description still says knob");
      g_value_widget = 0; g_value = -1.0f;
      g_gesture_begin = g_gesture_end = 0;
      check([e_knob isAccessibilitySelectorAllowed:@selector(accessibilityPerformIncrement)],
            "7  a slider offers increment");
      check([e_knob accessibilityPerformIncrement], "7  increment is performed");
      check(g_value_widget == knob.id && g_value > 0.55f && g_value < 0.65f,
            "7  ...moving the value one 10 % step (0.5 -> 0.6)");
      // The gesture pair is what a DAW records as one automation edit.
      check_eq_int(g_gesture_begin, 1, "7  it fires exactly one GESTURE_BEGIN");
      check_eq_int(g_gesture_end,   1, "7  ...and exactly one GESTURE_END");
      check([e_knob accessibilityPerformDecrement], "7  decrement is performed");
      check(g_value > 0.45f && g_value < 0.55f, "7  ...returning to 0.5");
    }

    // ---------------------------------------------------------------------
    // 8  VALUE TEXT - the declared range, not a percentage.
    if (e_knob)
      check_eq_str(str_of([e_knob accessibilityValue]), "-27",
                   "8  a -60..+6 range announces 0.5 as -27, not \"50 %\"");

    // ---------------------------------------------------------------------
    // 9  NUMBER VALUES for checkbox-ish roles.
    id e_chk  = element_labelled(va, @"Mono");
    id e_chk3 = element_labelled(va, @"Tri");
    check(e_chk != nil && e_chk3 != nil, "9  both checkboxes are published");
    if (e_chk) {
      id v = [e_chk accessibilityValue];
      check([v isKindOfClass:[NSNumber class]] && [(NSNumber*)v intValue] == 0,
            "9  an unchecked CHECKBOX reports the number 0");
      check([e_chk accessibilityPerformPress], "9  pressing it is performed");
      v = [e_chk accessibilityValue];
      check([v isKindOfClass:[NSNumber class]] && [(NSNumber*)v intValue] == 1,
            "9  ...and it then reports 1");
    }
    if (e_chk3) {
      id v = [e_chk3 accessibilityValue];
      check([v isKindOfClass:[NSNumber class]] && [(NSNumber*)v intValue] == 2,
            "9  a tri-state's indeterminate reports 2 (mixed), never 1");
    }

    // ---------------------------------------------------------------------
    // 10  SUB-ELEMENTS - rows, cells, headers, and a row press that selects.
    id e_list = element_with_role(va, @"AXList");
    check(e_list != nil, "10 the LISTBOX is published as AXList");
    if (e_list) {
      NSArray* rows = [e_list accessibilityChildren];
      check(rows.count > 0 && rows.count < 20,
            "10 it publishes a WINDOW of rows, not all 20");
      id r0 = rows.count ? rows[0] : nil;
      if (r0) {
        check_eq_str(str_of([r0 accessibilityRole]), "AXRow",
                     "10 a list row is AXRow");
        check_eq_str(str_of([r0 accessibilityLabel]), "item 0",
                     "10 ...named by its text");
      }
      // Pressing a row selects it - the synthesised-click path.
      if (rows.count > 1) {
        g_item_widget = 0; g_item_index = -1;
        check([rows[1] isAccessibilitySelectorAllowed:@selector(accessibilityPerformPress)],
              "10 a list row offers press");
        check([rows[1] accessibilityPerformPress], "10 row press is performed");
        check(g_item_widget == list.id && g_item_index == 1,
              "10 ...and selects that row (ITEM_SELECTED, index 1)");
      }
    }
    id e_grid = element_with_role(va, @"AXTable");
    check(e_grid != nil, "10 the GRID is published as AXTable");
    if (e_grid) {
      check(count_role(e_grid, @"AXCell") > 0, "10 it publishes cells");
      // The header is a Button whose role description says what it really is.
      id header = element_labelled(e_grid, @"Name");
      check(header != nil, "10 a column header is published");
      if (header)
        check_eq_str(str_of([header accessibilityRoleDescription]),
                     "column header",
                     "10 ...described as a column header, not just \"button\"");
    }

    // ---------------------------------------------------------------------
    // 11  IDENTITY across rebuilds, and a destroyed widget's element.
    id e_btn_again = element_labelled(va, @"Save");
    check(e_btn_again == e_btn,
          "11 a second traversal returns the SAME element object");
    // Force a rebuild: a text change repaints, which bumps the revision.
    w->set_text(sess, chk, "Mono!");
    pump(0.15);
    id e_btn_after = element_labelled(va, @"Save");
    check(e_btn_after == e_btn,
          "11 ...and it survives a rebuild (VoiceOver holds references)");

    id e_doomed = element_labelled(va, @"Mono!");
    check(e_doomed != nil, "11 the renamed checkbox is found under its new name");
    if (e_doomed) {
      w->destroy(sess, chk);
      pump(0.15);
      [va accessibilityChildren];            // force the rebuild
      // The element outlives its node. It must answer "gone" - NOT the facts of
      // whatever widget later occupies that slot, which is the whole reason a
      // node id carries a per-instance generation.
      check_eq_str(str_of([e_doomed accessibilityRole]), "AXUnknown",
                   "11 an element whose widget was destroyed reports unknown");
      check(NSIsEmptyRect([e_doomed accessibilityFrame]),
            "11 ...and an empty rect rather than someone else's");
      check([e_doomed accessibilityLabel] == nil,
            "11 ...and no name");
      check(![e_doomed accessibilityPerformPress],
            "11 ...and refuses to be pressed");
    }

    // ---------------------------------------------------------------------
    // 12  THE IN-PAINT GUARD. The probe ran inside a real WIDGET_PAINT.
    check(g_probe_done, "12 the in-paint probe actually ran (from WIDGET_PAINT)");
    check_eq_int(g_in_paint_node_count, 0,
                 "12 an accessibility query from inside a paint is REFUSED");
    // And the frame is unharmed: the next out-of-paint query still works.
    check([va accessibilityChildren].count > 0,
          "12 ...while a query outside the paint still answers");

    // 12b The same refusal reaching the PROVIDER must not blank the window. A
    //     rebuild that cannot be performed keeps the last good tree, because
    //     "no children" would make VoiceOver announce an empty window and stop -
    //     and an AT query landing during a paint is a matter of timing, not of
    //     anything being wrong.
    g_probe2_view  = va;
    g_probe2_armed = true;
    w->set_text(sess, btn, "Save.");         // force a repaint
    pump(0.25);
    check(g_probe2_done, "12b the in-paint PROVIDER probe ran");
    check(g_probe2_count > 0,
          "12b a provider query during a paint keeps the last good tree");

    // ---------------------------------------------------------------------
    // 13  DISABLED
    id e_dis = element_labelled(va, @"Nope");
    check(e_dis != nil, "13 a disabled BUTTON stays in the tree");
    if (e_dis) {
      check(![e_dis isAccessibilityEnabled], "13 ...reported as not enabled");
      check(![e_dis isAccessibilitySelectorAllowed:@selector(accessibilityPerformPress)],
            "13 ...and it does not offer the press action");
    }

    // ---------------------------------------------------------------------
    // 14  SECURE FIELD
    id e_pwd = element_labelled(va, @"Passphrase");
    check(e_pwd != nil, "14 a password INPUTBOX is published");
    if (e_pwd) {
      check_eq_str(str_of([e_pwd accessibilitySubrole]), "AXSecureTextField",
                   "14 ...with the secure subrole");
      check([e_pwd accessibilityValue] == nil,
            "14 ...and its contents are never offered as a value");
    }

    // An announcement has no node behind it; assert only that it is safe to
    // call (whether speech happens is VoiceOver's business, not testable here).
    a11y->announce(sess, "harness announcement", false);
    a11y->notify(sess, knob, NEUI_A11Y_CHANGE_VALUE);
    check(true, "-- announce + notify are safe to call with no AT attached");

    w->destroy(sess, fb);
    w->destroy(sess, fa);
    api->destroy(sess);

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "A11Y PROVIDER FAILED" : "A11Y PROVIDER OK",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
  }
}
