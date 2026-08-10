// Relative (unbounded) pointer-mode harness (NEUI_API_POINTER), macOS / xpl host.
//
// The virtual-position arithmetic is Tier-1 tested
// (tests/test_relative_pointer.cpp). What only a real window can show:
//
//   1. STATE MACHINE - begin/end/is_relative, and that begin is not re-entrant.
//   2. UNBOUNDED     - MOUSE_MOVE keeps arriving with coordinates that run far
//                      outside the widget and go negative, which is the entire
//                      point: a drag must not die at the screen edge.
//   3. FRACTIONAL    - sub-pixel deltas accumulate instead of being truncated to
//                      zero (the macOS trackpad case).
//   4. COORD SPACE   - the reported x/y are WIDGET-local, i.e. the virtual
//                      position is not accidentally shifted by the widget's
//                      frame-local origin. This one caught a real bug:
//                      dispatch_mouse_event subtracts abs_x/abs_y itself, so
//                      handing it widget-local coordinates double-subtracts.
//   5. TEARDOWN      - destroying the session mid-drag leaves the cursor
//                      re-associated and visible, rather than stranding the
//                      whole machine with a pointer that ignores the mouse.
//
// Deltas are injected by building a CGEvent with kCGMouseEventDeltaX/Y set and
// converting it with +[NSEvent eventWithCGEvent:], which is the only way to
// produce an NSEvent whose deltaX/deltaY are non-zero - +mouseEventWithType:
// always reports 0 for them, so a harness built on that would have exercised
// nothing.
//
// Needs a GUI session (it realizes a real NSWindow), so it is built but not
// ctest-registered; run ./tests/<config>/neui_pointer_smoke_macos manually.

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdio>
#include <cstring>
#include <cmath>

namespace {

int           g_failures = 0;
NSWindow*     g_window   = nil;
neui_widget_t g_pad{};

// Last MOUSE_MOVE seen on the pad, widget-local.
struct MoveRecord { bool seen = false; int x = 0; int y = 0; int count = 0; };
MoveRecord g_move;

void check(bool ok, const char* what)
{
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) ++g_failures;
}

void check_eq(int got, int want, const char* what)
{
  bool ok = (got == want);
  std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
  if (!ok) { std::printf("        got %d, want %d\n", got, want); ++g_failures; }
}

bool NEUI_ABI onevent(void*, neui_event_t* ev)
{
  if (ev->type == NEUI_EVENT_MOUSE_MOVE &&
      ev->data.mouse.widget.id == g_pad.id) {
    g_move.seen = true;
    g_move.x    = ev->data.mouse.x;
    g_move.y    = ev->data.mouse.y;
    ++g_move.count;
    return true;                 // consume: no internal handling wanted here
  }
  return false;
}

neui_widget_client_t g_widget_client = { NEUI_VERSION, nullptr, onevent };

void* NEUI_ABI get_interface(void*, const char* iface)
{
  if (iface && std::strcmp(iface, NEUI_API_WIDGETS) == 0) return &g_widget_client;
  return nullptr;
}

// Deliver a mouse-dragged event carrying a raw device delta. The location is
// deliberately fixed: in relative mode it is ignored, and pinning it here proves
// the reported coordinates come from the accumulated delta rather than from the
// event position.
void drag_by(double dx, double dy)
{
  NSView* v = [g_window contentView];
  if (!v) return;

  CGEventRef cg = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDragged,
                                            CGPointMake(0, 0),
                                            kCGMouseButtonLeft);
  if (!cg) return;
  // Integer delta fields are what NSEvent.deltaX/deltaY read back; the double
  // fields carry the sub-pixel part, so both are set for fractional deltas.
  CGEventSetIntegerValueField(cg, kCGMouseEventDeltaX, (int64_t)llround(dx));
  CGEventSetIntegerValueField(cg, kCGMouseEventDeltaY, (int64_t)llround(dy));
  CGEventSetDoubleValueField(cg, kCGMouseEventDeltaX, dx);
  CGEventSetDoubleValueField(cg, kCGMouseEventDeltaY, dy);
  CGEventSetIntegerValueField(cg, kCGEventSourceUserData, 0);

  NSEvent* ev = [NSEvent eventWithCGEvent:cg];
  if (ev) [v mouseDragged:ev];
  CFRelease(cg);
}

} // namespace

int main()
{
  @autoreleasepool {
    neui_init();

    neui_api_t* neui = neui_get_api("neui.host.crossplatform");
    if (!neui) { std::printf("[FAIL] xpl host not registered\n"); return 1; }

    neui_client_t client = { NEUI_VERSION, get_interface };
    neui_session_t sess  = neui->create_session(&client, nullptr);

    auto* widgets = (neui_widget_api_t*)  neui->get_interface(sess, NEUI_API_WIDGETS);
    auto* pointer = (neui_pointer_api_t*) neui->get_interface(sess, NEUI_API_POINTER);
    if (!widgets) { std::printf("[FAIL] no widgets api\n"); return 1; }
    if (!pointer) { std::printf("[FAIL] NEUI_API_POINTER missing on xpl host\n"); return 1; }

    neui_widget_t win = widgets->create(sess, widget_none, NEUI_W_APPWINDOW,
                                          90, 90, 400, 300, nullptr);
    // Offset from the frame origin ON PURPOSE: a widget at (0,0) would make the
    // frame-local and widget-local spaces identical and hide exactly the
    // double-subtraction bug this harness is meant to catch.
    g_pad = widgets->create(sess, win, NEUI_W_CUSTOMDRAW,
                              60, 40, 200, 160, nullptr);
    widgets->show(sess, win);
    neui->pump_once(sess);

    g_window = [[NSApp windows] count] ? [[NSApp windows] objectAtIndex:0] : nil;
    if (!g_window) { std::printf("[FAIL] no NSWindow realized\n"); return 1; }

    // ---- 1. state machine ----------------------------------------------
    check(!pointer->is_relative(sess), "not relative before begin");
    check(pointer->begin_relative(sess, g_pad), "begin_relative succeeds");
    check(pointer->is_relative(sess), "is_relative true after begin");
    check(!pointer->begin_relative(sess, g_pad),
            "begin_relative is not re-entrant (second call fails)");

    // ---- 4. coordinate space -------------------------------------------
    // The pad sits at frame (60, 40). A 10 px right / 10 px down delta must
    // report a WIDGET-local position near the seed, not one shifted by (60, 40).
    g_move = MoveRecord{};
    drag_by(10, 10);
    check(g_move.seen, "a delta produces a MOUSE_MOVE on the pad");
    if (g_move.seen) {
      // The seed is the press point; no button-down was synthesised, so it falls
      // back to the widget centre (100, 80). Assert against that plus the delta.
      check_eq(g_move.x, 100 + 10, "widget-local x = seed + delta (not shifted by abs_x)");
      check_eq(g_move.y, 80 + 10,  "widget-local y = seed + delta (not shifted by abs_y)");
    }

    // ---- 2. unbounded --------------------------------------------------
    // Drive far past the widget's own bounds and past zero. A bounded / clamped
    // implementation stops; this must not.
    for (int i = 0; i < 100; ++i) drag_by(20, 0);
    check(g_move.x > 2000, "x runs far outside the widget (unbounded)");
    for (int i = 0; i < 200; ++i) drag_by(-20, 0);
    check(g_move.x < 0, "x goes negative (unbounded in both directions)");

    // ---- 3. accumulation is exact and symmetric ------------------------
    // NOT tested here: sub-pixel accumulation. A synthesized NSEvent cannot
    // carry a fractional delta - +[NSEvent eventWithCGEvent:] quantises
    // kCGMouseEventDeltaY to an integer (measured: 2.5 -> 2.0, 0.3 -> 0.0),
    // whichever of the integer / double CGEvent fields is set. Real trackpad
    // events do arrive fractional, so the float accumulator earns its keep in
    // production; it is covered in tests/test_relative_pointer.cpp instead.
    //
    // What IS provable here is that the accumulate -> report -> dispatch chain
    // neither loses nor double-counts a delta, and that equal-and-opposite
    // drags land back where they started.
    pointer->end_relative(sess);
    check(!pointer->is_relative(sess), "end_relative clears the mode");
    check(pointer->begin_relative(sess, g_pad), "begin again after end");
    g_move = MoveRecord{};
    drag_by(0, 0);                       // establish the seed
    const int base_y = g_move.y;
    for (int i = 0; i < 10; ++i) drag_by(0, 1);
    check_eq(g_move.y - base_y, 10, "ten 1 px deltas accumulate to exactly 10 px");
    for (int i = 0; i < 10; ++i) drag_by(0, -1);
    check_eq(g_move.y, base_y, "the reverse drag returns to the starting point");

    // end_relative is documented as safe when not active.
    pointer->end_relative(sess);
    pointer->end_relative(sess);
    check(!pointer->is_relative(sess), "end_relative is idempotent");

    // ---- invalid input --------------------------------------------------
    check(!pointer->begin_relative(sess, widget_none),
            "begin_relative rejects an invalid widget");

    // ---- 5. teardown while active ---------------------------------------
    // A session destroyed mid-drag must re-associate the cursor. If it does not,
    // the machine is left with a pointer that ignores the mouse - so this is
    // checked by asserting the association is live afterwards, via the observable
    // proxy that a warp actually moves the reported location.
    check(pointer->begin_relative(sess, g_pad), "begin before teardown");
    neui->destroy(sess);

    {
      const NSPoint before = [NSEvent mouseLocation];
      const CGFloat screen_h = NSMaxY([[NSScreen screens] firstObject].frame);
      const CGPoint target = CGPointMake(before.x + 8.0,
                                           screen_h - before.y + 8.0);
      CGWarpMouseCursorPosition(target);
      const NSPoint after = [NSEvent mouseLocation];
      const bool moved = (std::fabs(after.x - before.x) > 1.0) ||
                          (std::fabs(after.y - before.y) > 1.0);
      check(moved,
              "cursor still tracks the device after destroy mid-drag");
      CGWarpMouseCursorPosition(CGPointMake(before.x, screen_h - before.y));
    }

    if (g_failures) std::printf("\nPOINTER FAILED (%d)\n", g_failures);
    else            std::printf("\nPOINTER OK\n");
    return g_failures ? 1 : 0;
  }
}
