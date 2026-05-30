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
#include "../shared/macos/keys_macos.h"
#include "../shared/macos/theme_provider_macos.h"
#include "../shared/widget_paint_knob.h"
#include "../shared/widget_paint_compound.h"
#include "../shared/widget_paint_section.h"
#include "../shared/painter.h"
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

  // Painter draw_asset thunk - mirror of
  // hosts/win32/widgets.cpp::w32_painter_draw_asset_thunk. Resolves the
  // neui_asset_t through the session's MacOSAssetManager, lazy-uploads a
  // per-(asset, ctx) CGImage on first use, then calls backend->draw_bitmap.
  void NEUI_ABI macos_painter_draw_asset_thunk(void* host_token,
                                                 neui_render_backend_t* backend,
                                                 neui_render_ctx_t ctx,
                                                 neui_asset_t asset,
                                                 float x, float y,
                                                 float w, float h);

  // Blocking popup menu. Builds an NSMenu from the NULL-terminated UTF-8
  // `items` ("-" = separator), presents it at `(x, y)` in `anchor`'s
  // (flipped) coordinate space, and returns the 1-based index of the picked
  // item (separators consume an index, matching the win32 host) or 0 on
  // dismiss. Defined after the view; the KNOB right-click handler calls it.
  int run_popup_menu_macos(NSView* anchor, int x, int y, const char* const* items);

  // Defined in widgets.mm. Routes a built-in command (NEUI_CMD_*) to the key
  // window's first responder; returns true if consumed. The menu-pick router
  // calls it before falling back to TREE_ITEM_ACTIVATED.
  bool invoke_focused_command_macos(uint32_t cmd);
}

// ---------------------------------------------------------------------------
// Behavior plumbing for CUSTOMDRAW.
//
// CUSTOMDRAW widgets carry an optional NEUI_ASSET_KIND_BEHAVIOR via
// widgets->set_asset. When attached, the shared dispatch
// (hosts/shared/behavior_runtime.h) interprets mouse / key / wheel events
// and writes target attrs in the widget's AttrBag, firing
// NEUI_EVENT_ATTR_CHANGED on user-driven mutations. These helpers wrap
// the host callbacks (invalidate, emit, popup) so the dispatch is a
// single call from the view's input methods.

namespace macos_host {

  static neui_detail::BehaviorAsset*
  resolve_widget_behavior_macos(WidgetData& wd, Session* sess)
  {
    if (!sess) return nullptr;
    neui_asset_t a = wd.behavior_asset;
    if (a.id == asset_none.id) return nullptr;
    if (((a.id >> 16) & 0xffff) != (sess->session_id() & 0xffff)) return nullptr;
    auto* e = sess->_asset_manager.get_slot(a.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    return e->behavior.get();
  }

  // Forward-decl: defined later in this TU after the painted view + popup
  // helper are in scope.
  static int macos_behavior_popup_menu(void* host_data, int local_x, int local_y,
                                         const char* const* items);

  static void macos_behavior_invalidate(void* host_data)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->native_control) return;
    NSView* v = (__bridge NSView*)wd->native_control;
    [v setNeedsDisplay:YES];
  }

  // Invalidate the painted view if its compound has state-filtered layers.
  // Called from NEUINativePaintedView's hover / press transitions so the
  // compound repaints when NEUI_LAYER_STATE_* changes.
  static void macos_invalidate_if_state_filtered_compound(WidgetData* wd)
  {
    if (!wd || !wd->session) return;
    if (wd->compound_asset.id == asset_none.id) return;
    if (((wd->compound_asset.id >> 16) & 0xffff)
          != (wd->session->session_id() & 0xffff)) return;
    auto* e = wd->session->_asset_manager.get_slot(wd->compound_asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return;
    if (!neui_detail::compound_has_state_filters(*e->compound)) return;
    if (!wd->native_control) return;
    NSView* v = (__bridge NSView*)wd->native_control;
    [v setNeedsDisplay:YES];
  }

  static void macos_behavior_emit_attr_changed(void* host_data,
                                                 const char* attr_key, float value)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->session || !wd->emit_events) return;
    neui_event_t ev = {};
    ev.type                 = NEUI_EVENT_ATTR_CHANGED;
    ev.data.attr.widget.id  = wd->widget_id;
    ev.data.attr.attr_key   = attr_key;
    ev.data.attr.value      = value;
    wd->session->dispatch_event(&ev);
  }

  static neui_detail::BehaviorDispatchCtx make_behavior_ctx_macos(WidgetData& wd)
  {
    neui_detail::BehaviorDispatchCtx ctx{};
    ctx.bag      = &neui_detail::ensure_attrs(wd.attrs);
    ctx.widget_w = static_cast<float>(wd.width);
    ctx.widget_h = static_cast<float>(wd.height);
    ctx.host_data         = &wd;
    ctx.invalidate        = &macos_behavior_invalidate;
    ctx.emit_attr_changed = &macos_behavior_emit_attr_changed;
    ctx.popup_menu        = &macos_behavior_popup_menu;
    return ctx;
  }

  // Dispatch an already-built neui_event_t through the widget's behavior
  // asset. Returns true if a handler consumed the event.
  static bool dispatch_behavior_mouse(uint32_t widget_id, neui_event_t* ev,
                                        float local_x, float local_y)
  {
    Session* sess = nullptr;
    auto* wd = widget_for_id(widget_id, &sess);
    if (!wd || !sess) return false;
    auto* ba = resolve_widget_behavior_macos(*wd, sess);
    if (!ba) return false;
    if (!wd->behavior_rt)
      wd->behavior_rt = std::make_unique<neui_detail::BehaviorRuntime>();
    auto ctx = make_behavior_ctx_macos(*wd);
    return neui_detail::behavior_dispatch_mouse(*ba, *wd->behavior_rt, ctx,
                                                  ev, local_x, local_y);
  }

  static bool dispatch_behavior_key(uint32_t widget_id, uint32_t keycode,
                                      uint32_t modifiers)
  {
    Session* sess = nullptr;
    auto* wd = widget_for_id(widget_id, &sess);
    if (!wd || !sess) return false;
    auto* ba = resolve_widget_behavior_macos(*wd, sess);
    if (!ba) return false;
    if (!wd->behavior_rt)
      wd->behavior_rt = std::make_unique<neui_detail::BehaviorRuntime>();
    auto ctx = make_behavior_ctx_macos(*wd);
    return neui_detail::behavior_dispatch_key(*ba, *wd->behavior_rt, ctx,
                                                keycode, modifiers);
  }

  static int macos_behavior_popup_menu(void* host_data, int local_x, int local_y,
                                         const char* const* items)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->native_control || !items) return 0;
    NSView* anchor = (__bridge NSView*)wd->native_control;
    return run_popup_menu_macos(anchor, local_x, local_y, items);
  }

} // namespace macos_host

// ---------------------------------------------------------------------------
// Module-private state.

namespace {

// Live APPWINDOW count. Hits 0 -> [NSApp stop:nil] + post wake-up.
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

// Make native text editing self-sufficient. Unlike the win32 native Edit
// control (which handles Ctrl+C/X/V/A itself), a Cocoa text field only
// receives CmdC/CmdX/CmdV/CmdA/CmdZ when a main-menu item claims those key
// equivalents - AppKit does not synthesize a standard Edit menu. Rather than
// force every client to add Cut/Copy/Paste items, route the standard editing
// shortcuts here through the same command path as menu / NEUI_API_COMMANDS.
//
// Order: NSApp matches main-menu key equivalents first, so a client-defined
// Edit > Undo (CmdZ) still wins; this override only fires for shortcuts the
// menu didn't claim. invoke_focused_command_macos returns false when no text
// responder consumes the action, so we fall through to super in that case.
- (BOOL)performKeyEquivalent:(NSEvent*)event
{
  if (event.modifierFlags & NSEventModifierFlagCommand) {
    NSString* chars = event.charactersIgnoringModifiers.lowercaseString;
    bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    uint32_t cmd = 0;
    if      ([chars isEqualToString:@"c"]) cmd = NEUI_CMD_COPY;
    else if ([chars isEqualToString:@"v"]) cmd = NEUI_CMD_PASTE;
    else if ([chars isEqualToString:@"x"]) cmd = NEUI_CMD_CUT;
    else if ([chars isEqualToString:@"a"]) cmd = NEUI_CMD_SELECT_ALL;
    else if ([chars isEqualToString:@"z"]) cmd = shift ? NEUI_CMD_REDO : NEUI_CMD_UNDO;
    if (cmd && macos_host::invoke_focused_command_macos(cmd))
      return YES;
  }
  return [super performKeyEquivalent:event];
}
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
static constexpr float NEUI_KNOB_SWEEP_RAD   = 4.71238898f;  // 1.5*PI (270deg)
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

// Only CUSTOMDRAW takes keyboard focus - it forwards keys to the client.
// KNOB / IMAGE / SECTION don't need key input.
- (BOOL)acceptsFirstResponder
{
  auto* wd = macos_host::widget_for_id(widget_id);
  return wd && wd->type && !strcmp(wd->type, NEUI_W_CUSTOMDRAW);
}

- (void)keyDown:(NSEvent*)event
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  bool cd = wd && sess && wd->emit_events && wd->type
            && !strcmp(wd->type, NEUI_W_CUSTOMDRAW);
  if (!cd) { [super keyDown:event]; return; }

  uint32_t mods = neui_detail::mac_modifiers_to_neui(event.modifierFlags);
  uint32_t kc   = neui_detail::mac_keycode_to_neui(event.keyCode);
  neui_event_t kd = {};
  kd.type                = NEUI_EVENT_KEYDOWN;
  kd.data.key.widget     = { wd->widget_id };
  kd.data.key.keycode    = kc;
  kd.data.key.modifiers  = mods;
  sess->dispatch_event(&kd);
  macos_host::dispatch_behavior_key(widget_id, kc, mods);

  // KEYCHAR for the produced text. Skip when Command is held (those are
  // shortcuts, not text - matches win32's WM_CHAR behaviour) and skip the
  // NSFunctionKey private-use range (arrows / F-keys come via KEYDOWN only).
  NSString* chars = event.characters;
  if (chars.length > 0 && !(event.modifierFlags & NSEventModifierFlagCommand)) {
    NSUInteger i = 0;
    while (i < chars.length) {
      unichar c = [chars characterAtIndex:i];
      uint32_t cp;
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars.length) {
        unichar lo = [chars characterAtIndex:i + 1];
        cp = 0x10000u + ((uint32_t)(c - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
        i += 2;
      } else {
        cp = c; i += 1;
      }
      if (cp >= 0xF700 && cp <= 0xF8FF) continue;  // function-key range
      neui_event_t kc = {};
      kc.type               = NEUI_EVENT_KEYCHAR;
      kc.data.key.widget    = { wd->widget_id };
      kc.data.key.keycode   = cp;
      kc.data.key.modifiers = mods;
      sess->dispatch_event(&kc);
    }
  }
}

- (void)keyUp:(NSEvent*)event
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  bool cd = wd && sess && wd->emit_events && wd->type
            && !strcmp(wd->type, NEUI_W_CUSTOMDRAW);
  if (!cd) { [super keyUp:event]; return; }
  neui_event_t ku = {};
  ku.type               = NEUI_EVENT_KEYUP;
  ku.data.key.widget    = { wd->widget_id };
  ku.data.key.keycode   = neui_detail::mac_keycode_to_neui(event.keyCode);
  ku.data.key.modifiers = neui_detail::mac_modifiers_to_neui(event.modifierFlags);
  sess->dispatch_event(&ku);
}

- (void)dealloc
{
  // The IMAGE bitmap now lives in the session's MacOSAssetManager, not on
  // the view; its per-ctx GPU cache for this render_ctx is dropped by
  // release_native_control_macos before teardown. Here we only destroy the
  // view's own render context.
  auto* backend = neui_cg_backend::get_backend();
  if (backend && render_ctx)
    backend->destroy_context(render_ctx);
  render_ctx = nullptr;
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
  // approach as the xpl host's paint_frame. SECTION uses a transparent
  // clear so the un-painted header band shows the parent's pixels.
  bool is_section = wd->type && !strcmp(wd->type, NEUI_W_SECTION);
  uint32_t clear = is_section
    ? 0x00000000
    : neui_detail::color(neui_detail::ColorRole::panel_bg);
  if (!is_section && wd->attrs && wd->attrs->has(NEUI_ATTR_BACKGROUND))
    clear = (uint32_t)wd->attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
  backend->begin_frame(render_ctx, clear);

  // Disabled painted widgets (IMAGE / KNOB / CUSTOMDRAW / SECTION) draw their
  // content at half opacity over the (full-opacity) background clear, matching
  // the xpl host's per-widget push_alpha(0.5) dim. begin_frame resets the
  // alpha stack, so this must be pushed after it; popped before end_frame.
  bool dim_disabled = !wd->enabled;
  if (dim_disabled) backend->push_alpha(render_ctx, 0.5f);

  if (wd->type && !strcmp(wd->type, NEUI_W_IMAGE)) {
    // Resolve the widget's bitmap asset (set via a set_text path or a
    // set_asset handle) against the session asset manager. The lazy per-ctx
    // GPU upload + draw is delegated to macos_painter_draw_asset_thunk - the
    // same path CUSTOMDRAW + compound use - so there is no per-view cache.
    // We only compute the aspect-fit destination rect and rotation here.
    if (wd->image_asset.id != asset_none.id &&
        ((wd->image_asset.id >> 16) & 0xffff) == (sess->session_id() & 0xffff)) {
      auto* entry = sess->_asset_manager.get_slot(wd->image_asset.id & 0xffff);
      if (entry && entry->kind == NEUI_ASSET_KIND_BITMAP && entry->scale > 0.0f) {
        // Aspect-preserving fit (letterbox / pillarbox, centred). Same shape
        // as the other hosts.
        float vw = (float)sz.width, vh = (float)sz.height;
        float bw = (float)entry->width_px  / entry->scale;
        float bh = (float)entry->height_px / entry->scale;
        if (bw > 0.0f && bh > 0.0f && vw > 0.0f && vh > 0.0f) {
          float scale = (bw / bh > vw / vh) ? (vw / bw) : (vh / bh);
          float dw = bw * scale, dh = bh * scale;
          float dx = (vw - dw) * 0.5f, dy = (vh - dh) * 0.5f;
          // Honour NEUI_ATTR_ROTATION via the renderer transform stack -
          // same shape as the xpl host's IMAGE paint path.
          float rot = wd->attrs ? wd->attrs->get_float(NEUI_ATTR_ROTATION, 0.0f) : 0.0f;
          if (rot != 0.0f) {
            backend->push_transform(render_ctx);
            backend->translate(render_ctx, dx + dw * 0.5f, dy + dh * 0.5f);
            backend->rotate(render_ctx, rot);
            backend->translate(render_ctx, -dw * 0.5f, -dh * 0.5f);
            macos_host::macos_painter_draw_asset_thunk(
              sess, backend, render_ctx, wd->image_asset, 0, 0, dw, dh);
            backend->pop_transform(render_ctx);
          } else {
            macos_host::macos_painter_draw_asset_thunk(
              sess, backend, render_ctx, wd->image_asset, dx, dy, dw, dh);
          }
        }
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
                             value, /*focused*/false, polarity, steps, value_text,
                             wd->attrs.get());
  } else if (wd->type && !strcmp(wd->type, NEUI_W_CUSTOMDRAW)) {
    // CUSTOMDRAW dispatch - either paint the attached compound or
    // forward a WIDGET_PAINT event. Mirror of paint_customdraw_w32.
    // Outer push_transform + push_clip(0..w,0..h) so client state
    // changes can't bleed past the widget rect.
    if (backend->push_transform) backend->push_transform(render_ctx);
    if (backend->push_clip)      backend->push_clip(render_ctx, 0.0f, 0.0f,
                                                     (float)sz.width, (float)sz.height);

    neui_painter painter{};
    painter.backend          = backend;
    painter.ctx              = render_ctx;
    painter.host_token       = sess;
    painter.draw_asset_thunk = &macos_host::macos_painter_draw_asset_thunk;

    // Resolve compound (if any). asset_none -> no compound -> dispatch
    // WIDGET_PAINT.
    neui_detail::CompoundAsset* ca = nullptr;
    if (wd->compound_asset.id != asset_none.id &&
        ((wd->compound_asset.id >> 16) & 0xffff) == (sess->session_id() & 0xffff))
    {
      auto* e = sess->_asset_manager.get_slot(wd->compound_asset.id & 0xffff);
      if (e && e->kind == NEUI_ASSET_KIND_COMPOUND && e->compound) ca = e->compound.get();
    }

    if (ca) {
      const neui_detail::AttrBag* bag = neui_detail::attrs_readonly(wd->attrs);
      uint32_t state_mask = neui_detail::compose_widget_state(
                              wd->enabled, wd->hovered, wd->pressed);
      neui_detail::paint_compound_below(&painter, *ca,
                                          (float)sz.width, (float)sz.height, bag,
                                          state_mask);
      neui_detail::paint_compound_above(&painter, *ca,
                                          (float)sz.width, (float)sz.height, bag,
                                          state_mask);
    } else if (wd->emit_events) {
      bool focused = (self.window.isKeyWindow
                       && self.window.firstResponder == self);
      neui_event_t ev{};
      ev.type = NEUI_EVENT_WIDGET_PAINT;
      ev.data.paint.widget.id   = wd->widget_id;
      ev.data.paint.painter_api = &neui_detail::k_painter_api;
      ev.data.paint.p           = &painter;
      ev.data.paint.width       = (float)sz.width;
      ev.data.paint.height      = (float)sz.height;
      ev.data.paint.focused     = focused;
      sess->dispatch_event(&ev);
    }

    if (backend->pop_clip)      backend->pop_clip(render_ctx);
    if (backend->pop_transform) backend->pop_transform(render_ctx);
  } else if (is_section) {
    // SECTION body + title chip. The shared helper leaves the band's
    // non-chip area UNPAINTED (transparent clear above) so the parent's
    // pixels show through - matches the xpl host's visual.
    //
    // Direction-aware lift: SECTION_BG_LIFT (+24) lifts the frame_bg
    // towards white, but on macOS in light mode frame_bg already lives
    // near white (windowBackgroundColor ~= 0xECECEC, often 0xFFFFFF under
    // newer appearances). When the lift saturates, the section becomes
    // invisible against the NSWindow background. Detect that and shade
    // down instead so the section reads as a depressed panel.
    uint32_t bg;
    if (wd->attrs && wd->attrs->has(NEUI_ATTR_BACKGROUND)) {
      bg = (uint32_t)wd->attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
    } else {
      uint32_t fbg    = neui_detail::color(neui_detail::ColorRole::frame_bg);
      uint32_t lifted = neui_detail::shade(fbg,  neui_detail::SECTION_BG_LIFT);
      if (lifted == fbg)
        lifted = neui_detail::shade(fbg, -neui_detail::SECTION_BG_LIFT);
      bg = lifted;
    }
    const char* align = wd->attrs ? wd->attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
    uint32_t text_argb = neui_detail::color(neui_detail::ColorRole::text_primary);
    neui_detail::paint_section(backend, render_ctx,
                                 0.0f, 0.0f, (float)sz.width, (float)sz.height,
                                 wd->text.c_str(),
                                 bg, align, text_argb,
                                 wd->attrs.get());
  }

  if (dim_disabled) backend->pop_alpha(render_ctx);

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

// --- CUSTOMDRAW raw-input plumbing -----------------------------------------
// CUSTOMDRAW widgets forward pointer input to the client as NEUI_EVENT_MOUSE_*
// (parity with the win32 host's subclass proc). KNOB drives its own value and
// IMAGE / SECTION are non-interactive, so only CUSTOMDRAW opts in here.

- (bool)customDrawWantsInput
{
  auto* wd = macos_host::widget_for_id(widget_id);
  return wd && wd->enabled && wd->emit_events && wd->type
         && !strcmp(wd->type, NEUI_W_CUSTOMDRAW);
}

- (NSPoint)localPoint:(NSEvent*)event
{
  // Widget-local, flipped (top-left origin) - matches the WIDGET_PAINT origin.
  return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)dispatchMouse:(neui_event_type_t)type at:(NSPoint)p buttonmap:(uint32_t)bmap
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  neui_event_t ev = {};
  ev.type                 = type;
  ev.data.mouse.widget    = { wd->widget_id };
  ev.data.mouse.x         = (int)p.x;
  ev.data.mouse.y         = (int)p.y;
  ev.data.mouse.buttonmap = bmap;
  sess->dispatch_event(&ev);
  // Run the behavior pass after the client has had first chance. The
  // dispatch is a no-op when no behavior asset is attached.
  macos_host::dispatch_behavior_mouse(widget_id, &ev, (float)p.x, (float)p.y);
}

// Tracking area drives MOUSE_MOVE / ENTER / LEAVE. Rebuilt whenever the view
// geometry changes (AppKit calls this after every setFrame:).
- (void)updateTrackingAreas
{
  [super updateTrackingAreas];
  for (NSTrackingArea* ta in [self.trackingAreas copy])
    [self removeTrackingArea:ta];
  NSTrackingArea* ta = [[NSTrackingArea alloc]
    initWithRect:self.bounds
    options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved
             | NSTrackingActiveInKeyWindow)
    owner:self userInfo:nil];
  [self addTrackingArea:ta];
}

- (void)mouseEntered:(NSEvent*)event
{
  if (![self customDrawWantsInput]) { [super mouseEntered:event]; return; }
  if (auto* wd = macos_host::widget_for_id(widget_id); wd && !wd->hovered) {
    wd->hovered = true;
    macos_host::macos_invalidate_if_state_filtered_compound(wd);
  }
  [self dispatchMouse:NEUI_EVENT_MOUSE_ENTER at:[self localPoint:event] buttonmap:0];
}
- (void)mouseExited:(NSEvent*)event
{
  if (![self customDrawWantsInput]) { [super mouseExited:event]; return; }
  if (auto* wd = macos_host::widget_for_id(widget_id); wd && wd->hovered) {
    wd->hovered = false;
    macos_host::macos_invalidate_if_state_filtered_compound(wd);
  }
  [self dispatchMouse:NEUI_EVENT_MOUSE_LEAVE at:[self localPoint:event] buttonmap:0];
}
- (void)mouseMoved:(NSEvent*)event
{
  if (![self customDrawWantsInput]) { [super mouseMoved:event]; return; }
  [self dispatchMouse:NEUI_EVENT_MOUSE_MOVE at:[self localPoint:event] buttonmap:0];
}

- (void)mouseDown:(NSEvent*)event
{
  if ([self isKnob]) {
    // Disabled knob ignores all pointer input (no drag, no double-click
    // reset). dragging stays false, so mouseDragged / mouseUp are inert too.
    {
      auto* wd = macos_host::widget_for_id(widget_id);
      if (wd && !wd->enabled) return;
    }
    // Double-click -> reset to NEUI_PARAM_DEFAULT.
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
    return;
  }
  if ([self customDrawWantsInput]) {
    // Grab keyboard focus so subsequent keyDown / keyUp route here (and the
    // paint pass reports focused = YES). NSView doesn't auto-focus on click.
    [self.window makeFirstResponder:self];
    if (auto* wd = macos_host::widget_for_id(widget_id); wd && !wd->pressed) {
      wd->pressed = true;
      macos_host::macos_invalidate_if_state_filtered_compound(wd);
    }
    neui_event_type_t t = (event.clickCount >= 2)
      ? NEUI_EVENT_MOUSE_BUTTON_DBLCLICK : NEUI_EVENT_MOUSE_BUTTON_DOWN;
    [self dispatchMouse:t at:[self localPoint:event] buttonmap:1];
    return;
  }
  [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent*)event
{
  if ([self isKnob]) {
    if (!dragging) { [super mouseDragged:event]; return; }
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
    return;
  }
  if ([self customDrawWantsInput]) {
    // A held-button drag surfaces as MOUSE_MOVE with NEUI_MK_LBUTTON set
    // so behavior dispatch (hosts/shared/behavior_runtime.h) can tell
    // the difference between an in-flight drag and a release-then-move.
    // Clients that don't use behaviors still receive the MOVE event as
    // before and can ignore the buttonmap field.
    [self dispatchMouse:NEUI_EVENT_MOUSE_MOVE at:[self localPoint:event] buttonmap:NEUI_MK_LBUTTON];
    return;
  }
  [super mouseDragged:event];
}

- (void)mouseUp:(NSEvent*)event
{
  if ([self isKnob]) { dragging = false; return; }
  if ([self customDrawWantsInput]) {
    if (auto* wd = macos_host::widget_for_id(widget_id); wd && wd->pressed) {
      wd->pressed = false;
      macos_host::macos_invalidate_if_state_filtered_compound(wd);
    }
    [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_UP at:[self localPoint:event] buttonmap:0];
    return;
  }
  [super mouseUp:event];
}

- (void)rightMouseDown:(NSEvent*)event
{
  if ([self isKnob]) {
    auto* wd = macos_host::widget_for_id(widget_id);
    if (wd && !wd->enabled) return;  // disabled knob: no context menu
    NSPoint p = [self localPointForKnobEvent:event];
    static const char* k_items[] = { "Reset to default", nullptr };
    int pick = macos_host::run_popup_menu_macos(self, (int)p.x, (int)p.y, k_items);
    if (pick == 1) {
      float def = 0.0f;
      if (wd && wd->attrs) def = neui_clamp01(wd->attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
      [self setKnobValueFromUser:def];
    }
    return;
  }
  if ([self customDrawWantsInput]) {
    [self dispatchMouse:NEUI_EVENT_MOUSE_RBUTTON_DOWN at:[self localPoint:event] buttonmap:0];
    return;
  }
  [super rightMouseDown:event];
}
- (void)rightMouseUp:(NSEvent*)event
{
  if ([self customDrawWantsInput]) {
    [self dispatchMouse:NEUI_EVENT_MOUSE_RBUTTON_UP at:[self localPoint:event] buttonmap:0];
    return;
  }
  [super rightMouseUp:event];
}

- (void)scrollWheel:(NSEvent*)event
{
  if ([self isKnob]) {
    {
      auto* wd = macos_host::widget_for_id(widget_id);
      if (wd && !wd->enabled) return;  // disabled knob ignores the wheel
    }
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
    return;
  }
  if ([self customDrawWantsInput]) {
    CGFloat raw = event.scrollingDeltaY;
    int delta = (raw > 0) ? 1 : (raw < 0 ? -1 : 0);
    if (delta == 0) return;
    macos_host::Session* sess = nullptr;
    auto* wd = macos_host::widget_for_id(widget_id, &sess);
    if (!wd || !sess) return;
    NSPoint p = [self localPoint:event];
    neui_event_t ev = {};
    ev.type              = NEUI_EVENT_MOUSE_WHEEL;
    ev.data.wheel.widget = { wd->widget_id };
    ev.data.wheel.x      = (int)p.x;
    ev.data.wheel.y      = (int)p.y;
    ev.data.wheel.delta  = delta;
    sess->dispatch_event(&ev);
    macos_host::dispatch_behavior_mouse(widget_id, &ev, (float)p.x, (float)p.y);
    return;
  }
  [super scrollWheel:event];
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
  // frame close (mirrors the win32 / xpl host's WM_CLOSE -> APP_QUIT path);
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

// Frame focus -> NEUI_EVENT_WIDGET_FOCUS for the frame widget. Mirror of the
// win32 host's WM_SETFOCUS / WM_KILLFOCUS path. Clients see logical focus at
// the frame granularity (Tier B per-widget focus proxies are deferred).
- (void)dispatchFrameFocus:(bool)gained
{
  if (!session || !session->_widgets.exists(widget_index)) return;
  neui_event_t ev = {};
  ev.type              = NEUI_EVENT_WIDGET_FOCUS;
  ev.data.focus.widget = { session->_widgets[widget_index].widget_id };
  ev.data.focus.focused = gained;
  session->dispatch_event(&ev);
}

- (void)windowDidBecomeKey:(NSNotification*)note { (void)note; [self dispatchFrameFocus:true];  }
- (void)windowDidResignKey:(NSNotification*)note { (void)note; [self dispatchFrameFocus:false]; }

// Frame resize -> NEUI_EVENT_RESIZE with the new content size in logical
// pixels. Mirror of the win32 host's WM_SIZE path. The content view is
// isFlipped with a backing-scale CTM, so contentView.bounds is already in
// logical points (= logical px at 96 DPI).
- (void)windowDidResize:(NSNotification*)note
{
  if (!session || !session->_widgets.exists(widget_index)) return;
  NSWindow* win = (NSWindow*)note.object;
  if (![win isKindOfClass:[NSWindow class]]) return;
  NSSize sz = win.contentView ? win.contentView.bounds.size : win.frame.size;

  auto& wd = session->_widgets[widget_index];
  wd.width  = (int)sz.width;
  wd.height = (int)sz.height;

  neui_event_t ev = {};
  ev.type               = NEUI_EVENT_RESIZE;
  ev.data.resize.widget = { wd.widget_id };
  ev.data.resize.width  = (int)sz.width;
  ev.data.resize.height = (int)sz.height;
  session->dispatch_event(&ev);
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

  // Painter draw_asset thunk - resolves a neui_asset_t through the
  // owning session's MacOSAssetManager, lazy-uploads a CGImage on first
  // use per (asset, ctx) pair, then calls backend->draw_bitmap. Mirror of
  // hosts/win32/widgets.cpp::w32_painter_draw_asset_thunk - CG's
  // get_context_generation is a constant so the generation comparison
  // is a no-op here, but the shape is kept for symmetry.
  void NEUI_ABI macos_painter_draw_asset_thunk(void* host_token,
                                                 neui_render_backend_t* backend,
                                                 neui_render_ctx_t ctx,
                                                 neui_asset_t asset,
                                                 float x, float y,
                                                 float w, float h)
  {
    auto* s = static_cast<Session*>(host_token);
    if (!s || !backend || !ctx) return;
    if (asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    uint32_t slot = asset.id & 0xffff;
    auto* entry = s->_asset_manager.get_slot(slot);
    if (!entry) return;
    const uint32_t gen = backend->get_context_generation
      ? backend->get_context_generation(ctx) : 0u;
    auto it = entry->bitmaps.find(ctx);
    if (it != entry->bitmaps.end() && it->second.generation != gen) {
      if (backend->destroy_bitmap && it->second.bmp)
        backend->destroy_bitmap(ctx, it->second.bmp);
      entry->bitmaps.erase(it);
      it = entry->bitmaps.end();
    }
    if (it == entry->bitmaps.end()) {
      if (!backend->create_bitmap) return;
      void* bmp = backend->create_bitmap(ctx,
                                          entry->width_px, entry->height_px,
                                          entry->pixels.data(),
                                          entry->scale);
      if (!bmp) return;
      it = entry->bitmaps.emplace(ctx, MacOSCtxBitmap{ bmp, gen }).first;
    }
    if (backend->draw_bitmap)
      backend->draw_bitmap(ctx, it->second.bmp,
                            0.0f, 0.0f, 0.0f, 0.0f, // full bitmap
                            x, y, w, h);
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
// NEUIPopupCollector - records the picked NSMenuItem's tag for the blocking
// popup_menu helper. Each item's action targets this object; after the
// nested popup tracking loop returns, pickedTag holds the 1-based choice
// (or 0 if the menu was dismissed without a pick).

@interface NEUIPopupCollector : NSObject
{
@public
  int pickedTag;
}
- (void)neuiPopupPick:(id)sender;
@end

@implementation NEUIPopupCollector
- (instancetype)init { if ((self = [super init])) pickedTag = 0; return self; }
- (void)neuiPopupPick:(id)sender
{
  NSMenuItem* mi = (NSMenuItem*)sender;
  if ([mi isKindOfClass:[NSMenuItem class]]) pickedTag = (int)mi.tag;
}
@end

namespace macos_host {
  int run_popup_menu_macos(NSView* anchor, int x, int y, const char* const* items)
  {
    if (!items) return 0;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    NEUIPopupCollector* collector = [[NEUIPopupCollector alloc] init];

    int idx = 0;
    for (const char* const* p = items; *p; ++p) {
      ++idx;  // separators consume an index slot (matches the win32 host)
      const char* t = *p;
      if (t[0] == '-' && t[1] == '\0') {
        [menu addItem:[NSMenuItem separatorItem]];
        continue;
      }
      NSMenuItem* mi = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithUTF8String:t]
               action:@selector(neuiPopupPick:)
        keyEquivalent:@""];
      mi.target  = collector;
      mi.tag     = idx;
      mi.enabled = YES;
      [menu addItem:mi];
    }

    if (anchor) {
      [menu popUpMenuPositioningItem:nil
                          atLocation:NSMakePoint(x, y)
                              inView:anchor];
    } else {
      NSWindow* kw = [NSApp keyWindow];
      if (kw.contentView)
        [menu popUpMenuPositioningItem:nil
                            atLocation:NSMakePoint(x, y)
                                inView:kw.contentView];
    }
    return collector->pickedTag;
  }
}

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

  // Routed-command binding (tree->set_menu_cmd): a built-in command goes to
  // the focused widget first. If it's consumed there, the client does NOT see
  // TREE_ITEM_ACTIVATED. Mirror of the win32 host's dispatch_menu_event.
  auto cmd_it = wd.tree_items.find(item_id);
  if (cmd_it != wd.tree_items.end()) {
    uint32_t cmd = cmd_it->second.menu_cmd;
    if (cmd != 0 && cmd < NEUI_CMD_USER_BASE &&
        macos_host::invoke_focused_command_macos(cmd)) {
      return;  // consumed by the focused widget
    }
  }

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

// Double-click -> TREE_ITEM_ACTIVATED. Wired via the table view's
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
  // Forward decl - defined below alongside the descendant walker.
  static bool widget_is_native_container(const WidgetData& w);

  // Walk up the widget tree from `idx` until we find an ancestor that
  // can host a child view. Two cases (return the closest one):
  //   - A container widget (SECTION) with an NSView native_control -
  //     return that view, so SECTION children layout in section-local
  //     coords.
  //   - A frame (APPWINDOW / DIALOG / PLUGWINDOW) with native_window -
  //     return its NSWindow.contentView.
  // get_all_parents returns parents in nearest-first order, so the first
  // match wins. Returns nil if no usable ancestor exists yet.
  static NSView* find_parent_content_view(Session* s, uint32_t idx)
  {
    if (!s) return nil;
    auto parents = s->_widgets.get_all_parents(idx);
    for (uint32_t p : parents) {
      if (p == 0) continue;
      if (!s->_widgets.exists(p)) continue;
      auto& pw = s->_widgets[p];
      if (widget_is_native_container(pw) && pw.native_control) {
        id obj = (__bridge id)pw.native_control;
        if ([obj isKindOfClass:[NSView class]]) return (NSView*)obj;
      }
      if (pw.native_window) {
        NSWindow* w = native_window_from(pw.native_window);
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
    if ([obj isKindOfClass:[NEUINativePaintedView class]]) {
      // Painted views own a render context whose per-ctx GPU bitmaps are
      // cached on the session's asset manager (IMAGE source + CUSTOMDRAW
      // compound assets). Drop that cache while the context is still valid -
      // the view's dealloc destroys the context right after. Mirror of the
      // win32 PaintedWndProc WM_DESTROY -> _asset_manager.release_context.
      NEUINativePaintedView* pv = (NEUINativePaintedView*)obj;
      if (wd.session && pv->render_ctx)
        wd.session->_asset_manager.release_context(pv->render_ctx,
                                                   neui_cg_backend::get_backend());
      [pv removeFromSuperview];
    } else if ([obj isKindOfClass:[NSView class]]) {
      [(NSView*)obj removeFromSuperview];
    } else if ([obj isKindOfClass:[NSMenu class]]) {
      if (NSApp.mainMenu == (NSMenu*)obj) NSApp.mainMenu = nil;
    }
  }

  // Request a repaint of a widget's painted view. Mirror of the win32
  // host's InvalidateRect(hwnd, nullptr, FALSE). Called from
  // Session::invalidate_widgets_with_compound + widget_invalidate +
  // the compound-attached attribute-setter invalidation hooks. No-op
  // for widgets without an NSView backing (native NSControl widgets get
  // their repaint through AppKit's normal invalidation path).
  void mark_widget_dirty_for_paint(WidgetData& wd)
  {
    if (!wd.native_control) return;
    id obj = (__bridge id)wd.native_control;
    if (![obj isKindOfClass:[NSView class]]) return;
    [(NSView*)obj setNeedsDisplay:YES];
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

    // Tab / Shift-Tab focus traversal: the key-view loop is built manually in
    // widget-creation order (see rebuild_key_view_loop_macos, called after the
    // descendants are created) so the order matches the win32 + xpl hosts.
    // AppKit's autorecalculatesKeyViewLoop is left OFF because it orders by
    // geometry (top-to-bottom / left-to-right), which would diverge.
    window.autorecalculatesKeyViewLoop = NO;

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

  // CSS weight (100..900, 0 = unset) -> AppKit NSFontWeight scale. Mirror of
  // the cg backend's css_weight_to_nsfontweight (kept local so window.mm
  // doesn't depend on the backend's internals).
  static CGFloat css_weight_to_nsfontweight_macos(int weight)
  {
    if (weight <= 0)  return NSFontWeightRegular;
    if (weight < 150) return NSFontWeightUltraLight;
    if (weight < 250) return NSFontWeightThin;
    if (weight < 350) return NSFontWeightLight;
    if (weight < 450) return NSFontWeightRegular;
    if (weight < 550) return NSFontWeightMedium;
    if (weight < 650) return NSFontWeightSemibold;
    if (weight < 750) return NSFontWeightBold;
    if (weight < 850) return NSFontWeightHeavy;
    return NSFontWeightBlack;
  }

  // Apply NEUI_ATTR_FONT_FAMILY / _SIZE / _WEIGHT to a native control's
  // NSFont. Mirror of the win32 host's ensure_custom_font_w32 (WM_SETFONT):
  // when none of the font attrs are set the control keeps its system font;
  // a partial set keeps the unspecified dimensions (size falls back to the
  // control's current point size). An unknown family (e.g. a Windows family
  // like "Consolas") gracefully falls back to the system font at the
  // requested size/weight. NSScrollView-hosted MULTILINE targets its
  // document NSTextView; painted widgets get their font via the cg font
  // stack instead, so this is a no-op for them.
  void apply_font_native_macos(WidgetData& wd)
  {
    if (!wd.native_control || !wd.attrs) return;
    id obj = (__bridge id)wd.native_control;
    NSControl*  ctrl = nil;
    NSTextView* tv   = nil;
    if ([obj isKindOfClass:[NSControl class]]) {
      ctrl = (NSControl*)obj;
    } else if ([obj isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)obj).documentView;
      if ([doc isKindOfClass:[NSTextView class]]) tv = (NSTextView*)doc;
    }
    if (!ctrl && !tv) return;  // tables / painted views: not handled here

    const char* family = wd.attrs->get_string(NEUI_ATTR_FONT_FAMILY);
    float       size   = wd.attrs->get_float (NEUI_ATTR_FONT_SIZE,   0.0f);
    int         weight = wd.attrs->get_int   (NEUI_ATTR_FONT_WEIGHT, 0);
    bool fam_set = family && *family;
    bool sz_set  = size > 0.0f;
    bool wt_set  = weight > 0;
    if (!fam_set && !sz_set && !wt_set) return;  // no override

    NSFont* cur = ctrl ? ctrl.font : tv.font;
    CGFloat eff_size = sz_set ? (CGFloat)size
                              : (cur ? cur.pointSize : [NSFont systemFontSize]);
    CGFloat ns_weight = css_weight_to_nsfontweight_macos(weight);

    NSFont* font = nil;
    if (fam_set) {
      NSString* fam = [NSString stringWithUTF8String:family];
      if (fam) {
        NSFontDescriptor* desc = [NSFontDescriptor fontDescriptorWithFontAttributes:@{
          NSFontFamilyAttribute : fam,
          NSFontTraitsAttribute : @{ NSFontWeightTrait : @(ns_weight) },
        }];
        font = [NSFont fontWithDescriptor:desc size:eff_size];
      }
    }
    if (!font) font = [NSFont systemFontOfSize:eff_size weight:ns_weight];
    if (ctrl) ctrl.font = font;
    else      tv.font   = font;
  }

  // Push WidgetData geometry (x/y/width/height, logical px) into the live
  // native object. Mirror of the win32 host's SetWindowPos path. Child
  // controls get a parent-relative frame (the content view is isFlipped, so
  // (x, y) is top-left); frames get a content-size + screen re-origin
  // (top-left semantics, converted to AppKit's bottom-left screen space);
  // painted views also resize their CG render context. No-op until the
  // native object exists.
  void apply_geometry_native_macos(WidgetData& wd)
  {
    if (wd.native_window) {
      NSWindow* win = (__bridge NSWindow*)wd.native_window;
      [win setContentSize:NSMakeSize(wd.width, wd.height)];
      if (NSScreen.mainScreen) {
        CGFloat sh = NSScreen.mainScreen.frame.size.height;
        [win setFrameTopLeftPoint:NSMakePoint(wd.x, sh - wd.y)];
      }
      return;
    }
    if (!wd.native_control) return;
    id obj = (__bridge id)wd.native_control;
    if (![obj isKindOfClass:[NSView class]]) return;
    NSView* v = (NSView*)obj;
    [v setFrame:NSMakeRect(wd.x, wd.y, wd.width, wd.height)];
    if ([obj isKindOfClass:[NEUINativePaintedView class]]) {
      NEUINativePaintedView* pv = (NEUINativePaintedView*)obj;
      auto* backend = neui_cg_backend::get_backend();
      if (backend && backend->resize && pv->render_ctx)
        backend->resize(pv->render_ctx, (uint32_t)wd.width, (uint32_t)wd.height);
      [pv setNeedsDisplay:YES];
    }
  }

  // Push WidgetData::enabled into the live native control. Mirror of the
  // win32 host's EnableWindow path (hosts/win32/widgets.cpp::set_enabled +
  // the deferred apply in create_child_windows). Called from w_set_enabled
  // (live change) and from create_native_for_widget (deferred apply right
  // after the NSView/NSControl is instantiated). No-op until the control
  // exists - the flag lives on WidgetData and is re-applied at creation.
  //
  // Three shapes:
  //  - NEUINativePaintedView (IMAGE / KNOB / CUSTOMDRAW / SECTION): no
  //    NSControl setEnabled:; the dim is applied in drawRect: (push_alpha)
  //    and input is gated in the mouse handlers. Just request a repaint.
  //  - NSControl leaves (LABEL / BUTTON / INPUTBOX / CHECKBOX / COMBOBOX /
  //    SLIDER): drive [NSControl setEnabled:] directly - AppKit greys the
  //    control and stops routing input to it.
  //  - NSScrollView-hosted controls (LISTBOX / TREEVIEW = NSTableView /
  //    NSOutlineView, both NSControls; MULTILINE = NSTextView, which is not
  //    an NSControl): reach the document view and disable it there.
  void apply_enabled_native_macos(WidgetData& wd)
  {
    if (!wd.native_control) return;
    id obj = (__bridge id)wd.native_control;
    BOOL en = wd.enabled ? YES : NO;

    if ([obj isKindOfClass:[NEUINativePaintedView class]]) {
      [(NSView*)obj setNeedsDisplay:YES];
      return;
    }
    if ([obj isKindOfClass:[NSControl class]]) {
      [(NSControl*)obj setEnabled:en];
      return;
    }
    if ([obj isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)obj).documentView;
      if ([doc isKindOfClass:[NSControl class]]) {
        [(NSControl*)doc setEnabled:en];
      } else if ([doc isKindOfClass:[NSTextView class]]) {
        // NSTextView is not an NSControl. Re-derive editability from the
        // readonly attr when re-enabling so set_enabled doesn't clobber a
        // client's readonly intent; gate selection + dim the glyphs while
        // disabled. textColor uses the dynamic system colours so it tracks
        // light / dark appearance.
        NSTextView* tv = (NSTextView*)doc;
        tv.editable   = en ? !is_readonly(wd) : NO;
        tv.selectable = en ? YES : NO;
        tv.textColor  = en ? NSColor.textColor : NSColor.disabledControlTextColor;
      }
      return;
    }
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
    // Initialise from any check state set before widget_show. w_set_check
    // caches the logical state on the attrs but can't touch the NSButton
    // until it exists (deferred creation), so the initial image must be
    // derived from the cached value rather than hardcoded to UNCHECKED.
    int init_state = w.attrs
      ? w.attrs->get_int("neui.macoshost.checkstate", NEUI_CHECK_UNCHECKED)
      : NEUI_CHECK_UNCHECKED;
    b.image         = checkbox_image_for_state(init_state);
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
            || !strcmp(w.type, NEUI_W_KNOB)
            || !strcmp(w.type, NEUI_W_CUSTOMDRAW)
            || !strcmp(w.type, NEUI_W_SECTION)) {
      // SECTION uses a painted view for the body fill + title chip; child
      // widgets nest into it via the recursive descendant walker (see
      // create_descendants_native). Pointer-events pass through to
      // siblings via NSView default hit-test - the widget is non-
      // interactive in v1.
      NEUINativePaintedView* v = create_painted_view(w);
      [parent_content addSubview:v];
      w.native_control = (__bridge_retained void*)v;
    }

    // Apply any pre-show enabled state now that the native control exists.
    // Children default to enabled=true, so this only matters when the client
    // disabled the widget before widget_show. Mirror of the win32 host's
    // deferred EnableWindow in create_child_windows.
    if (w.native_control && !w.enabled)
      apply_enabled_native_macos(w);

    // Apply any pre-show custom font (NEUI_ATTR_FONT_*) - no-op when unset.
    if (w.native_control)
      apply_font_native_macos(w);
  }

  // True for widget types whose NSView should act as the parent container
  // for nested children. Today only SECTION needs this - the rest are
  // leaves (LABEL / BUTTON / INPUTBOX / ...) or are addressed through
  // dedicated paths (MENUBAR is an NSMenu, not an NSView). CUSTOMDRAW is
  // intentionally NOT a container - matching the win32 native host's
  // behaviour where child HWNDs of a CUSTOMDRAW HWND paint above the
  // parent's compound layer stack but don't get z-interleaved with it.
  static bool widget_is_native_container(const WidgetData& w)
  {
    return w.type && !strcmp(w.type, NEUI_W_SECTION);
  }

  // After a frame is created, walk every descendant and instantiate its
  // native control. Mirror of hosts/win32/widgets.cpp::create_child_windows.
  // MENUBAR children are special: the NSMenu was already allocated at
  // widget_create time, and instead of adding a subview we hand it to
  // [NSApp setMainMenu:] so the system menu bar shows the items.
  //
  // For container widgets (SECTION today) the recursion descends with the
  // container's NSView as the new parent, so children with their
  // section-local (x, y) lay out correctly inside it. For non-containers
  // descendants keep parenting to the same enclosing view - mirrors the
  // pre-section behaviour and avoids leaves like NSButton accidentally
  // becoming hosts for unrelated child widgets.
  // The NSView that represents this widget in the key-view loop, or nil if
  // it isn't a tab stop. The tab-stop SET mirrors the win32 host's WS_TABSTOP
  // controls (BUTTON / INPUTBOX / MULTILINE / CHECKBOX[3] / LISTBOX / COMBOBOX
  // / TREEVIEW / SLIDER / CUSTOMDRAW); LABEL / SECTION / IMAGE / KNOB are not.
  // NEUI_ATTR_TAB_STOP = 0 removes a widget explicitly (default on). For
  // NSScrollView-hosted controls the document view (table / outline / text)
  // is the responder, so it - not the scroll container - goes in the loop.
  static NSView* tab_stop_view_macos(WidgetData& w)
  {
    if (!w.native_control || !w.type || !w.visible) return nil;
    if (w.attrs && w.attrs->has(NEUI_ATTR_TAB_STOP) &&
        w.attrs->get_int(NEUI_ATTR_TAB_STOP, 1) == 0)
      return nil;
    const char* t = w.type;
    bool is_stop =
      !strcmp(t, NEUI_W_BUTTON)   || !strcmp(t, NEUI_W_INPUTBOX)  ||
      !strcmp(t, NEUI_W_MULTILINE)|| !strcmp(t, NEUI_W_CHECKBOX)  ||
      !strcmp(t, NEUI_W_CHECKBOX3)|| !strcmp(t, NEUI_W_LISTBOX)   ||
      !strcmp(t, NEUI_W_COMBOBOX) || !strcmp(t, NEUI_W_TREEVIEW)  ||
      !strcmp(t, NEUI_W_SLIDER)   || !strcmp(t, NEUI_W_CUSTOMDRAW);
    if (!is_stop) return nil;
    id obj = (__bridge id)w.native_control;
    if (![obj isKindOfClass:[NSView class]]) return nil;
    NSView* v = (NSView*)obj;
    if ([v isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)v).documentView;
      if (doc) return doc;
    }
    return v;
  }

  // Collect tab-stop views under `parent_idx` in widget-creation order
  // (pre-order DFS: node before its descendants, siblings in insertion order)
  // - identical to the win32 z-order walk + xpl collect_tab_stops.
  static void collect_tab_stops_macos(Session* s, uint32_t parent_idx,
                                        std::vector<NSView*>& out)
  {
    uint32_t i = s->_widgets.child(parent_idx);
    while (i != 0) {
      if (s->_widgets.exists(i)) {
        NSView* v = tab_stop_view_macos(s->_widgets[i]);
        if (v) out.push_back(v);
        collect_tab_stops_macos(s, i, out);
      }
      i = s->_widgets.next(i);
    }
  }

  // Wire the frame's key-view loop in creation order so Tab / Shift-Tab match
  // win32 + xpl. Disabled controls left in the chain are skipped automatically
  // by AppKit (they return NO from acceptsFirstResponder), so the loop only
  // needs rebuilding when widgets are added / removed - done on each show.
  void rebuild_key_view_loop_macos(Session* s, uint32_t frame_idx, NSWindow* window)
  {
    if (!window) return;
    std::vector<NSView*> stops;
    collect_tab_stops_macos(s, frame_idx, stops);
    window.autorecalculatesKeyViewLoop = NO;
    if (stops.empty()) return;
    const size_t n = stops.size();
    for (size_t k = 0; k < n; ++k)
      [stops[k] setNextKeyView:stops[(k + 1) % n]];
    if (!window.initialFirstResponder)
      window.initialFirstResponder = stops.front();
  }

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
        NSView* child_parent = parent_content;
        if (widget_is_native_container(cw) && cw.native_control) {
          id obj = (__bridge id)cw.native_control;
          if ([obj isKindOfClass:[NSView class]])
            child_parent = (NSView*)obj;
        }
        create_descendants_native(s, i, child_parent);
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
      // WM_CREATE -> create_child_windows path.
      NSView* cv = native_window_from(w.native_window).contentView;
      create_descendants_native(this, index, cv);
      // Build the Tab / Shift-Tab key-view loop in creation order now that all
      // descendant controls exist.
      rebuild_key_view_loop_macos(this, index, native_window_from(w.native_window));
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
