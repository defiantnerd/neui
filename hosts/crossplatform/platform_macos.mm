// macOS platform layer for the crossplatform host.
//
// Step 2: NSApplication + NSWindow + NEUIView render loop. drawRect: pulls
// the AppKit-supplied CGContextRef and routes it through the CG backend's
// set_current_frame, then calls Session::paint_frame (which itself wraps
// begin_frame / paint_widgets / end_frame).
//
// Step 3: NSResponder mouse + key handlers on NEUIView. Mouse events route
// through Session::widget_at + dispatch_mouse_event with a captured
// _pressed_widget across drags. Key events translate kVK_* -> NEUI_KEY_*
// (matching Win32 VK_*) and Cmd -> NEUI_KMOD_CTRL / Control -> NEUI_KMOD_META
// (Cmd is the platform-primary modifier per neui's convention). Window
// key/resign-key notifications mirror the win32 host's WM_SETFOCUS / WM_KILLFOCUS
// path on the frame.
//
// Step 5 (clipboard), step 6 (IME), step 7 (NSMenu) and later live in this
// same file - added incrementally per the plan.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cstring>

#include "host.h"
#include "platform.h"
#include "../shared/macos/modal_pump_macos.h"
#include "../../backends/cg/cg_backend.h"
#include "../shared/macos/clipboard_macos.h"
// This TU owns the single out-of-line @implementation NEUIDragSource body
// (the header is also included by the native host's widgets.mm, which co-links).
#define NEUI_DND_SOURCE_MACOS_IMPLEMENTATION
#include "../shared/dnd_modifier_suggest.h"
#include "../shared/macos/dnd_source_macos.h"
#include "../shared/macos/image_loader_macos.h"
#include "../shared/macos/window_helpers_macos.h"
#include "../shared/macos/keys_macos.h"
#include "../shared/macos/menubar_macos.h"
#include "../shared/macos/theme_provider_macos.h"

// ---------------------------------------------------------------------------
// Forward declarations (Objective-C classes).

@class NEUIView;
@class NEUIWindowDelegate;

namespace xpl_host { class Session; struct WidgetData; }

// Key + modifier + button translation primitives now live in
// hosts/shared/macos/keys_macos.h and are shared with the native macOS host.
using neui_detail::mac_keycode_to_neui;
using neui_detail::mac_modifiers_to_neui;
using neui_detail::mac_buttonmap;
using neui_detail::is_printable_codepoint;

// ---------------------------------------------------------------------------
// Module-private state.

namespace {

// Live APPWINDOW count. Hits 0 -> [NSApp stop:nil] (mirrors the win32 host's
// PostQuitMessage on the last APPWINDOW close). DIALOG / PLUGWINDOW do NOT
// participate.
int g_appwindow_count = 0;

// Posts a no-op event so [NSApp stop:nil] takes effect on the next pump
// iteration. AppKit only checks the stop flag between event dispatches.
void wake_app_event_pump()
{
  NSEvent* dummy = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
  [NSApp postEvent:dummy atStart:NO];
}

} // namespace

// ---------------------------------------------------------------------------
// NEUIView - content view for every neui frame. drawRect: drives the render
// pipeline; isFlipped=YES so the CTM matches the renderer.h convention
// (origin top-left, Y down). Mouse / keyboard hooks land in step 3.

@interface NEUIView : NSView<NSTextInputClient, NSDraggingDestination>
{
@public
  xpl_host::Session* session;
  uint32_t           widget_index;
@private
  NSTrackingArea*    _tracking_area;
  // IME composition state. _composing tracks whether we're between
  // setMarkedText: and unmarkText: / a committing insertText:. The marked
  // text length (UTF-16 chars) is used to answer markedRange queries.
  BOOL               _composing;
  NSUInteger         _marked_text_len;
  // GRID smooth-scroll spring-back animation. _grid_bounce_widget is the tree
  // slot of the grid currently bouncing; the kinetics state itself lives in
  // that grid's GridModel.scroll_kin (shared with the elastic math).
  NSTimer*           _grid_bounce_timer;
  uint32_t           _grid_bounce_widget;
  // Scrolling-SECTION spring-back animation - same shape as the GRID pair
  // above; the kinetics live in the section's SectionScrollState.kin_v/_h.
  NSTimer*           _section_bounce_timer;
  uint32_t           _section_bounce_widget;
}
@end

@implementation NEUIView

- (BOOL)isFlipped { return YES; }

- (BOOL)isOpaque  { return YES; }

// Without this the view never receives keyDown:, and the window's first
// responder falls back to the window itself which doesn't dispatch our
// events. Required for keyboard input to reach session->dispatch_event.
- (BOOL)acceptsFirstResponder { return YES; }

// AppKit's default click-to-focus path will ignore the very first mouseDown:
// in a window unless the view opts in here. Without this you have to click
// twice - once to focus the window, again to register the click.
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }

- (void)drawRect:(NSRect)dirtyRect
{
  (void)dirtyRect;
  if (!session) return;
  xpl_host::WidgetData* wd = session->get_widget(widget_index);
  if (!wd || !wd->render_ctx) return;

  CGContextRef cg = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
  if (!cg) return;

  NSSize sz = self.bounds.size;
  neui_cg_backend::set_current_frame(wd->render_ctx, (void*)cg,
                                      (float)sz.width, (float)sz.height);
  session->paint_frame(wd->render_ctx, widget_index);
}

// ---------------------------------------------------------------------------
// Mouse tracking - install/refresh an NSTrackingArea that fills the view, so
// mouseEntered: / mouseExited: / mouseMoved: fire whenever the cursor is
// over us in the active key window.

- (void)updateTrackingAreas
{
  [super updateTrackingAreas];
  if (_tracking_area) {
    [self removeTrackingArea:_tracking_area];
    _tracking_area = nil;
  }
  NSTrackingAreaOptions opts =
      NSTrackingMouseEnteredAndExited
    | NSTrackingMouseMoved
    | NSTrackingActiveInKeyWindow
    | NSTrackingInVisibleRect;
  _tracking_area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                 options:opts
                                                   owner:self
                                                userInfo:nil];
  [self addTrackingArea:_tracking_area];
}

// Convert an NSEvent's cursor location into top-left-origin view-local logical
// pixels (matches the renderer.h coordinate convention).
- (NSPoint)localPointForEvent:(NSEvent*)event
{
  return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)dispatchMouseEventForType:(neui_event_type_t)type
                            event:(NSEvent*)event
{
  if (!session) return;
  NSPoint p = [self localPointForEvent:event];
  float lx = (float)p.x;
  float ly = (float)p.y;
  uint32_t hit = session->widget_at(lx, ly, widget_index);

  // For button-down we update _pressed_widget + focus; for drag/up we route
  // to the originally-pressed widget. mouseMoved: takes the live hit-test.
  uint32_t target = hit;
  if (type == NEUI_EVENT_MOUSE_BUTTON_DOWN
   || type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK
   || type == NEUI_EVENT_MOUSE_RBUTTON_DOWN) {
    if (type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      session->set_focus(hit);
      session->set_pressed(hit);
    }
    target = hit;
  } else if (type == NEUI_EVENT_MOUSE_MOVE) {
    session->set_hovered(hit);
    NSUInteger pressed_buttons = NSEvent.pressedMouseButtons;
    if (session->_pressed_widget != 0 && (pressed_buttons & 0x1)) {
      target = session->_pressed_widget;
    }
  }

  if (target == 0) return;
  auto* hw = session->get_widget(target);
  if (!hw) return;

  uint32_t btnmap = mac_buttonmap(NSEvent.pressedMouseButtons, event.modifierFlags);

  neui_event_t ev = {};
  ev.type                 = type;
  ev.data.mouse.widget    = { hw->widget_id };
  ev.data.mouse.x         = (int)lx;
  ev.data.mouse.y         = (int)ly;
  ev.data.mouse.buttonmap = btnmap;
  session->dispatch_mouse_event(target, &ev);
}

- (void)mouseDown:(NSEvent*)event
{
  if (!session) return;
  // clickCount == 2 -> DBLCLICK; clickCount >= 3 not modeled.
  if (event.clickCount == 2) {
    [self dispatchMouseEventForType:NEUI_EVENT_MOUSE_BUTTON_DBLCLICK event:event];
    return;
  }
  NSPoint p = [self localPointForEvent:event];
  // Popup menu overlay absorbs the click - picks an item or dismisses.
  // Mirrors the win32 WM_LBUTTONDOWN wiring.
  if (session->_popup_active) {
    session->handle_popup_click((float)p.x, (float)p.y);
    return;
  }
  // When a combo overlay is open, all clicks go to combo handling only -
  // consumed regardless of where they land (inside or outside the overlay).
  // No explicit capture needed: AppKit keeps sending mouseDragged: to this
  // view while the button is held, so an overlay scrollbar drag just works.
  if (session->handle_combo_click((float)p.x, (float)p.y)) return;
  [self dispatchMouseEventForType:NEUI_EVENT_MOUSE_BUTTON_DOWN event:event];
}

// Shared overlay pre-checks for the MOUSE_MOVE paths (mouseMoved: +
// mouseDragged:). Returns YES when an overlay consumed the move - mirrors
// the win32 WM_MOUSEMOVE wiring order: popup hover, combo scrollbar drag,
// combo row hover.
- (BOOL)overlayHandledMove:(NSEvent*)event
{
  if (!session) return NO;
  NSPoint p = [self localPointForEvent:event];
  if (session->_popup_active) {
    session->handle_popup_hover((float)p.x, (float)p.y);
    return YES;
  }
  if (session->handle_combo_scroll_drag((float)p.y)) return YES;
  if (session->handle_combo_hover((float)p.x, (float)p.y)) return YES;
  return NO;
}

- (void)mouseDragged:(NSEvent*)event
{
  // On macOS, a dragged event is just a mouseMoved with primary button down.
  // The dispatch_mouse_event helper already routes to _pressed_widget when a
  // button is held - same shape as Win32's capture-driven path.
  if ([self overlayHandledMove:event]) return;
  [self dispatchMouseEventForType:NEUI_EVENT_MOUSE_MOVE event:event];
}

- (void)mouseUp:(NSEvent*)event
{
  if (!session) return;
  // End a combo overlay scrollbar drag if one was active (win32 parity).
  if (session->_combo_sb_dragging) {
    session->_combo_sb_dragging = false;
    return;
  }
  NSPoint p = [self localPointForEvent:event];
  float lx = (float)p.x;
  float ly = (float)p.y;
  uint32_t hit     = session->widget_at(lx, ly, widget_index);
  uint32_t pressed = session->_pressed_widget;
  session->set_pressed(0);

  if (hit == 0) return;
  auto* hw = session->get_widget(hit);
  if (!hw) return;

  neui_event_t ev = {};
  ev.data.mouse.widget    = { hw->widget_id };
  ev.data.mouse.x         = (int)lx;
  ev.data.mouse.y         = (int)ly;
  ev.data.mouse.buttonmap = 0;

  ev.type = NEUI_EVENT_MOUSE_BUTTON_UP;
  session->dispatch_mouse_event(hit, &ev);

  // CLICK fires only when the up landed on the same widget as the down.
  if (hit == pressed) {
    ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
    session->dispatch_mouse_event(hit, &ev);
  }
}

- (void)rightMouseDown:(NSEvent*)event
{
  if (!session) return;
  // If a popup is up, a right-click outside dismisses it (win32 parity).
  if (session->_popup_active) {
    NSPoint p = [self localPointForEvent:event];
    session->handle_popup_click((float)p.x, (float)p.y);
    return;
  }
  [self dispatchMouseEventForType:NEUI_EVENT_MOUSE_RBUTTON_DOWN event:event];
}

- (void)rightMouseUp:(NSEvent*)event
{
  if (!session) return;
  NSPoint p = [self localPointForEvent:event];
  float lx = (float)p.x;
  float ly = (float)p.y;
  uint32_t hit = session->widget_at(lx, ly, widget_index);
  if (hit == 0) return;
  auto* hw = session->get_widget(hit);
  if (!hw) return;
  neui_event_t ev = {};
  ev.type                 = NEUI_EVENT_MOUSE_RBUTTON_UP;
  ev.data.mouse.widget    = { hw->widget_id };
  ev.data.mouse.x         = (int)lx;
  ev.data.mouse.y         = (int)ly;
  ev.data.mouse.buttonmap = 0;
  session->dispatch_mouse_event(hit, &ev);
}

- (void)mouseMoved:(NSEvent*)event
{
  if ([self overlayHandledMove:event]) return;
  [self dispatchMouseEventForType:NEUI_EVENT_MOUSE_MOVE event:event];
}

- (void)mouseEntered:(NSEvent*)event
{
  // Hover state updates from the regular mouseMoved path; the dedicated
  // ENTER notification isn't fired through the public event API
  // (Session::set_hovered emits MOUSE_ENTER itself when the hovered widget
  // changes). The override exists so AppKit doesn't bubble the event up the
  // responder chain.
  (void)event;
}

- (void)mouseExited:(NSEvent*)event
{
  if (!session) return;
  // Cursor left the view - clear hover so any current MOUSE_LEAVE fires.
  session->set_hovered(0);
  (void)event;
}

// --- GRID smooth-scroll spring-back animation ------------------------------

- (void)gridStopBounce
{
  if (_grid_bounce_timer) { [_grid_bounce_timer invalidate]; _grid_bounce_timer = nil; }
}

- (void)gridStartBounce:(uint32_t)widgetIdx
{
  [self gridStopBounce];
  _grid_bounce_widget = widgetIdx;
  _grid_bounce_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                               target:self
                                             selector:@selector(gridBounceTick:)
                                             userInfo:nil
                                              repeats:YES];
  [[NSRunLoop currentRunLoop] addTimer:_grid_bounce_timer
                               forMode:NSRunLoopCommonModes];
}

- (void)gridBounceTick:(NSTimer*)timer
{
  (void)timer;
  if (!session) { [self gridStopBounce]; return; }
  auto* hw = session->get_widget(_grid_bounce_widget);
  neui_detail::GridModel* model = hw ? hw->grid_model_ptr() : nullptr;
  if (!model) { [self gridStopBounce]; return; }
  auto cfg = neui_detail::grid_read_config(hw->attrs.get());
  neui_detail::GridViewport vp =
    neui_detail::grid_compute_viewport(*model, hw->width, hw->height,
                                        cfg.row_h, cfg.header_h);
  bool more = neui_detail::grid_scroll_bounce_step(*model, vp, cfg.row_h);
  [self setNeedsDisplay:YES];
  if (!more) [self gridStopBounce];
}

// --- Scrolling-SECTION smooth-scroll spring-back animation ------------------
// Same shape as the GRID timer above; both axes step in one tick (a "both"
// section can overscroll vertically and horizontally in the same gesture).

- (void)sectionStopBounce
{
  if (_section_bounce_timer) {
    [_section_bounce_timer invalidate];
    _section_bounce_timer = nil;
  }
}

- (void)sectionStartBounce:(uint32_t)widgetIdx
{
  [self sectionStopBounce];
  _section_bounce_widget = widgetIdx;
  _section_bounce_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                                  target:self
                                                selector:@selector(sectionBounceTick:)
                                                userInfo:nil
                                                 repeats:YES];
  [[NSRunLoop currentRunLoop] addTimer:_section_bounce_timer
                               forMode:NSRunLoopCommonModes];
}

- (void)sectionBounceTick:(NSTimer*)timer
{
  (void)timer;
  if (!session) { [self sectionStopBounce]; return; }
  auto* hw = session->get_widget(_section_bounce_widget);
  neui_detail::SectionScrollState* st = hw ? hw->scroll_state_ptr() : nullptr;
  const neui_detail::SectionLayout* L = hw ? hw->section_layout_ptr() : nullptr;
  if (!st || !L) { [self sectionStopBounce]; return; }
  bool more_v = neui_detail::section_scroll_bounce_step(*st, *L, false);
  bool more_h = neui_detail::section_scroll_bounce_step(*st, *L, true);
  [self setNeedsDisplay:YES];
  if (!more_v && !more_h) [self sectionStopBounce];
}

// Feed a wheel NSEvent into a scrolling SECTION's per-axis kinetics. The
// widgets BELOW the section already had first refusal (bounded bubble in
// scrollWheel:); this is the SECTION-consumes path - the pixel-precise
// twin of the GRID smooth-scroll branch, same curve + tuning.
- (void)sectionKineticWheel:(NSEvent*)event section:(uint32_t)secIdx
{
  using namespace neui_detail;
  auto* sw = session->get_widget(secIdx);
  SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
  const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
  if (!st || !L) return;

  bool prec = event.hasPreciseScrollingDeltas;
  ScrollWheelInput base;
  base.precise        = prec;
  base.phase_began    = (event.phase == NSEventPhaseBegan);
  base.phase_changed  = (event.phase == NSEventPhaseChanged);
  base.phase_ended    = (event.phase == NSEventPhaseEnded) ||
                        (event.phase == NSEventPhaseCancelled);
  base.momentum       = (event.momentumPhase != NSEventPhaseNone);
  base.momentum_ended = (event.momentumPhase == NSEventPhaseEnded);

  // Precise deltas are already px; classic wheel lines scale through the
  // shared per-line step so notch speed matches the win32 host.
  double dv = (double)event.scrollingDeltaY;
  double dh = (double)event.scrollingDeltaX;
  if (!prec) { dv *= SECTION_WHEEL_LINE_PX; dh *= SECTION_WHEEL_LINE_PX; }

  bool has_v = section_axis_has_v(st->axis);
  bool has_h = section_axis_has_h(st->axis);
  // Single-axis fallback, matching section_apply_wheel's routing: a
  // vertical-only delta still scrolls a horizontal-only section (plain
  // wheel mice have no horizontal axis) and vice versa.
  if (!has_v && has_h && dh == 0.0 && dv != 0.0) { dh = dv; dv = 0.0; }
  if (has_v && !has_h && dv == 0.0 && dh != 0.0) { dv = dh; dh = 0.0; }

  // Zero-delta events still matter when they carry gesture-phase edges
  // (phase_began cancels a bounce; phase_ended / momentum_ended drive the
  // spring-back + momentum-suppression bookkeeping).
  bool phase_signal = base.phase_began || base.phase_ended ||
                      base.momentum || base.momentum_ended;

  ScrollWheelAction act_v{}, act_h{};
  if (has_v && (dv != 0.0 || phase_signal)) {
    ScrollWheelInput in = base;
    in.delta_px = dv;
    act_v = section_scroll_wheel_kinetic(*st, *L, in, false);
  }
  if (has_h && (dh != 0.0 || phase_signal)) {
    ScrollWheelInput in = base;
    in.delta_px = dh;
    act_h = section_scroll_wheel_kinetic(*st, *L, in, true);
  }

  if (act_v.stop_bounce  || act_h.stop_bounce)  [self sectionStopBounce];
  if (act_v.changed      || act_h.changed)      [self setNeedsDisplay:YES];
  if (act_v.start_bounce || act_h.start_bounce) [self sectionStartBounce:secIdx];
}

- (void)scrollWheel:(NSEvent*)event
{
  if (!session) return;
  NSPoint p = [self localPointForEvent:event];
  float lx = (float)p.x;
  float ly = (float)p.y;

  // An open combo overlay consumes the wheel to scroll its list (win32
  // parity: checked before any widget dispatch). Line-delta conversion
  // matches the generic wheel path below.
  if (session->_open_combo) {
    CGFloat craw = event.scrollingDeltaY;
    int cd;
    if (event.hasPreciseScrollingDeltas) {
      cd = (int)(craw / 16.0);
      if (cd == 0 && craw != 0.0) cd = (craw > 0) ? 1 : -1;
    } else {
      cd = (int)craw;
      if (cd == 0 && craw != 0.0) cd = (craw > 0) ? 1 : -1;
    }
    if (cd != 0 && session->handle_combo_wheel(lx, ly, cd)) return;
  }

  uint32_t hit = session->widget_at(lx, ly, widget_index);
  if (hit == 0) return;
  auto* hw = session->get_widget(hit);
  if (!hw) return;

  // GRID: pixel-precise smooth scroll + inertial momentum + elastic
  // rubber-band, using the shared kinetics in grid_model.h (identical to the
  // native macOS host). All other widgets keep the line-delta wheel model.
  if (neui_detail::GridModel* model = hw->grid_model_ptr()) {
    using namespace neui_detail;
    auto cfg = grid_read_config(hw->attrs.get());
    GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                             cfg.row_h, cfg.header_h);

    // macOS default = SMOOTH; the attr can flip it to STEPPED for a Win32-like
    // coarse row scroll.
    if (!grid_smooth_enabled(cfg, /*platform_default_smooth=*/true)) {
      // Drop momentum events so a flick doesn't keep stepping after release.
      if (event.momentumPhase != NSEventPhaseNone) return;
      CGFloat raw = event.scrollingDeltaY;
      if (raw == 0) return;
      // Same accumulator math as the line-delta path below, but applied to
      // the grid model rather than dispatched as MOUSE_WHEEL.
      int delta;
      if (event.hasPreciseScrollingDeltas) {
        delta = (int)(raw / 16.0);
        if (delta == 0 && raw != 0.0) delta = (raw > 0) ? 1 : -1;
      } else {
        delta = (int)raw;
        if (delta == 0 && raw != 0.0) delta = (raw > 0) ? 1 : -1;
      }
      if (delta == 0) return;
      [self gridStopBounce];
      if (grid_scroll_step_rows(*model, vp, cfg.row_h, -delta))
        [self setNeedsDisplay:YES];
      return;
    }

    GridWheelInput in;
    in.precise        = event.hasPreciseScrollingDeltas;
    double dy         = (double)event.scrollingDeltaY;
    in.delta_px       = in.precise ? dy : dy * (double)(cfg.row_h > 0 ? cfg.row_h : 1);
    in.phase_began    = (event.phase == NSEventPhaseBegan);
    in.phase_changed  = (event.phase == NSEventPhaseChanged);
    in.phase_ended    = (event.phase == NSEventPhaseEnded) ||
                        (event.phase == NSEventPhaseCancelled);
    in.momentum       = (event.momentumPhase != NSEventPhaseNone);
    in.momentum_ended = (event.momentumPhase == NSEventPhaseEnded);

    GridWheelAction act = grid_scroll_wheel(*model, vp, cfg.row_h, in);
    if (act.stop_bounce)  [self gridStopBounce];
    if (act.changed)      [self setNeedsDisplay:YES];
    if (act.start_bounce) [self gridStartBounce:hit];
    return;
  }

  // Scrolling SECTION (the hit itself or its nearest ancestor): widgets
  // below the section get first refusal via a bounded bubble of the
  // classic line-delta event; when nothing below consumes, the section
  // eats the raw NSEvent through its per-axis kinetics (pixel-precise +
  // inertial momentum + elastic rubber-band - identical dynamics to GRID).
  uint32_t sec_idx = 0;
  if (hw->scroll_state_ptr()) {
    sec_idx = hit;
  } else {
    auto parents = session->_widgets.get_all_parents(hit);
    for (uint32_t pidx : parents) {
      auto* pw = session->get_widget(pidx);
      if (pw && pw->scroll_state_ptr()) { sec_idx = pidx; break; }
    }
  }

  // Win32 normalises wheel delta to lines (positive = scroll up). macOS:
  // event.scrollingDelta{X,Y} in lines if hasPreciseScrollingDeltas == NO,
  // otherwise pixels - convert to lines using ~16 pixels/line and clamp.
  //
  // Horizontal parity with the win32 host's WM_MOUSEHWHEEL path: when the
  // gesture is horizontally dominant (trackpad two-finger horizontal, or a
  // wheel mouse whose Shift+scroll AppKit already swapped onto deltaX) the
  // event dispatches with is_horizontal = 1. AppKit's sign convention
  // (positive = scroll-left, mirroring deltaY's positive = scroll-up)
  // already matches the win32 path's flipped delta - no sign adjustment.
  CGFloat raw_y = event.scrollingDeltaY;
  CGFloat raw_x = event.scrollingDeltaX;
  bool horizontal = fabs(raw_x) > fabs(raw_y);
  CGFloat raw = horizontal ? raw_x : raw_y;
  // Shift + vertical wheel = conventional horizontal-scroll fallback for
  // devices AppKit doesn't swap. Flip the sign so positive = scroll-left,
  // matching the win32 Shift+WM_MOUSEWHEEL branch.
  if (!horizontal && raw_x == 0.0 &&
      (event.modifierFlags & NSEventModifierFlagShift)) {
    horizontal = true;
    raw = -raw;
  }
  int delta;
  if (event.hasPreciseScrollingDeltas) {
    delta = (int)(raw / 16.0);
    if (delta == 0 && raw != 0.0) delta = (raw > 0) ? 1 : -1;
  } else {
    delta = (int)raw;
    if (delta == 0 && raw != 0.0) delta = (raw > 0) ? 1 : -1;
  }
  if (delta == 0) {
    // Zero-delta events still carry gesture-phase edges the SECTION
    // kinetics need (phase begin/end, momentum end).
    if (sec_idx != 0) [self sectionKineticWheel:event section:sec_idx];
    return;
  }

  neui_event_t ev = {};
  ev.type                     = NEUI_EVENT_MOUSE_WHEEL;
  ev.data.wheel.widget        = { hw->widget_id };
  ev.data.wheel.x             = (int)lx;
  ev.data.wheel.y             = (int)ly;
  ev.data.wheel.delta         = delta;
  ev.data.wheel.is_horizontal = horizontal ? 1 : 0;

  if (sec_idx != 0) {
    // Bounded bubble: only the widgets strictly below the section get the
    // line event (a MULTILINE / LISTBOX child keeps consuming its own
    // wheel); the section itself scrolls through its kinetics instead of
    // its line-delta on_mouse_event fallback.
    if (hit != sec_idx && session->dispatch_wheel_event(hit, &ev, sec_idx))
      return;
    [self sectionKineticWheel:event section:sec_idx];
    return;
  }

  // Wheel bubbles up to ancestors so a scrolling SECTION consumes the
  // wheel when the inner widget under the cursor doesn't.
  session->dispatch_wheel_event(hit, &ev);
}

// ---------------------------------------------------------------------------
// Keyboard.

- (void)keyDown:(NSEvent*)event
{
  if (!session) return;
  uint32_t mods = mac_modifiers_to_neui(event.modifierFlags);
  uint32_t keycode = mac_keycode_to_neui(event.keyCode);

  // Tab cycles logical focus inside our hand-rolled Tab traversal - same as
  // the win32 path. Consume here; the focus_next path doesn't fire KEYDOWN.
  if (keycode == NEUI_KEY_TAB) {
    session->focus_next(!(mods & NEUI_KMOD_SHIFT));
    return;
  }

  // KEYDOWN: client gets first chance via dispatch_event (for the focused
  // widget), then the focused widget's on_keydown virtual.
  if (keycode != 0) {
    uint32_t fw = session->_focused_widget;
    if (fw != 0 && session->_widgets.exists(fw)) {
      auto& wd = session->_widgets[fw];
      bool consumed = false;
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type     = NEUI_EVENT_KEYDOWN;
        ev.data.key = { { wd.widget_id }, keycode, mods };
        consumed = session->dispatch_event(&ev);
      }
      if (!consumed)
        session->handle_input_key(NEUI_EVENT_KEYDOWN, keycode, mods);
    }
  }

  // Character / IME routing. interpretKeyEvents: dispatches to:
  //   - insertText:replacementRange:                - plain typing OR IME commit
  //   - setMarkedText:selectedRange:replacementRange: - IME composition update
  //   - doCommandBySelector:                         - navigation keys (we no-op)
  //
  // Skip when Cmd is held: those keystrokes are command shortcuts, not text,
  // and the widget's on_keydown above already routed them.
  if (mods & NEUI_KMOD_CTRL) return;
  [self interpretKeyEvents:@[ event ]];
}

- (void)keyUp:(NSEvent*)event
{
  if (!session) return;
  uint32_t keycode = mac_keycode_to_neui(event.keyCode);
  if (keycode == 0) return;

  uint32_t mods = mac_modifiers_to_neui(event.modifierFlags);
  uint32_t fw = session->_focused_widget;
  if (fw == 0 || !session->_widgets.exists(fw)) return;
  auto& wd = session->_widgets[fw];
  if (!wd.emit_events) return;

  neui_event_t ev = {};
  ev.type     = NEUI_EVENT_KEYUP;
  ev.data.key = { { wd.widget_id }, keycode, mods };
  session->dispatch_event(&ev);
  // No on_keyup fallback - widgets with KEYUP semantics observe via the
  // public event path, mirroring the win32 host's dispatch_key_to_focused.
}

// AppKit beeps when an unhandled key reaches the responder chain. Override
// to swallow keys we already routed (Tab, control keys we handle in
// keyDown:) so the user doesn't hear a beep on every navigation key.
- (void)doCommandBySelector:(SEL)selector
{
  (void)selector;
  // Intentionally empty: we consume keys in keyDown:; nothing to do here.
}

// ---------------------------------------------------------------------------
// Focus. The view becomes first responder when its window becomes key -
// relay to Session::_os_focused so paint suppresses the focus-ring on
// background frames (matches WM_SETFOCUS / WM_KILLFOCUS on the frame HWND).

- (BOOL)becomeFirstResponder
{
  if (session) {
    session->_os_focused = true;
    if (session->_focused_widget != 0
     && session->_widgets.exists(session->_focused_widget)) {
      auto& wd = session->_widgets[session->_focused_widget];
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type                = NEUI_EVENT_WIDGET_FOCUS;
        ev.data.focus.widget   = { wd.widget_id };
        ev.data.focus.focused  = true;
        session->dispatch_event(&ev);
      }
    }
    [self setNeedsDisplay:YES];
  }
  return YES;
}

- (BOOL)resignFirstResponder
{
  if (session) {
    session->_os_focused = false;
    if (session->_focused_widget != 0
     && session->_widgets.exists(session->_focused_widget)) {
      auto& wd = session->_widgets[session->_focused_widget];
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type                = NEUI_EVENT_WIDGET_FOCUS;
        ev.data.focus.widget   = { wd.widget_id };
        ev.data.focus.focused  = false;
        session->dispatch_event(&ev);
      }
    }
    [self setNeedsDisplay:YES];
  }
  return YES;
}

// ---------------------------------------------------------------------------
// NSTextInputClient - IME composition. The xpl host's text widgets already
// expose the platform-agnostic on_composition(kind, utf8, byte_len, caret_byte,
// per_byte_attrs) seam. Win32 forwards WM_IME_* messages into it; here we
// forward NSTextInputClient calls.

// Helper: returns the focused widget if there is one, else nullptr.
- (xpl_host::WidgetData*)focusedWidget
{
  if (!session) return nullptr;
  uint32_t fw = session->_focused_widget;
  if (fw == 0 || !session->_widgets.exists(fw)) return nullptr;
  return &session->_widgets[fw];
}

// Convert a UTF-16 character offset within an NSString to a UTF-8 byte offset.
static int utf16_caret_to_utf8_bytes(NSString* s, NSUInteger u16_offset)
{
  if (!s || u16_offset == 0) return 0;
  if (u16_offset > s.length) u16_offset = s.length;
  NSString* prefix = [s substringWithRange:NSMakeRange(0, u16_offset)];
  return (int)[prefix lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

// insertText: receives committed text - either plain typing (no preceding
// composition) or the IME's commit string at the end of composition. Distinguish
// by _composing: when set, the call is a COMP_RESULT; when not, fire a normal
// KEYCHAR per codepoint.
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
  (void)replacementRange;
  if (!session) return;
  NSString* s = [string isKindOfClass:[NSAttributedString class]]
                  ? [(NSAttributedString*)string string]
                  : (NSString*)string;
  if (!s || s.length == 0) {
    if (_composing) {
      auto* wd = [self focusedWidget];
      if (wd) wd->on_composition(xpl_host::WidgetData::COMP_END,
                                  nullptr, 0, 0, nullptr);
      _composing       = NO;
      _marked_text_len = 0;
    }
    return;
  }

  if (_composing) {
    // IME commit. Single COMP_RESULT carries the entire committed string;
    // the widget pushes one undo entry against its pre-composition snapshot.
    const char* utf8 = [s UTF8String];
    int len = (int)[s lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    auto* wd = [self focusedWidget];
    if (wd) wd->on_composition(xpl_host::WidgetData::COMP_RESULT,
                                utf8, len, 0, nullptr);
    _composing       = NO;
    _marked_text_len = 0;
    return;
  }

  // Plain typing - fire one KEYCHAR per Unicode codepoint, with the same
  // dispatch_event -> on_keychar two-stage routing as keyDown:.
  uint32_t fw = session->_focused_widget;
  if (fw == 0 || !session->_widgets.exists(fw)) return;
  auto& wd = session->_widgets[fw];

  for (NSUInteger i = 0; i < s.length; ) {
    uint32_t cp = (uint32_t)[s characterAtIndex:i];
    NSUInteger consumed = 1;
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.length) {
      uint32_t lo = (uint32_t)[s characterAtIndex:i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
        consumed = 2;
      }
    }
    i += consumed;

    if (!is_printable_codepoint(cp)) continue;

    uint32_t mods = mac_modifiers_to_neui(NSEvent.modifierFlags);
    bool client_consumed = false;
    if (wd.emit_events) {
      neui_event_t ev = {};
      ev.type     = NEUI_EVENT_KEYCHAR;
      ev.data.key = { { wd.widget_id }, cp, mods };
      client_consumed = session->dispatch_event(&ev);
    }
    if (!client_consumed)
      session->handle_input_key(NEUI_EVENT_KEYCHAR, cp, mods);
  }
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange
{
  (void)replacementRange;
  if (!session) return;
  NSString* s = [string isKindOfClass:[NSAttributedString class]]
                  ? [(NSAttributedString*)string string]
                  : (NSString*)string;

  // Empty marked text -> composition cancellation. Same effect as unmarkText:
  // for our pipeline.
  if (!s || s.length == 0) {
    if (_composing) {
      auto* wd = [self focusedWidget];
      if (wd) wd->on_composition(xpl_host::WidgetData::COMP_END,
                                  nullptr, 0, 0, nullptr);
      _composing       = NO;
      _marked_text_len = 0;
    }
    return;
  }

  auto* wd = [self focusedWidget];
  if (!wd) return;

  if (!_composing) {
    // First setMarkedText: in this composition - snapshot pre-state.
    if (!wd->on_composition(xpl_host::WidgetData::COMP_START,
                             nullptr, 0, 0, nullptr)) {
      // Widget refused to enter composition (e.g. read-only). Bail; the
      // OS will retry, and a future text widget can pick it up.
      return;
    }
    _composing = YES;
  }

  // Caret byte offset within the marked string. AppKit reports the active
  // edit caret via selectedRange (NSMaxRange = caret position). Convert
  // UTF-16 offsets to UTF-8 bytes for the widget's contract.
  const char* utf8 = [s UTF8String];
  int byte_len = (int)[s lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
  int caret_byte = utf16_caret_to_utf8_bytes(s, NSMaxRange(selectedRange));

  // Per-clause attribute byte array left null in v1: NSAttributedString's
  // NSMarkedClauseSegment can be mapped to WidgetData::CompAttr_*, but the
  // widget paint already falls back to a single underline when attrs is
  // null. Multi-clause Japanese still composes correctly; only the
  // per-clause underline coloring is missing.
  wd->on_composition(xpl_host::WidgetData::COMP_UPDATE,
                      utf8, byte_len, caret_byte, nullptr);

  _marked_text_len = s.length;
}

- (void)unmarkText
{
  if (!session) return;
  if (_composing) {
    auto* wd = [self focusedWidget];
    if (wd) wd->on_composition(xpl_host::WidgetData::COMP_END,
                                nullptr, 0, 0, nullptr);
    _composing       = NO;
    _marked_text_len = 0;
  }
}

- (BOOL)hasMarkedText { return _composing; }

- (NSRange)markedRange
{
  if (!_composing) return NSMakeRange(NSNotFound, 0);
  return NSMakeRange(0, _marked_text_len);
}

- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actualRange
{
  (void)range;
  if (actualRange) *actualRange = NSMakeRange(NSNotFound, 0);
  return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
  (void)point;
  return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range
                          actualRange:(NSRangePointer)actualRange
{
  (void)range;
  if (actualRange) *actualRange = NSMakeRange(NSNotFound, 0);
  if (!session) return NSZeroRect;
  uint32_t fw = session->_focused_widget;
  if (fw == 0 || !session->_widgets.exists(fw)) return NSZeroRect;
  auto& fwd = session->_widgets[fw];

  auto* backend = xpl_host::platform_get_backend();
  if (!backend) return NSZeroRect;

  float cx = 0, cy = 0, ch = 0;
  if (!fwd.caret_rect_local(backend, fwd.render_ctx, &cx, &cy, &ch))
    return NSZeroRect;

  // caret_rect_local is in widget-local logical coords (top-left). Translate
  // by the widget's frame-local absolute origin (abs_x / abs_y, computed by
  // the paint walk - the widget's stored x/y is parent-relative) to get
  // view-local coords (still top-left since the view is isFlipped=YES).
  NSRect view_r = NSMakeRect((CGFloat)(cx + fwd.abs_x),
                              (CGFloat)(cy + fwd.abs_y),
                              1.0,
                              (CGFloat)ch);
  // convertRect:toView:nil un-flips into window coords (NSWindow uses
  // bottom-left origin). convertRectToScreen: is again bottom-left, screen.
  NSRect win_r    = [self convertRect:view_r toView:nil];
  NSRect screen_r = [self.window convertRectToScreen:win_r];
  return screen_r;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }

// ---------------------------------------------------------------------------
// NSDraggingDestination - external drag&drop routing. The NEUIView is the
// only NSDraggingDestination per frame; the framework hit-tests via
// Session::dispatch_dnd_* and the client receives NEUI_EVENT_DND_*.

// NSDragOperation <-> NEUI_DND_ACTION_* translation + the modifier-aware
// suggestion live in hosts/shared/macos/dnd_helpers_macos.h
// (dnd_suggested_from_nsop / dnd_nsop_from_action), shared with the
// native host's NEUINativeContentView.

- (NSPoint)neuiLocalPointFromDragInfo:(id<NSDraggingInfo>)sender
{
  NSPoint loc = [sender draggingLocation];
  // draggingLocation is in window coords (bottom-left origin); convert to
  // view-local. NEUIView is isFlipped=YES so the Y is already top-down.
  return [self convertPoint:loc fromView:nil];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
  if (!session) return NSDragOperationNone;
  NSPasteboard* pb = [sender draggingPasteboard];
  auto ml = neui_detail::pb_collect_mime_list_macos(pb);
  NSPoint p = [self neuiLocalPointFromDragInfo:sender];
  uint32_t suggested = neui_detail::dnd_suggested_from_nsop([sender draggingSourceOperationMask]);
  uint32_t accepted = session->dispatch_dnd_enter(widget_index,
                                                    (int)p.x, (int)p.y,
                                                    ml.ptrs.data(),
                                                    (uint32_t)ml.ptrs.size(),
                                                    suggested, 0);
  return neui_detail::dnd_nsop_from_action(accepted);
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
  if (!session) return NSDragOperationNone;
  NSPasteboard* pb = [sender draggingPasteboard];
  auto ml = neui_detail::pb_collect_mime_list_macos(pb);
  NSPoint p = [self neuiLocalPointFromDragInfo:sender];
  uint32_t suggested = neui_detail::dnd_suggested_from_nsop([sender draggingSourceOperationMask]);
  uint32_t accepted = session->dispatch_dnd_move(widget_index,
                                                   (int)p.x, (int)p.y,
                                                   ml.ptrs.data(),
                                                   (uint32_t)ml.ptrs.size(),
                                                   suggested, 0);
  return neui_detail::dnd_nsop_from_action(accepted);
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
  (void)sender;
  if (session) session->dispatch_dnd_leave();
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
  if (!session) return NO;
  NSPasteboard* pb = [sender draggingPasteboard];
  neui_detail::DataItem item;
  neui_detail::pb_read_item_macos(pb, item);
  auto ml = neui_detail::pb_collect_mime_list_macos(pb);
  NSPoint p = [self neuiLocalPointFromDragInfo:sender];
  uint32_t suggested = neui_detail::dnd_suggested_from_nsop([sender draggingSourceOperationMask]);
  uint32_t accepted = session->dispatch_dnd_drop(widget_index,
                                                   (int)p.x, (int)p.y,
                                                   ml.ptrs.data(),
                                                   (uint32_t)ml.ptrs.size(),
                                                   suggested, 0, &item);
  return accepted ? YES : NO;
}

@end

// ---------------------------------------------------------------------------
// Window delegate - close-button routing + quit-on-last-appwindow.

@interface NEUIWindowDelegate : NSObject<NSWindowDelegate>
{
@public
  xpl_host::Session* session;
  uint32_t           widget_index;
  bool               is_appwindow;  // counts toward g_appwindow_count
  bool               handled_close; // idempotency: close-button + programmatic
                                    // destroy both route through windowWillClose:.
  // For modal DIALOG with an owner: the owner window we'll present this
  // dialog as a sheet on. nil for non-sheet windows. Held weakly so a closed
  // owner doesn't keep this delegate alive.
  __weak NSWindow*   sheet_owner;
  bool               sheet_active; // beginSheet:'d -> must endSheet on close
}
@end

@implementation NEUIWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender
{
  (void)sender;
  if (!session) return YES;
  // Mirror the win32 host's WM_CLOSE -> APP_QUIT path: the client sees the
  // event first and may veto the close by returning false.
  neui_event_t ev = {};
  ev.type = NEUI_EVENT_APP_QUIT;
  bool allow = session->dispatch_event(&ev);
  return allow ? YES : NO;
}

- (void)windowWillClose:(NSNotification*)note
{
  (void)note;
  if (handled_close) return;
  handled_close = true;

  // Tear down the render context - the view's drawable is about to go away.
  // Mirrors the win32 WM_DESTROY path.
  if (session) {
    auto* wd = session->get_widget(widget_index);
    if (wd) {
      auto* backend = xpl_host::platform_get_backend();
      if (backend && wd->render_ctx) {
        session->_asset_manager.release_context(wd->render_ctx, backend);
        backend->destroy_context(wd->render_ctx);
        wd->render_ctx = nullptr;
      }
      // Drop the modal pump flag so widget_show unwinds and returns.
      if (auto* fw = dynamic_cast<xpl_host::FrameWidget*>(wd))
        fw->modal_pump_active = false;
    }
  }

  // The +1 retain on the NSWindow lives on wd->native_handle until
  // platform_destroy_window runs (either from widget_destroy on the
  // crash path, or from the session destructor on the normal path).
  // Releasing it inside this notification would dealloc the window
  // mid-close-sequence - classic AppKit footgun.
  if (is_appwindow) {
    if (--g_appwindow_count <= 0) {
      [NSApp stop:nil];
      wake_app_event_pump();
    }
  }
}

@end

// ---------------------------------------------------------------------------
// NEUIMenuTarget - singleton ObjC target for every NSMenuItem we create.
// neuiMenuPick: reads the item's tag (= cmd_id assigned by widgets.cpp),
// walks up the supermenu chain to the root NSMenu, looks up the owning
// menubar's widget_id from the associated object, then calls
// Session::dispatch_menu_event on the owning Session.

extern "C" {
extern char kNEUIMenubarWidgetIdAssoc;
}
char kNEUIMenubarWidgetIdAssoc = 0;

// host.cpp owns the per-process session registry inside namespace xpl_host;
// re-declare here so neuiMenuPick: can route by session_id without dragging
// the entire host.h declaration into this translation unit.
namespace xpl_host {
  extern std::vector<std::unique_ptr<Session>> sessions;
}

@interface NEUIMenuTarget : NSObject
+ (instancetype)shared;
- (void)neuiMenuPick:(id)sender;
@end

@implementation NEUIMenuTarget

+ (instancetype)shared
{
  static NEUIMenuTarget* inst = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ inst = [[NEUIMenuTarget alloc] init]; });
  return inst;
}

- (void)neuiMenuPick:(id)sender
{
  NSMenuItem* item = (NSMenuItem*)sender;
  uint32_t cmd_id = (uint32_t)item.tag;

  // Walk up to the topmost menu (which is the menubar's NSMenu).
  NSMenu* m = item.menu;
  while (m && m.supermenu) m = m.supermenu;
  if (!m) return;

  NSNumber* assoc = objc_getAssociatedObject(m, &kNEUIMenubarWidgetIdAssoc);
  if (!assoc) return;
  uint32_t mb_widget_id = (uint32_t)assoc.unsignedIntValue;

  uint32_t session_id = (mb_widget_id >> 16) & 0xffff;
  if (session_id == 0) return;
  size_t sess_idx = static_cast<size_t>(session_id) - 1;
  if (sess_idx >= xpl_host::sessions.size() || !xpl_host::sessions[sess_idx]) return;
  xpl_host::sessions[sess_idx]->dispatch_menu_event(cmd_id);
}

@end

// Menubar helpers (menu_title_only, find_item_with_tag, find_popup_item,
// neui_mods_to_appkit, key_to_keyEquivalent, install_app_menu) live in
// hosts/shared/macos/menubar_macos.h and are shared with the native macOS
// host.
using neui_detail::macos_menu_title_only;
using neui_detail::macos_find_item_with_tag;
using neui_detail::macos_find_popup_item;
using neui_detail::macos_neui_mods_to_appkit;
using neui_detail::macos_key_to_keyEquivalent;
using neui_detail::macos_install_app_menu;

// ---------------------------------------------------------------------------
// Helpers.

namespace {

// Geometry + style-mask bodies shared with the native macOS host
// (hosts/shared/macos/window_helpers_macos.h); thin local names kept so
// call sites stay terse.
NSRect logical_window_rect(int x, int y, int w, int h)
{ return neui_detail::logical_window_rect_macos(x, y, w, h); }

NSWindow* native_window(void* nh)
{
  return (__bridge NSWindow*)nh;
}

NSWindowStyleMask styles_for_appwindow()
{ return neui_detail::styles_for_appwindow_macos(); }

NSWindowStyleMask styles_for_dialog()
{ return neui_detail::styles_for_dialog_macos(); }

void install_view_and_context(xpl_host::Session* session,
                              uint32_t widget_index,
                              xpl_host::WidgetData& wd,
                              NSWindow* window,
                              bool is_appwindow)
{
  // Apply pre-show state - title, position, etc.
  if (!wd.text.empty())
    [window setTitle:[NSString stringWithUTF8String:wd.text.c_str()]];
  [window setReleasedWhenClosed:NO];
  // Without this, mouseMoved: only fires when a button is held. The view's
  // NSTrackingArea already gates by mouse-over, so the cost is negligible.
  [window setAcceptsMouseMovedEvents:YES];

  // Content view (also the render target).
  NEUIView* view = [[NEUIView alloc] initWithFrame:NSMakeRect(0, 0, wd.width, wd.height)];
  view->session      = session;
  view->widget_index = widget_index;
  [window setContentView:view];

  // Force the view to be the initial / current first responder. Without this
  // AppKit may leave the window itself as first responder (which doesn't
  // forward keyDown:), so keyboard input would never reach our view.
  [window setInitialFirstResponder:view];
  [window makeFirstResponder:view];

  // Window delegate routes close button + counts toward the quit threshold.
  NEUIWindowDelegate* delegate = [[NEUIWindowDelegate alloc] init];
  delegate->session      = session;
  delegate->widget_index = widget_index;
  delegate->is_appwindow = is_appwindow;
  [window setDelegate:delegate];
  // Keep the delegate alive for the window's lifetime. NSWindow does not
  // retain its delegate. Using objc_setAssociatedObject parks it on the
  // window so it dies with the window (and not before).
  objc_setAssociatedObject(window,
                           "NEUIWindowDelegate",
                           delegate,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);

  // Set up the render context with the view as the native handle.
  auto* backend = xpl_host::platform_get_backend();
  if (backend) {
    wd.render_ctx = backend->create_context((__bridge void*)view,
                                             (uint32_t)wd.width,
                                             (uint32_t)wd.height);
  }

  // Read back the actual backing scale (DPI source on macOS).
  CGFloat scale = window.backingScaleFactor;
  if (scale <= 0) scale = 1.0;
  wd.dpi = (uint32_t)(96.0 * scale + 0.5);

  // Hand back a +1 retain on the NSWindow as the native_handle. ARC will
  // release it via __bridge_transfer in platform_destroy_window or in the
  // delegate's windowWillClose: handler, whichever comes first.
  wd.native_handle = (__bridge_retained void*)window;

  // Apply pre-show attribute-driven state. Icon is process-wide on macOS so
  // applying once per frame is harmless (the last one wins, same as Win32).
  if (wd.attrs) {
    if (const char* icon_path = wd.attrs->get_string(NEUI_ATTR_ICON_PATH);
        icon_path && *icon_path)
    {
      xpl_host::platform_set_window_icon(wd, icon_path);
    }
    int min_w = wd.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
    int min_h = wd.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
    int max_w = wd.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
    int max_h = wd.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
    if (min_w || min_h || max_w || max_h) {
      xpl_host::platform_apply_size_constraints(wd.native_handle,
                                                 min_w, min_h, max_w, max_h);
    }
  }

  if (is_appwindow) ++g_appwindow_count;
}

} // namespace

// ---------------------------------------------------------------------------

namespace xpl_host
{
  // -------------------------------------------------------------------------
  // Lifecycle / window management

  void platform_init()
  {
    static bool initialised = false;
    if (initialised) return;
    initialised = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    // Seed the theme palette + install Appearance / Accent observers so the
    // rest of the host (Session::on_theme_changed via register_theme_listener)
    // picks up live changes for free.
    neui_detail::ensure_theme_provider_macos();
  }

  neui_render_backend_t* platform_get_backend()
  {
    return neui_cg_backend::get_backend();
  }

  void platform_create_appwindow(Session* session, uint32_t widget_index,
                                  WidgetData& wd)
  {
    NSRect frame_rect = logical_window_rect(wd.x, wd.y, wd.width, wd.height);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame_rect
                                                    styleMask:styles_for_appwindow()
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    install_view_and_context(session, widget_index, wd, window, /*is_appwindow*/true);
  }

  void platform_create_plugwindow(Session* session, uint32_t widget_index,
                                   WidgetData& wd)
  {
    NSRect frame_rect = logical_window_rect(wd.x, wd.y, wd.width, wd.height);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame_rect
                                                    styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    install_view_and_context(session, widget_index, wd, window, /*is_appwindow*/false);
  }

  void platform_create_dialog(Session* session, uint32_t widget_index,
                               WidgetData& wd, void* owner_native)
  {
    // Auto-centre over owner when the client did not supply a position.
    if (owner_native && wd.x == 0 && wd.y == 0) {
      NSWindow* owner = native_window(owner_native);
      NSRect or_rect  = owner.frame;
      CGFloat screen_h = NSScreen.mainScreen.frame.size.height;
      // Convert owner's bottom-left frame back to top-left logical coords.
      int or_x = (int)or_rect.origin.x;
      int or_y = (int)(screen_h - or_rect.origin.y - or_rect.size.height);
      wd.x = or_x + ((int)or_rect.size.width  - wd.width)  / 2;
      wd.y = or_y + ((int)or_rect.size.height - wd.height) / 2;
      if (wd.x < 0) wd.x = 0;
      if (wd.y < 0) wd.y = 0;
    }

    NSRect frame_rect = logical_window_rect(wd.x, wd.y, wd.width, wd.height);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame_rect
                                                    styleMask:styles_for_dialog()
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    install_view_and_context(session, widget_index, wd, window, /*is_appwindow*/false);

    // Modal dialog presents as a sheet on macOS - owner is automatically
    // input-blocked while the sheet is up. NEUI_ATTR_MODAL defaults to 1
    // when unset (per CLAUDE.md). Non-modal dialogs stay as floating
    // windows and the owner remains interactive.
    if (owner_native) {
      bool modal = true;  // default per the public attr contract
      if (wd.attrs && wd.attrs->has(NEUI_ATTR_MODAL))
        modal = wd.attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
      if (modal) {
        NEUIWindowDelegate* d =
          objc_getAssociatedObject(window, "NEUIWindowDelegate");
        if (d) d->sheet_owner = native_window(owner_native);
      }
    }
  }

  void platform_destroy_window(WidgetData& wd)
  {
    if (!wd.native_handle) return;
    // Release the +1 retain installed in install_view_and_context. ARC
    // releases the local NSWindow* when this scope ends; AppKit then
    // closes and frees it.
    NSWindow* w = (__bridge_transfer NSWindow*)wd.native_handle;
    wd.native_handle = nullptr;

    // Sheet-attached dialogs need an explicit endSheet: before close so the
    // owner is re-enabled and the slide-up animation kicks in cleanly.
    NEUIWindowDelegate* d = (NEUIWindowDelegate*)w.delegate;
    if (d && d->sheet_active && d->sheet_owner) {
      [d->sheet_owner endSheet:w];
      d->sheet_active = false;
    }
    [w close];
    // The view's render_ctx is freed by the delegate's windowWillClose:
    // (called from -close above), so we don't need to call destroy_context
    // here.
  }

  void platform_show_window(void* native_handle)
  {
    if (!native_handle) return;
    NSWindow* w = native_window(native_handle);
    NEUIWindowDelegate* d = (NEUIWindowDelegate*)w.delegate;
    if (d && d->sheet_owner && !d->sheet_active) {
      // Present as a sheet on the owner - AppKit auto-blocks the owner.
      [d->sheet_owner beginSheet:w completionHandler:^(NSModalResponse /*r*/){}];
      d->sheet_active = true;
    } else {
      [w makeKeyAndOrderFront:nil];
    }
  }

  void platform_hide_window(void* native_handle)
  {
    if (!native_handle) return;
    NSWindow* w = native_window(native_handle);
    [w orderOut:nil];
  }

  void platform_set_window_enabled(void* /*native_handle*/, bool /*enabled*/)
  {
    // True modal blocking comes online with the menubar / sheet work in
    // steps 7 & 10. NSWindow has no setEnabled:; that work uses
    // [NSApp beginSheet:] for modals.
  }

  void platform_activate_window(void* native_handle)
  {
    if (!native_handle) return;
    NSWindow* w = native_window(native_handle);
    [NSApp activateIgnoringOtherApps:YES];
    [w makeKeyAndOrderFront:nil];
  }

  void platform_set_window_title(void* native_handle, const char* text)
  {
    if (!native_handle) return;
    NSWindow* w = native_window(native_handle);
    [w setTitle:[NSString stringWithUTF8String:text ? text : ""]];
  }

  void platform_set_window_pos(void* native_handle,
                                int x, int y, int w, int h, uint32_t /*dpi*/)
  {
    if (!native_handle) return;
    NSWindow* win = native_window(native_handle);
    [win setFrame:logical_window_rect(x, y, w, h) display:YES];
  }

  void platform_post_close(void* native_handle)
  {
    if (!native_handle) return;
    NSWindow* w = native_window(native_handle);
    [w performClose:nil];
  }

  float platform_get_scale_factor(void* native_handle)
  {
    if (!native_handle) return 1.0f;
    NSWindow* w = native_window(native_handle);
    CGFloat s = w.backingScaleFactor;
    return s > 0 ? (float)s : 1.0f;
  }

  void platform_invalidate(void* native_handle)
  {
    if (!native_handle) return;
    NSWindow* w = native_window(native_handle);
    [w.contentView setNeedsDisplay:YES];
  }

  bool platform_run()
  {
    [NSApp run];
    return true;
  }

  bool platform_pump_once()
  {
    NSEvent* ev = nil;
    while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                    untilDate:[NSDate distantPast]
                                       inMode:NSDefaultRunLoopMode
                                      dequeue:YES])) {
      [NSApp sendEvent:ev];
    }
    return true;
  }

  bool platform_run_modal_until(bool* keep_running)
  {
    neui_detail::run_modal_pump_macos(keep_running);
    return true;
  }

  // -------------------------------------------------------------------------
  // Native menu bar (NSMenu / NSMenuItem). The macOS menu bar is process-global
  // (NSApp.mainMenu); per-frame menubars all funnel into it. NSMenuItem
  // activations route through NEUIMenuTarget.neuiMenuPick: -> the matching
  // Session::dispatch_menu_event.

  void* platform_menubar_create(uint32_t menubar_widget_id)
  {
    NSMenu* m = [[NSMenu alloc] init];
    [m setAutoenablesItems:NO];  // explicit enable/disable; matches Win32 path
    // Stash the menubar's widget_id on the NSMenu so neuiMenuPick: can route
    // back to the correct Session.
    objc_setAssociatedObject(m,
                              &kNEUIMenubarWidgetIdAssoc,
                              @(menubar_widget_id),
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return (__bridge_retained void*)m;
  }

  void platform_menubar_destroy(void* hmenu)
  {
    if (!hmenu) return;
    NSMenu* m = (__bridge_transfer NSMenu*)hmenu;
    (void)m;  // ARC releases on scope exit; submenus + items follow.
  }

  void platform_menubar_attach(void* /*frame*/, void* hmenu)
  {
    // Per the plan: NSApp.mainMenu is the single global menu bar; the most
    // recently-attached menubar wins. Add the App menu first so Cmd+Q always
    // works regardless of what menus the client builds.
    if (!hmenu) return;
    NSMenu* m = (__bridge NSMenu*)hmenu;
    macos_install_app_menu(m);
    [NSApp setMainMenu:m];
  }

  void* platform_menubar_add_popup(void* hmenu, const char* display_text)
  {
    if (!hmenu) return nullptr;
    NSMenu* parent = (__bridge NSMenu*)hmenu;
    NSString* title = macos_menu_title_only(display_text);

    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                   action:nil
                                            keyEquivalent:@""];
    NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
    [sub setAutoenablesItems:NO];
    item.submenu = sub;
    [parent addItem:item];
    // Ownership: the parent's NSMenuItem.submenu retains `sub`; the void*
    // we hand back is non-owning and matches the lifetime of that item.
    return (__bridge void*)sub;
  }

  void platform_menubar_add_item(void* parent_hmenu, uint32_t cmd_id,
                                  const char* display_text)
  {
    if (!parent_hmenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSString* title = macos_menu_title_only(display_text);

    NSMenuItem* item = [[NSMenuItem alloc]
      initWithTitle:title
             action:@selector(neuiMenuPick:)
      keyEquivalent:@""];
    item.target  = [NEUIMenuTarget shared];
    item.tag     = (NSInteger)cmd_id;
    item.enabled = YES;
    [parent addItem:item];
  }

  void platform_menubar_add_separator(void* parent_hmenu, uint32_t cmd_id)
  {
    if (!parent_hmenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenuItem* sep = [NSMenuItem separatorItem];
    sep.tag = (NSInteger)cmd_id;
    [parent addItem:sep];
  }

  void platform_menubar_remove_popup(void* parent_hmenu, void* submenu)
  {
    if (!parent_hmenu || !submenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenu* sub    = (__bridge NSMenu*)submenu;
    NSMenuItem* it = macos_find_popup_item(parent, sub);
    if (it) [parent removeItem:it];
  }

  void platform_menubar_remove_item(void* parent_hmenu, uint32_t cmd_id)
  {
    if (!parent_hmenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenuItem* it = macos_find_item_with_tag(parent, cmd_id);
    if (it) [parent removeItem:it];
  }

  void platform_menubar_enable_item(void* parent_hmenu, uint32_t cmd_id, bool enabled)
  {
    if (!parent_hmenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenuItem* it = macos_find_item_with_tag(parent, cmd_id);
    if (it) it.enabled = enabled ? YES : NO;
  }

  void platform_menubar_enable_popup(void* parent_hmenu, void* submenu, bool enabled)
  {
    if (!parent_hmenu || !submenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenu* sub    = (__bridge NSMenu*)submenu;
    NSMenuItem* it = macos_find_popup_item(parent, sub);
    if (it) it.enabled = enabled ? YES : NO;
  }

  void platform_menubar_set_item_text(void* parent_hmenu, uint32_t cmd_id,
                                       const char* display_text)
  {
    if (!parent_hmenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenuItem* it = macos_find_item_with_tag(parent, cmd_id);
    if (!it) return;
    // Strip "\tShortcut" suffix - keyEquivalent is set separately via
    // platform_menubar_set_item_shortcut.
    it.title = macos_menu_title_only(display_text);
  }

  void platform_menubar_set_item_shortcut(void* parent_hmenu, uint32_t cmd_id,
                                           uint32_t modifiers, uint32_t key)
  {
    if (!parent_hmenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenuItem* it = macos_find_item_with_tag(parent, cmd_id);
    if (!it) return;
    if (key == NEUI_KEY_NONE) {
      it.keyEquivalent             = @"";
      it.keyEquivalentModifierMask = 0;
      return;
    }
    it.keyEquivalent             = macos_key_to_keyEquivalent(key);
    it.keyEquivalentModifierMask = macos_neui_mods_to_appkit(modifiers);
  }

  void platform_menubar_refresh(void* /*frame*/) {}

  // -------------------------------------------------------------------------
  // Polish (step 10).

  void platform_set_window_icon(WidgetData& /*wd*/, const char* path_utf8)
  {
    if (!path_utf8 || !*path_utf8) return;
    NSString* ns_path = [NSString stringWithUTF8String:path_utf8];
    if (!ns_path) return;
    NSImage* img = [[NSImage alloc] initWithContentsOfFile:ns_path];
    if (!img) return;
    // macOS has no per-window icon - this updates the Dock icon for the
    // process. ARC handles NSImage lifetime so wd.native_icon stays unused.
    [NSApp setApplicationIconImage:img];
  }

  void platform_apply_size_constraints(void* native_handle,
                                        int min_w, int min_h,
                                        int max_w, int max_h)
  {
    if (!native_handle) return;
    NSWindow* w = native_window(native_handle);
    NSSize min_sz = NSMakeSize(min_w > 0 ? min_w : 0,
                                min_h > 0 ? min_h : 0);
    NSSize max_sz = NSMakeSize(max_w > 0 ? max_w : FLT_MAX,
                                max_h > 0 ? max_h : FLT_MAX);
    [w setContentMinSize:min_sz];
    [w setContentMaxSize:max_sz];
  }

  // -------------------------------------------------------------------------
  // Image loading. Delegates to the shared loader
  // (hosts/shared/macos/image_loader_macos.h) - same ImageIO decode +
  // BGRA8-premul normalisation, plus the bundle-Resources fallback for
  // relative paths the native macOS host already gets. Without the
  // fallback, app-bundle launches (cwd = / via Finder / `open`) fail to
  // resolve the example's bundled images on the xpl host while the native
  // host loads them fine. Allocation stays new[] (freed by
  // platform_free_image's delete[], matching free_image_bgra8).

  uint8_t* platform_load_image(const char* path,
                                uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_macos(path, width_out, height_out);
  }

  void platform_free_image(uint8_t* pixels) { delete[] pixels; }

  // -------------------------------------------------------------------------
  // System clipboard. Delegates to hosts/shared/macos/clipboard_macos.h -
  // same shape as the Win32 path delegates to clipboard_win32.h.

  bool platform_clipboard_set_text(const char* utf8, uint32_t length)
  {
    return neui_detail::clipboard_set_text_macos(utf8, length);
  }

  int platform_clipboard_get_text(char* buf, int buflen)
  {
    return neui_detail::clipboard_get_text_macos(buf, buflen);
  }

  bool platform_clipboard_has_text()
  {
    return neui_detail::clipboard_has_text_macos();
  }

  bool platform_clipboard_write_item(const neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_write_item_macos(item);
  }

  bool platform_clipboard_read_item(neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_read_item_macos(item);
  }

  // -------------------------------------------------------------------------
  // Drag & drop. The content NEUIView for each frame is the
  // NSDraggingDestination; registerForDraggedTypes lets AppKit start
  // routing drags to its protocol callbacks. We register a broad set
  // of types; the framework hit-tests + format-matches against the
  // drop_target widget's accepted_mimes list before firing events.

  bool platform_dnd_register_window(void* native_handle, void* /*session_ptr*/,
                                     uint32_t /*frame_widget_id*/)
  {
    if (!native_handle) return false;
    NSWindow* win = (__bridge NSWindow*)native_handle;
    NSView* cv = [win contentView];
    if (![cv isKindOfClass:[NEUIView class]]) return false;
    [cv registerForDraggedTypes:@[
      NSPasteboardTypeString,
      NSPasteboardTypeHTML,
      NSPasteboardTypeFileURL,
    ]];
    return true;
  }

  void platform_dnd_unregister_window(void* native_handle)
  {
    if (!native_handle) return;
    NSWindow* win = (__bridge NSWindow*)native_handle;
    NSView* cv = [win contentView];
    if ([cv isKindOfClass:[NEUIView class]])
      [cv unregisterDraggedTypes];
  }

  void* platform_make_drag_preview(const uint8_t* bgra_premul,
                                     uint32_t w_px, uint32_t h_px,
                                     float scale)
  {
    NSImage* img = neui_detail::macos_make_drag_nsimage(bgra_premul, w_px,
                                                          h_px, scale);
    // Retained transfer: caller owns until handed to platform_dnd_begin_drag.
    return (__bridge_retained void*)img;
  }

  uint32_t platform_dnd_begin_drag(void* native_handle,
                                    neui_detail::DataItem* item,
                                    uint32_t allowed_actions,
                                    void* preview_native,
                                    int hot_x, int hot_y)
  {
    // Take ownership of the bridged NSImage* immediately so every exit
    // path releases it (the platform.h contract hands it off
    // unconditionally). Pass nil if no preview.
    NSImage* preview = (__bridge_transfer NSImage*)preview_native;
    if (!native_handle || !item) return 0;
    NSWindow* win = (__bridge NSWindow*)native_handle;
    NSView* cv = [win contentView];
    if (!cv) return 0;
    return neui_detail::macos_run_drag_source(cv, *item, allowed_actions,
                                                preview, hot_x, hot_y);
  }

  // -------------------------------------------------------------------------
  // Mouse cursor.

  void platform_set_cursor(int kind)
  {
    switch (kind) {
      case NEUI_CURSOR_EW_RESIZE: [[NSCursor resizeLeftRightCursor] set]; break;
      case NEUI_CURSOR_DEFAULT:
      default:                    [[NSCursor arrowCursor] set];          break;
    }
  }

} // namespace xpl_host
