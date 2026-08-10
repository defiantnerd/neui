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

#include <unordered_map>
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
#include "../shared/macos/message_box_macos.h"

// ---------------------------------------------------------------------------
// Forward declarations (Objective-C classes).

@class NEUIView;
@class NEUIWindowDelegate;

namespace xpl_host { class Session; class WidgetData; }

// Cursor state, defined with the rest of the cursor code far below but needed
// up here by NEUIView's -cursorUpdate:.
namespace xpl_host {
  extern int  g_cursor_kind;
  void        mac_apply_cursor(int kind);
}

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
  // Toast animation heartbeat. 60 Hz tick that just invalidates the view;
  // paint_toast self-terminates when the toast lifetime expires.
  NSTimer*           _toast_timer;
  // Last LOGICAL size reported through NEUI_EVENT_RESIZE. Seeded at view
  // creation so the create-time sizing is not reported, and compared (rather
  // than the view's previous frame) so a pure ZOOM change - which moves the
  // native frame while the logical size is unchanged - fires no RESIZE, while a
  // real resize at any zoom still does. Comparing against wd.width/height would
  // NOT work: widget_set_size updates those BEFORE calling the platform layer,
  // so every programmatic resize would suppress itself.
  int                _reported_w;
  int                _reported_h;
  // Set while platform_set_window_pos is applying OUR OWN geometry. Such a
  // resize must neither report a RESIZE (the client asked for it, and
  // wd.width/height are already authoritative) nor recompute the logical size
  // by dividing the native size back out: `logical -> native -> logical` is
  // lossy at fractional zoom. 402 px at zoom 0.75 rounds to 302 native, which
  // divides back to 403 - a silent 1 px growth per call plus a spurious RESIZE.
  BOOL               _self_resizing;
}
- (void)toastStart;
- (void)toastStop;
- (void)seedReportedSize:(int)w height:(int)h;
- (void)beginSelfResize:(int)w height:(int)h;
- (void)endSelfResize;
@end

@implementation NEUIView

- (void)seedReportedSize:(int)w height:(int)h
{
  _reported_w = w;
  _reported_h = h;
}

- (void)beginSelfResize:(int)w height:(int)h
{
  // Record the INTENDED logical size, not whatever the native round-trip
  // produces, so the next externally-driven resize compares against the truth.
  _self_resizing = YES;
  _reported_w = w;
  _reported_h = h;
}

- (void)endSelfResize { _self_resizing = NO; }

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

// A DAW-embedded frame is inserted into the host's window, which neui never
// created - so the two window-level facts install_view_and_context sets up
// for standalone frames have to be (re)established whenever we land in a
// window. Harmless for standalone frames (idempotent, same values).
- (void)viewDidMoveToWindow
{
  [super viewDidMoveToWindow];
  NSWindow* w = self.window;
  if (!w) return;
  // Without this, mouseMoved: only fires while a button is held, so hover
  // states and cursor changes are dead inside a DAW. The NSTrackingArea
  // gates by mouse-over; this flag is what makes the events flow at all.
  [w setAcceptsMouseMovedEvents:YES];
  // Re-read the backing scale: an embedded view is normally created BEFORE
  // insertion, so the create-time read had no window to ask and fell back
  // to the main screen. Getting this wrong strands the frame at the wrong
  // scale for its lifetime on a mixed-DPI setup.
  if (!session) return;
  xpl_host::WidgetData* wd = session->get_widget(widget_index);
  if (!wd) return;
  CGFloat scale = w.backingScaleFactor;
  if (scale <= 0) scale = 1.0;
  uint32_t dpi = (uint32_t)(96.0 * scale + 0.5);
  if (dpi != wd->dpi) {
    wd->dpi = dpi;
    [self setNeedsDisplay:YES];
  }
}

// Frame resize -> NEUI_EVENT_RESIZE with the new client size in logical px.
// Hooked on the VIEW rather than the window delegate on purpose: NEUIView is
// the content view of a standalone frame (AppKit resizes it with the window)
// AND the root subview of a DAW-embedded PLUGWINDOW, where there is no
// NSWindow of ours to get a windowDidResize: from. One hook covers both.
// Mirrors the win32 xpl WM_SIZE path and the macOS-native windowDidResize:.
//
// The view is isFlipped with a backing-scale CTM, so its size is already in
// logical points (= logical px at 96 DPI). Programmatic resizes report too,
// matching win32 (SetWindowPos also raises WM_SIZE).
- (void)setFrameSize:(NSSize)newSize
{
  const NSSize old = self.frame.size;
  [super setFrameSize:newSize];

  // session is nil until install_view_and_context / the embed path assigns it,
  // so the initWithFrame: sizing never reaches a client.
  if (!session) return;
  // Our own geometry application - beginSelfResize already recorded the
  // intended logical size, and wd.width/height are authoritative.
  if (_self_resizing) return;

  // Divide out the frame zoom. The view's frame is in ZOOMED points (that is
  // what platform_set_window_pos sets it to), but wd.width/height and every
  // client-facing size are LOGICAL px at 96 DPI by design - the zoom exists
  // only in the paint transform and the platform conversions. Storing the
  // zoomed value here would make get_client_rect report 800x480 at zoom 2.0
  // for a 400x240 frame, and a reset to 1.0 would not restore the original.
  const float z = [self frameZoom];
  const int w_log = (int)((float)newSize.width  / (z > 0.0f ? z : 1.0f) + 0.5f);
  const int h_log = (int)((float)newSize.height / (z > 0.0f ? z : 1.0f) + 0.5f);
  if (w_log == _reported_w && h_log == _reported_h) return;
  _reported_w = w_log;
  _reported_h = h_log;
  (void)old;

  xpl_host::WidgetData* wd = session->get_widget(widget_index);
  if (!wd) return;
  wd->width  = w_log;
  wd->height = h_log;

  // Report the height BELOW any in-frame menubar band so client layout matches
  // the other hosts. 0 on macOS (the menubar is the system menu bar), but
  // routing through the accessor keeps the three xpl platforms identical.
  const int inset = session->frame_top_inset(widget_index);

  neui_event_t ev = {};
  ev.type               = NEUI_EVENT_RESIZE;
  ev.data.resize.widget = { wd->widget_id };
  ev.data.resize.width  = w_log;
  ev.data.resize.height = h_log - inset;
  session->dispatch_event(&ev);
}

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
    // Makes AppKit send -cursorUpdate: to this view, which is what lets
    // NEUI_ATTR_CURSOR stick: AppKit resets the cursor from cursor rects on
    // every mouse-moved, so a cursor merely `set` from platform_set_cursor is
    // reverted before it is seen. Without this option -cursorUpdate: is never
    // called at all (it is opt-in per tracking area, not a free NSView hook).
    | NSTrackingCursorUpdate
    | NSTrackingActiveInKeyWindow
    | NSTrackingInVisibleRect;
  _tracking_area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                 options:opts
                                                   owner:self
                                                userInfo:nil];
  [self addTrackingArea:_tracking_area];
}

// AppKit's "you own the cursor here" hook, paired with NSTrackingCursorUpdate
// above. Reapplies whatever Session::refresh_cursor last resolved rather than
// letting AppKit fall back to the arrow.
//
// Deliberately does NOT call super: NSView's implementation applies the view's
// cursor rects, which is precisely the reset being overridden.
- (void)cursorUpdate:(NSEvent*)event
{
  (void)event;
  xpl_host::mac_apply_cursor(xpl_host::g_cursor_kind);
}

// The frame's user zoom (NEUI_ATTR_UI_SCALE), 1.0 when unset. AppKit points
// already equal neui logical px, so the zoom is the ONLY conversion factor on
// this platform - unlike win32/Linux there is no DPI ratio to fold in (the
// backing scale is handled inside the CGContext's own CTM).
- (float)frameZoom
{
  if (!session) return 1.0f;
  xpl_host::WidgetData* wd = session->get_widget(widget_index);
  return wd ? wd->ui_scale() : 1.0f;
}

// Convert an NSEvent's cursor location into top-left-origin view-local logical
// pixels (matches the renderer.h coordinate convention). Divides out the frame
// zoom so input arrives in the same logical space the widget tree, the cached
// abs_x/abs_y and the paint walk use. Every mouse handler on this view funnels
// through here, so this is the single conversion point.
- (NSPoint)localPointForEvent:(NSEvent*)event
{
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  float z = [self frameZoom];
  if (z != 1.0f && z > 0.0f) { p.x /= z; p.y /= z; }
  return p;
}

- (void)dispatchMouseEventForType:(neui_event_type_t)type
                            event:(NSEvent*)event
{
  if (!session) return;

  // Relative (unbounded) pointer mode: the cursor is decoupled from the device
  // (CGAssociateMouseAndMouseCursorPosition(false)), so localPointForEvent: is
  // frozen at the anchor and useless. Consume the raw delta instead and let
  // Session report an accumulated virtual position.
  //
  // NSEvent's mouse deltaY is in DEVICE orientation (positive = downward), which
  // already matches the y-down widget space this view uses - no flip, unlike the
  // scroll-wheel deltas elsewhere in this file.
  //
  // Only MOVE is redirected: a button DOWN / UP still carries a meaningful
  // position (the anchor), and end_relative_pointer runs off the UP.
  if (type == NEUI_EVENT_MOUSE_MOVE && session->is_relative_pointer()) {
    // Divide by the frame zoom, exactly as localPointForEvent: does for absolute
    // positions: dispatch_relative_motion's contract is LOGICAL px. Without this
    // a drag's sensitivity jumped by the zoom factor the instant relative mode
    // began, and macOS disagreed with win32 (logical_to_physical) and Linux
    // (window_scale), both of which already divide the delta.
    const float z = [self frameZoom];
    const float s = (z > 0.0f) ? z : 1.0f;
    session->dispatch_relative_motion((float)event.deltaX / s,
                                        (float)event.deltaY / s,
                                        mac_buttonmap(NSEvent.pressedMouseButtons,
                                                       event.modifierFlags));
    return;
  }

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
  // Pull keyboard focus on click. Standalone frames get this at install time
  // (setInitialFirstResponder), but a DAW-embedded view shares the host's
  // window with the DAW's own views, so the click must claim first-responder
  // for keyboard input to route here. No-op when already first responder.
  if (self.window && self.window.firstResponder != self)
    [self.window makeFirstResponder:self];
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
  // Standalone tree popup (widgets->popup_tree_menu) does the same: it is modal
  // over the frame while open, so it either picks / descends or dismisses.
  if (session->_tree_popup_active) {
    session->handle_tree_popup_click(widget_index, (float)p.x, (float)p.y);
    return;
  }
  // Toast overlay absorbs a click that lands on its rect, jumping it
  // to the fade-out phase.
  if (session->handle_toast_click(widget_index, (float)p.x, (float)p.y)) return;
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
  if (session->_tree_popup_active) {
    session->handle_tree_popup_hover(widget_index, (float)p.x, (float)p.y);
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
  // Report the modifier bits (the buttons are already released by the time an
  // UP arrives, which is exactly what pressedMouseButtons reflects). win32 xpl
  // populates the same events; leaving these at 0 made Shift/Ctrl invisible to
  // a client deciding what a click meant.
  ev.data.mouse.buttonmap = mac_buttonmap(NSEvent.pressedMouseButtons,
                                           event.modifierFlags);

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
  if (session->_tree_popup_active) {
    NSPoint p = [self localPointForEvent:event];
    session->handle_tree_popup_click(widget_index, (float)p.x, (float)p.y);
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
  ev.data.mouse.buttonmap = mac_buttonmap(NSEvent.pressedMouseButtons,
                                           event.modifierFlags);
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
  hw->notify_scroll_changed();
  if (!more_v && !more_h) [self sectionStopBounce];
}

// --- Toast animation heartbeat --------------------------------------------

- (void)toastStop
{
  if (_toast_timer) { [_toast_timer invalidate]; _toast_timer = nil; }
}

- (void)toastStart
{
  [self toastStop];
  _toast_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                          target:self
                                        selector:@selector(toastTick:)
                                        userInfo:nil
                                         repeats:YES];
  [[NSRunLoop currentRunLoop] addTimer:_toast_timer
                               forMode:NSRunLoopCommonModes];
}

- (void)toastTick:(NSTimer*)timer
{
  (void)timer;
  [self setNeedsDisplay:YES];
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
  // Asymmetric single-axis fallback. A horizontal-only section absorbs a
  // pure vertical wheel because classic wheel mice have no horizontal axis
  // and the user otherwise has no way to scroll it (matches the macOS
  // Finder gallery view). A vertical-only section does NOT absorb pure
  // horizontal input - tilt wheels, trackpad two-finger left/right, and
  // Shift+wheel all represent an explicit horizontal-scroll intent that
  // should simply be ignored when the axis isn't supported, not silently
  // re-aimed at the vertical axis.
  if (!has_v && has_h && dh == 0.0 && dv != 0.0) { dh = dv; dv = 0.0; }

  // NEUI_ATTR_SCROLL_KINETICS opt-in (macOS default = SMOOTH).
  int  kin_mode = section_read_kinetics_mode(sw->attrs.get());
  bool smooth   = scroll_kinetics_smooth_enabled(kin_mode,
                                                  /*platform_default_smooth=*/true);
  if (!smooth) {
    if (event.momentumPhase != NSEventPhaseNone) return;
    bool changed = false;
    if (has_v && dv != 0.0 && section_scroll_step_px(*st, *L, dv, false))
      changed = true;
    if (has_h && dh != 0.0 && section_scroll_step_px(*st, *L, dh, true))
      changed = true;
    if (changed) {
      [self sectionStopBounce];
      [self setNeedsDisplay:YES];
      sw->notify_scroll_changed();
    }
    return;
  }

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
  if (act_v.changed      || act_h.changed) {
    [self setNeedsDisplay:YES];
    sw->notify_scroll_changed();
  }
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
  // Same NEUI_MK_* bits as the mouse path, from the same shared helper.
  ev.data.wheel.buttonmap     = mac_buttonmap(NSEvent.pressedMouseButtons,
                                               event.modifierFlags);

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
  // Multiply back up by the zoom: the caret is painted through the zoom
  // transform, so its logical position has to be re-scaled into the view's
  // point space or the candidate window detaches from the text at any
  // zoom != 100%. (Inverse of the divide in localPointForEvent:.)
  const CGFloat z = (CGFloat)[self frameZoom];
  NSRect view_r = NSMakeRect((CGFloat)(cx + fwd.abs_x) * z,
                              (CGFloat)(cy + fwd.abs_y) * z,
                              1.0 * z,
                              (CGFloat)ch * z);
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
  NSPoint p = [self convertPoint:loc fromView:nil];
  // Same zoom divide as localPointForEvent: - drop hit-testing walks the same
  // logical widget rects as mouse hit-testing, so it needs the same space.
  float z = [self frameZoom];
  if (z != 1.0f && z > 0.0f) { p.x /= z; p.y /= z; }
  return p;
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

// A frame's native_handle is an NSWindow* for standalone frames and an
// NEUIView* for a DAW-embedded PLUGWINDOW (see platform_set_embed_parent).
// These three accessors are the only place that distinction lives: window-
// level operations must check is_embedded_view (or take the nil window)
// rather than assume the handle owns its NSWindow - for an embedded frame
// native_window() returns the DAW's window, which neui must never retitle,
// move, or close.
bool is_embedded_view(void* nh)
{
  return nh && ![(__bridge id)nh isKindOfClass:[NSWindow class]];
}

NSWindow* native_window(void* nh)
{
  if (!nh) return nil;
  id obj = (__bridge id)nh;
  if ([obj isKindOfClass:[NSWindow class]]) return (NSWindow*)obj;
  return [(NSView*)obj window];   // the DAW's window; nil before attach
}

NSView* frame_content_view(void* nh)
{
  if (!nh) return nil;
  id obj = (__bridge id)nh;
  if ([obj isKindOfClass:[NSWindow class]]) return [(NSWindow*)obj contentView];
  return (NSView*)obj;
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
  // The content view fills the (already zoomed) window content rect; the zoom
  // multiplies here because wd.width/height stay LOGICAL at every zoom.
  const CGFloat vz = (CGFloat)wd.ui_scale();
  NEUIView* view = [[NEUIView alloc]
    initWithFrame:NSMakeRect(0, 0, wd.width * vz, wd.height * vz)];
  view->session      = session;
  view->widget_index = widget_index;
  // Seed the RESIZE baseline so create-time sizing is not reported.
  [view seedReportedSize:wd.width height:wd.height];
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
                                             (uint32_t)(wd.width  * vz),
                                             (uint32_t)(wd.height * vz));
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
    // Zoom scales the window's content rect; wd.width/height stay logical.
    const float wz = wd.ui_scale();
    NSRect frame_rect = logical_window_rect(wd.x, wd.y,
                                            (int)(wd.width  * wz + 0.5f),
                                            (int)(wd.height * wz + 0.5f));
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame_rect
                                                    styleMask:styles_for_appwindow()
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    install_view_and_context(session, widget_index, wd, window, /*is_appwindow*/true);
  }

  void platform_create_plugwindow(Session* session, uint32_t widget_index,
                                   WidgetData& wd)
  {
    // wd.embed_parent (set via platform_set_embed_parent) selects the
    // DAW-embedded path: the frame roots directly in an NEUIView added as a
    // subview of the DAW-provided parent NSView - no NSWindow of our own.
    // The DAW's main runloop then drives drawRect: / input / NSTimer
    // animations for free; neui owns no loop in embedded mode. The view is
    // created hidden and revealed by platform_show_window, matching the
    // standalone create-then-show contract.
    if (wd.embed_parent) {
      NSView* parent = (__bridge NSView*)reinterpret_cast<void*>(wd.embed_parent);
      // Zoom scales the view's footprint inside the DAW's parent; a plugin
      // adapter that also negotiates a new size with the host should set both.
      const CGFloat ez = (CGFloat)wd.ui_scale();
      NEUIView* view = [[NEUIView alloc]
        initWithFrame:NSMakeRect(wd.x, wd.y, wd.width * ez, wd.height * ez)];
      view->session      = session;
      view->widget_index = widget_index;
      // Seed the RESIZE baseline so create-time sizing is not reported.
      [view seedReportedSize:wd.width height:wd.height];
      view.hidden = YES;
      [parent addSubview:view];

      auto* backend = platform_get_backend();
      if (backend) {
        wd.render_ctx = backend->create_context((__bridge void*)view,
                                                 (uint32_t)(wd.width  * ez),
                                                 (uint32_t)(wd.height * ez));
      }

      CGFloat scale = parent.window ? parent.window.backingScaleFactor
                                    : NSScreen.mainScreen.backingScaleFactor;
      if (scale <= 0) scale = 1.0;
      wd.dpi = (uint32_t)(96.0 * scale + 0.5);

      // +1 retain on the NEUIView as the native_handle; released via
      // __bridge_transfer in platform_destroy_window (there is no window
      // delegate on this path, so destroy also owns the render-ctx teardown).
      wd.native_handle = (__bridge_retained void*)view;
      return;
    }

    // Zoom scales the window's content rect; wd.width/height stay logical.
    const float wz = wd.ui_scale();
    NSRect frame_rect = logical_window_rect(wd.x, wd.y,
                                            (int)(wd.width  * wz + 0.5f),
                                            (int)(wd.height * wz + 0.5f));
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

    // Zoom scales the window's content rect; wd.width/height stay logical.
    const float wz = wd.ui_scale();
    NSRect frame_rect = logical_window_rect(wd.x, wd.y,
                                            (int)(wd.width  * wz + 0.5f),
                                            (int)(wd.height * wz + 0.5f));
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

    // Embedded PLUGWINDOW: no NSWindow, no delegate - release the render ctx
    // here (standalone frames do it in the delegate's windowWillClose:) and
    // detach the view from the DAW's hierarchy.
    if (is_embedded_view(wd.native_handle)) {
      NSView* v = (__bridge_transfer NSView*)wd.native_handle;
      wd.native_handle = nullptr;
      auto* backend = platform_get_backend();
      if (backend && wd.render_ctx) {
        if (wd.session)
          wd.session->_asset_manager.release_context(wd.render_ctx, backend);
        backend->destroy_context(wd.render_ctx);
        wd.render_ctx = nullptr;
      }
      [v removeFromSuperview];
      return;
    }

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
    if (is_embedded_view(native_handle)) {
      [frame_content_view(native_handle) setHidden:NO];
      return;
    }
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
    if (is_embedded_view(native_handle)) {
      [frame_content_view(native_handle) setHidden:YES];
      return;
    }
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
    // A DAW-embedded frame never activates the process or reorders the
    // host's windows - focus ownership stays with the DAW.
    if (is_embedded_view(native_handle)) return;
    NSWindow* w = native_window(native_handle);
    [NSApp activateIgnoringOtherApps:YES];
    [w makeKeyAndOrderFront:nil];
  }

  void platform_set_window_title(void* native_handle, const char* text)
  {
    if (!native_handle) return;
    // Embedded: the enclosing NSWindow is the DAW's - never retitle it.
    if (is_embedded_view(native_handle)) return;
    NSWindow* w = native_window(native_handle);
    [w setTitle:[NSString stringWithUTF8String:text ? text : ""]];
  }

  void platform_set_window_pos(void* native_handle,
                                int x, int y, int w, int h, uint32_t /*dpi*/)
  {
    if (!native_handle) return;
    // w/h are the logical CLIENT size; the frame's user zoom scales it (the
    // client keeps thinking in logical units at any zoom).
    float z = 1.0f;
    if (NSView* cv = frame_content_view(native_handle)) {
      if ([cv isKindOfClass:[NEUIView class]]) {
        NEUIView* nv = (NEUIView*)cv;
        if (nv->session) {
          if (auto* wd = nv->session->get_widget(nv->widget_index))
            z = wd->ui_scale();
        }
      }
    }
    const int zw = (int)((float)w * z + 0.5f);
    const int zh = (int)((float)h * z + 0.5f);

    // Bracket the geometry application so -setFrameSize: knows this resize is
    // ours: it must not re-derive the logical size from the native one (lossy
    // at fractional zoom) nor report a RESIZE the client just asked for.
    NEUIView* nv = nil;
    if (NSView* cv = frame_content_view(native_handle))
      if ([cv isKindOfClass:[NEUIView class]]) nv = (NEUIView*)cv;
    [nv beginSelfResize:w height:h];

    if (is_embedded_view(native_handle)) {
      // (x, y) is parent-view-relative in the parent's coordinate system;
      // drawRect: reads bounds each frame so the render follows the resize.
      [frame_content_view(native_handle) setFrame:NSMakeRect(x, y, zw, zh)];
      [nv endSelfResize];
      return;
    }
    NSWindow* win = native_window(native_handle);
    // (zw, zh) is the CLIENT area at the current zoom - the same contract
    // create() uses (it passes this rect to initWithContentRect:) and the same
    // one win32 maintains via AdjustWindowRectExForDpi. Sizing the OUTER frame
    // to it instead let the title bar eat into the client, so a
    // set_size(520, 300) produced a 520x268 client and every subsequent layout
    // was short by the chrome. frameRectForContentRect: preserves the content
    // rect's screen origin and grows the frame upward, so the position
    // semantics match create() too.
    NSRect content = logical_window_rect(x, y, zw, zh);
    if (x == NEUI_WINDOW_POS_KEEP || y == NEUI_WINDOW_POS_KEEP) {
      // Size-only: keep the window's current top-left on screen. In AppKit's
      // Y-up frame space the TOP edge is origin.y + height, so preserve that
      // rather than the origin, or the window would creep as it grows.
      NSRect cur = [win frame];
      NSRect want = [win frameRectForContentRect:content];
      want.origin.x = cur.origin.x;
      want.origin.y = cur.origin.y + cur.size.height - want.size.height;
      [win setFrame:want display:YES];
      [nv endSelfResize];
      return;
    }
    [win setFrame:[win frameRectForContentRect:content] display:YES];
    // Cleared unconditionally after the call, not inside the hook: setFrame:
    // does not invoke -setFrameSize: when the size is unchanged, and a leaked
    // flag would swallow the next genuine user resize.
    [nv endSelfResize];
  }

  void platform_post_close(void* native_handle)
  {
    if (!native_handle) return;
    // Embedded: closing is the adapter's job (widget_destroy); performClose
    // here would close the DAW's window.
    if (is_embedded_view(native_handle)) return;
    NSWindow* w = native_window(native_handle);
    [w performClose:nil];
  }

  float platform_get_scale_factor(void* native_handle)
  {
    if (!native_handle) return 1.0f;
    // For an embedded view native_window() is the DAW's window - exactly the
    // backing scale we want; falls back to 1.0 before attach.
    NSWindow* w = native_window(native_handle);
    CGFloat s = w.backingScaleFactor;
    return s > 0 ? (float)s : 1.0f;
  }

  void platform_invalidate(void* native_handle)
  {
    if (!native_handle) return;
    [frame_content_view(native_handle) setNeedsDisplay:YES];
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

  // ---- DAW-embedding seams. -------------------------------------------------
  // On macOS an embedded PLUGWINDOW is an NEUIView subview inside the DAW's
  // window, so the DAW's main runloop already drives drawRect: / input /
  // NSTimer animations - there is no dedicated connection to poll and
  // nothing to tick. The seams exist so a plugin adapter can drive one
  // platform-uniform loop across Win32 / macOS / Linux.

  void platform_set_embed_parent(Session* session, uint32_t widget_index,
                                 void* native_parent)
  {
    if (!session) return;
    auto* wd = session->get_widget(widget_index);
    if (wd) wd->embed_parent = reinterpret_cast<uintptr_t>(native_parent);
  }

  int platform_embed_event_fd(void* /*native_handle*/) { return -1; }

  void platform_embed_pump_and_tick(void* /*native_handle*/) {}

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

  bool platform_menubar_in_frame() { return false; }   // macOS uses the global NSMenu
  int  platform_frame_extra_top_inset(void* /*nh*/, bool /*has_menubar*/) { return 0; }

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

  void platform_menubar_check_item(void* parent_hmenu, uint32_t cmd_id, bool checked)
  {
    if (!parent_hmenu) return;
    NSMenu* parent = (__bridge NSMenu*)parent_hmenu;
    NSMenuItem* it = macos_find_item_with_tag(parent, cmd_id);
    if (it) it.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
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
    // Embedded: the size is the DAW's to negotiate; never constrain the
    // host's window.
    if (is_embedded_view(native_handle)) return;
    NSWindow* w = native_window(native_handle);
    // The constraints are LOGICAL, but setContentMinSize:/MaxSize: bound the
    // window in POINTS - which the zoom has already scaled. Without the factor
    // a MIN_WIDTH of 400 at zoom 2 lets the user drag down to logical 200.
    float z = 1.0f;
    if (NSView* cv = frame_content_view(native_handle))
      if ([cv isKindOfClass:[NEUIView class]]) {
        NEUIView* nv = (NEUIView*)cv;
        if (nv->session)
          if (auto* wd = nv->session->get_widget(nv->widget_index)) z = wd->ui_scale();
      }
    if (!(z > 0.0f)) z = 1.0f;
    NSSize min_sz = NSMakeSize(min_w > 0 ? min_w * z : 0,
                                min_h > 0 ? min_h * z : 0);
    NSSize max_sz = NSMakeSize(max_w > 0 ? max_w * z : FLT_MAX,
                                max_h > 0 ? max_h * z : FLT_MAX);
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

  // No PRIMARY selection on macOS.
  void platform_clipboard_set_primary(const char* /*utf8*/, uint32_t /*length*/) {}
  int  platform_clipboard_get_primary(char* /*buf*/, int /*buflen*/) { return 0; }

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
    NSView* cv = frame_content_view(native_handle);
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
    NSView* cv = frame_content_view(native_handle);
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
    NSView* cv = frame_content_view(native_handle);
    if (!cv) return 0;
    return neui_detail::macos_run_drag_source(cv, *item, allowed_actions,
                                                preview, hot_x, hot_y);
  }

  // -------------------------------------------------------------------------
  // Mouse cursor.
  //
  // AppKit owns the cursor: it resets it from the view's cursor rects on every
  // mouse-moved, so a bare `[cursor set]` here does not stick. The active kind
  // is tracked in g_cursor_kind and reapplied from NEUIView's -cursorUpdate:,
  // which is the documented hook for "this view decides its own cursor".
  //
  // AppKit is also missing shapes that win32 and X11 both have, though less so
  // than it used to be: macOS 15 added public +frameResizeCursorFromPosition:
  // inDirections: (the diagonals) and +columnResizeCursor / +rowResizeCursor.
  // Those are preferred where they apply (mac_public_cursor). What still has no
  // public equivalent at all is move, wait, progress and help; each tries a
  // private class method - reached via +respondsToSelector:, so a macOS that
  // drops one degrades instead of throwing - and otherwise falls back to the
  // closest public shape.
  //
  // Note wait/progress cannot be fixed by a better selector: macOS has no
  // app-settable busy cursor by design. The beachball is the window server's
  // response to an app that stops answering events, so a Mac client should draw
  // progress rather than set a cursor. Listed in docs/deferred-issues.md.
  //
  // +resizeLeftRightCursor / +resizeUpDownCursor (used for EW / NS below) are
  // marked API_TO_BE_DEPRECATED in the macOS 15 SDK in favour of
  // +columnResizeCursor / +rowResizeCursor. They still work and still look
  // right, so they stay for now - swapping them would change the existing GRID
  // column-divider appearance - but that migration is queued in
  // docs/deferred-issues.md.

  int g_cursor_kind = NEUI_CURSOR_DEFAULT;

  // True while we hold a -hide. [NSCursor hide]/unhide is a BALANCED COUNTER,
  // not a flag: hiding twice needs unhiding twice, and one stray extra unhide
  // leaves the pointer permanently visible in a relative-pointer drag. So the
  // transition is tracked explicitly and only ever toggled on a change.
  bool g_cursor_hidden = false;

  // macOS 15 finally added PUBLIC cursors for shapes AppKit had never exposed:
  // +frameResizeCursorFromPosition:inDirections: (the diagonals) plus
  // +columnResizeCursor / +rowResizeCursor. Prefer those when both the SDK
  // declares them and the running OS has them; older systems fall through to
  // the guarded private selectors below.
  //
  // Returns nil when there is no public answer for `kind`, so the caller keeps
  // its existing fallback chain.
  static NSCursor* mac_public_cursor(int kind)
  {
  #if defined(MAC_OS_VERSION_15_0) && \
      MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
    if (@available(macOS 15.0, *)) {
      switch (kind) {
        // NSCursorFrameResizeDirectionsAll = the double-headed arrow, which is what a resize
        // affordance wants; Inward/Outward are the single-headed variants for a
        // frame that can only grow or only shrink on that edge.
        case NEUI_CURSOR_NESW_RESIZE:
          return [NSCursor
            frameResizeCursorFromPosition:NSCursorFrameResizePositionTopRight
                             inDirections:NSCursorFrameResizeDirectionsAll];
        case NEUI_CURSOR_NWSE_RESIZE:
          return [NSCursor
            frameResizeCursorFromPosition:NSCursorFrameResizePositionTopLeft
                             inDirections:NSCursorFrameResizeDirectionsAll];
        default: break;
      }
    }
  #else
    (void)kind;
  #endif
    return nil;
  }

  // A private +[NSCursor foo] shape, or nil when this macOS doesn't have it.
  static NSCursor* mac_private_cursor(const char* selname)
  {
    SEL sel = NSSelectorFromString([NSString stringWithUTF8String:selname]);
    if (!sel || ![NSCursor respondsToSelector:sel]) return nil;
    // -performSelector: on a class object returning an id: the cursor is
    // autoreleased and owned by AppKit, exactly like the public accessors.
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    id c = [NSCursor performSelector:sel];
    #pragma clang diagnostic pop
    return [c isKindOfClass:[NSCursor class]] ? (NSCursor*)c : nil;
  }

  // The NSCursor for a kind, or nil for NEUI_CURSOR_NONE (hide is a mode, not
  // a shape - see cursor_win32.h for the same split).
  NSCursor* mac_cursor_for_kind(int kind)
  {
    switch (kind) {
      case NEUI_CURSOR_IBEAM:       return [NSCursor IBeamCursor];
      case NEUI_CURSOR_CROSSHAIR:   return [NSCursor crosshairCursor];
      case NEUI_CURSOR_HAND:        return [NSCursor pointingHandCursor];
      case NEUI_CURSOR_OPEN_HAND:   return [NSCursor openHandCursor];
      case NEUI_CURSOR_CLOSED_HAND: return [NSCursor closedHandCursor];
      case NEUI_CURSOR_EW_RESIZE:   return [NSCursor resizeLeftRightCursor];
      case NEUI_CURSOR_NS_RESIZE:   return [NSCursor resizeUpDownCursor];

      // Diagonals: public since macOS 15, private selector before that, and a
      // same-axis double-arrow as the last resort - on a corner grip that still
      // reads as "resize", where an arrow reads as "nothing here".
      case NEUI_CURSOR_NESW_RESIZE:
      case NEUI_CURSOR_NWSE_RESIZE: {
        if (NSCursor* pub = mac_public_cursor(kind)) return pub;
        NSCursor* c = mac_private_cursor(
          kind == NEUI_CURSOR_NESW_RESIZE
            ? "_windowResizeNorthEastSouthWestCursor"
            : "_windowResizeNorthWestSouthEastCursor");
        return c ? c : [NSCursor resizeUpDownCursor];
      }

      // No public move cursor; the open hand is the idiomatic Mac stand-in
      // (Preview / Finder use it for exactly this).
      case NEUI_CURSOR_MOVE: {
        NSCursor* c = mac_private_cursor("_moveCursor");
        return c ? c : [NSCursor openHandCursor];
      }

      // macOS has no app-controlled busy cursor at all: the spinning beachball
      // is the WINDOW SERVER's response to an app that stops answering events,
      // and an app cannot ask for it. So WAIT / PROGRESS degrade to the arrow
      // unless the private shape exists. A client that wants visible progress
      // on the Mac should draw it, not set a cursor.
      case NEUI_CURSOR_WAIT:
      case NEUI_CURSOR_PROGRESS: {
        NSCursor* c = mac_private_cursor("busyButClickableCursor");
        return c ? c : [NSCursor arrowCursor];
      }
      case NEUI_CURSOR_HELP: {
        NSCursor* c = mac_private_cursor("_helpCursor");
        return c ? c : [NSCursor arrowCursor];
      }

      case NEUI_CURSOR_NOT_ALLOWED: return [NSCursor operationNotAllowedCursor];
      case NEUI_CURSOR_NONE:        return nil;   // hide
      case NEUI_CURSOR_ARROW:
      case NEUI_CURSOR_DEFAULT:
      default:                      return [NSCursor arrowCursor];
    }
  }

  // Apply the tracked kind. Called from platform_set_cursor and again from
  // -cursorUpdate: every time AppKit would otherwise reset us.
  void mac_apply_cursor(int kind)
  {
    const bool want_hidden = neui_detail::cursor_kind_is_hidden(kind);
    if (want_hidden != g_cursor_hidden) {
      if (want_hidden) [NSCursor hide]; else [NSCursor unhide];
      g_cursor_hidden = want_hidden;
    }
    if (want_hidden) return;   // nothing to shape while hidden
    if (NSCursor* c = mac_cursor_for_kind(kind)) [c set];
  }

  void platform_set_cursor(int kind)
  {
    g_cursor_kind = kind;
    mac_apply_cursor(kind);
  }

  // -------------------------------------------------------------------------
  // Relative (unbounded) pointer mode.
  //
  // macOS has the cleanest primitive of the three platforms:
  // CGAssociateMouseAndMouseCursorPosition(false) stops the cursor tracking the
  // device while NSEvent keeps delivering deltaX/deltaY. So there is no
  // per-move warp and no synthetic motion event to filter out - unlike win32
  // and X11, which must warp back on every move.
  //
  // The hide is done directly rather than through platform_set_cursor, so it is
  // independent of NEUI_ATTR_CURSOR: a drag must not clobber the widget's
  // configured shape, and on end the widget's own cursor has to come back. Kept
  // balanced with an explicit flag for the same reason g_cursor_hidden exists -
  // hide/unhide is a counter, and an unbalanced hide costs the process its
  // pointer for good.

  static bool g_relative_hidden = false;

  bool platform_supports_relative_pointer() { return true; }

  bool platform_begin_relative_pointer(void* native_handle,
                                        int* out_anchor_x, int* out_anchor_y)
  {
    (void)native_handle;
    // Anchor in Quartz GLOBAL DISPLAY coordinates (y DOWN from the top-left of
    // the main display) because that is what CGWarpMouseCursorPosition consumes
    // on the way back. NSEvent.mouseLocation is y-UP in Cocoa screen space, so
    // it is flipped here once rather than at the (easier to forget) warp site.
    const NSPoint cocoa = [NSEvent mouseLocation];
    const CGFloat screen_h = NSMaxY([[NSScreen screens] firstObject].frame);
    if (out_anchor_x) *out_anchor_x = (int)(cocoa.x + 0.5);
    if (out_anchor_y) *out_anchor_y = (int)(screen_h - cocoa.y + 0.5);

    // Decouple cursor from device. Deltas keep arriving; the cursor stops.
    CGAssociateMouseAndMouseCursorPosition(false);
    if (!g_relative_hidden) { [NSCursor hide]; g_relative_hidden = true; }
    return true;
  }

  void platform_end_relative_pointer(void* native_handle,
                                      int anchor_x, int anchor_y)
  {
    (void)native_handle;
    // Warp FIRST, then re-associate. Both orders put the cursor back, because a
    // warp moves it even while decoupled - but CGWarpMouseCursorPosition starts a
    // local-events suppression interval (~250 ms by default) during which real
    // mouse input is dropped. Re-associating after the warp is the standard way
    // to avoid that: the association call ends the decoupled state and the
    // suppression does not apply to the freshly re-associated device, so the
    // pointer is live again immediately instead of feeling dead for a quarter
    // second after every knob drag.
    CGWarpMouseCursorPosition(CGPointMake((CGFloat)anchor_x, (CGFloat)anchor_y));
    CGAssociateMouseAndMouseCursorPosition(true);
    if (g_relative_hidden) { [NSCursor unhide]; g_relative_hidden = false; }
    // Re-apply the widget's own cursor: the hide above bypassed
    // platform_set_cursor, so g_cursor_kind is still whatever the widget asked
    // for and simply needs re-asserting now that the pointer is visible again.
    mac_apply_cursor(g_cursor_kind);
  }

  // -------------------------------------------------------------------------
  // Toast animation heartbeat. The toast paints inside the frame's
  // NEUIView so we just kick that view's per-frame timer to drive the
  // animation.

  void platform_start_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    NSView* cv = frame_content_view(native_handle);
    if ([cv isKindOfClass:[NEUIView class]]) {
      [(NEUIView*)cv toastStart];
    }
  }

  void platform_stop_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    NSView* cv = frame_content_view(native_handle);
    if ([cv isKindOfClass:[NEUIView class]]) {
      [(NEUIView*)cv toastStop];
    }
  }

  // Client timers (NEUI_API_TIMER). Session-scoped, so this hangs off the main
  // runloop rather than off a view - unlike the toast heartbeat above, there is
  // no one frame that owns it.
  //
  // NSRunLoopCommonModes (not the default mode) so the tick keeps running while
  // AppKit is in a modal / event-tracking loop - i.e. an animation driven by a
  // client timer does not freeze while the user holds a mouse button down. In a
  // DAW the host's own runloop services this exactly the same way, which is what
  // makes timers work under NEUI_API_EMBED with no loop of our own.
  static std::unordered_map<Session*, NSTimer*>& mac_session_timers()
  {
    // Deliberately IMMORTAL (leaked), not a plain function-local static. The
    // global `sessions` vector in host.cpp outlives this translation unit's
    // statics: it is constructed first and so destroyed last, and ~Session
    // calls platform_timer_stop(). A destructible map here is therefore read
    // AFTER it has been destroyed during static teardown - a SEGV at exit for
    // any client that uses a timer and never calls destroy(session), which is
    // exactly what CLAUDE.md's own canonical usage does.
    static auto* m = new std::unordered_map<Session*, NSTimer*>();
    return *m;
  }

  // macOS: localPointForEvent divides by frameZoom.
  bool platform_supports_ui_scale() { return true; }

  void platform_timer_start(Session* session, uint32_t interval_ms)
  {
    if (!session || interval_ms == 0) return;
    platform_timer_stop(session);   // idempotent re-arm at the new interval
    NSTimer* t = [NSTimer timerWithTimeInterval:(double)interval_ms / 1000.0
                                        repeats:YES
                                          block:^(NSTimer*) {
      // Re-check membership: a session torn down between fires would otherwise
      // be a dangling pointer, since the block captures the raw Session*.
      auto& m = mac_session_timers();
      if (m.find(session) == m.end()) return;
      session->tick_client_timers();
    }];
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    mac_session_timers()[session] = t;
  }

  void platform_timer_stop(Session* session)
  {
    if (!session) return;
    auto& m = mac_session_timers();
    auto it = m.find(session);
    if (it == m.end()) return;
    [it->second invalidate];
    m.erase(it);
  }

  uint64_t platform_now_ms()
  {
    // mach_absolute_time + NSProcessInfo systemUptime both work; pick the
    // simplest portable option: CFAbsoluteTimeGetCurrent returns seconds
    // since 2001, granular enough for animation.
    return static_cast<uint64_t>(CFAbsoluteTimeGetCurrent() * 1000.0);
  }

  // -------------------------------------------------------------------------
  // Modal message box - shared NSAlert mapping.

  int platform_message_box(void* native_handle, const char* text,
                           const char* caption, uint32_t flags)
  {
    if (!native_handle) return 0;
    // For an embedded frame this resolves to the DAW's window - the right
    // sheet parent; nil (view not yet attached) means nothing to anchor to.
    NSWindow* win = native_window(native_handle);
    if (!win) return 0;
    return neui_detail::message_box_macos(win, text, caption, flags);
  }

} // namespace xpl_host
