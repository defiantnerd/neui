// Cursor acceptance harness (NEUI_ATTR_CURSOR), macOS / xpl host.
//
// The name<->kind mapping is Tier-1 tested (tests/test_cursor_kind.cpp). What
// that cannot reach is the part that was actually broken, and which only a real
// window shows:
//
//   1. RESOLUTION  - the cursor follows the widget under the pointer, and a
//                    widget with no cursor of its own INHERITS from its
//                    ancestors rather than snapping back to an arrow.
//   2. LIVE        - writing NEUI_ATTR_CURSOR while the pointer is already
//                    inside a widget takes effect immediately, without waiting
//                    for a hover transition.
//   3. STICKINESS  - the cursor SURVIVES AppKit's own cursor management. This is
//                    the regression that motivated -cursorUpdate:: AppKit resets
//                    the cursor from the view's cursor rects on every
//                    mouse-moved, so a shape merely `set` once is reverted. The
//                    check here performs AppKit's reset and then calls
//                    -cursorUpdate: exactly as AppKit does.
//   4. OVERRIDE    - the GRID's positional column-resize cursor still works, and
//                    off the divider it now falls back to the widget's own
//                    NEUI_ATTR_CURSOR instead of forcing an arrow.
//
// Hover is driven by posting real NSEvents into the view's -mouseMoved:, which
// is the same entry point AppKit uses, so the whole production path runs
// (platform layer -> dispatch_mouse_event -> set_hovered -> refresh_cursor).
// Nothing test-only is added to the public API, and the result is read from
// [NSCursor currentCursor] - the process's actual cursor, not our bookkeeping -
// so the harness cannot pass by agreeing with itself.
//
// Needs a GUI session (it realizes a real NSWindow), so it is built but not
// ctest-registered; run ./tests/<config>/neui_cursor_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <cstring>

namespace {

int       g_failures = 0;
NSWindow* g_window   = nil;

const char* cursor_desc()
{
  NSCursor* c = [NSCursor currentCursor];
  if (c == [NSCursor arrowCursor])               return "arrow";
  if (c == [NSCursor IBeamCursor])               return "ibeam";
  if (c == [NSCursor pointingHandCursor])        return "hand";
  if (c == [NSCursor crosshairCursor])           return "crosshair";
  if (c == [NSCursor resizeLeftRightCursor])     return "ew-resize";
  if (c == [NSCursor resizeUpDownCursor])        return "ns-resize";
  if (c == [NSCursor openHandCursor])            return "open-hand";
  if (c == [NSCursor closedHandCursor])          return "closed-hand";
  if (c == [NSCursor operationNotAllowedCursor]) return "not-allowed";
  return "<other>";
}

// NSCursor stock accessors return process-wide singletons, so identity
// comparison is the documented way to ask which stock cursor is showing.
void expect_cursor(NSCursor* want, const char* what)
{
  bool ok = ([NSCursor currentCursor] == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) {
    std::printf("        got: %s\n", cursor_desc());
    ++g_failures;
  }
}

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

// Move the pointer to a LOGICAL widget-tree coordinate (y down from the client
// top-left, i.e. neui's own convention) by synthesising the NSEvent AppKit
// would deliver. locationInWindow is y-UP from the window's bottom-left, so the
// y axis is flipped here; the view is isFlipped:YES and converts back.
void move_mouse_to(float lx, float ly)
{
  NSView* v = [g_window contentView];
  if (!v) return;
  const CGFloat h = [v bounds].size.height;
  NSEvent* ev = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                    location:NSMakePoint(lx, h - ly)
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:[g_window windowNumber]
                                     context:nil
                                 eventNumber:0
                                  clickCount:0
                                    pressure:0];
  [v mouseMoved:ev];
}

bool NEUI_ABI onevent(void*, neui_event_t*) { return false; }

neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_interface(void*, const char* iface)
{
  if (iface && std::strcmp(iface, NEUI_API_WIDGETS) == 0) return &g_widget_client;
  return nullptr;
}

} // namespace

int main()
{
  @autoreleasepool {
    neui_init();

    // The xpl host explicitly: NEUI_ATTR_CURSOR is resolved by the xpl host's
    // hover walk, and on macOS neui_get_api(NULL) hands back the NATIVE host
    // first (same caveat as NEUI_API_TIMER / _EMBED).
    neui_api_t* neui = neui_get_api("neui.host.crossplatform");
    if (!neui) { std::printf("[FAIL] xpl host not registered\n"); return 1; }

    neui_client_t client = { NEUI_VERSION, get_interface };
    neui_session_t sess  = neui->create_session(&client, nullptr);

    auto* widgets = (neui_widget_api_t*) neui->get_interface(sess, NEUI_API_WIDGETS);
    auto* attrs   = (neui_attr_api_t*)   neui->get_interface(sess, NEUI_API_ATTRS);
    auto* grid    = (neui_grid_api_t*)   neui->get_interface(sess, NEUI_API_GRID);
    if (!widgets || !attrs || !grid) {
      std::printf("[FAIL] missing interfaces\n");
      return 1;
    }

    // A SECTION carrying "ibeam" with two BUTTONs inside: one that sets nothing
    // (must INHERIT the I-beam) and one that overrides with the CSS alias
    // "pointer". A GRID below it carries "open-hand" so the column-divider
    // override has something of the client's to fall back to.
    neui_widget_t win = widgets->create(sess, widget_none, NEUI_W_APPWINDOW,
                                          80, 80, 460, 380, nullptr);
    neui_widget_t sect = widgets->create(sess, win, NEUI_W_SECTION,
                                           10, 10, 430, 90, nullptr);
    neui_widget_t inherit_btn = widgets->create(sess, sect, NEUI_W_BUTTON,
                                                  20, 30, 120, 28, nullptr);
    neui_widget_t hand_btn = widgets->create(sess, sect, NEUI_W_BUTTON,
                                               170, 30, 120, 28, nullptr);
    neui_widget_t g = widgets->create(sess, win, NEUI_W_GRID,
                                        10, 120, 430, 240, nullptr);

    attrs->set_string(sess, sect, NEUI_ATTR_CURSOR, "ibeam");
    attrs->set_string(sess, hand_btn, NEUI_ATTR_CURSOR, "pointer");
    attrs->set_string(sess, g, NEUI_ATTR_CURSOR, "open-hand");

    // Two columns of known width, so the divider between them sits at a
    // computable x and the harness doesn't have to guess.
    const int kCol0 = 120;
    grid->add_column(sess, g, "A", kCol0);
    grid->add_column(sess, g, "B", 120);
    for (int r = 0; r < 6; ++r) {
      const char* vals[] = { "a", "b", nullptr };
      grid->add_row(sess, g, vals);
    }

    widgets->show(sess, win);
    neui->pump_once(sess);

    g_window = [[NSApp windows] count] ? [[NSApp windows] objectAtIndex:0] : nil;
    if (!g_window) { std::printf("[FAIL] no NSWindow realized\n"); return 1; }

    // A chip-less SECTION's body fills its rect, so a child at section-relative
    // (20, 30) sits at client (30, 40). Aim at each button's centre.
    const float inherit_cx = 10 + 20 + 60,  inherit_cy = 10 + 30 + 14;
    const float hand_cx    = 10 + 170 + 60, hand_cy    = 10 + 30 + 14;

    // ---- 1. resolution + inheritance -----------------------------------
    move_mouse_to(inherit_cx, inherit_cy);
    expect_cursor([NSCursor IBeamCursor],
                    "a button with no cursor inherits the section's ibeam");

    move_mouse_to(hand_cx, hand_cy);
    expect_cursor([NSCursor pointingHandCursor],
                    "a button setting \"pointer\" overrides the inherited ibeam");

    // Over the window background, outside the section and the grid.
    move_mouse_to(455, 110);
    expect_cursor([NSCursor arrowCursor],
                    "outside any cursor-bearing widget resolves to arrow");

    // ---- 3. stickiness against AppKit ----------------------------------
    // Re-enter the ibeam widget, then do what AppKit does on every mouse-moved:
    // clobber the cursor and send -cursorUpdate:. Before NSTrackingCursorUpdate
    // + -cursorUpdate: the shape stayed clobbered, which is the bug this exists
    // to catch.
    move_mouse_to(inherit_cx, inherit_cy);
    NSView* v = [g_window contentView];
    if (v && [v respondsToSelector:@selector(cursorUpdate:)]) {
      [[NSCursor arrowCursor] set];                  // AppKit's reset
      check([NSCursor currentCursor] == [NSCursor arrowCursor],
              "precondition: an AppKit reset does clobber our cursor");
      // A real event, not nil: -cursorUpdate: is declared non-null and the build
      // is warning-clean by policy. The TYPE is mouse-moved rather than
      // cursor-update because +mouseEventWithType: rejects the latter outright
      // (NSInternalInconsistencyException); our -cursorUpdate: ignores the
      // event and only re-applies the tracked kind, so any valid event does.
      NSEvent* cu = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:[g_window windowNumber]
                                         context:nil
                                     eventNumber:0
                                      clickCount:0
                                        pressure:0];
      [(id)v cursorUpdate:cu];                       // AppKit's hook
      expect_cursor([NSCursor IBeamCursor],
                      "cursorUpdate: restores our cursor after AppKit resets it");
    } else {
      std::printf("[FAIL] content view does not implement cursorUpdate:\n");
      ++g_failures;
    }

    // ---- 2. live attr change -------------------------------------------
    // The pointer is still inside inherit_btn, so this must apply NOW rather
    // than at the next hover transition.
    attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "crosshair");
    expect_cursor([NSCursor crosshairCursor],
                    "writing NEUI_ATTR_CURSOR under the pointer applies at once");

    attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "");
    expect_cursor([NSCursor IBeamCursor],
                    "clearing the attr restores inheritance (not arrow)");

    attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "no-such-cursor");
    expect_cursor([NSCursor IBeamCursor],
                    "an unknown cursor name inherits rather than forcing arrow");

    // remove() is the same operation as set_string(key, "") from a client's
    // point of view, so it has to be live too. It was not: the shape stayed
    // pinned until the next hover transition (review finding).
    attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "crosshair");
    expect_cursor([NSCursor crosshairCursor], "precondition: crosshair pinned");
    attrs->remove(sess, inherit_btn, NEUI_ATTR_CURSOR);
    expect_cursor([NSCursor IBeamCursor],
                    "attrs->remove is live too, not just set_string(\"\")");

    // ---- 5. canonical read-back ----------------------------------------
    // <neui/d/attrs.h> promises get_string returns the CANONICAL name, not the
    // alias written. It did not until the normalisation moved into a_set_string
    // (review finding: the header documented a round-trip that did not happen).
    {
      attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "pointer");
      const char* rb = attrs->get_string(sess, inherit_btn, NEUI_ATTR_CURSOR);
      check(rb && std::strcmp(rb, "hand") == 0,
              "get_string canonicalises an alias (\"pointer\" -> \"hand\")");
      if (rb && std::strcmp(rb, "hand") != 0)
        std::printf("        got: \"%s\"\n", rb);

      attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "col_resize");
      rb = attrs->get_string(sess, inherit_btn, NEUI_ATTR_CURSOR);
      check(rb && std::strcmp(rb, "ew-resize") == 0,
              "underscore alias canonicalises too (\"col_resize\" -> \"ew-resize\")");

      // An unknown name reads back as "default", so a client can SEE that its
      // value was not understood instead of getting its own typo echoed back.
      attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "not-a-cursor");
      rb = attrs->get_string(sess, inherit_btn, NEUI_ATTR_CURSOR);
      check(rb && std::strcmp(rb, "default") == 0,
              "an unknown name reads back as \"default\", not echoed");
      attrs->remove(sess, inherit_btn, NEUI_ATTR_CURSOR);
    }

    // ---- 6. hide / unhide is balanced ----------------------------------
    // [NSCursor hide]/unhide is a COUNTER, so an unbalanced hide is permanent
    // for the whole process. NSCursor exposes no "is hidden" query, so this
    // checks the observable proxy: after cursor="none" and back, a normal shape
    // still resolves, and the hidden state does not stick to the next widget.
    attrs->set_string(sess, inherit_btn, NEUI_ATTR_CURSOR, "none");
    move_mouse_to(hand_cx, hand_cy);          // leave the hidden widget
    expect_cursor([NSCursor pointingHandCursor],
                    "leaving a cursor=\"none\" widget restores a real shape");
    move_mouse_to(inherit_cx, inherit_cy);    // hide again
    move_mouse_to(455, 110);                  // out to the background
    expect_cursor([NSCursor arrowCursor],
                    "cursor=\"none\" does not leak to the frame background");
    attrs->remove(sess, inherit_btn, NEUI_ATTR_CURSOR);

    // ---- 7. destroying the hovered widget releases the cursor ----------
    // No pointer-leave event arrives when a widget is destroyed out from under a
    // stationary pointer (the DAW-closes-the-editor case). Before
    // forget_dead_hover, _hovered_widget kept pointing at a freed slot - and a
    // cursor="none" widget left the pointer HIDDEN process-wide (review finding).
    {
      neui_widget_t doomed = widgets->create(sess, win, NEUI_W_BUTTON,
                                               300, 60, 100, 24, nullptr);
      attrs->set_string(sess, doomed, NEUI_ATTR_CURSOR, "not-allowed");
      widgets->show(sess, doomed);
      neui->pump_once(sess);
      move_mouse_to(300 + 50, 60 + 12);
      expect_cursor([NSCursor operationNotAllowedCursor],
                      "precondition: the doomed widget owns the cursor");
      widgets->destroy(sess, doomed);
      expect_cursor([NSCursor arrowCursor],
                      "destroying the hovered widget releases its cursor");
    }

    // ---- 4. GRID positional override -----------------------------------
    // Over a data row: the grid's own "open-hand" shows, proving the divider
    // code path clears its override instead of pinning an arrow.
    move_mouse_to(10 + 60, 120 + 90);
    expect_cursor([NSCursor openHandCursor],
                    "inside the GRID body, the grid's own cursor attr shows");

    // Over the divider between column A and B, inside the header band. The
    // divider sits at the column-A boundary; the header band is at the top of
    // the grid, so a few px down from its origin.
    move_mouse_to(10 + kCol0, 120 + 8);
    expect_cursor([NSCursor resizeLeftRightCursor],
                    "the GRID header column divider still shows ew-resize");

    // Move off the divider, still in the header: back to the grid's attr.
    move_mouse_to(10 + kCol0 / 2, 120 + 8);
    expect_cursor([NSCursor openHandCursor],
                    "off the divider, the override yields to the widget attr");

    // Leaving the GRID entirely must not leak the override.
    move_mouse_to(10 + kCol0, 120 + 8);      // arm it again
    move_mouse_to(455, 110);                  // out to the background
    expect_cursor([NSCursor arrowCursor],
                    "a divider override does not leak outside the GRID");

    neui->destroy(sess);

    if (g_failures) std::printf("\nCURSOR FAILED (%d)\n", g_failures);
    else            std::printf("\nCURSOR OK\n");
    return g_failures ? 1 : 0;
  }
}
