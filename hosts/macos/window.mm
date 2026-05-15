// Native macOS host - NSApp lifecycle, NSWindow plumbing, NEUINativeWindowDelegate,
// NEUINativeControlTarget (target-action sink), NEUINativeContentView (flipped
// container), and Session::widget_show dispatch.
//
// Step 4 of plans/native-macos-host.md adds LABEL + BUTTON. Per-step
// extension below the switch in widget_show.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "host.h"
#include "checkbox_image.h"
#include "../shared/macos/image_loader_macos.h"
#include "../shared/macos/theme_provider_macos.h"
#include "../shared/widget_paint_knob.h"
#include "../../backends/cg/cg_backend.h"

#include <cstring>

@class NEUINativeWindowDelegate;
@class NEUINativeControlTarget;
@class NEUINativeContentView;
@class NEUINativeTextDelegate;
@class NEUINativeListSource;
@class NEUINativeOutlineSource;
@class NEUINativeMenuTarget;
@class NEUINativePaintedView;

// Forward declaration of the widget-by-id lookup (defined further down
// inside namespace macos_host) - NEUINativePaintedView's @implementation
// references it before the definition appears.
namespace macos_host {
  WidgetData* widget_for_id(uint32_t widget_id, Session** out_session = nullptr);
}

// ---------------------------------------------------------------------------
// Module-private state.

namespace {

// Live APPWINDOW count. Hits 0 → [NSApp stop:nil] + post wake-up.
int g_appwindow_count = 0;

bool g_nsapp_initialised = false;

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

// Listener that re-resolves SF Symbol images on every checkbox after the
// user toggles system Appearance or Accent Color. NSImageSymbolConfiguration
// captures the resolved color at config time, so the cached NSImage doesn't
// auto-update; we have to rebuild it.
void refresh_all_checkbox_images(void* /*token*/)
{
  using namespace macos_host;
  for (auto& sptr : sessions) {
    if (!sptr) continue;
    auto& tree = sptr->_widgets;
    auto order = tree.release_order();
    for (uint32_t idx : order) {
      if (!tree.exists(idx)) continue;
      auto& wd = tree[idx];
      if (!wd.type || !wd.native_control) continue;
      bool is_checkbox = !std::strcmp(wd.type, NEUI_W_CHECKBOX)
                      || !std::strcmp(wd.type, NEUI_W_CHECKBOX3);
      if (!is_checkbox) continue;
      NSView* v = (__bridge NSView*)wd.native_control;
      if (![v isKindOfClass:[NSButton class]]) continue;
      int state = wd.attrs ? wd.attrs->get_int("neui.macoshost.checkstate",
                                                 NEUI_CHECK_UNCHECKED)
                           : NEUI_CHECK_UNCHECKED;
      ((NSButton*)v).image = checkbox_image_for_state(state);
    }
  }
}

void ensure_nsapp_initialised()
{
  if (g_nsapp_initialised) return;
  g_nsapp_initialised = true;
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  neui_detail::ensure_theme_provider_macos();
  neui_detail::register_theme_listener(&refresh_all_checkbox_images, nullptr);
}

NSRect logical_window_rect(int x, int y, int w, int h)
{
  CGFloat screen_h = NSScreen.mainScreen.frame.size.height;
  if (w <= 0) w = 1;
  if (h <= 0) h = 1;
  return NSMakeRect(x, screen_h - y - h, w, h);
}

NSWindow* native_window_from(void* nh) { return (__bridge NSWindow*)nh; }
NSView*   native_view_from  (void* nh) { return (__bridge NSView*)nh; }

NSWindowStyleMask styles_for_appwindow()
{
  return NSWindowStyleMaskTitled
       | NSWindowStyleMaskClosable
       | NSWindowStyleMaskMiniaturizable
       | NSWindowStyleMaskResizable;
}

} // namespace

// ---------------------------------------------------------------------------
// NEUINativeContentView - flipped container so child widgets' frames use
// top-left logical coordinates (matches wd.x / wd.y / wd.width / wd.height).

@interface NEUINativeContentView : NSView
@end

@implementation NEUINativeContentView
- (BOOL)isFlipped { return YES; }
@end

// ---------------------------------------------------------------------------
// NEUINativePaintedView - host for self-painted widgets (IMAGE, KNOB, future
// painted controls). Each instance owns its own CGContextState handle from
// neui_cg_backend::create_context. drawRect: calls set_current_frame +
// begin_frame, dispatches per-type paint, then end_frame.

@interface NEUINativePaintedView : NSView
{
@public
  uint32_t          widget_id;
  neui_render_ctx_t render_ctx;
  void*             bitmap_handle;       // CGBitmapHandle* from cg_backend
  uint32_t          bitmap_w_px;
  uint32_t          bitmap_h_px;
  float             bitmap_scale;
  // KNOB drag state. Mirror of the xpl host's KnobWidget drag fields:
  // dragging gates the move handler; drag_prev_angle is the last
  // pointer-relative-to-center angle; drag_continuous is an unsnapped
  // accumulator so step-snapped values still respond to small deltas.
  bool              dragging;
  float             drag_prev_angle;
  float             drag_continuous;
}
@end

// KNOB drag tunables - mirror of the xpl host's KnobWidget constants
// (hosts/crossplatform/host.cpp around the KnobWidget block).
static constexpr float NEUI_KNOB_SWEEP_RAD   = 4.71238898f;  // 1.5*PI (270°)
static constexpr float NEUI_KNOB_DEAD_ZONE_R = 4.0f;          // logical px
static constexpr float NEUI_KNOB_FINE_SCALE  = 0.2f;          // Shift = 1/5

static float neui_knob_wrap_pi(float d)
{
  const float PI    = 3.14159265358979323846f;
  const float TWOPI = 2.0f * PI;
  while (d >  PI) d -= TWOPI;
  while (d < -PI) d += TWOPI;
  return d;
}

static float neui_clamp01(float v)
{
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

// Snap to evenly-spaced positions on [0..1] when steps >= 2. Matches the
// xpl host's snap_to_steps so the native + xpl knobs feel identical.
static float neui_snap_to_steps(float v, int steps)
{
  if (steps < 2) return v;
  float n = (float)(steps - 1);
  return std::round(v * n) / n;
}

@implementation NEUINativePaintedView

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque  { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }

- (void)dealloc
{
  auto* backend = neui_cg_backend::get_backend();
  if (backend) {
    if (bitmap_handle && render_ctx)
      backend->destroy_bitmap(render_ctx, bitmap_handle);
    if (render_ctx)
      backend->destroy_context(render_ctx);
  }
  bitmap_handle = nullptr;
  render_ctx    = nullptr;
}

// Load (or reload) the IMAGE bitmap from the widget's text path. Lazy on
// first paint; rebuilds on text change via set_text → setNeedsDisplay.
- (void)ensureImageBitmap:(const std::string&)path
{
  if (bitmap_handle) return;  // simple v1: load once, cache forever
  auto* backend = neui_cg_backend::get_backend();
  if (!backend || !render_ctx || path.empty()) return;
  uint32_t w = 0, h = 0;
  uint8_t* px = neui_detail::load_image_bgra8_macos(path.c_str(), &w, &h);
  if (!px) return;
  bitmap_handle = backend->create_bitmap(render_ctx, w, h, px, /*scale*/1.0f);
  bitmap_w_px = w;
  bitmap_h_px = h;
  bitmap_scale = 1.0f;
  neui_detail::free_image_bgra8(px);
}

- (void)drawRect:(NSRect)dirtyRect
{
  (void)dirtyRect;
  auto* backend = neui_cg_backend::get_backend();
  if (!backend || !render_ctx) return;
  CGContextRef cg = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
  if (!cg) return;

  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;

  NSSize sz = self.bounds.size;
  neui_cg_backend::set_current_frame(render_ctx, (void*)cg,
                                      (float)sz.width, (float)sz.height);
  // Clear with the panel-bg colour matching the rest of the window. Same
  // approach as the xpl host's paint_frame.
  uint32_t clear = neui_detail::color(neui_detail::ColorRole::panel_bg);
  if (wd->attrs && wd->attrs->has(NEUI_ATTR_BACKGROUND))
    clear = (uint32_t)wd->attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
  backend->begin_frame(render_ctx, clear);

  if (wd->type && !strcmp(wd->type, NEUI_W_IMAGE)) {
    [self ensureImageBitmap:wd->text];
    if (bitmap_handle) {
      // Aspect-preserving fit (letterbox / pillarbox, centred). Same shape
      // as the existing hosts.
      float vw = (float)sz.width, vh = (float)sz.height;
      float bw = (float)bitmap_w_px / bitmap_scale;
      float bh = (float)bitmap_h_px / bitmap_scale;
      float scale = (bw / bh > vw / vh) ? (vw / bw) : (vh / bh);
      float dw = bw * scale, dh = bh * scale;
      float dx = (vw - dw) * 0.5f, dy = (vh - dh) * 0.5f;
      // Honour NEUI_ATTR_ROTATION via the renderer transform stack - same
      // shape as the xpl host's IMAGE paint path.
      float rot = wd->attrs ? wd->attrs->get_float(NEUI_ATTR_ROTATION, 0.0f) : 0.0f;
      if (rot != 0.0f) {
        backend->push_transform(render_ctx);
        backend->translate(render_ctx, dx + dw * 0.5f, dy + dh * 0.5f);
        backend->rotate(render_ctx, rot);
        backend->translate(render_ctx, -dw * 0.5f, -dh * 0.5f);
        backend->draw_bitmap(render_ctx, bitmap_handle,
                              0, 0, 0, 0, 0, 0, dw, dh);
        backend->pop_transform(render_ctx);
      } else {
        backend->draw_bitmap(render_ctx, bitmap_handle,
                              0, 0, 0, 0, dx, dy, dw, dh);
      }
    }
  } else if (wd->type && !strcmp(wd->type, NEUI_W_KNOB)) {
    // Fire pre-update so the client can refresh NEUI_ATTR_VALUE_TEXT (etc.)
    // before paint_knob reads the value, matching the xpl host's pattern.
    if (wd->emit_events) {
      neui_event_t pe = {};
      pe.type = NEUI_EVENT_WIDGET_PREUPDATE;
      pe.data.preupdate.widget = { wd->widget_id };
      sess->dispatch_event(&pe);
    }
    float value = wd->attrs ? wd->attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f;
    int   steps = wd->attrs ? wd->attrs->get_int  (NEUI_ATTR_STEPS,  0   ) : 0;
    const char* polarity_str = wd->attrs ? wd->attrs->get_string(NEUI_ATTR_POLARITY) : nullptr;
    const char* value_text   = wd->attrs ? wd->attrs->get_string(NEUI_ATTR_VALUE_TEXT) : nullptr;
    auto polarity = neui_detail::parse_knob_polarity(polarity_str);

    neui_detail::paint_knob(backend, render_ctx,
                             0, 0, (float)sz.width, (float)sz.height,
                             value, /*focused*/false, polarity, steps, value_text);
  }

  backend->end_frame(render_ctx);
}

// ---------------------------------------------------------------------------
// KNOB mouse handling. Same shape as xpl_host::KnobWidget::on_mouse_event.

- (BOOL)isKnob
{
  auto* wd = macos_host::widget_for_id(widget_id);
  return wd && wd->type && !strcmp(wd->type, NEUI_W_KNOB);
}

// Read NEUI_PARAM_VALUE clamped to [0..1]. Step-snapping is already applied
// when the value was last written, so reading is just a fetch + clamp.
- (float)knobValue
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->attrs) return 0.0f;
  return neui_clamp01(wd->attrs->get_float(NEUI_PARAM_VALUE, 0.0f));
}

- (int)knobSteps
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->attrs) return 0;
  return wd->attrs->get_int(NEUI_ATTR_STEPS, 0);
}

// Write a new value: clamp + snap, store on attrs, fire VALUE_CHANGED if
// the snapped value actually moved. Mirrors xpl's widget_set_value_user.
- (void)setKnobValueFromUser:(float)v
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  v = neui_snap_to_steps(neui_clamp01(v), [self knobSteps]);
  float old = [self knobValue];
  if (v == old) return;
  neui_detail::ensure_attrs(wd->attrs).set_float(NEUI_PARAM_VALUE, v);
  if (wd->emit_events) {
    neui_event_t ev = {};
    ev.type              = NEUI_EVENT_VALUE_CHANGED;
    ev.data.value.widget = { wd->widget_id };
    ev.data.value.value  = v;
    sess->dispatch_event(&ev);
  }
  [self setNeedsDisplay:YES];
}

- (NSPoint)knobCenter
{
  NSSize sz = self.bounds.size;
  return NSMakePoint(sz.width * 0.5, sz.height * 0.5);
}

- (NSPoint)localPointForKnobEvent:(NSEvent*)event
{
  // Pointer in our (flipped, top-left-origin) view coordinates.
  return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent*)event
{
  if (![self isKnob]) { [super mouseDown:event]; return; }

  // Double-click → reset to NEUI_PARAM_DEFAULT.
  if (event.clickCount >= 2) {
    auto* wd = macos_host::widget_for_id(widget_id);
    float def = 0.0f;
    if (wd && wd->attrs) def = neui_clamp01(wd->attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
    [self setKnobValueFromUser:def];
    return;
  }

  NSPoint p = [self localPointForKnobEvent:event];
  NSPoint c = [self knobCenter];
  float dx = (float)(p.x - c.x);
  float dy = (float)(p.y - c.y);
  float r2 = dx * dx + dy * dy;
  if (r2 < NEUI_KNOB_DEAD_ZONE_R * NEUI_KNOB_DEAD_ZONE_R) return;

  dragging        = true;
  drag_prev_angle = std::atan2(dy, dx);
  // Seed the continuous accumulator with the snapped current value so the
  // first delta nudges off it (rather than starting from 0).
  drag_continuous = [self knobValue];
}

- (void)mouseDragged:(NSEvent*)event
{
  if (![self isKnob] || !dragging) { [super mouseDragged:event]; return; }
  NSPoint p = [self localPointForKnobEvent:event];
  NSPoint c = [self knobCenter];
  float dx = (float)(p.x - c.x);
  float dy = (float)(p.y - c.y);
  float r2 = dx * dx + dy * dy;
  if (r2 < NEUI_KNOB_DEAD_ZONE_R * NEUI_KNOB_DEAD_ZONE_R) return;  // unstable

  float cur_angle = std::atan2(dy, dx);
  float delta     = neui_knob_wrap_pi(cur_angle - drag_prev_angle);
  bool fine = (event.modifierFlags & NSEventModifierFlagShift) != 0;
  float scale = (fine ? NEUI_KNOB_FINE_SCALE : 1.0f) / NEUI_KNOB_SWEEP_RAD;
  drag_continuous = neui_clamp01(drag_continuous + delta * scale);
  [self setKnobValueFromUser:drag_continuous];
  drag_prev_angle = cur_angle;
}

- (void)mouseUp:(NSEvent*)event
{
  if (![self isKnob]) { [super mouseUp:event]; return; }
  dragging = false;
}

- (void)scrollWheel:(NSEvent*)event
{
  if (![self isKnob]) { [super scrollWheel:event]; return; }
  CGFloat raw = event.scrollingDeltaY;
  if (raw == 0) return;
  // Match the xpl host's wheel-up = decrease convention (audio-plugin feel).
  bool fine = (event.modifierFlags & NSEventModifierFlagShift) != 0;
  int steps = [self knobSteps];
  float magnitude = (steps >= 2)
    ? (1.0f / (float)(steps - 1))
    : (fine ? 0.01f : 0.05f);
  float sign = (raw > 0) ? -1.0f : 1.0f;
  [self setKnobValueFromUser:[self knobValue] + sign * magnitude];
}

@end

// ---------------------------------------------------------------------------
// NEUINativeWindowDelegate - close button + quit-on-last-appwindow.

@interface NEUINativeWindowDelegate : NSObject<NSWindowDelegate>
{
@public
  macos_host::Session* session;
  uint32_t             widget_index;
  bool                 is_appwindow;
  bool                 handled_close;
  __weak NSWindow*     sheet_owner;
  bool                 sheet_active;
}
@end

@implementation NEUINativeWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender
{
  if (!session) return YES;

  // Give the client a chance to veto. APP_QUIT is the veto event for any
  // frame close (mirrors the win32 / xpl host's WM_CLOSE → APP_QUIT path);
  // dialogs and appwindows go through the same dispatch.
  neui_event_t ev = {};
  ev.type = NEUI_EVENT_APP_QUIT;
  if (!session->dispatch_event(&ev)) return NO;

  // For a modal sheet, returning YES would cause AppKit to call -close on
  // the sheet without ending the sheet session - the parent stays disabled
  // and the next beginSheet: would assert. Drive endSheet: ourselves; the
  // completion handler installed in create_dialog closes the sheet's
  // window after detachment.
  if (sheet_active && sheet_owner) {
    sheet_active = false;
    [sheet_owner endSheet:sender];
    return NO;
  }
  return YES;
}

- (void)windowWillClose:(NSNotification*)note
{
  (void)note;
  if (handled_close) return;
  handled_close = true;
  if (is_appwindow) {
    if (--g_appwindow_count <= 0) {
      [NSApp stop:nil];
      wake_app_event_pump();
    }
  }
}

@end

// ---------------------------------------------------------------------------
// NEUINativeControlTarget - singleton action sink for NSControls. Each
// NSControl's tag = widget_id ((session_id<<16) | tree_index). The selector
// looks up the session in macos_host::sessions and dispatches the
// appropriate neui_event_t.

@interface NEUINativeControlTarget : NSObject
+ (instancetype)shared;
- (void)neuiControlAction:(id)sender;
@end

@implementation NEUINativeControlTarget

+ (instancetype)shared
{
  static NEUINativeControlTarget* s = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = [[NEUINativeControlTarget alloc] init]; });
  return s;
}

- (void)neuiControlAction:(id)sender
{
  if (![sender isKindOfClass:[NSControl class]]) return;
  NSControl* c = (NSControl*)sender;
  uint32_t widget_id  = (uint32_t)c.tag;
  uint32_t session_id = (widget_id >> 16) & 0xffff;
  uint32_t idx        = widget_id & 0xffff;
  if (session_id == 0) return;
  size_t sess_idx = static_cast<size_t>(session_id) - 1;
  if (sess_idx >= macos_host::sessions.size()) return;
  auto& sess_ptr = macos_host::sessions[sess_idx];
  if (!sess_ptr) return;
  auto* sess = sess_ptr.get();
  if (!sess->_widgets.exists(idx)) return;
  auto& wd = sess->_widgets[idx];
  if (!wd.emit_events) return;

  if (!wd.type) return;
  if (!strcmp(wd.type, NEUI_W_BUTTON)) {
    neui_event_t ev = {};
    ev.type                 = NEUI_EVENT_MOUSE_BUTTON_CLICK;
    ev.data.mouse.widget    = { wd.widget_id };
    ev.data.mouse.buttonmap = 0;
    sess->dispatch_event(&ev);
    return;
  }
  if (!strcmp(wd.type, NEUI_W_CHECKBOX) || !strcmp(wd.type, NEUI_W_CHECKBOX3)) {
    NSButton* btn = (NSButton*)c;

    // The button is NSButtonTypeMomentaryChange (no auto-toggle), so we
    // own the entire state machine: read cached state, advance by 2 or 3
    // depending on the tristate attr, swap the SF Symbol image, write
    // back. NSButton.state is never read or written - we never want
    // AppKit to render its checkbox cell here.
    bool tristate = wd.attrs && wd.attrs->get_int(NEUI_ATTR_TRISTATE, 0) != 0;
    int prev = wd.attrs ? wd.attrs->get_int("neui.macoshost.checkstate",
                                              NEUI_CHECK_UNCHECKED)
                        : NEUI_CHECK_UNCHECKED;
    int mod  = tristate ? 3 : 2;
    int next = (prev + 1) % mod;
    btn.image = macos_host::checkbox_image_for_state(next);
    neui_detail::ensure_attrs(wd.attrs).set_int("neui.macoshost.checkstate", next);

    neui_event_t ev = {};
    ev.type                 = NEUI_EVENT_CHECKBOX_CHANGED;
    ev.data.checkbox.widget = { wd.widget_id };
    ev.data.checkbox.state  = (neui_check_state_t)next;
    sess->dispatch_event(&ev);
    return;
  }
  if (!strcmp(wd.type, NEUI_W_COMBOBOX)) {
    NSPopUpButton* pb = (NSPopUpButton*)c;
    NSInteger idx = pb.indexOfSelectedItem;
    uint32_t selected = (idx >= 0 && (size_t)idx < wd.items.size())
                         ? (uint32_t)idx : NEUI_ITEM_NONE;
    wd.selected_item = selected;
    neui_event_t ev = {};
    ev.type             = NEUI_EVENT_ITEM_SELECTED;
    ev.data.item.widget = { wd.widget_id };
    ev.data.item.index  = selected;
    sess->dispatch_event(&ev);
    return;
  }
  if (!strcmp(wd.type, NEUI_W_SLIDER)) {
    NSSlider* sl = (NSSlider*)c;
    float v = (float)sl.doubleValue;
    if (v < 0) v = 0; if (v > 1) v = 1;
    if (wd.attrs) wd.attrs->set_float(NEUI_PARAM_VALUE, v);
    neui_event_t ev = {};
    ev.type              = NEUI_EVENT_VALUE_CHANGED;
    ev.data.value.widget = { wd.widget_id };
    ev.data.value.value  = v;
    sess->dispatch_event(&ev);
    return;
  }
}

@end

// ---------------------------------------------------------------------------
// NEUINativeTextDelegate - singleton sink for NSTextField / NSTextView change
// notifications. Looks up the changed control's owning widget via tag (set
// by create_inputbox / create_multiline) and fires NEUI_EVENT_WIDGET_UPDATED.

namespace macos_host {
  // Helper used by the delegate to dispatch a WIDGET_UPDATED event without
  // duplicating the (session_id<<16 | tree_idx) decoding.
  static void dispatch_widget_updated(uint32_t widget_id)
  {
    uint32_t session_id = (widget_id >> 16) & 0xffff;
    uint32_t idx        = widget_id & 0xffff;
    if (session_id == 0) return;
    size_t sess_idx = static_cast<size_t>(session_id) - 1;
    if (sess_idx >= sessions.size()) return;
    auto& sp = sessions[sess_idx];
    if (!sp) return;
    auto* sess = sp.get();
    if (!sess->_widgets.exists(idx)) return;
    auto& wd = sess->_widgets[idx];
    if (!wd.emit_events) return;
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_WIDGET_UPDATED;
    sess->dispatch_event(&ev);
  }
}

// ---------------------------------------------------------------------------
// NEUINativeListSource - per-LISTBOX NSTableViewDataSource + Delegate. Holds
// the owning widget_id so reads / selection events can route back to the
// session. One source instance per LISTBOX widget; the NSScrollView owns a
// strong reference via objc_setAssociatedObject.

@interface NEUINativeListSource : NSObject<NSTableViewDataSource, NSTableViewDelegate>
{
@public
  uint32_t widget_id;
}
@end

namespace macos_host {
  // Helper to look up a Session + WidgetData by widget_id (decodes the
  // upper 16 bits as session_id, lower 16 as tree slot). Returns null on
  // any lookup miss. Non-static so NEUINativePaintedView (defined earlier
  // in this TU) can reference via the forward declaration at file top.
  WidgetData* widget_for_id(uint32_t widget_id, Session** out_session)
  {
    uint32_t session_id = (widget_id >> 16) & 0xffff;
    uint32_t idx        = widget_id & 0xffff;
    if (session_id == 0) return nullptr;
    size_t sess_idx = static_cast<size_t>(session_id) - 1;
    if (sess_idx >= sessions.size()) return nullptr;
    auto& sp = sessions[sess_idx];
    if (!sp) return nullptr;
    auto* sess = sp.get();
    if (!sess->_widgets.exists(idx)) return nullptr;
    if (out_session) *out_session = sess;
    return &sess->_widgets[idx];
  }
}

@implementation NEUINativeListSource

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
  (void)tableView;
  auto* wd = macos_host::widget_for_id(widget_id);
  return wd ? (NSInteger)wd->items.size() : 0;
}

- (NSView*)tableView:(NSTableView*)tableView
   viewForTableColumn:(NSTableColumn*)tableColumn
                  row:(NSInteger)row
{
  (void)tableColumn;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || row < 0 || (size_t)row >= wd->items.size()) return nil;

  static NSString* const k_id = @"neui.listcell";
  NSTableCellView* cv = [tableView makeViewWithIdentifier:k_id owner:self];
  if (!cv) {
    cv = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    cv.identifier = k_id;
    NSTextField* tf = [NSTextField labelWithString:@""];
    tf.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:tf];
    cv.textField = tf;
    [NSLayoutConstraint activateConstraints:@[
      [tf.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor  constant:4],
      [tf.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-4],
      [tf.centerYAnchor  constraintEqualToAnchor:cv.centerYAnchor],
    ]];
  }
  cv.textField.stringValue =
    [NSString stringWithUTF8String:wd->items[(size_t)row].text.c_str()];
  return cv;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note
{
  NSTableView* tv = (NSTableView*)note.object;
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !wd->emit_events) return;
  NSInteger row = tv.selectedRow;
  uint32_t idx = (row >= 0 && (size_t)row < wd->items.size())
                  ? (uint32_t)row : NEUI_ITEM_NONE;
  wd->selected_item = idx;
  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_ITEM_SELECTED;
  ev.data.item.widget = { wd->widget_id };
  ev.data.item.index  = idx;
  sess->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUINativeMenuTarget - singleton sink for menu-item picks. Decodes
// (widget_id, item_id) from the NSMenuItem's representedObject + tag, looks
// up the macos_host::Session, fires NEUI_EVENT_TREE_ITEM_ACTIVATED.

@interface NEUINativeMenuTarget : NSObject
+ (instancetype)shared;
- (void)neuiNativeMenuPick:(id)sender;
@end

@implementation NEUINativeMenuTarget

+ (instancetype)shared
{
  static NEUINativeMenuTarget* s = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = [[NEUINativeMenuTarget alloc] init]; });
  return s;
}

- (void)neuiNativeMenuPick:(id)sender
{
  NSMenuItem* it = (NSMenuItem*)sender;
  if (![it isKindOfClass:[NSMenuItem class]]) return;
  NSNumber* widget_id_num = (NSNumber*)it.representedObject;
  if (!widget_id_num) return;
  uint32_t widget_id = widget_id_num.unsignedIntValue;
  uint32_t item_id   = (uint32_t)it.tag;

  uint32_t session_id = (widget_id >> 16) & 0xffff;
  uint32_t idx        = widget_id & 0xffff;
  if (session_id == 0) return;
  size_t sess_idx = static_cast<size_t>(session_id) - 1;
  if (sess_idx >= macos_host::sessions.size()) return;
  auto& sp = macos_host::sessions[sess_idx];
  if (!sp) return;
  auto* sess = sp.get();
  if (!sess->_widgets.exists(idx)) return;
  auto& wd = sess->_widgets[idx];

  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_TREE_ITEM_ACTIVATED;
  ev.data.tree.widget = { wd.widget_id };
  ev.data.tree.item   = { item_id };
  sess->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUINativeOutlineSource - per-TREEVIEW NSOutlineViewDataSource + Delegate.
// Items are NSNumber-wrapped neui tree_ids; child:ofItem: walks the
// tree_items_ordered vector filtering by parent_id so children appear in
// insertion order (matches the xpl host / win32 host's behaviour).

@interface NEUINativeOutlineSource : NSObject<NSOutlineViewDataSource, NSOutlineViewDelegate>
{
@public
  uint32_t widget_id;
}
@end

namespace macos_host {
  // Walk tree_items_ordered, return the n-th item whose parent_id matches.
  // Used by NEUINativeOutlineSource's child:ofItem: + numberOfChildrenOfItem:.
  static uint32_t tree_nth_child(const WidgetData& wd, uint32_t parent_id, NSInteger n)
  {
    NSInteger seen = 0;
    for (uint32_t id : wd.tree_items_ordered) {
      auto it = wd.tree_items.find(id);
      if (it == wd.tree_items.end()) continue;
      if (it->second.parent_id != parent_id) continue;
      if (seen == n) return id;
      ++seen;
    }
    return 0;
  }

  static NSInteger tree_count_children(const WidgetData& wd, uint32_t parent_id)
  {
    NSInteger c = 0;
    for (uint32_t id : wd.tree_items_ordered) {
      auto it = wd.tree_items.find(id);
      if (it == wd.tree_items.end()) continue;
      if (it->second.parent_id == parent_id) ++c;
    }
    return c;
  }
}

@implementation NEUINativeOutlineSource

- (NSInteger)outlineView:(NSOutlineView*)outlineView numberOfChildrenOfItem:(id)item
{
  (void)outlineView;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return 0;
  uint32_t parent_id = item ? (uint32_t)((NSNumber*)item).unsignedIntValue : 0;
  return macos_host::tree_count_children(*wd, parent_id);
}

- (id)outlineView:(NSOutlineView*)outlineView child:(NSInteger)index ofItem:(id)item
{
  (void)outlineView;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return nil;
  uint32_t parent_id = item ? (uint32_t)((NSNumber*)item).unsignedIntValue : 0;
  uint32_t cid = macos_host::tree_nth_child(*wd, parent_id, index);
  return cid ? @(cid) : nil;
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item
{
  (void)outlineView;
  if (!item) return YES;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return NO;
  uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
  return macos_host::tree_count_children(*wd, id_v) > 0;
}

- (id)outlineView:(NSOutlineView*)outlineView
       objectValueForTableColumn:(NSTableColumn*)tableColumn
                          byItem:(id)item
{
  (void)outlineView; (void)tableColumn;
  if (!item) return @"";
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return @"";
  uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
  auto it = wd->tree_items.find(id_v);
  if (it == wd->tree_items.end()) return @"";
  return [NSString stringWithUTF8String:it->second.text.c_str()];
}

// View-based row rendering. Required on macOS 26 / Liquid Glass - cell-based
// NSOutlineView misaligns the disclosure chevron (it sits below the text
// baseline) because the legacy cell vertical-centre math wasn't updated for
// the new row metrics. View-based mode lays the chevron out against the
// NSTableCellView's textField bounds correctly.
- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item
{
  (void)tableColumn;
  NSTableCellView* cell = [outlineView makeViewWithIdentifier:@"NEUITreeCell" owner:self];
  if (!cell) {
    cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)];
    cell.identifier = @"NEUITreeCell";
    NSTextField* tf = [NSTextField labelWithString:@""];
    tf.translatesAutoresizingMaskIntoConstraints = NO;
    tf.drawsBackground = NO;
    tf.editable        = NO;
    tf.selectable      = NO;
    tf.bezeled         = NO;
    tf.lineBreakMode   = NSLineBreakByTruncatingTail;
    [cell addSubview:tf];
    cell.textField = tf;
    [NSLayoutConstraint activateConstraints:@[
      [tf.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor],
      [tf.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor],
      [tf.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
    ]];
  }
  NSString* text = @"";
  if (item) {
    auto* wd = macos_host::widget_for_id(widget_id);
    if (wd) {
      uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
      auto it = wd->tree_items.find(id_v);
      if (it != wd->tree_items.end())
        text = [NSString stringWithUTF8String:it->second.text.c_str()];
    }
  }
  cell.textField.stringValue = text;
  return cell;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)note
{
  NSOutlineView* ov = (NSOutlineView*)note.object;
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  NSInteger row = ov.selectedRow;
  uint32_t id_v = 0;
  if (row >= 0) {
    id selectedItem = [ov itemAtRow:row];
    if (selectedItem) id_v = (uint32_t)((NSNumber*)selectedItem).unsignedIntValue;
  }
  wd->selected_tree_item = id_v ? id_v : UINT32_MAX;
  if (!wd->emit_events) return;
  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_TREE_ITEM_SELECTED;
  ev.data.tree.widget = { wd->widget_id };
  ev.data.tree.item   = { id_v };
  sess->dispatch_event(&ev);
}

// Double-click → TREE_ITEM_ACTIVATED. Wired via the table view's
// doubleAction in create_treeview.
- (void)neuiOutlineDoubleClick:(id)sender
{
  NSOutlineView* ov = (NSOutlineView*)sender;
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  NSInteger row = ov.clickedRow;
  if (row < 0) return;
  id item = [ov itemAtRow:row];
  if (!item) return;
  uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
  if (!wd->emit_events) return;
  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_TREE_ITEM_ACTIVATED;
  ev.data.tree.widget = { wd->widget_id };
  ev.data.tree.item   = { id_v };
  sess->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUINativeTextDelegate.

@interface NEUINativeTextDelegate : NSObject<NSTextFieldDelegate, NSTextViewDelegate>
+ (instancetype)shared;
@end

@implementation NEUINativeTextDelegate

+ (instancetype)shared
{
  static NEUINativeTextDelegate* s = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = [[NEUINativeTextDelegate alloc] init]; });
  return s;
}

// NSTextField: edit notifications come via control:textShouldEndEditing: and
// controlTextDidChange:. We use controlTextDidChange: for the live-update path.
- (void)controlTextDidChange:(NSNotification*)notification
{
  NSTextField* f = (NSTextField*)notification.object;
  if (![f isKindOfClass:[NSTextField class]]) return;
  macos_host::dispatch_widget_updated((uint32_t)f.tag);
}

// NSTextView change notification. The widget_id is stashed in the
// TextView's identifier (since NSTextView doesn't have a tag).
- (void)textDidChange:(NSNotification*)notification
{
  NSTextView* tv = (NSTextView*)notification.object;
  if (![tv isKindOfClass:[NSTextView class]]) return;
  NSString* idstr = tv.identifier;
  if (!idstr) return;
  uint32_t widget_id = (uint32_t)[idstr longLongValue];
  macos_host::dispatch_widget_updated(widget_id);
}

@end

// ---------------------------------------------------------------------------
// macos_host::Session::widget_show + Session::run + helpers.

namespace macos_host
{
  // Walk up the widget tree from `idx` until we find an ancestor whose
  // native_window is set. Returns its NSWindow.contentView (the flipped
  // NEUINativeContentView), or nil if there's no native frame above.
  static NSView* find_parent_content_view(Session* s, uint32_t idx)
  {
    if (!s) return nil;
    auto parents = s->_widgets.get_all_parents(idx);
    for (uint32_t p : parents) {
      if (p == 0) continue;
      if (s->_widgets.exists(p) && s->_widgets[p].native_window) {
        NSWindow* w = native_window_from(s->_widgets[p].native_window);
        return w.contentView;
      }
    }
    return nil;
  }

  // Released from widgets.mm's Session::widget_destroy.
  void release_native_window_macos(WidgetData& wd)
  {
    if (!wd.native_window) return;
    NSWindow* w = (__bridge_transfer NSWindow*)wd.native_window;
    wd.native_window = nullptr;
    NEUINativeWindowDelegate* d = (NEUINativeWindowDelegate*)w.delegate;
    if (d && d->sheet_active && d->sheet_owner) {
      [d->sheet_owner endSheet:w];
      d->sheet_active = false;
    }
    [w close];
  }

  void release_native_control_macos(WidgetData& wd)
  {
    if (!wd.native_control) return;
    // MENUBAR stores an NSMenu*; everything else stores an NSView subclass.
    // NSMenu is not an NSView - sending removeFromSuperview to it crashes.
    id obj = (__bridge_transfer id)wd.native_control;
    wd.native_control = nullptr;
    if ([obj isKindOfClass:[NSView class]]) {
      [(NSView*)obj removeFromSuperview];
    } else if ([obj isKindOfClass:[NSMenu class]]) {
      if (NSApp.mainMenu == (NSMenu*)obj) NSApp.mainMenu = nil;
    }
  }

  // -------------------------------------------------------------------------
  // Per-type widget_show helpers.

  // Apply pre-show frame-level attribute state: title, icon, min/max size
  // constraints. Same shape as the xpl host's install_view_and_context block.
  static void apply_frame_attrs(NSWindow* window, WidgetData& w)
  {
    if (!w.text.empty())
      [window setTitle:[NSString stringWithUTF8String:w.text.c_str()]];

    if (w.attrs) {
      const char* icon_path = w.attrs->get_string(NEUI_ATTR_ICON_PATH);
      if (icon_path && *icon_path) {
        NSString* ns_path = [NSString stringWithUTF8String:icon_path];
        if (ns_path) {
          NSImage* img = [[NSImage alloc] initWithContentsOfFile:ns_path];
          if (img) [NSApp setApplicationIconImage:img];
        }
      }

      int min_w = w.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
      int min_h = w.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
      int max_w = w.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
      int max_h = w.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
      NSSize min_sz = NSMakeSize(min_w > 0 ? min_w : 0,
                                  min_h > 0 ? min_h : 0);
      NSSize max_sz = NSMakeSize(max_w > 0 ? max_w : FLT_MAX,
                                  max_h > 0 ? max_h : FLT_MAX);
      [window setContentMinSize:min_sz];
      [window setContentMaxSize:max_sz];
    }
  }

  static NSWindowStyleMask styles_for_dialog()
  {
    return NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
  }

  // Generic frame creator. Used for APPWINDOW (full chrome), DIALOG (titled +
  // closable), and PLUGWINDOW (borderless). When `owner` is set + modal, the
  // dialog is presented as a sheet on widget_show; this hook records that
  // intent on the delegate.
  static NSWindow* create_native_frame(Session* s, uint32_t idx, WidgetData& w,
                                        NSWindowStyleMask style_mask,
                                        bool counts_toward_quit,
                                        NSWindow* sheet_owner_or_nil)
  {
    ensure_nsapp_initialised();

    NSRect frame_rect = logical_window_rect(w.x, w.y, w.width, w.height);
    NSWindow* window = [[NSWindow alloc]
       initWithContentRect:frame_rect
                 styleMask:style_mask
                   backing:NSBackingStoreBuffered
                     defer:NO];
    [window setReleasedWhenClosed:NO];

    NEUINativeContentView* cv =
      [[NEUINativeContentView alloc] initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    [window setContentView:cv];

    NEUINativeWindowDelegate* d = [[NEUINativeWindowDelegate alloc] init];
    d->session      = s;
    d->widget_index = idx;
    d->is_appwindow = counts_toward_quit;
    d->sheet_owner  = sheet_owner_or_nil;
    [window setDelegate:d];
    objc_setAssociatedObject(window, "NEUINativeWindowDelegate", d,
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    w.native_window = (__bridge_retained void*)window;
    if (counts_toward_quit) ++g_appwindow_count;

    apply_frame_attrs(window, w);
    return window;
  }

  static void create_appwindow(Session* s, uint32_t idx, WidgetData& w)
  {
    NSWindow* window = create_native_frame(s, idx, w,
                                             styles_for_appwindow(),
                                             /*counts_toward_quit*/true,
                                             /*sheet_owner*/nil);
    [window makeKeyAndOrderFront:nil];
  }

  static void create_plugwindow(Session* s, uint32_t idx, WidgetData& w)
  {
    NSWindow* window = create_native_frame(s, idx, w,
                                             NSWindowStyleMaskBorderless,
                                             /*counts_toward_quit*/false,
                                             /*sheet_owner*/nil);
    [window makeKeyAndOrderFront:nil];
  }

  static void create_dialog(Session* s, uint32_t idx, WidgetData& w)
  {
    NSWindow* owner_window = nil;
    if (w.owner_index != 0 && s->_widgets.exists(w.owner_index)) {
      auto& ow = s->_widgets[w.owner_index];
      if (ow.native_window) owner_window = native_window_from(ow.native_window);
    }
    bool modal = !w.attrs || w.attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;

    NSWindow* window = create_native_frame(s, idx, w,
                                             styles_for_dialog(),
                                             /*counts_toward_quit*/false,
                                             modal ? owner_window : nil);
    if (modal && owner_window) {
      NEUINativeWindowDelegate* d =
        objc_getAssociatedObject(window, "NEUINativeWindowDelegate");
      if (d) d->sheet_active = true;
      // The completion handler closes the sheet's window after the
      // close-button path's endSheet: detaches it from the owner. Weak
      // ref so the block doesn't keep the dialog alive past teardown.
      __weak NSWindow* weak_window = window;
      [owner_window beginSheet:window completionHandler:^(NSModalResponse /*r*/){
        NSWindow* w2 = weak_window;
        if (w2 && w2.visible) [w2 close];
      }];
    } else {
      [window makeKeyAndOrderFront:nil];
    }
  }

  static NSTextField* create_label(WidgetData& w)
  {
    NSTextField* tf = [[NSTextField alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    tf.editable        = NO;
    tf.selectable      = NO;
    tf.bezeled         = NO;
    tf.bordered        = NO;
    tf.drawsBackground = NO;
    tf.stringValue     = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    return tf;
  }

  static NSButton* create_button(WidgetData& w)
  {
    NSButton* b = [[NSButton alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    [b setBezelStyle:NSBezelStyleRounded];
    [b setTitle:[NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""]];
    b.target = [NEUINativeControlTarget shared];
    b.action = @selector(neuiControlAction:);
    b.tag    = (NSInteger)w.widget_id;
    return b;
  }

  static bool is_readonly(WidgetData& w)
  {
    return w.attrs && w.attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
  }

  static NEUINativePaintedView* create_painted_view(WidgetData& w)
  {
    NEUINativePaintedView* v = [[NEUINativePaintedView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    v->widget_id = w.widget_id;
    auto* backend = neui_cg_backend::get_backend();
    if (backend) {
      v->render_ctx = backend->create_context((__bridge void*)v,
                                                (uint32_t)w.width,
                                                (uint32_t)w.height);
    }
    v->bitmap_handle = nullptr;
    v->bitmap_w_px = 0; v->bitmap_h_px = 0; v->bitmap_scale = 1.0f;
    return v;
  }

  static NSSlider* create_slider(WidgetData& w)
  {
    NSSlider* sl = [[NSSlider alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sl.minValue = 0.0;
    sl.maxValue = 1.0;
    sl.continuous = YES;
    int steps = w.attrs ? w.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
    if (steps >= 2) {
      sl.numberOfTickMarks         = steps;
      sl.allowsTickMarkValuesOnly  = YES;
    } else {
      sl.numberOfTickMarks         = 0;
      sl.allowsTickMarkValuesOnly  = NO;
    }
    float v = w.attrs ? w.attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f;
    if (v < 0) v = 0; if (v > 1) v = 1;
    sl.doubleValue = v;
    sl.target = [NEUINativeControlTarget shared];
    sl.action = @selector(neuiControlAction:);
    sl.tag    = (NSInteger)w.widget_id;
    return sl;
  }

  static NSScrollView* create_treeview(WidgetData& w)
  {
    NSScrollView* sv = [[NSScrollView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sv.hasVerticalScroller = YES;
    sv.borderType          = NSBezelBorder;
    sv.autohidesScrollers  = YES;

    NSOutlineView* ov = [[NSOutlineView alloc]
      initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"col0"];
    col.width = w.width;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [ov addTableColumn:col];
    ov.outlineTableColumn = col;
    ov.headerView = nil;
    ov.allowsEmptySelection = YES;
    ov.allowsMultipleSelection = NO;

    NEUINativeOutlineSource* src = [[NEUINativeOutlineSource alloc] init];
    src->widget_id = w.widget_id;
    ov.dataSource = src;
    ov.delegate   = src;
    ov.target     = src;
    ov.doubleAction = @selector(neuiOutlineDoubleClick:);

    sv.documentView = ov;
    objc_setAssociatedObject(sv, "NEUINativeOutlineSource", src,
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return sv;
  }

  static NSPopUpButton* create_combobox(WidgetData& w)
  {
    NSPopUpButton* pb = [[NSPopUpButton alloc]
       initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)
           pullsDown:NO];
    pb.target = [NEUINativeControlTarget shared];
    pb.action = @selector(neuiControlAction:);
    pb.tag    = (NSInteger)w.widget_id;

    // Items are typically added via the items API before widget_show, when
    // wd.native_control is still null - the lazy reload in widgets.mm
    // short-circuits in that window. Sync the current items + selection
    // now so the button has its menu populated at first display.
    // -addItemWithTitle: dedupes by title, so use an empty insert then
    // setTitle to preserve duplicates.
    for (auto& it : w.items) {
      [pb addItemWithTitle:@""];
      [pb.lastItem setTitle:[NSString stringWithUTF8String:it.text.c_str()]];
    }
    if (w.selected_item != NEUI_ITEM_NONE
        && w.selected_item < w.items.size())
      [pb selectItemAtIndex:(NSInteger)w.selected_item];

    // The xpl host treats a COMBOBOX's frame as "button + reserved space
    // for the inline drop overlay" - so callers commonly pass an oversized
    // height (e.g. 150). NSPopUpButton is a single-row control whose cell
    // vertically centres its content in the frame, which would float the
    // button down into surrounding widgets. The native dropdown is a real
    // NSMenu and doesn't need the reserved space, so clamp to the cell's
    // intrinsic height, anchored to the frame's top in the flipped view.
    CGFloat intrinsic_h = pb.intrinsicContentSize.height;
    if (intrinsic_h > 0 && intrinsic_h < (CGFloat)w.height)
      pb.frame = NSMakeRect(w.x, w.y, w.width, intrinsic_h);
    return pb;
  }

  static NSScrollView* create_listbox(WidgetData& w)
  {
    NSScrollView* sv = [[NSScrollView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sv.hasVerticalScroller = YES;
    sv.borderType          = NSBezelBorder;
    sv.autohidesScrollers  = YES;

    NSTableView* tv = [[NSTableView alloc]
      initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"col0"];
    col.width = w.width;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [tv addTableColumn:col];
    tv.headerView         = nil;
    tv.allowsEmptySelection = YES;
    tv.allowsMultipleSelection = NO;

    NEUINativeListSource* src = [[NEUINativeListSource alloc] init];
    src->widget_id = w.widget_id;
    tv.dataSource = src;
    tv.delegate   = src;

    sv.documentView = tv;

    // Keep the source alive for the scroll view's lifetime.
    objc_setAssociatedObject(sv, "NEUINativeListSource", src,
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return sv;
  }

  static NSButton* create_checkbox(WidgetData& w)
  {
    // SF-Symbol-driven borderless NSButton (not a checkbox cell). On
    // macOS 26 / Sequoia, +checkboxWithTitle: + setState:NSControlStateValueMixed
    // promotes the cell to a bezeled pull-down with up/down chevrons even
    // when allowsMixedState=NO. Driving an SF Symbol image manually
    // avoids the cell promotion path. State is cached on the widget's
    // attrs ("neui.macoshost.checkstate") and the click handler swaps
    // the image.
    NSString* title = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    b.title         = title;
    b.bordered      = NO;
    b.buttonType    = NSButtonTypeMomentaryChange;  // disables AppKit's auto-toggle of .state
    b.imagePosition = NSImageLeft;
    b.alignment     = NSTextAlignmentLeft;
    b.image         = checkbox_image_for_state(NEUI_CHECK_UNCHECKED);
    b.target        = [NEUINativeControlTarget shared];
    b.action        = @selector(neuiControlAction:);
    b.tag           = (NSInteger)w.widget_id;
    return b;
  }

  static NSTextField* create_inputbox(WidgetData& w)
  {
    NSTextField* tf = [[NSTextField alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    tf.editable        = !is_readonly(w);
    tf.selectable      = YES;
    tf.bezeled         = YES;
    tf.bordered        = YES;
    tf.drawsBackground = YES;
    tf.stringValue     = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    tf.delegate        = [NEUINativeTextDelegate shared];
    tf.tag             = (NSInteger)w.widget_id;
    return tf;
  }

  // MULTILINE: NSTextView wrapped in NSScrollView so it scrolls + has a frame.
  // The wrapper is what we hand back as native_control (positioned at w.x/y);
  // native_scroll points at the same NSScrollView for ARC bookkeeping. The
  // NSTextView itself is retrieved via [scroll documentView] for stringValue
  // / setString: in widget_get_text / widget_set_text.
  static NSScrollView* create_multiline(WidgetData& w)
  {
    NSScrollView* sv = [[NSScrollView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sv.hasVerticalScroller   = YES;
    sv.hasHorizontalScroller = NO;
    sv.borderType            = NSBezelBorder;
    sv.autohidesScrollers    = YES;

    NSTextView* tv = [[NSTextView alloc]
      initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    [tv setMinSize:NSMakeSize(0, 0)];
    [tv setMaxSize:NSMakeSize(FLT_MAX, FLT_MAX)];
    tv.verticallyResizable    = YES;
    tv.horizontallyResizable  = NO;
    tv.autoresizingMask       = NSViewWidthSizable;
    tv.editable               = !is_readonly(w);
    tv.selectable             = YES;
    tv.richText               = NO;
    tv.allowsUndo             = YES;
    tv.string                 = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    tv.delegate               = [NEUINativeTextDelegate shared];
    // NSTextView has no tag; stash widget_id on .identifier as a string.
    tv.identifier             = [NSString stringWithFormat:@"%u", w.widget_id];

    sv.documentView = tv;
    return sv;
  }

  // -------------------------------------------------------------------------

  // Per-widget native-control creation. Called from widget_show on a leaf,
  // and recursively for each descendant when a frame is shown for the first
  // time. Other widget types fall through (no-op) until their step lands.
  static void create_native_for_widget(Session* /*s*/, WidgetData& w, NSView* parent_content)
  {
    if (!parent_content || w.native_control || !w.type) return;
    if (!strcmp(w.type, NEUI_W_LABEL)) {
      NSTextField* tf = create_label(w);
      [parent_content addSubview:tf];
      w.native_control = (__bridge_retained void*)tf;
    } else if (!strcmp(w.type, NEUI_W_BUTTON)) {
      NSButton* b = create_button(w);
      [parent_content addSubview:b];
      w.native_control = (__bridge_retained void*)b;
    } else if (!strcmp(w.type, NEUI_W_INPUTBOX)) {
      NSTextField* tf = create_inputbox(w);
      [parent_content addSubview:tf];
      w.native_control = (__bridge_retained void*)tf;
    } else if (!strcmp(w.type, NEUI_W_MULTILINE)) {
      NSScrollView* sv = create_multiline(w);
      [parent_content addSubview:sv];
      w.native_control = (__bridge_retained void*)sv;
    } else if (!strcmp(w.type, NEUI_W_CHECKBOX)
            || !strcmp(w.type, NEUI_W_CHECKBOX3)) {
      NSButton* b = create_checkbox(w);
      [parent_content addSubview:b];
      w.native_control = (__bridge_retained void*)b;
    } else if (!strcmp(w.type, NEUI_W_LISTBOX)) {
      NSScrollView* sv = create_listbox(w);
      [parent_content addSubview:sv];
      w.native_control = (__bridge_retained void*)sv;
    } else if (!strcmp(w.type, NEUI_W_COMBOBOX)) {
      NSPopUpButton* pb = create_combobox(w);
      [parent_content addSubview:pb];
      w.native_control = (__bridge_retained void*)pb;
    } else if (!strcmp(w.type, NEUI_W_TREEVIEW)) {
      NSScrollView* sv = create_treeview(w);
      [parent_content addSubview:sv];
      w.native_control = (__bridge_retained void*)sv;
    } else if (!strcmp(w.type, NEUI_W_SLIDER)) {
      NSSlider* sl = create_slider(w);
      [parent_content addSubview:sl];
      w.native_control = (__bridge_retained void*)sl;
    } else if (!strcmp(w.type, NEUI_W_IMAGE)
            || !strcmp(w.type, NEUI_W_KNOB)) {
      NEUINativePaintedView* v = create_painted_view(w);
      [parent_content addSubview:v];
      w.native_control = (__bridge_retained void*)v;
    }
  }

  // After a frame is created, walk every descendant and instantiate its
  // native control. Mirror of hosts/win32/widgets.cpp::create_child_windows.
  // MENUBAR children are special: the NSMenu was already allocated at
  // widget_create time, and instead of adding a subview we hand it to
  // [NSApp setMainMenu:] so the system menu bar shows the items.
  static void create_descendants_native(Session* s, uint32_t parent_idx,
                                         NSView* parent_content)
  {
    uint32_t i = s->_widgets.child(parent_idx);
    while (i != 0) {
      if (s->_widgets.exists(i)) {
        auto& cw = s->_widgets[i];
        cw.visible = true;
        if (cw.type && !strcmp(cw.type, NEUI_W_MENUBAR)) {
          if (cw.native_control) {
            NSMenu* m = (__bridge NSMenu*)cw.native_control;
            [NSApp setMainMenu:m];
          }
        } else {
          create_native_for_widget(s, cw, parent_content);
        }
        create_descendants_native(s, i, parent_content);
      }
      i = s->_widgets.next(i);
    }
  }

  void Session::widget_show(neui_widget_t widget)
  {
    uint32_t index = widget.id & 0xffff;
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.visible = true;

    if (w.native_window) {
      [native_window_from(w.native_window) makeKeyAndOrderFront:nil];
      return;
    }
    if (w.native_control) {
      [native_view_from(w.native_control) setHidden:NO];
      return;
    }

    if (w.isroot) {
      if (!w.type) return;
      if (!strcmp(w.type, NEUI_W_APPWINDOW)) {
        create_appwindow(this, index, w);
      } else if (!strcmp(w.type, NEUI_W_PLUGWINDOW)) {
        create_plugwindow(this, index, w);
      } else if (!strcmp(w.type, NEUI_W_DIALOG)) {
        create_dialog(this, index, w);
      } else {
        return;
      }
      // Now that contentView exists, recursively create native controls for
      // every descendant. This is the equivalent of the win32 host's
      // WM_CREATE → create_child_windows path.
      NSView* cv = native_window_from(w.native_window).contentView;
      create_descendants_native(this, index, cv);
      return;
    }

    // Leaf widget shown directly (parent frame already exists).
    NSView* parent_content = find_parent_content_view(this, index);
    create_native_for_widget(this, w, parent_content);
  }

  bool Session::run()
  {
    ensure_nsapp_initialised();
    if (g_appwindow_count == 0) return true;
    [NSApp run];
    return true;
  }

} // namespace macos_host
