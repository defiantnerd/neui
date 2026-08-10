// iOS platform layer for the crossplatform host.
//
// MILESTONE 2: the real window + paint + touch-input wiring. This is the
// UIKit twin of platform_macos.mm:
//
//   NEUIView : UIView          drawRect: -> UIGraphicsGetCurrentContext() ->
//                              neui_cg_backend::set_current_frame ->
//                              Session::paint_frame. UIKit's drawRect context
//                              is already upper-left-origin (Y-down), matching
//                              the backend's macOS isFlipped=YES convention, so
//                              no extra Y-flip is applied. touchesBegan/Moved/
//                              Ended/Cancelled: synthesize the mouse-event
//                              stream the shared widgets expect.
//   NEUIViewController         hosts the NEUIView filling its bounds; drives a
//                              RESIZE event from viewDidLayoutSubviews /
//                              viewWillTransitionToSize: when the bounds change
//                              (rotation, split-view, Stage Manager).
//   platform_create_appwindow  builds a UIWindow on the foreground-active
//                              UIWindowScene, makes it key + visible, and wires
//                              the render context through the CG backend exactly
//                              as install_view_and_context does on macOS.
//   platform_create_dialog     presents a UIViewController modally (NON-blocking
//                              on iOS - see the divergence note below).
//
// Key divergences from the desktop contract (documented in the plan):
//   - neui->run() does NOT own the loop on iOS; UIApplicationMain does. So
//     platform_run / platform_pump_once / platform_run_modal_until are no-ops
//     returning true. The client builds its UI from the scene delegate AFTER
//     the scene connects, never from a blocking neui->run().
//   - widgets->show(dialog) is non-blocking: platform_create_dialog presents
//     and returns; the client dismisses via widgets->destroy.
//   - No mouse cursor / no hover on touch, so MOUSE_ENTER / MOUSE_LEAVE are
//     never synthesized (iPad pointer hover is a later milestone).
//
// MILESTONE 3 adds the theme (UITraitCollection light/dark + live
// traitCollectionDidChange: updates), clipboard (UIPasteboard), and image
// loader (CGImageSource) seams, plus scene-bounds frame seeding so the frame
// fills the device at create time.
//
// MILESTONE 5 adds the notify seams:
//   - The toast is painted by SHARED code (Session::paint_toast as the topmost
//     overlay in the frame's paint pass) and only needs a repaint heartbeat to
//     advance its fly-in / hold / fade-out phases. platform_start/stop_toast_
//     animation drive a ~60 Hz CADisplayLink on the NEUIView (the vsync-aligned
//     UIKit analogue of the macOS NSTimer heartbeat) that calls setNeedsDisplay
//     each tick; platform_now_ms supplies the monotonic clock the phase math
//     reads (CACurrentMediaTime). The toast anchor already sits below the safe
//     area + hamburger band because the shared code anchors to widget_client_rect
//     (M4 inset), and a tap routes to the shared handle_toast_click via the
//     existing touch path.
//   - platform_message_box presents a UIAlertController (message_box_ios.h).
//     UIAlertController is ASYNC: unlike Win32 MessageBoxExW / macOS NSAlert
//     runModal it cannot block + return the chosen button synchronously, so it
//     returns a sentinel (NEUI_MB_IOS_PENDING) immediately after presenting and
//     result-to-client delivery is a deferred follow-up. See message_box_ios.h.
//
// MILESTONE 6 adds iPad pointer hover + hardware-keyboard input, the UIKit twin
// of platform_macos.mm's keyDown:/keyUp:/mouseMoved: paths:
//   - Hardware keyboard. NEUIView becomes first-responder-capable and overrides
//     pressesBegan:/pressesEnded: (UIPress/UIKey, iOS 13.4+). Each press is
//     translated via keys_ios.h (ios_keycode_to_neui + ios_modifiers_to_neui)
//     and dispatched through the SAME path macOS uses: client first
//     (dispatch_event to the focused widget) then Session::handle_input_key, so
//     the focused xpl widget (InputBoxWidget / MultilineWidget) gets on_keydown.
//     UIKey.characters feeds KEYCHAR per codepoint so text types into INPUTBOX /
//     MULTILINE. Tab / Shift-Tab move focus via Session::focus_next, exactly
//     like the macOS keyDown: Tab branch.
//   - Menu accelerators. -keyCommands publishes a UIKeyCommand per menubar item
//     shortcut (built via the menu_ios.h neui->UIKit helpers). This gets the
//     iPad ⌘-discoverability HUD and reliably captures Command-combos.
//     DOUBLE-FIRE SPLIT: UIKeyCommand owns the menubar accelerators; pressesBegan
//     owns everything else. UIKit matches -keyCommands BEFORE delivering
//     pressesBegan: for the same chord, so a matched accelerator never reaches
//     pressesBegan: - no guard is needed and try_menubar_accel is NOT called
//     from the press path (it is the keyCommand action target instead). The
//     single dispatch point for an accelerator is the UIKeyCommand action ->
//     Session::try_menubar_accel.
//   - iPad pointer hover. A UIHoverGestureRecognizer on the content view drives
//     wd.hovered (the hover-only path the plan endorses, simpler than a full
//     UIPointerInteraction delegate): on .changed -> widget_at -> set_hovered,
//     on .ended/.cancelled -> set_hovered(0). A UIPointerInteraction is also
//     attached so the trackpad shows a pointer cursor, but hover-state
//     correctness rides the gesture recognizer. Touch (M2) keeps working
//     unchanged - hover only fires for a pointing device, never for a finger,
//     so the "no hover on touch" divergence holds (a finger never sets hovered).
//
// DnD stays a null no-op stub for a later phase; the native iOS host is still a
// stub (see plan step 7).

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>   // CACurrentMediaTime for platform_now_ms
#import <objc/runtime.h>

#include <cstdio>  // printf for the [neui-menu] diagnostic
#include <cstring>
#include <cmath>   // std::fabs for the touch-pan momentum decay

#include "host.h"
#include "platform.h"

// Full DataItem definition for the item-based clipboard path below.
#include "../shared/clipboard_item.h"

// The shared CoreGraphics backend (made iOS-aware via TARGET_OS_IPHONE
// conditionals in backends/cg/cg_backend.mm).
#include "../../backends/cg/cg_backend.h"

// iOS shared seam implementations (UIKit twins of the macOS headers). Include
// from exactly this TU - they carry inline statics (theme provider seed flag).
#include "../shared/ios/theme_provider_ios.h"
#include "../shared/ios/clipboard_ios.h"
#include "../shared/ios/image_loader_ios.h"
#include "../shared/ios/keys_ios.h"
#include "../shared/ios/menu_ios.h"
#include "../shared/ios/message_box_ios.h"
#include "../shared/ios/dnd_ios.h"
#include "../shared/dnd_modifier_suggest.h"
#include "../shared/metrics.h"

// ---------------------------------------------------------------------------
// Forward declarations.

@class NEUIView;
@class NEUIViewController;

namespace xpl_host { class Session; class WidgetData; Session* session_by_id(uint32_t); uint32_t session_count(); }

// Dispatch NEUI_EVENT_METRICS_CHANGED to a frame's client (same path as RESIZE).
// Fired when the Dynamic Type scale recomputes or the safe area / rotation
// changes, so clients can re-run their responsive layout. Defined here (host.h's
// full Session is already included) so the view methods below can call it.
inline void dispatch_metrics_changed_xpl_ios(xpl_host::Session* s, uint32_t frame_index)
{
  if (!s || !s->_widgets.exists(frame_index)) return;
  auto& fw = s->_widgets[frame_index];
  neui_event_t ev = {};
  ev.type                  = NEUI_EVENT_METRICS_CHANGED;
  ev.data.metrics.widget   = { fw.widget_id };
  ev.data.metrics.ui_scale = neui_detail::painted_ui_scale();
  s->dispatch_event(&ev);
}

// Height (logical px) of the hamburger band reserved BELOW the status-bar/notch
// safe area when a frame carries a MENUBAR child. A standard 44 pt touch target.
// The total iOS top inset is safeAreaInsets.top + (this when a menubar exists);
// frame_top_inset (host.cpp) reads it through platform_frame_extra_top_inset.
static constexpr int kHamburgerBandH = 44;

// Touch-pan scroll target kinds (see NEUIView::_pan_kind). Plain ints (not an
// enum class) so the ivar stays POD-initialisable to None.
enum {
  kPanTargetNone    = 0,   // nothing scrollable under the gesture start - inert
  kPanTargetGrid    = 1,   // GRID smooth-scroll (grid_scroll_wheel, vertical)
  kPanTargetSection = 2,   // scrolling SECTION / TABPAGE (per-axis kinetics)
  kPanTargetLine    = 3,   // LISTBOX / TREEVIEW / MULTILINE (discrete MOUSE_WHEEL)
};

// Momentum decay per ~60 Hz frame (velocity *= this each tick) and the
// points/sec cutoff below which the glide is considered settled. 0.95 ≈ the feel
// of the macOS inertial scroll the OS hands us as a momentum event stream; here
// we synthesize that stream ourselves since UIKit's pan only reports a release
// velocity, not ongoing momentum events.
static constexpr double kScrollMomentumDecay  = 0.95;
static constexpr double kScrollMomentumCutoff = 6.0;   // points/sec
// One pan point maps 1:1 to one logical scroll pixel: UIKit pan translation is
// already in points (logical px at 96 DPI in neui's model), matching the
// pixel-precise trackpad delta the macOS path feeds (precise = true).
static constexpr double kScrollPanPxPerPoint  = 1.0;
// Points per discrete wheel notch for the line-delta path (LISTBOX/TREEVIEW/
// MULTILINE): a finger drag of this many points emits one MOUSE_WHEEL notch.
static constexpr double kScrollLinePointsPerNotch = 24.0;

// iOS-only rubber-band feel tuning (live-feedback). A finger flick wants a
// longer, gentler overscroll than the mouse-wheel / trackpad default the
// shared constants target, so the iOS host bumps two per-instance multipliers
// on the latched target's ScrollKinetics (see scroll_kinetics.h). They DEFAULT
// to 1.0 on every other host, so macOS / Win32 / Linux / null are byte-for-byte
// unchanged; only a target latched by an iOS pan ever sees these values.
//   - +50% overscroll RANGE: the content stretches 1.5x further past an edge.
//   - slower spring-back TIME: the return is gentler, ~2.5x as long. The
//     spring-back is an exponential ease (raw += d * lerp each 60 Hz tick), so
//     time scales ~inversely with the lerp rate; 1/2.5 = 0.4 lengthens it ~2.5x.
static constexpr double kIosOverscrollRangeScale = 1.5;
static constexpr double kIosBounceRateScale      = 1.0 / 2.5;

// ---------------------------------------------------------------------------
// NEUIView - content view for every neui frame. drawRect: drives the render
// pipeline. UIKit hands drawRect: an upper-left-origin (Y-down) CGContext, so
// the CTM already matches renderer.h's convention - no isFlipped / Y-flip is
// needed (UIView has no isFlipped; the context is top-down by default).

API_AVAILABLE(ios(13.4))
@interface NEUIView : UIView <UIPointerInteractionDelegate, UIGestureRecognizerDelegate,
                              UIDropInteractionDelegate, UIDragInteractionDelegate>
{
@public
  xpl_host::Session* session;
  uint32_t           widget_index;
@private
  // The tree slot of the single active touch. UIKit can deliver several
  // simultaneous touches; v1 tracks just the first and ignores the rest so
  // the synthesized mouse stream stays single-pointer (matches the desktop
  // hosts). _active_touch is the UITouch we latched on touchesBegan:.
  __weak UITouch*    _active_touch;
  // --- Touch-pan scroll (PHASE-2) ----------------------------------------
  // A UIPanGestureRecognizer drives natural swipe-to-scroll for overflowing
  // GRID / scrolling-SECTION (incl. TABPAGE) / LISTBOX / TREEVIEW / MULTILINE.
  // It is the UIKit twin of platform_macos.mm's scrollWheel:; the pan's
  // incremental translation feeds the SAME shared kinetics (grid_scroll_wheel /
  // section_scroll_wheel_kinetic) as a precise trackpad delta, and the release
  // velocity drives a synthesized momentum + rubber-band stream on a CADisplayLink
  // (the analogue of macOS's OS-generated momentum events + 60 Hz NSTimer
  // spring-back). It coexists with the touch handlers: the pan only latches a
  // target on .began when the gesture starts over a scrollable widget, so a swipe
  // on a button / empty area never steals the tap (see gestureRecognizer:should...
  // + the _pan_kind == None bail-outs).
  UIPanGestureRecognizer* _pan;
  // Per-frame momentum/spring-back heartbeat for the pan. Distinct from the toast
  // link so the two animations don't interfere; both only ever -setNeedsDisplay
  // (never relayout), so neither can starve drawRect: or loop layout.
  CADisplayLink*          _scroll_link;
  // The latched scroll target for the in-flight pan, picked on .began under the
  // gesture start point (mirrors the macOS scrollWheel: target pick). One of:
  //   PanTarget::Grid     -> _scroll_widget is a GRID (grid_scroll_wheel)
  //   PanTarget::Section  -> _scroll_widget is the nearest scrolling SECTION
  //                          (section_scroll_wheel_kinetic, vertical+horizontal);
  //                          _scroll_inner is the deepest hit so widgets below the
  //                          section keep first refusal via dispatch_wheel_event.
  //   PanTarget::Line     -> _scroll_inner is a LISTBOX/TREEVIEW/MULTILINE that
  //                          consumes a discrete MOUSE_WHEEL (line-delta path).
  //   PanTarget::None     -> nothing scrollable under the start point; the pan is
  //                          inert (does not consume, does not move content).
  int                     _pan_kind;
  uint32_t                _scroll_widget;   // GRID / SECTION tree index (0 = none)
  uint32_t                _scroll_inner;    // deepest hit under start point (line / bubble)
  // Last cumulative translation (points) seen on the pan, so each .changed yields
  // the INCREMENTAL delta (points since the previous callback) the kinetics want.
  CGPoint                 _pan_last;
  // Fractional line accumulator for the PanTarget::Line path: a discrete
  // MOUSE_WHEEL carries an integer notch count, so sub-line pan motion is banked
  // here and flushed a notch at a time (mirrors the macOS line-delta accumulator).
  double                  _line_accum_x;
  double                  _line_accum_y;
  // Synthesized inertial velocity (points/sec) captured at pan .ended, decayed
  // each _scroll_link tick into a momentum delta fed to the kinetics. iOS gives
  // a release velocity but (unlike macOS) no momentum event stream, so the host
  // generates one. Zeroed once the glide settles into the spring-back phase.
  CGPoint                 _scroll_vel;
  // True between pan .ended and the moment the momentum glide has decayed below
  // the cutoff; while set, the link feeds momentum deltas. After it clears the
  // link keeps running only to drive the rubber-band *_bounce_step until settled.
  bool                    _scroll_momentum;
  // Set once the in-flight pan latches a kinetic target AND actually moves past
  // the recognizer's slop (PHASE-2 fix #3). While set, the matching touchesEnded:
  // suppresses the synthesized MOUSE_BUTTON_CLICK so a swipe that started on a
  // selectable row/button scrolls WITHOUT also selecting it. A stationary tap
  // never trips this (the pan never reaches .changed), so a tap still selects.
  // A scrollbar-thumb drag also never trips it: pickScrollTargetAt: leaves
  // _pan_kind == None when the touch is already driving a thumb, so the thumb
  // rides the touch MOUSE path untouched and the pan stays inert.
  bool                    _touch_consumed_by_scroll;
  // --- Deferred-tap model (PHASE-2 fix #4) -------------------------------
  // iOS UIScrollView-style "delays content touches". The bug: GRID / LISTBOX /
  // TREEVIEW / COMBOBOX commit their selection on MOUSE_BUTTON_DOWN, not on
  // CLICK, so synthesizing a DOWN eagerly on touchesBegan: meant a swipe-to-
  // scroll ALSO selected the row under the finger (suppressing the trailing
  // CLICK was too late - the DOWN had already selected). The fix defers the
  // DOWN until the gesture's intent is known: a TAP delivers the full
  // DOWN->UP->CLICK on release; a SCROLL delivers nothing to the widget.
  //
  // Each touch is classified on touchesBegan: into one of two paths:
  //   - IMMEDIATE-DOWN (self-dragging / caret controls: SLIDER, KNOB,
  //     CUSTOMDRAW, INPUTBOX, MULTILINE) - dispatch DOWN at once as before;
  //     these own their drag, so the pan is suppressed for the touch
  //     (_pan_suppressed) and _touch_pending stays false.
  //   - DEFERRED-TAP (everything else: BUTTON, CHECKBOX[3], LISTBOX, COMBOBOX,
  //     TREEVIEW, GRID, LABEL, SECTION rows) - record a pending tap and emit
  //     NOTHING yet; the pan may latch a scroll. On a stationary lift the full
  //     DOWN->UP->CLICK is synthesized; if the pan moved first, the pending tap
  //     is discarded (a scroll, no selection).
  // True while a deferred touch is in flight with no DOWN yet dispatched.
  bool                    _touch_pending;
  // The widget hit under the deferred touch's start point (the tap target),
  // and that start point in view-local logical px (for the synthesized tap).
  uint32_t                _pending_widget;
  CGPoint                 _pending_point;
  // Set on touchesBegan: when the hit widget is an immediate-DOWN control, so
  // pickScrollTargetAt: leaves the pan inert (the control owns its own drag and
  // must not be hijacked into a kinetic scroll). Cleared each fresh touch.
  bool                    _pan_suppressed;
  // The native hamburger button shown in the top inset band when the frame has
  // a MENUBAR child. A real subview (NOT painted by paint_menubar, which is
  // gated off on iOS): tapping it opens the menu tree as a native UIMenu
  // popover. nil until the frame gains a menubar.
  UIButton*          _hamburger;
  // ~60 Hz repaint heartbeat for the shared toast (and any future per-frame
  // animation). A CADisplayLink is the idiomatic vsync-aligned UIKit timer; it
  // calls setNeedsDisplay each tick so Session::paint_toast re-runs and advances
  // the fly-in / hold / fade-out phases. nil while no animation is running;
  // platform_start_toast_animation creates it (idempotent), platform_stop_toast_
  // animation invalidates it (which paint_toast itself triggers when the toast
  // finishes its lifetime, so the link doesn't spin forever).
  CADisplayLink*     _toast_link;
  // iPad pointer hover (MILESTONE 6). A UIHoverGestureRecognizer fires only for
  // a pointing device (trackpad / mouse), never for a finger, so it drives
  // wd.hovered without breaking the "no hover on touch" divergence. nil until
  // installEnhancedInput attaches it (iOS 13.4+, where UIKey input also exists).
  UIHoverGestureRecognizer* _hover;
  // The widget index of the most recent UIDragInteraction drag source, so the
  // session-did-end delegate can write the negotiated action back to its
  // DRAG_SOURCE behavior's result_attr (the iOS analogue of the desktop
  // begin_drag return-value feedback). 0 when no drag is in flight.
  uint32_t                  _drag_source_widget;
}
// Start / stop the repaint heartbeat. Idempotent: toastStart reuses a live link
// rather than stacking a second one; toastStop is a silent no-op if none runs.
- (void)toastStart;
- (void)toastStop;
// Attach the iPad pointer hover gesture + pointer interaction. Idempotent; a
// no-op on iOS < 13.4 (the UIKey / hover machinery doesn't exist there).
- (void)installEnhancedInput;
// Attach the touch-pan scroll recognizer. Idempotent; safe before/after the view
// joins a window (a fresh gesture is added the first call, reused after).
- (void)installScrollPan;
// Attach the drag&drop interactions (UIDropInteraction drop-target +
// UIDragInteraction drag-source). Idempotent. Called from platform_dnd_register_
// window; the per-widget set_drop_target / DRAG_SOURCE-behavior gating happens
// inside the delegate callbacks via the shared dnd_dispatch + behavior machinery.
- (void)installDragDrop;
// Reserve / lay out the hamburger band, build its UIMenu from the frame's
// MENUBAR model, and report the safe-area top so frame_top_inset can offset
// neui content below the notch + band. Called when the menu model changes
// (platform_menubar_*) and on safe-area / layout changes.
- (void)refreshHamburger;
@end

@implementation NEUIView

// Required so UIKit calls drawRect: with a CoreGraphics context (rather than
// only compositing a CALayer). Matches the per-frame CGContextRef contract the
// CG backend's set_current_frame expects.
- (BOOL)isOpaque { return YES; }

- (void)drawRect:(CGRect)rect
{
  (void)rect;
  if (!session) return;
  xpl_host::WidgetData* wd = session->get_widget(widget_index);
  if (!wd || !wd->render_ctx) return;

  CGContextRef cg = UIGraphicsGetCurrentContext();
  if (!cg) return;

  CGSize sz = self.bounds.size;
  neui_cg_backend::set_current_frame(wd->render_ctx, (void*)cg,
                                      (float)sz.width, (float)sz.height);
  session->paint_frame(wd->render_ctx, widget_index);
}

// Live system dark/light tracking. iOS has no global appearance signal; each
// UI object is told its environment changed via -traitCollectionDidChange:.
// Repopulate the palette + broadcast so Session::on_theme_changed repaints
// NEUI_ATTR_FOLLOW_SYSTEM_THEME frames. Guarded on the userInterfaceStyle
// actually flipping so size-class / layout-direction trait changes don't churn.
- (void)traitCollectionDidChange:(UITraitCollection*)previous
{
  [super traitCollectionDidChange:previous];
  if (@available(iOS 13.0, *)) {
    // Dynamic Type (content-size category) change. Independent of the light/dark
    // flip below - it keeps the same userInterfaceStyle, so it must be handled
    // BEFORE that early return. Recompute the painted-UI scale against the new
    // category, then repaint so painted widgets re-read it (their row / line /
    // chip metrics + default font are read at paint time, so a setNeedsDisplay
    // suffices; the GridModel re-reads its config from attrs each paint too).
    // Native controls with adjustsFontForContentSizeCategory self-update.
    bool content_size_changed =
        previous &&
        previous.preferredContentSizeCategory !=
            self.traitCollection.preferredContentSizeCategory;
    if (content_size_changed) {
      neui_detail::recompute_painted_ui_scale_ios();
      [self setNeedsDisplay];
      // Notify the client so it can re-run its responsive layout against the new
      // metrics (control heights / margins / measured text widths grow with the
      // Dynamic Type scale). Same dispatch path as RESIZE.
      if (session) dispatch_metrics_changed_xpl_ios(session, widget_index);
    }

    if (previous &&
        previous.userInterfaceStyle == self.traitCollection.userInterfaceStyle)
      return;
    // currentTraitCollection reflects self.traitCollection during this call,
    // so the provider resolves the correct light/dark variant.
    neui_detail::refresh_theme_palette_ios();
    [self setNeedsDisplay];
  }
}

// ---------------------------------------------------------------------------
// Touch input. UITouch -> synthesized NEUI_EVENT_MOUSE_* stream:
//   touchesBegan   -> MOUSE_BUTTON_DOWN  (+ set_focus / set_pressed)
//   touchesMoved   -> MOUSE_MOVE         (routed to the pressed widget, like a
//                                          desktop button-held drag)
//   touchesEnded   -> MOUSE_BUTTON_UP  (+ MOUSE_BUTTON_CLICK when released over
//                                          the originally-pressed widget)
//   touchesCancelled -> clear pressed state (no UP/CLICK)
//
// No hover / ENTER / LEAVE: there is no cursor on touch.

- (CGPoint)localPointForTouch:(UITouch*)touch
{
  // locationInView: is already in view-local logical points (top-left origin),
  // matching renderer.h's coordinate convention.
  return [touch locationInView:self];
}

- (void)dispatchMouse:(neui_event_type_t)type
               widget:(uint32_t)target
                    x:(float)lx
                    y:(float)ly
{
  if (!session || target == 0) return;
  auto* hw = session->get_widget(target);
  if (!hw) return;
  neui_event_t ev = {};
  ev.type                 = type;
  ev.data.mouse.widget    = { hw->widget_id };
  ev.data.mouse.x         = (int)lx;
  ev.data.mouse.y         = (int)ly;
  // Primary button held for DOWN / MOVE; released for UP / CLICK. NEUI_MK_LBUTTON
  // mirrors the Win32 MK_LBUTTON bit the shared widgets read off buttonmap.
  ev.data.mouse.buttonmap = (type == NEUI_EVENT_MOUSE_BUTTON_DOWN ||
                             type == NEUI_EVENT_MOUSE_MOVE)
                              ? NEUI_MK_LBUTTON : 0;
  session->dispatch_mouse_event(target, &ev);
}

// Classify a hit widget for the deferred-tap model. Returns true for the
// IMMEDIATE-DOWN controls that own their own drag / caret and therefore need a
// DOWN on touchesBegan: (SLIDER / KNOB thumb-drag, CUSTOMDRAW raw input +
// behavior assets, INPUTBOX / MULTILINE caret + focus). Everything else is a
// DEFERRED-TAP widget (BUTTON, CHECKBOX[3], LISTBOX, COMBOBOX, TREEVIEW, GRID,
// LABEL, SECTION rows) - it must NOT see a DOWN until the gesture is known to be
// a tap, so a swipe-to-scroll never selects the row under the finger. An unknown
// / null type defers (the safe default: a stray DOWN is the bug we're fixing).
- (bool)isImmediateDownWidget:(uint32_t)hit
{
  if (!session || hit == 0) return false;
  auto* hw = session->get_widget(hit);
  if (!hw || !hw->type) return false;
  const char* t = hw->type;
  return !strcmp(t, NEUI_W_SLIDER)    ||
         !strcmp(t, NEUI_W_KNOB)      ||
         !strcmp(t, NEUI_W_CUSTOMDRAW)||
         !strcmp(t, NEUI_W_INPUTBOX)  ||
         !strcmp(t, NEUI_W_MULTILINE);
}

// Synthesize the FULL desktop-style tap (DOWN -> UP -> CLICK) for a deferred
// touch that turned out to be a stationary lift. This is where a deferred
// selectable widget (GRID / LISTBOX / TREEVIEW select, BUTTON activate, CHECKBOX
// toggle, COMBOBOX open) finally gets its DOWN - exactly the desktop sequence,
// so the shared select-on-DOWN logic in host.cpp runs unchanged.
- (void)synthesizeTapAt:(CGPoint)p widget:(uint32_t)hit
{
  if (!session || hit == 0) return;
  float lx = (float)p.x;
  float ly = (float)p.y;
  session->set_focus(hit);
  session->set_pressed(hit);
  [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_DOWN widget:hit x:lx y:ly];
  [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_UP   widget:hit x:lx y:ly];
  [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_CLICK widget:hit x:lx y:ly];
  session->set_pressed(0);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  if (!session) return;
  // Latch the first touch; ignore additional simultaneous touches in v1.
  if (_active_touch) return;
  UITouch* touch = touches.anyObject;
  if (!touch) return;
  _active_touch = touch;

  CGPoint p = [self localPointForTouch:touch];
  float lx = (float)p.x;
  float ly = (float)p.y;
  // A fresh touch sequence: reset the per-touch state.
  _touch_consumed_by_scroll = NO;
  _touch_pending            = NO;
  _pending_widget           = 0;
  _pending_point            = CGPointZero;
  _pan_suppressed           = NO;
  // A tap on a live toast jumps it to the fade-out phase (shared overlay
  // handling, mirroring the desktop hosts). Consumed: the touch dismisses the
  // toast and is not forwarded to the widget tree, and _active_touch is cleared
  // so the matching touchesEnded: doesn't synthesize a stray UP/CLICK.
  if (session->handle_toast_click(widget_index, lx, ly)) {
    _active_touch = nil;
    return;
  }
  // When a COMBOBOX dropdown overlay is open, the tap is DEFERRED through the
  // same pending-tap path (PHASE-2 fix #4): a swipe inside the open overlay must
  // SCROLL the list (or do nothing), NOT commit an item, while a stationary tap
  // commits. So we do NOT call handle_combo_click here on touchesBegan: anymore
  // (that committed on press, defeating swipe-to-scroll inside the overlay) -
  // touchesEnded: routes a stationary lift through handle_combo_click instead.
  // The pan recognizer still latches the list's scroll target on .began.
  uint32_t hit = session->widget_at(lx, ly, widget_index);

  if ([self isImmediateDownWidget:hit]) {
    // IMMEDIATE-DOWN: a self-dragging / caret control. Dispatch DOWN now and
    // suppress the pan for this touch so the control owns its drag (a slider /
    // knob inside a scrolling SECTION would otherwise be hijacked into a pan).
    _pan_suppressed = YES;
    session->set_focus(hit);
    session->set_pressed(hit);
    [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_DOWN widget:hit x:lx y:ly];
    return;
  }

  // DEFERRED-TAP: record the pending tap and emit NOTHING yet. The DOWN is
  // synthesized on a stationary lift in touchesEnded:; if the pan latches +
  // moves first this touch is a scroll and the pending tap is discarded.
  _touch_pending  = YES;
  _pending_widget = hit;
  _pending_point  = p;
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  if (!session || !_active_touch) return;
  if (![touches containsObject:_active_touch]) return;
  // A DEFERRED-TAP touch (no DOWN dispatched yet) never synthesizes MOUSE_MOVE:
  // either it stays a tap (handled on lift) or the pan claims it as a scroll and
  // discards the pending tap (handleScrollPan .changed). Feeding moves here would
  // either start a stray drag on a widget that hasn't seen its DOWN, or fight the
  // pan. So defer entirely while pending.
  if (_touch_pending) return;
  // Once the pan has claimed this touch as a scroll (PHASE-2 fix #3), stop
  // synthesizing MOUSE_MOVE: the press was already cancelled, so a continued
  // move stream would only feed stray hover/drag into whatever sits under the
  // finger while the content scrolls. The pan recognizer drives the scroll.
  if (_touch_consumed_by_scroll) return;

  CGPoint p = [self localPointForTouch:_active_touch];
  float lx = (float)p.x;
  float ly = (float)p.y;
  // A finger drag is a move with the primary button held - route to the
  // originally-pressed widget so an in-progress drag (slider thumb, knob)
  // keeps receiving moves even when the finger strays outside its bounds.
  uint32_t target = session->_pressed_widget;
  if (target == 0) target = session->widget_at(lx, ly, widget_index);
  [self dispatchMouse:NEUI_EVENT_MOUSE_MOVE widget:target x:lx y:ly];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  if (!session || !_active_touch) return;
  if (![touches containsObject:_active_touch]) return;

  CGPoint p = [self localPointForTouch:_active_touch];
  float lx = (float)p.x;
  float ly = (float)p.y;
  uint32_t hit     = session->widget_at(lx, ly, widget_index);
  uint32_t pressed = session->_pressed_widget;
  session->set_pressed(0);
  _active_touch = nil;

  // If the pan latched a kinetic scroll target and moved (PHASE-2 fix #3), this
  // touch was a swipe-to-scroll, not a tap: any in-progress press was already
  // cancelled (immediate-DOWN) or never dispatched (deferred), so synthesize
  // nothing here - that's what keeps the row / button under the start point from
  // being selected by the swipe.
  if (_touch_consumed_by_scroll) {
    _touch_consumed_by_scroll = NO;
    _touch_pending            = NO;
    _pending_widget           = 0;
    return;
  }

  // DEFERRED-TAP lift with no scroll: a stationary tap. Synthesize the full
  // desktop click sequence NOW so the deferred selectable widget gets its DOWN
  // (PHASE-2 fix #4). Use the START widget (where the finger went down) as the
  // tap target, matching the desktop press-then-release-over-same-widget contract.
  if (_touch_pending) {
    uint32_t tapw = _pending_widget;
    _touch_pending  = NO;
    _pending_widget = 0;
    // COMBOBOX overlay commit moved to the tap path: while a dropdown is open,
    // route a stationary tap through the SAME shared handler the macOS mouseDown:
    // path uses. A tap inside the overlay commits the item (+ closes); a tap
    // outside closes. (A swipe inside the overlay scrolled instead via the pan,
    // and never reaches here because _touch_consumed_by_scroll caught it above.)
    if (session->_open_combo != 0 && session->handle_combo_click(lx, ly))
      return;
    // Lift must land on the same widget the finger went down on, mirroring the
    // desktop CLICK contract; a finger that strayed off the start widget (without
    // tripping the pan) is treated as a miss, no selection.
    if (tapw != 0 && tapw == hit)
      [self synthesizeTapAt:p widget:tapw];
    return;
  }

  // IMMEDIATE-DOWN control (DOWN already dispatched on touchesBegan:): finish the
  // sequence with UP + CLICK exactly as the desktop hosts.
  [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_UP widget:hit x:lx y:ly];
  // CLICK fires only when the lift landed on the same widget as the press,
  // matching the desktop hosts.
  if (hit != 0 && hit == pressed)
    [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_CLICK widget:hit x:lx y:ly];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  if (!session || !_active_touch) return;
  if (![touches containsObject:_active_touch]) return;
  session->set_pressed(0);
  _active_touch = nil;
  _touch_consumed_by_scroll = NO;
  // Drop any pending deferred tap - a cancelled gesture is neither a tap nor a
  // committed scroll, so the widget under the finger sees nothing.
  _touch_pending  = NO;
  _pending_widget = 0;
  _pan_suppressed = NO;
  // No UP / CLICK on a cancelled gesture (system interruption: incoming call,
  // gesture recognizer takeover, etc.).
}

// ---------------------------------------------------------------------------
// Hardware keyboard input (MILESTONE 6). The UIKit twin of platform_macos.mm's
// keyDown:/keyUp:. The view must be able to become first responder for UIPress
// events to reach it; the touch path already sets logical focus, and a frame
// with a focused text widget wants the hardware keyboard, so allow it
// unconditionally (mirrors NEUINativeContentView's acceptsFirstResponder).

- (BOOL)canBecomeFirstResponder { return YES; }

// Route a translated key through the SAME two-stage path the macOS keyDown:
// uses: the focused widget's client gets first chance via dispatch_event, then
// (if not consumed) the focused widget's on_keydown via handle_input_key.
- (void)routeKeyDown:(uint32_t)keycode mods:(uint32_t)mods
{
  if (!session || keycode == 0) return;
  uint32_t fw = session->_focused_widget;
  if (fw == 0 || !session->_widgets.exists(fw)) return;
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

// One KEYCHAR per printable codepoint in `chars`, with the same dispatch_event
// -> on_keychar two-stage routing keyDown: uses, so text types into INPUTBOX /
// MULTILINE. Supplementary codepoints are reassembled from UTF-16 surrogate
// pairs (UIKey.characters is an NSString).
- (void)routeCharacters:(NSString*)chars mods:(uint32_t)mods
{
  if (!session || chars.length == 0) return;
  uint32_t fw = session->_focused_widget;
  if (fw == 0 || !session->_widgets.exists(fw)) return;
  auto& wd = session->_widgets[fw];

  for (NSUInteger i = 0; i < chars.length; ) {
    uint32_t cp = (uint32_t)[chars characterAtIndex:i];
    NSUInteger step = 1;
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < chars.length) {
      uint32_t lo = (uint32_t)[chars characterAtIndex:i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
        step = 2;
      }
    }
    i += step;

    if (!neui_detail::ios_is_printable_codepoint(cp)) continue;

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

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event
{
  if (!session) { [super pressesBegan:presses withEvent:event]; return; }
  if (@available(iOS 13.4, *)) {
    bool handled_any = false;
    for (UIPress* press in presses) {
      UIKey* key = press.key;
      if (!key) continue;
      uint32_t mods    = neui_detail::ios_modifiers_to_neui(key.modifierFlags);
      uint32_t keycode = neui_detail::ios_keycode_to_neui((uint16_t)key.keyCode);

      // Tab cycles logical focus inside the hand-rolled Tab traversal, exactly
      // as the macOS keyDown: Tab branch. Consume here; focus_next fires no
      // KEYDOWN. (Menubar Command-accelerators never arrive here - UIKit matches
      // -keyCommands first - so there is no double-fire to guard against.)
      if (keycode == NEUI_KEY_TAB) {
        session->focus_next(!(mods & NEUI_KMOD_SHIFT));
        handled_any = true;
        continue;
      }

      if (keycode != 0) { [self routeKeyDown:keycode mods:mods]; handled_any = true; }

      // Character input -> KEYCHAR. Skip when Command is held: those are command
      // shortcuts (handled above / via -keyCommands), not text - mirrors the
      // macOS keyDown: `if (mods & NEUI_KMOD_CTRL) return;` guard before
      // interpretKeyEvents:.
      if (!(mods & NEUI_KMOD_CTRL) && key.characters.length > 0) {
        [self routeCharacters:key.characters mods:mods];
        handled_any = true;
      }
    }
    // Forward presses we didn't translate (media keys, etc.) so the system
    // still works; if we handled everything, swallow so no beep / bubbling.
    if (!handled_any) [super pressesBegan:presses withEvent:event];
    return;
  }
  [super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event
{
  if (!session) { [super pressesEnded:presses withEvent:event]; return; }
  if (@available(iOS 13.4, *)) {
    bool handled_any = false;
    for (UIPress* press in presses) {
      UIKey* key = press.key;
      if (!key) continue;
      uint32_t keycode = neui_detail::ios_keycode_to_neui((uint16_t)key.keyCode);
      if (keycode == 0 || keycode == NEUI_KEY_TAB) continue;
      uint32_t mods = neui_detail::ios_modifiers_to_neui(key.modifierFlags);

      uint32_t fw = session->_focused_widget;
      if (fw == 0 || !session->_widgets.exists(fw)) continue;
      auto& wd = session->_widgets[fw];
      if (!wd.emit_events) continue;

      neui_event_t ev = {};
      ev.type     = NEUI_EVENT_KEYUP;
      ev.data.key = { { wd.widget_id }, keycode, mods };
      session->dispatch_event(&ev);
      handled_any = true;
    }
    if (!handled_any) [super pressesEnded:presses withEvent:event];
    return;
  }
  [super pressesEnded:presses withEvent:event];
}

// ---------------------------------------------------------------------------
// Menu accelerators (MILESTONE 6). Publish a UIKeyCommand per menubar item
// shortcut. UIKit matches -keyCommands BEFORE pressesBegan: for a matching
// chord, so this is the single dispatch point for menubar accelerators (the
// press path never sees them). Each command routes its (mods, key) back through
// Session::try_menubar_accel, which finds the matching item and dispatches it
// (built-in command first, then client TREE_ITEM_ACTIVATED) - the same routing
// the Linux key path uses. On iPad this also surfaces the ⌘-hold discoverability
// HUD listing the shortcuts.

- (NSArray<UIKeyCommand*>*)keyCommands
{
  if (!session || !session->_widgets.exists(widget_index)) return nil;
  xpl_host::MenubarWidget* mb = session->frame_menubar(widget_index);
  if (!mb) return nil;

  // DOUBLE-FIRE SPLIT (system menu bar vs -keyCommands): when the iPad system
  // menu bar is active, the menubar items are contributed via
  // -buildMenuWithBuilder: as UIMenu elements that carry their own
  // UIKeyCommand-bearing actions (handleMenuItemSelected:), so the menu bar owns
  // + shows the accelerators. Publishing the SAME chords here too would make
  // UIKit match one of the two arbitrarily and the other never fire (or, worse,
  // both routes resolving the chord). So -keyCommands yields the menubar
  // accelerators to the menu bar and contributes none of its own; the menu-bar
  // path is the single dispatch point on iPad. On iPhone / no menu bar this
  // stays the only accelerator surface (unchanged from M6).
  if (neui_detail::menu_ios_system_menubar_available()) return nil;

  NSMutableArray<UIKeyCommand*>* cmds = [NSMutableArray array];
  // Walk every menubar item; for each with a bound shortcut, build a UIKeyCommand
  // whose action target is self (handleMenuKeyCommand:). The command carries the
  // neui (mods, key) in its propertyList so the handler can replay it through
  // try_menubar_accel without re-deriving from UIKit modifier flags.
  for (uint32_t id : mb->menu_item_ids_ordered) {
    auto it = mb->menu_items.find(id);
    if (it == mb->menu_items.end()) continue;
    const auto& data = it->second;
    if (data.is_separator) continue;
    if (data.shortcut_key == NEUI_KEY_NONE) continue;

    NSString* input = neui_detail::menu_ios_key_to_input(data.shortcut_key);
    if (!input) continue;  // function / editing keys have no UIKeyCommand input
    UIKeyModifierFlags flags = neui_detail::menu_ios_mods_to_uikit(data.shortcut_mods);

    UIKeyCommand* kc =
      [UIKeyCommand keyCommandWithInput:input
                          modifierFlags:flags
                                 action:@selector(handleMenuKeyCommand:)];
    if (@available(iOS 13.0, *)) {
      NSString* title = [NSString stringWithUTF8String:data.text.c_str()];
      if (title) kc.discoverabilityTitle = title;
    }
    [cmds addObject:kc];
  }
  return cmds.count ? cmds : nil;
}

- (void)handleMenuKeyCommand:(UIKeyCommand*)cmd
{
  if (!session) return;
  // Recover the neui (key, mods) from the matched command's input + modifier
  // flags (UIKeyCommand carries no settable user data via the public API; the
  // input/modifiers pair is its identity). The keyCommands list only ever
  // contains combos try_menubar_accel can match, so this resolves to a menubar
  // item.
  uint32_t key  = neui_detail::ios_input_to_neui_key(cmd.input);
  uint32_t mods = neui_detail::ios_modifiers_to_neui(cmd.modifierFlags);
  if (key == NEUI_KEY_NONE) return;
  // try_menubar_accel finds the item by (key, mods) and dispatches it - built-in
  // command (e.g. Copy to the focused field editor) first, then client
  // TREE_ITEM_ACTIVATED. The single accelerator dispatch point (see keyCommands).
  session->try_menubar_accel(key, mods);
}

// ---------------------------------------------------------------------------
// System menu bar contribution moved to the APP level (NEUIApplication, see
// hosts/ios/application_ios.mm + the neui_ios_xpl_build_menubar_menus hook at the
// bottom of this TU). A UIVIEW's -buildMenuWithBuilder: is only consulted while
// that view is in the ACTIVE responder chain, which on a freshly-launched app
// with no first responder is unreliable - on a real iPad NOTHING appeared.
// UIApplication is always in the chain, so the contribution lives there now. The
// content view no longer overrides -buildMenuWithBuilder: (it would
// double-contribute). The rest of the menu plumbing stays here: -keyCommands
// (iPhone / no-bar accelerators), -handleMenuKeyCommand: (UIKeyCommand routing),
// the hamburger build, and the setNeedsRebuild triggers.

// ---------------------------------------------------------------------------
// iPad pointer hover (MILESTONE 6). A UIHoverGestureRecognizer drives
// wd.hovered. It fires ONLY for a pointing device (trackpad / mouse), never for
// a finger, so the "no hover on touch" divergence holds: a finger touch never
// sets hovered (only the touch path's set_pressed runs), and hover-state
// compound filters stay collapsed under touch but light up under the pointer.

- (void)installEnhancedInput
{
  if (@available(iOS 13.4, *)) {
    if (!_hover) {
      _hover = [[UIHoverGestureRecognizer alloc]
                 initWithTarget:self action:@selector(handleHover:)];
      [self addGestureRecognizer:_hover];
    }
    // Best-effort pointer cursor shape; hover-state correctness rides _hover.
    bool has_pointer = false;
    for (id<UIInteraction> it in self.interactions)
      if ([it isKindOfClass:[UIPointerInteraction class]]) { has_pointer = true; break; }
    if (!has_pointer)
      [self addInteraction:[[UIPointerInteraction alloc] initWithDelegate:self]];
  }
}

- (void)handleHover:(UIHoverGestureRecognizer*)g API_AVAILABLE(ios(13.0))
{
  if (!session) return;
  switch (g.state) {
    case UIGestureRecognizerStateBegan:
    case UIGestureRecognizerStateChanged: {
      CGPoint p = [g locationInView:self];
      uint32_t hit = session->widget_at((float)p.x, (float)p.y, widget_index);
      // set_hovered emits MOUSE_ENTER / MOUSE_LEAVE on transitions + invalidates
      // the owning frame, exactly as the macOS mouseMoved: hover path.
      session->set_hovered(hit);
      break;
    }
    case UIGestureRecognizerStateEnded:
    case UIGestureRecognizerStateCancelled:
    case UIGestureRecognizerStateFailed:
      // Pointer left the view - clear hover so any current MOUSE_LEAVE fires.
      session->set_hovered(0);
      break;
    default:
      break;
  }
}

// UIPointerInteractionDelegate - default arrow-style pointer over the view. The
// hover STATE is driven by _hover above; this just gives the trackpad a cursor.
- (UIPointerStyle*)pointerInteraction:(UIPointerInteraction*)interaction
                       styleForRegion:(UIPointerRegion*)region
    API_AVAILABLE(ios(13.4))
{
  (void)interaction; (void)region;
  return nil;  // nil = system default pointer style
}

// ---------------------------------------------------------------------------
// Touch-pan scroll (PHASE-2). The UIKit twin of platform_macos.mm's scrollWheel:.
// A single UIPanGestureRecognizer drives natural swipe-to-scroll for every
// overflowing scrollable: GRID, scrolling SECTION / TABPAGE, and the line-delta
// widgets (LISTBOX / TREEVIEW / MULTILINE). The pan's incremental translation is
// fed to the SAME shared kinetics the wheel path uses - as a pixel-precise delta
// (precise = true) so the rubber-band + momentum machinery engages (a non-precise
// delta would hard-clamp, see scroll_kinetics.h). Momentum is synthesized on a
// CADisplayLink from the release velocity because UIKit, unlike AppKit, reports
// no ongoing momentum event stream.

- (void)installScrollPan
{
  if (_pan) return;
  _pan = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                 action:@selector(handleScrollPan:)];
  _pan.delegate = self;
  // One-finger drag, like a UIScrollView pan; trackpad two-finger scroll on iPad
  // also surfaces here as an indirect-pointer pan.
  _pan.maximumNumberOfTouches = 1;
  [self addGestureRecognizer:_pan];
}

// ---------------------------------------------------------------------------
// Drag & drop. The NEUIView hosts one UIDropInteraction (drop-target) and one
// UIDragInteraction (drag-source) for the whole frame; the framework hit-tests
// the widget tree in software (Session::dispatch_dnd_* + dnd_resolve_drag_source)
// exactly like the macOS NEUIView is the sole NSDraggingDestination/source. The
// neui-side plumbing (dnd_dispatch state machine, DataItem payloads, MIME<->UTI
// mapping) is shared verbatim with the desktop hosts; only the UIKit glue is
// iOS-local.

- (void)installDragDrop
{
  // Idempotent: a second register_window call (theme flip / re-show) must not
  // stack interactions. Scan the existing list.
  for (id<UIInteraction> i in self.interactions) {
    if ([i isKindOfClass:[UIDropInteraction class]] ||
        [i isKindOfClass:[UIDragInteraction class]])
      return;
  }
  UIDropInteraction* drop =
      [[UIDropInteraction alloc] initWithDelegate:self];
  [self addInteraction:drop];
  UIDragInteraction* drag =
      [[UIDragInteraction alloc] initWithDelegate:self];
  // Allow drags to other apps too (Split View / Slide Over), matching the
  // desktop "drag to any app" behaviour.
  drag.allowsSimultaneousRecognitionDuringLift = YES;
  [self addInteraction:drag];
}

// View-local logical point for a drop session (top-left origin, matches the
// touch / paint convention - UIView is natively Y-down, no flip needed).
- (CGPoint)neuiDropPoint:(id<UIDropSession>)session
{
  return [session locationInView:self];
}

// ---- UIDropInteractionDelegate (drop target) ----

- (BOOL)dropInteraction:(UIDropInteraction*)interaction
       canHandleSession:(id<UIDropSession>)session
{
  (void)interaction;
  if (!self->session) return NO;
  NSMutableArray<NSItemProvider*>* providers = [NSMutableArray array];
  for (UIDragItem* it in session.items)
    if (it.itemProvider) [providers addObject:it.itemProvider];
  return neui_detail::dnd_session_has_known_type_ios(providers) ? YES : NO;
}

- (void)dropInteraction:(UIDropInteraction*)interaction
        sessionDidEnter:(id<UIDropSession>)dropSession
{
  (void)interaction; (void)dropSession;
  // ENTER is folded into the first sessionDidUpdate: (which has the location);
  // UIKit always sends an update right after enter. Nothing to do here.
}

- (UIDropProposal*)dropInteraction:(UIDropInteraction*)interaction
                  sessionDidUpdate:(id<UIDropSession>)dropSession
{
  (void)interaction;
  if (!session)
    return [[UIDropProposal alloc] initWithDropOperation:UIDropOperationCancel];

  NSMutableArray<NSItemProvider*>* providers = [NSMutableArray array];
  for (UIDragItem* it in dropSession.items)
    if (it.itemProvider) [providers addObject:it.itemProvider];
  auto ml = neui_detail::dnd_collect_mimes_ios(providers);
  CGPoint p = [self neuiDropPoint:dropSession];
  // No keyboard modifiers during a touch drag; suggest the first available
  // action (Copy > Move > Link) - the client's accept() decides the real one.
  uint32_t suggested = neui_detail::dnd_suggest_action(
      NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE | NEUI_DND_ACTION_LINK,
      false, false);

  // First update of a session also serves as ENTER; track whether we've
  // entered so re-target / MOVE go through the right shared path.
  uint32_t accepted;
  if (session->_current_drop_target == UINT32_MAX) {
    accepted = session->dispatch_dnd_enter(widget_index,
                                            (int)p.x, (int)p.y,
                                            ml.ptrs.data(),
                                            (uint32_t)ml.ptrs.size(),
                                            suggested, 0);
  } else {
    accepted = session->dispatch_dnd_move(widget_index,
                                           (int)p.x, (int)p.y,
                                           ml.ptrs.data(),
                                           (uint32_t)ml.ptrs.size(),
                                           suggested, 0);
  }

  UIDropOperation op;
  if (accepted == NEUI_DND_ACTION_NONE)      op = UIDropOperationCancel;
  else if (accepted & NEUI_DND_ACTION_MOVE)  op = UIDropOperationMove;
  else if (accepted & NEUI_DND_ACTION_COPY)  op = UIDropOperationCopy;
  else                                       op = UIDropOperationForbidden;
  return [[UIDropProposal alloc] initWithDropOperation:op];
}

- (void)dropInteraction:(UIDropInteraction*)interaction
         sessionDidExit:(id<UIDropSession>)dropSession
{
  (void)interaction; (void)dropSession;
  if (session) session->dispatch_dnd_leave();
}

- (void)dropInteraction:(UIDropInteraction*)interaction
        sessionDidEnd:(id<UIDropSession>)dropSession
{
  (void)interaction; (void)dropSession;
  // sessionDidEnd fires on cancel without a performDrop; clear any lingering
  // target. (performDrop already cleared via dispatch_dnd_drop on a real drop;
  // calling leave again is a harmless no-op since the target is reset.)
  if (session && session->_current_drop_target != UINT32_MAX)
    session->dispatch_dnd_leave();
}

- (void)dropInteraction:(UIDropInteraction*)interaction
            performDrop:(id<UIDropSession>)dropSession
{
  (void)interaction;
  if (!session) return;
  NSMutableArray<NSItemProvider*>* providers = [NSMutableArray array];
  for (UIDragItem* it in dropSession.items)
    if (it.itemProvider) [providers addObject:it.itemProvider];

  // Synchronously pull the bytes for every mappable representation into a
  // transient DataItem (NSItemProvider loads are async; dnd_read_drop_item_ios
  // blocks with a bounded timeout). dispatch_dnd_drop materialises it into the
  // session store for the duration of the DROP callback.
  neui_detail::DataItem item;
  neui_detail::dnd_read_drop_item_ios(providers, item);
  auto ml = neui_detail::dnd_collect_mimes_ios(providers);
  CGPoint p = [self neuiDropPoint:dropSession];
  uint32_t suggested = neui_detail::dnd_suggest_action(
      NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE | NEUI_DND_ACTION_LINK,
      false, false);
  session->dispatch_dnd_drop(widget_index, (int)p.x, (int)p.y,
                             ml.ptrs.data(), (uint32_t)ml.ptrs.size(),
                             suggested, 0, &item);
}

// ---- UIDragInteractionDelegate (drag source) ----

- (NSArray<UIDragItem*>*)dragInteraction:(UIDragInteraction*)interaction
                  itemsForBeginningSession:(id<UIDragSession>)dragSession
{
  (void)interaction;
  _drag_source_widget = 0;
  if (!session) return @[];
  CGPoint p = [dragSession locationInView:self];

  // Resolve the widget under the gesture: a drag source iff it carries a
  // DRAG_SOURCE behavior asset. The Session copies that handler's DataItem
  // (from its drag_data_key attr) into `item` + reports allowed_actions.
  neui_detail::DataItem item;
  uint32_t allowed = 0;
  uint32_t w = session->dnd_resolve_drag_source(widget_index,
                                                 (int)p.x, (int)p.y,
                                                 &item, allowed);
  if (w == 0) return @[];  // not a drag source -> no drag begins

  NSItemProvider* provider = neui_detail::dnd_item_provider_for_item_ios(item);
  if (!provider) return @[];  // empty payload -> nothing to drag

  _drag_source_widget = w;
  UIDragItem* di = [[UIDragItem alloc] initWithItemProvider:provider];
  // Stash the source widget id on the drag item so the end delegate can map
  // back even if a later drag clobbers _drag_source_widget.
  di.localObject = @(w);
  return @[ di ];
}

- (void)dragInteraction:(UIDragInteraction*)interaction
                session:(id<UIDragSession>)dragSession
   didEndWithOperation:(UIDropOperation)operation
{
  (void)interaction;
  if (!session) return;
  // Map the iOS drop operation back to a neui action and write the DRAG_SOURCE
  // behavior's result_attr (+ ATTR_CHANGED + invalidate), mirroring the desktop
  // begin_drag return-value feedback.
  uint32_t action = NEUI_DND_ACTION_NONE;
  switch (operation) {
    case UIDropOperationCopy: action = NEUI_DND_ACTION_COPY; break;
    case UIDropOperationMove: action = NEUI_DND_ACTION_MOVE; break;
    default:                  action = NEUI_DND_ACTION_NONE; break;
  }
  uint32_t w = _drag_source_widget;
  // Prefer the per-item localObject (robust if multiple drags overlapped).
  for (UIDragItem* di in dragSession.items) {
    if ([di.localObject isKindOfClass:[NSNumber class]]) {
      w = (uint32_t)[(NSNumber*)di.localObject unsignedIntValue];
      break;
    }
  }
  if (w != 0) session->dnd_report_drag_result(w, action);
  _drag_source_widget = 0;
}

// Find the scroll target under a gesture-start point, exactly as the macOS
// scrollWheel: target pick: a GRID hit scrolls the grid; otherwise the nearest
// scrolling-SECTION ancestor (or the hit itself if it is one) scrolls; otherwise
// a LISTBOX/TREEVIEW/MULTILINE under the point takes the discrete line-delta
// path. Sets _pan_kind / _scroll_widget / _scroll_inner; leaves _pan_kind == None
// when nothing scrollable is under the point (so the pan stays inert + the tap is
// never stolen).
- (void)pickScrollTargetAt:(CGPoint)p
{
  _pan_kind      = kPanTargetNone;
  _scroll_widget = 0;
  _scroll_inner  = 0;
  if (!session) return;

  // An IMMEDIATE-DOWN control (slider / knob / customdraw / text field) owns its
  // own drag (PHASE-2 fix #4): leave the pan inert so the same finger isn't ALSO
  // fed to the kinetics. (Set in touchesBegan: when the start widget classified
  // as immediate-DOWN.)
  if (_pan_suppressed) return;   // _pan_kind stays None

  // TRADEOFF (PHASE-2 fix #4): touch scrollbar-THUMB DRAG no longer starts. The
  // scrollable widgets (GRID / LISTBOX / TREEVIEW) are DEFERRED-TAP, so a touch
  // that lands on the thumb does NOT dispatch a DOWN on touchesBegan: - the press
  // that would have armed scrollbar_dragging() never happens. So this guard is
  // effectively dead for touch (it stays as defensive cover for the pointer/
  // trackpad pan path). This is the iOS-idiomatic behavior: scroll indicators are
  // position readouts, not draggable handles; swipe-to-scroll replaces dragging
  // the thumb. The thumb still RENDERS as a position indicator. Preserving thumb
  // drag would require detecting a touch starting on the thumb region and treating
  // it as immediate-DOWN - deliberately not done (over-engineering for v1).
  if (uint32_t pressed = session->_pressed_widget) {
    auto* pw = session->get_widget(pressed);
    if (pw && pw->scrollbar_dragging()) return;   // _pan_kind stays None
  }

  uint32_t hit = session->widget_at((float)p.x, (float)p.y, widget_index);
  if (hit == 0) return;
  auto* hw = session->get_widget(hit);
  if (!hw) return;
  _scroll_inner = hit;

  // GRID smooth-scroll (vertical), like the macOS grid_model_ptr branch.
  if (auto* gm = hw->grid_model_ptr()) {
    _pan_kind      = kPanTargetGrid;
    _scroll_widget = hit;
    // iOS-only rubber-band feel: +50% overscroll range, ~1.5x spring-back
    // time. Set per-latch on this grid's single ScrollKinetics; defaults stay
    // 1.0 on every other host (see kIos*Scale + scroll_kinetics.h).
    gm->scroll_kin.overscroll_range_scale = kIosOverscrollRangeScale;
    gm->scroll_kin.bounce_rate_scale      = kIosBounceRateScale;
    return;
  }

  // Scrolling SECTION: the hit itself, else the nearest scrolling ancestor
  // (TABPAGE is a chip-less scrolling SECTION, so a swipe inside a tab page that
  // doesn't land on an inner scrollable still pans the page).
  uint32_t sec = 0;
  if (hw->scroll_state_ptr()) {
    sec = hit;
  } else {
    for (uint32_t pidx : session->_widgets.get_all_parents(hit)) {
      auto* pw = session->get_widget(pidx);
      if (pw && pw->scroll_state_ptr()) { sec = pidx; break; }
    }
  }
  if (sec != 0) {
    _pan_kind      = kPanTargetSection;
    _scroll_widget = sec;
    // Same iOS-only rubber-band feel as GRID, applied to BOTH axis integrators
    // of the scrolling SECTION (the reported issue is GRID, but the SECTION
    // shares the exact same kinetics + feel, so keep them consistent). Per-latch
    // assignment; 1.0 default keeps the desktop SECTION feel untouched.
    if (auto* sw2 = session->get_widget(sec)) {
      if (auto* st = sw2->scroll_state_ptr()) {
        st->kin_v.overscroll_range_scale = kIosOverscrollRangeScale;
        st->kin_v.bounce_rate_scale      = kIosBounceRateScale;
        st->kin_h.overscroll_range_scale = kIosOverscrollRangeScale;
        st->kin_h.bounce_rate_scale      = kIosBounceRateScale;
      }
    }
    return;
  }

  // Line-delta widget (LISTBOX / TREEVIEW / MULTILINE): consumes a discrete
  // MOUSE_WHEEL. Only latch when the widget (or an ancestor reachable by the
  // bounded bubble) actually consumes a probe wheel, so a pan over a non-scrolling
  // widget (a button, a label) stays inert and never steals the tap.
  if (hw->emit_events && hw->enabled) {
    neui_event_t probe = {};
    probe.type                     = NEUI_EVENT_MOUSE_WHEEL;
    probe.data.wheel.widget        = { hw->widget_id };
    probe.data.wheel.x             = (int)p.x;
    probe.data.wheel.y             = (int)p.y;
    probe.data.wheel.delta         = 1;   // probe one notch up; reverted below
    probe.data.wheel.is_horizontal = 0;
    if (session->dispatch_wheel_event(hit, &probe)) {
      // Undo the probe so the latch is side-effect free (the real deltas follow
      // on .changed). A LISTBOX/TREEVIEW that consumed +1 line scrolls back -1.
      neui_event_t undo = probe;
      undo.data.wheel.delta = -1;
      session->dispatch_wheel_event(hit, &undo);
      _pan_kind     = kPanTargetLine;
      _scroll_inner = hit;
    }
  }
}

// Build a precise (finger) ScrollWheelInput for the kinetics with the given
// per-axis pixel delta + gesture-phase flags. delta_px follows neui's natural
// scroll convention (positive = content scrolls up / position decreases) which
// matches a finger moving UP (negative pan translation) reducing the position,
// so we pass the raw incremental pan delta straight through (see callers).
static neui_detail::ScrollWheelInput
ios_make_scroll_input(double delta_px, bool began, bool changed,
                      bool ended, bool momentum, bool momentum_ended)
{
  neui_detail::ScrollWheelInput in;
  in.precise        = true;   // finger == precise/pixel input -> rubber-band on
  in.delta_px       = delta_px;
  in.phase_began    = began;
  in.phase_changed  = changed;
  in.phase_ended    = ended;
  in.momentum       = momentum;
  in.momentum_ended = momentum_ended;
  return in;
}

// Feed one (dx, dy) increment (logical px) + phase flags to the latched target's
// kinetics. dx/dy use the natural-scroll convention above. Starts the momentum/
// spring-back link when the kinetics report start_bounce. Returns nothing - it
// repaints + fires SCROLL_CHANGED through the shared commit paths.
- (void)feedScrollDX:(double)dx dy:(double)dy
               began:(bool)began changed:(bool)changed ended:(bool)ended
            momentum:(bool)momentum momentumEnded:(bool)momentumEnded
{
  using namespace neui_detail;
  if (!session) return;

  if (_pan_kind == kPanTargetGrid) {
    auto* hw = session->get_widget(_scroll_widget);
    GridModel* model = hw ? hw->grid_model_ptr() : nullptr;
    if (!model) return;
    auto cfg = grid_read_config(hw->attrs.get());
    GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                             cfg.row_h, cfg.header_h);
    // GRID kinetics are vertical-only (X is a plain offset), matching macOS:
    // only dy feeds grid_scroll_wheel.
    GridWheelInput in = ios_make_scroll_input(dy, began, changed, ended,
                                              momentum, momentumEnded);
    GridWheelAction act = grid_scroll_wheel(*model, vp, cfg.row_h, in);
    if (act.changed)      [self setNeedsDisplay];
    if (act.start_bounce) [self scrollLinkStart];
    return;
  }

  if (_pan_kind == kPanTargetSection) {
    auto* sw = session->get_widget(_scroll_widget);
    SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
    const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
    if (!st || !L) return;

    bool has_v = section_axis_has_v(st->axis);
    bool has_h = section_axis_has_h(st->axis);

    // Widgets strictly below the section get first refusal on the live drag
    // (a LISTBOX inside a scrolling SECTION keeps consuming its own scroll),
    // exactly as the macOS bounded bubble. Only on .changed with real motion;
    // phase-only edges go straight to the section's kinetics.
    if (changed && !momentum && (dx != 0.0 || dy != 0.0) &&
        _scroll_inner != 0 && _scroll_inner != _scroll_widget) {
      // Convert the dominant axis to a notch and try the inner widget first.
      // MOUSE_WHEEL.delta is positive = scroll up (offset decreases). A finger
      // moving DOWN (positive incremental dy) must reveal content above = scroll
      // up = positive delta, so the notch carries the finger-motion sign directly.
      // Banks the fractional remainder per-axis (shared helper) so a diagonal
      // swipe doesn't leak horizontal motion into the vertical accumulator.
      bool horiz = false;
      int notch = neui_detail::pan_delta_to_notch(dx, dy, kScrollLinePointsPerNotch,
                                                  _line_accum_x, _line_accum_y, horiz);
      if (notch != 0) {
        neui_event_t ev = {};
        ev.type                     = NEUI_EVENT_MOUSE_WHEEL;
        ev.data.wheel.x             = 0;
        ev.data.wheel.y             = 0;
        ev.data.wheel.delta         = notch;
        ev.data.wheel.is_horizontal = horiz ? 1 : 0;
        if (session->dispatch_wheel_event(_scroll_inner, &ev, _scroll_widget))
          return;   // inner widget ate it; section stays put
      }
    }

    ScrollWheelAction av{}, ah{};
    bool phase_signal = began || ended || momentum || momentumEnded;
    if (has_v && (dy != 0.0 || phase_signal)) {
      ScrollWheelInput in = ios_make_scroll_input(dy, began, changed, ended,
                                                  momentum, momentumEnded);
      av = section_scroll_wheel_kinetic(*st, *L, in, false);
    }
    if (has_h && (dx != 0.0 || phase_signal)) {
      ScrollWheelInput in = ios_make_scroll_input(dx, began, changed, ended,
                                                  momentum, momentumEnded);
      ah = section_scroll_wheel_kinetic(*st, *L, in, true);
    }
    if (av.changed || ah.changed) {
      [self setNeedsDisplay];
      sw->notify_scroll_changed();
    }
    if (av.start_bounce || ah.start_bounce) [self scrollLinkStart];
    return;
  }

  if (_pan_kind == kPanTargetLine) {
    // Discrete MOUSE_WHEEL line-delta: bank the pan motion and flush a notch at
    // a time. Only the live drag scrolls these (no rubber-band / momentum model
    // for plain line widgets - matches the desktop wheel feel).
    if (!changed) return;
    // delta>0 = scroll up (offset decreases); a finger moving DOWN (positive
    // incremental mag) reveals content above = scroll up, so notch sign == finger
    // sign (natural scroll: content follows the finger). Per-axis banking via
    // the shared helper (same path the section inner-bubble above takes).
    bool horiz = false;
    int notch = neui_detail::pan_delta_to_notch(dx, dy, kScrollLinePointsPerNotch,
                                                _line_accum_x, _line_accum_y, horiz);
    if (notch == 0) return;
    neui_event_t ev = {};
    ev.type                     = NEUI_EVENT_MOUSE_WHEEL;
    ev.data.wheel.x             = 0;
    ev.data.wheel.y             = 0;
    ev.data.wheel.delta         = notch;
    ev.data.wheel.is_horizontal = horiz ? 1 : 0;
    if (session->dispatch_wheel_event(_scroll_inner, &ev))
      [self setNeedsDisplay];
    return;
  }
}

- (void)handleScrollPan:(UIPanGestureRecognizer*)g
{
  if (!session) return;
  switch (g.state) {
    case UIGestureRecognizerStateBegan: {
      // A live spring-back / momentum glide from a previous pan is interrupted by
      // the new touch - stop the link and let the fresh gesture take over (the
      // phase_began we feed below also clears the kinetics' bounce state).
      [self scrollLinkStop];
      CGPoint p = [g locationInView:self];
      [self pickScrollTargetAt:p];
      _pan_last     = [g translationInView:self];
      _line_accum_x = 0.0;
      _line_accum_y = 0.0;
      if (_pan_kind == kPanTargetNone) return;
      [self feedScrollDX:0.0 dy:0.0 began:true changed:false ended:false
                momentum:false momentumEnded:false];
      break;
    }
    case UIGestureRecognizerStateChanged: {
      if (_pan_kind == kPanTargetNone) return;
      // First real movement of a latched pan: this touch is a swipe-to-scroll,
      // not a tap (PHASE-2 fix #4). With the deferred-tap model the DOWN was
      // NEVER dispatched for the (deferred) widget under the start point, so
      // there is nothing to "un-press" - we simply DISCARD the pending tap so
      // touchesEnded: synthesizes no DOWN/UP/CLICK, and the swipe scrolls without
      // selecting. Done once per gesture (guarded by the flag); later .changed
      // frames just scroll. (Immediate-DOWN controls never reach here - they set
      // _pan_suppressed, so the pan stays inert / _pan_kind == None.)
      if (!_touch_consumed_by_scroll) {
        _touch_consumed_by_scroll = YES;
        _touch_pending  = NO;     // drop the pending deferred tap: this is a scroll
        _pending_widget = 0;
        session->set_pressed(0);  // defensive: nothing should be pressed here
        session->set_hovered(0);
      }
      CGPoint t = [g translationInView:self];
      // Incremental delta in points since the last callback. Natural-scroll
      // convention: a finger moving DOWN (t.y increasing) should reveal content
      // above = decrease the scroll position. neui's delta_px is SUBTRACTED from
      // the position, so position decreases when delta_px is positive; thus a
      // downward finger (positive incremental) must map to a positive delta_px.
      // The incremental finger motion is (t - _pan_last); pass it straight (the
      // kinetics subtract it), giving content-follows-finger.
      double dx = (double)(t.x - _pan_last.x) * kScrollPanPxPerPoint;
      double dy = (double)(t.y - _pan_last.y) * kScrollPanPxPerPoint;
      _pan_last = t;
      [self feedScrollDX:dx dy:dy began:false changed:true ended:false
                momentum:false momentumEnded:false];
      break;
    }
    case UIGestureRecognizerStateEnded:
    case UIGestureRecognizerStateCancelled:
    case UIGestureRecognizerStateFailed: {
      if (_pan_kind == kPanTargetNone) return;
      // Tell the kinetics the finger lifted (arms spring-back if overscrolled),
      // then capture the release velocity to synthesize a momentum glide. The
      // velocity is points/sec; feed it per-tick decayed on the link.
      [self feedScrollDX:0.0 dy:0.0 began:false changed:false ended:true
                momentum:false momentumEnded:false];
      // Only a real lift (Ended) flings; a system-cancelled / failed gesture
      // (incoming call, gesture takeover, parent claim) stops dead - an
      // interrupted pan must not synthesize release momentum.
      CGPoint v = (g.state == UIGestureRecognizerStateEnded)
                    ? [g velocityInView:self] : CGPointZero;
      // Only GRID + SECTION carry momentum/rubber-band; line widgets stop dead on
      // release (desktop wheel parity).
      if ((_pan_kind == kPanTargetGrid || _pan_kind == kPanTargetSection) &&
          (std::fabs(v.x) > kScrollMomentumCutoff ||
           std::fabs(v.y) > kScrollMomentumCutoff)) {
        _scroll_vel      = v;
        _scroll_momentum = true;
        [self scrollLinkStart];
      } else {
        _scroll_vel      = CGPointZero;
        _scroll_momentum = false;
        // If the lift left an overscroll, feedScroll's ended already armed the
        // bounce; start the link so the rubber-band settles.
        [self maybeStartBounceLink];
      }
      break;
    }
    default:
      break;
  }
}

// Start the momentum/spring-back heartbeat (idempotent). Only -setNeedsDisplay is
// driven from the tick (via the kinetics commit), so it cannot starve drawRect:
// or trigger a layout loop.
- (void)scrollLinkStart
{
  if (_scroll_link) return;
  _scroll_link = [CADisplayLink displayLinkWithTarget:self
                                             selector:@selector(scrollTick:)];
  [_scroll_link addToRunLoop:[NSRunLoop mainRunLoop]
                     forMode:NSRunLoopCommonModes];
}

- (void)scrollLinkStop
{
  if (!_scroll_link) return;
  [_scroll_link invalidate];
  _scroll_link = nil;
  _scroll_momentum = false;
  _scroll_vel = CGPointZero;
}

// Start the link only if the latched target actually has an in-flight bounce to
// settle (called on a slow release with no momentum). Cheap: one bounce_step
// probe; if it reports "done" we never start the link.
- (void)maybeStartBounceLink
{
  if (![self scrollBounceStep]) return;   // nothing to settle
  [self scrollLinkStart];
}

// Advance one momentum + spring-back frame for the latched target. The link
// keeps running while either the momentum glide is alive OR the rubber-band is
// still settling.
- (void)scrollTick:(CADisplayLink*)link
{
  (void)link;
  if (!session || _pan_kind == kPanTargetNone) { [self scrollLinkStop]; return; }

  // 1) Momentum glide: feed the decaying velocity as a momentum delta so the
  //    kinetics carry the fling past the lift (and rubber-stretch + spring back
  //    at the edge). One frame ≈ 1/60 s; velocity is points/sec.
  if (_scroll_momentum) {
    const double dt = 1.0 / 60.0;
    double dx = (double)_scroll_vel.x * dt * kScrollPanPxPerPoint;
    double dy = (double)_scroll_vel.y * dt * kScrollPanPxPerPoint;
    _scroll_vel.x = (CGFloat)((double)_scroll_vel.x * kScrollMomentumDecay);
    _scroll_vel.y = (CGFloat)((double)_scroll_vel.y * kScrollMomentumDecay);
    bool spent = (std::fabs(_scroll_vel.x) <= kScrollMomentumCutoff &&
                  std::fabs(_scroll_vel.y) <= kScrollMomentumCutoff);
    [self feedScrollDX:dx dy:dy began:false changed:false ended:false
              momentum:true momentumEnded:spent];
    if (spent) _scroll_momentum = false;
    // The momentum feed above also drives the at-edge spring-back through the
    // kinetics, so fall through to the bounce step for the settle frames.
  }

  // 2) Rubber-band spring-back. Returns false once settled (and no momentum is
  //    still in flight) -> stop the link.
  bool animating = [self scrollBounceStep];
  if (!animating && !_scroll_momentum) [self scrollLinkStop];
}

// One spring-back step for the latched target via the shared *_bounce_step.
// Returns true while still animating. Repaints + fires SCROLL_CHANGED through the
// shared commit paths (same as the macOS gridBounceTick / sectionBounceTick).
- (bool)scrollBounceStep
{
  using namespace neui_detail;
  if (!session) return false;

  if (_pan_kind == kPanTargetGrid) {
    auto* hw = session->get_widget(_scroll_widget);
    GridModel* model = hw ? hw->grid_model_ptr() : nullptr;
    if (!model) return false;
    auto cfg = grid_read_config(hw->attrs.get());
    GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                             cfg.row_h, cfg.header_h);
    bool more = grid_scroll_bounce_step(*model, vp, cfg.row_h);
    [self setNeedsDisplay];
    return more;
  }

  if (_pan_kind == kPanTargetSection) {
    auto* sw = session->get_widget(_scroll_widget);
    SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
    const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
    if (!st || !L) return false;
    bool more_v = section_scroll_bounce_step(*st, *L, false);
    bool more_h = section_scroll_bounce_step(*st, *L, true);
    [self setNeedsDisplay];
    sw->notify_scroll_changed();
    return more_v || more_h;
  }

  return false;   // line widgets have no spring-back
}

// UIGestureRecognizerDelegate: let the pan run alongside the view's own touch
// handling. UIKit's touch* callbacks fire regardless of recognizer state, so a
// pan that scrolls and a tap that selects coexist. Under the deferred-tap model
// (PHASE-2 fix #4) a selectable widget's DOWN is held back until the gesture
// settles: a stationary lift synthesizes the full tap (touchesEnded:), while a
// pan .began latches a scroll + discards the pending tap (handleScrollPan:), so a
// swipe never selects the row under the finger. Recognizing simultaneously with
// the hover gesture is also fine.
- (BOOL)gestureRecognizer:(UIGestureRecognizer*)g
shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer*)other
{
  (void)g; (void)other;
  return YES;
}

// Stop a stray scroll link if the view leaves the hierarchy mid-glide (handled
// alongside the toast link in willMoveToWindow: below).

// ---------------------------------------------------------------------------
// Toast repaint heartbeat. The shared Session::paint_toast advances the toast's
// phase math (read from platform_now_ms) each time the frame repaints, so all
// this needs to supply is a steady ~60 Hz setNeedsDisplay. CADisplayLink is the
// vsync-aligned UIKit timer (the analogue of the macOS NSTimer toast heartbeat);
// it self-throttles to the display refresh and pauses with the app, so it can't
// starve drawRect: the way a tight invalidate loop could.

- (void)toastTick:(CADisplayLink*)link
{
  (void)link;
  // paint_toast clears ts.active + calls platform_stop_toast_animation once the
  // lifetime elapses, so this tick stops driving repaints on its own.
  [self setNeedsDisplay];
}

- (void)toastStart
{
  if (_toast_link) return;  // already running - don't stack a second link
  _toast_link = [CADisplayLink displayLinkWithTarget:self
                                            selector:@selector(toastTick:)];
  [_toast_link addToRunLoop:[NSRunLoop mainRunLoop]
                    forMode:NSRunLoopCommonModes];
}

- (void)toastStop
{
  if (!_toast_link) return;
  [_toast_link invalidate];   // removes it from the runloop + releases the link
  _toast_link = nil;
}

// Stop the heartbeat when the view leaves the hierarchy so a stray link can't
// outlive its frame (e.g. a frame destroyed mid-toast).
- (void)willMoveToWindow:(UIWindow*)newWindow
{
  [super willMoveToWindow:newWindow];
  if (!newWindow) {
    [self toastStop];
    [self scrollLinkStop];   // don't let a momentum glide outlive the frame
  }
}

// ---------------------------------------------------------------------------
// Hamburger menu. When the frame has a MENUBAR child, a native UIButton lives
// in the top inset band (below the status bar / notch). Its .menu is built from
// the MenubarWidget model (menu_ios.h) and showsMenuAsPrimaryAction = YES, so a
// tap opens the native UIMenu popover with no manual tap handling. Activations
// route each UIAction back through Session::dispatch_menu_event(cmd_id), exactly
// as the desktop hosts route menu picks. The Linux host-drawn cascading band
// (Session::paint_menubar) is gated off on iOS, so none of that code runs.

- (void)refreshHamburger
{
  if (!session || !session->_widgets.exists(widget_index)) {
    if (_hamburger) { [_hamburger removeFromSuperview]; _hamburger = nil; }
    return;
  }

  xpl_host::MenubarWidget* mb = session->frame_menubar(widget_index);

  // The system menu bar (iPad) is cached by UIKit; a model mutation (tree
  // add/remove/set_text/set_shortcut routes here via platform_menubar_refresh)
  // must mark it dirty so -buildMenuWithBuilder: re-runs with the new tree. Only
  // request a rebuild when a menu bar is actually relevant (a MENUBAR exists and
  // the system bar is available) so we don't churn the bar on iPhone / menubar-
  // less frames. setNeedsRebuild coalesces, so multiple mutations in a tick cost
  // one rebuild.
  if (mb && neui_detail::menu_ios_system_menubar_available()) {
    if (@available(iOS 13.0, *)) [UIMenuSystem.mainSystem setNeedsRebuild];
  }

  // Hamburger-visibility rule (menu_ios_hamburger_should_hide): hide the in-frame
  // hamburger only where the OS provides a navigable system menu bar AND it's
  // reachable - iPad on iPadOS 26+ AND windowed (Stage Manager / Split View),
  // where buildMenuWithBuilder: feeds the swipe-from-top bar. Show it everywhere
  // else - full-screen iPad 26 (the menu bar exists but can't be swiped down),
  // iPhone, iPad < 26 - so the menu stays reachable. The window drives the
  // full-screen test; re-evaluated on resize (see reportResizeIfChanged).
  bool fullscreen = neui_detail::menu_ios_window_is_fullscreen(self.window);
  bool want_button = (mb != nullptr) &&
                     !neui_detail::menu_ios_hamburger_should_hide(self.window);

  // Log the verdict so the iPad-sim version split (and the full-screen vs
  // windowed split) is observable.
  printf("[neui-menu] hamburger: menubar_avail=%d fullscreen=%d shown=%d\n",
         neui_detail::menu_ios_system_menubar_available() ? 1 : 0,
         fullscreen ? 1 : 0,
         want_button ? 1 : 0);

  if (!want_button) {
    if (_hamburger) { [_hamburger removeFromSuperview]; _hamburger = nil; }
    return;
  }

  if (!_hamburger) {
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    // A list-style glyph reads as a menu affordance; fall back to a text glyph
    // on older OSes without SF Symbols on UIButton.
    if (@available(iOS 13.0, *)) {
      UIImage* img = [UIImage systemImageNamed:@"line.3.horizontal"];
      if (img) [b setImage:img forState:UIControlStateNormal];
      else     [b setTitle:@"☰" forState:UIControlStateNormal];
    } else {
      [b setTitle:@"☰" forState:UIControlStateNormal];  // U+2630 TRIGRAM
    }
    // Tap opens the menu directly - no target/action needed.
    if (@available(iOS 14.0, *)) b.showsMenuAsPrimaryAction = YES;
    [self addSubview:b];
    _hamburger = b;
  }

  // Build the UIMenu from the menubar model. Capture the Session* + the encoded
  // menubar widget id; route picks through dispatch_menu_event on the owning
  // session (the button is torn down with the frame, so the captured pointer
  // stays valid for the button's lifetime).
  xpl_host::Session* sess = session;
  if (@available(iOS 14.0, *)) {
    auto pick = [sess](uint32_t cmd_id) {
      if (sess) sess->dispatch_menu_event(cmd_id);
    };
    _hamburger.menu = neui_detail::menu_ios_build_uimenu(*mb, @"Menu", pick);
  }

  // Position the (possibly newly-created) button now. Deliberately NOT
  // [self setNeedsLayout] - refreshHamburger is itself called outside the
  // layout pass (menu mutations / safe-area changes), and re-triggering layout
  // from here would loop with any layout-driven refresh.
  [self layoutHamburger];
}

// Position the hamburger in the inset band: leading edge, below the safe-area
// top. Split out from layoutSubviews so refreshHamburger can re-place a
// freshly-created button without a full layout pass.
- (void)layoutHamburger
{
  if (!_hamburger) return;
  CGFloat top = 0;
  if (@available(iOS 11.0, *)) top = self.safeAreaInsets.top;
  const CGFloat pad = 6;
  _hamburger.frame = CGRectMake(pad, top + (kHamburgerBandH - 36) * 0.5f,
                                44, 36);
}

// Re-place the hamburger on every layout pass (bounds / safe-area changes).
// Does NOT rebuild the menu - that only happens on model mutations via
// refreshHamburger - so this is a cheap frame reposition with no layout loop.
- (void)layoutSubviews
{
  [super layoutSubviews];
  [self layoutHamburger];
}

@end

// ---------------------------------------------------------------------------
// NEUIViewController - root VC whose view is the NEUIView. Drives RESIZE on
// bounds changes (rotation / split-view / Stage Manager). The painted view
// fills the controller's view bounds.

@interface NEUIViewController : UIViewController
{
@public
  xpl_host::Session* session;
  uint32_t           widget_index;
@private
  // Last logical size we reported via RESIZE, so layout passes that don't
  // actually change the bounds stay silent.
  int                _last_w;
  int                _last_h;
  // Last top inset (safe area + hamburger band) reported, so a safe-area flip
  // that doesn't change the bounds (status bar appearing, first inset resolve)
  // still re-fires RESIZE - the usable client rect changed even though the
  // frame size didn't, so the client must re-lay-out below the new inset.
  int                _last_top_inset;
}
@end

@implementation NEUIViewController

- (void)loadView
{
  NEUIView* view = [[NEUIView alloc] initWithFrame:CGRectZero];
  view->session      = session;
  view->widget_index = widget_index;
  // Match the screen scale so Retina draws crisply (UIView default already does
  // this, but be explicit - the CG backend reads scale off the window/screen).
  view.contentScaleFactor = UIScreen.mainScreen.scale;
  view.multipleTouchEnabled = NO;  // single-pointer synthesis in v1
  // iPad pointer hover + pointer cursor (M6). Idempotent; no-op pre-13.4.
  [view installEnhancedInput];
  // Touch-pan scroll (PHASE-2). Coexists with the touch handlers; latches a
  // scrollable target only on .began over scrollable content, so taps are safe.
  [view installScrollPan];
  self.view = view;

  // Refresh the palette now that a view trait environment exists. platform_init
  // seeds it earlier from currentTraitCollection, which can still be the default
  // (light) before any window connects; by loadView the scene appearance is
  // current. Live flips arrive via traitCollectionDidChange:. No-op if nothing
  // changed.
  neui_detail::refresh_theme_palette_ios();
}

- (NEUIView*)neuiView
{
  return [self.view isKindOfClass:[NEUIView class]] ? (NEUIView*)self.view : nil;
}

- (void)reportResizeIfChanged
{
  if (!session || !session->_widgets.exists(widget_index)) return;
  CGSize sz = self.view.bounds.size;
  int w = (int)sz.width;
  int h = (int)sz.height;
  if (w <= 0 || h <= 0) return;
  // The effective top inset (status-bar/notch safe area + hamburger band when
  // a menubar exists) feeds widget_client_rect; track it alongside the bounds.
  int top_inset = session->frame_top_inset(widget_index);
  if (w == _last_w && h == _last_h && top_inset == _last_top_inset) return;
  _last_w = w;
  _last_h = h;
  _last_top_inset = top_inset;

  auto& wd = session->_widgets[widget_index];
  wd.width  = w;
  wd.height = h;
  session->resize_render_ctx(widget_index, (uint32_t)w, (uint32_t)h);

  neui_event_t ev = {};
  ev.type               = NEUI_EVENT_RESIZE;
  ev.data.resize.widget = { wd.widget_id };
  ev.data.resize.width  = w;
  ev.data.resize.height = h;
  session->dispatch_event(&ev);
  // The safe-area insets (and thus safe_area_insets / get_client_rect) change on
  // rotation + when the notch/status-bar inset first resolves, so notify the
  // client that the metrics changed too (alongside RESIZE).
  dispatch_metrics_changed_xpl_ios(session, widget_index);
  // A bounds change can be a full-screen <-> windowed transition (entering/
  // leaving Stage Manager / Split View), which flips the hamburger-visibility
  // rule on iPad 26+. Re-evaluate so the hamburger appears when going full-screen
  // and disappears when windowing. refreshHamburger is cheap when nothing changed
  // (it re-uses the existing button) and is called outside the layout pass, so it
  // won't loop. No-op on iPhone / iPad < 26 (rule is version-gated).
  [[self neuiView] refreshHamburger];
  [self.view setNeedsDisplay];
}

// Make the content view first responder so hardware-keyboard UIPress events
// reach pressesBegan:/keyCommands (M6). A focused text widget will still pop the
// soft keyboard via its own first-responder path; this is for the physical
// keyboard accelerators + key routing into the painted xpl widgets. Harmless on
// touch-only devices (no UIPress arrives).
- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];
  if (self.view && ![self.view isFirstResponder])
    [self.view becomeFirstResponder];
  // This frame's view just entered the responder chain; on iPad with a system
  // menu bar that means its -buildMenuWithBuilder: should now contribute the
  // frame's menubar. UIKit usually rebuilds on a responder-chain change, but
  // request it explicitly so a freshly-shown frame's menus appear without
  // waiting for the next implicit rebuild. Gated so it's a no-op on iPhone.
  if (neui_detail::menu_ios_system_menubar_available()) {
    if (@available(iOS 13.0, *)) [UIMenuSystem.mainSystem setNeedsRebuild];
  }
}

- (void)viewDidLayoutSubviews
{
  [super viewDidLayoutSubviews];
  // NOTE: do NOT rebuild the hamburger here - refreshHamburger creates/positions
  // a subview, which marks the view needing layout and would re-enter this
  // callback in an infinite loop. The button is built once on menu attach /
  // safe-area change, and re-placed cheaply in NEUIView::layoutSubviews.
  [self reportResizeIfChanged];
}

// A safe-area change (status bar appearing, the first inset resolve after the
// window binds to its scene, or rotation revealing a notch on a new edge) does
// not change the view bounds, so reportResizeIfChanged would otherwise stay
// silent. The inset feeds widget_client_rect + the hamburger band position, so
// re-fire RESIZE (now inset-aware) and re-layout the button.
- (void)viewSafeAreaInsetsDidChange
{
  [super viewSafeAreaInsetsDidChange];
  [[self neuiView] refreshHamburger];
  [self reportResizeIfChanged];
}

- (void)viewWillTransitionToSize:(CGSize)size
       withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
  [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
  // The bounds-driven reportResizeIfChanged in viewDidLayoutSubviews fires
  // after the transition commits, so the size is already current there. The
  // override exists so the controller participates in the rotation animation;
  // a per-step repaint keeps the content live during the spin.
  [coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext> ctx) {
    (void)ctx;
    [self.view setNeedsDisplay];
  } completion:nil];
}

@end

// ---------------------------------------------------------------------------
// Helpers.

namespace {

// Returns the foreground-active UIWindowScene, falling back to the first
// connected window scene. A scene is always connected by the time the client
// builds UI (it builds from scene:willConnectTo:), so this is non-null in
// practice; it returns nil only if called before any scene connects.
API_AVAILABLE(ios(13.0))
UIWindowScene* active_window_scene()
{
  UIWindowScene* fallback = nil;
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
    if (![scene isKindOfClass:[UIWindowScene class]]) continue;
    UIWindowScene* ws = (UIWindowScene*)scene;
    if (!fallback) fallback = ws;
    if (scene.activationState == UISceneActivationStateForegroundActive)
      return ws;
  }
  return fallback;
}

// Resolve the NEUIView from a retained UIWindow native handle (the window's
// rootViewController is a NEUIViewController whose view is the NEUIView).
NEUIView* neui_view_for_window(void* native_handle)
{
  if (!native_handle) return nil;
  UIWindow* w = (__bridge UIWindow*)native_handle;
  UIViewController* vc = w.rootViewController;
  if ([vc isKindOfClass:[NEUIViewController class]])
    return [(NEUIViewController*)vc neuiView];
  if ([vc.view isKindOfClass:[NEUIView class]])
    return (NEUIView*)vc.view;
  return nil;
}

// Status-bar / notch safe-area top (logical px) for a frame native handle.
// Resolved off the frame's view, falling back to the window's safeAreaInsets.
// Returns 0 before the view is laid out (insets are 0 until the window joins a
// scene) - frame_top_inset re-reads it on every layout / safe-area change.
int safe_area_top_for_window(void* native_handle)
{
  NEUIView* view = neui_view_for_window(native_handle);
  if (@available(iOS 11.0, *)) {
    if (view) return (int)(view.safeAreaInsets.top + 0.5);
    if (native_handle) {
      UIWindow* w = (__bridge UIWindow*)native_handle;
      return (int)(w.safeAreaInsets.top + 0.5);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// NEUI_API_METRICS iOS-real seams for the xpl host (installed into the shared
// vtable at platform_init). measure_text resolves a UIFont and measures the
// string; safe_area_insets reads the frame view's safeAreaInsets, folding the
// hamburger band into `top` to stay consistent with frame_top_inset /
// get_client_rect.

// UIFont for measurement. Empty family => system; weight 0 => Regular,
// 700+ => Bold (same mapping as the cg backend). size <= 0 => the painted body
// default scaled by the Dynamic-Type scale (== NEUI_METRIC_BODY_FONT_SIZE).
UIFont* metrics_font_xpl_ios(const char* family, float size_px, int weight)
{
  if (size_px <= 0.0f)
    size_px = neui_detail::METRIC_BASE_BODY_FONT * neui_detail::painted_ui_scale();
  // Memoize the last-resolved font: a layout pass measures many strings at the
  // same (family, size, weight), so this collapses N UIFont constructions to
  // one. Main-thread only (UIKit), so a plain static memo is safe.
  static UIFont*   s_font   = nil;
  static float     s_size   = 0.0f;
  static int       s_weight = INT_MIN;
  static NSString* s_family = nil;
  NSString* fam = (family && *family) ? [NSString stringWithUTF8String:family] : nil;
  if (s_font && s_size == size_px && s_weight == weight &&
      ((s_family == nil) == (fam == nil)) &&
      (fam == nil || [s_family isEqualToString:fam]))
    return s_font;
  UIFont* f = nil;
  if (fam) f = [UIFont fontWithName:fam size:size_px];
  if (!f) {
    UIFontWeight w = (weight >= 700) ? UIFontWeightBold : UIFontWeightRegular;
    f = [UIFont systemFontOfSize:size_px weight:w];
  }
  s_font = f; s_size = size_px; s_weight = weight; s_family = fam;
  return f;
}

int metrics_measure_text_xpl_ios(neui_session_t /*session*/, const char* text,
                                 const char* family, float size_px, int weight)
{
  if (!text || !*text) return 0;
  UIFont* font = metrics_font_xpl_ios(family, size_px, weight);
  if (!font) return 0;
  NSString* str = [NSString stringWithUTF8String:text];
  if (!str) return 0;
  CGSize sz = [str sizeWithAttributes:@{ NSFontAttributeName : font }];
  return (int)(sz.width + 0.5f);
}

void metrics_safe_area_xpl_ios(neui_session_t session, neui_widget_t frame,
                               int* left, int* top, int* right, int* bottom)
{
  if (left)   *left   = 0;
  if (top)    *top    = 0;
  if (right)  *right  = 0;
  if (bottom) *bottom = 0;
  xpl_host::Session* s = xpl_host::session_by_id(session.session);
  if (!s) return;
  uint32_t idx = frame.id & 0xffff;
  if (!s->_widgets.exists(idx)) return;
  auto& fw = s->_widgets[idx];
  if (!fw.is_frame()) return;
  NEUIView* view = neui_view_for_window(fw.native_handle);
  if (!view) return;
  if (@available(iOS 11.0, *)) {
    UIEdgeInsets ins = view.safeAreaInsets;
    if (left)   *left   = (int)(ins.left + 0.5);
    if (right)  *right  = (int)(ins.right + 0.5);
    if (bottom) *bottom = (int)(ins.bottom + 0.5);
    // Fold the hamburger menu band into `top` (safe-area top + band) so this
    // matches the content rect get_client_rect / frame_top_inset report.
    if (top) *top = s->frame_top_inset(idx);
  }
}

// Build the UIWindow + NEUIViewController + NEUIView triad, wire the render
// context, and stash a +1 retain on wd.native_handle. The UIKit analogue of
// macOS install_view_and_context. `present_modally` selects DIALOG behavior
// (presented on the appwindow's root VC) vs. APPWINDOW (own key window).
void install_window_and_context(xpl_host::Session* session,
                                uint32_t widget_index,
                                xpl_host::WidgetData& wd)
{
  if (@available(iOS 13.0, *)) {
    UIWindowScene* scene = active_window_scene();

    UIWindow* window = scene ? [[UIWindow alloc] initWithWindowScene:scene]
                             : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];

    NEUIViewController* vc = [[NEUIViewController alloc] init];
    vc->session      = session;
    vc->widget_index = widget_index;
    window.rootViewController = vc;

    // Seed the frame size from the scene's coordinate space (the device /
    // split-view area the window will fill) rather than the client's create()
    // seed. On iOS a UIWindow always fills its scene, so laying out children
    // against the create() seed (e.g. 390x800) before the first RESIZE leaves
    // the frame painting only the top fraction of the screen. Seeding now means
    // the client's layout (run from the scene delegate right after this) sees
    // the true size; viewDidLayoutSubviews still fires RESIZE on later changes
    // (rotation / Stage Manager) to correct it.
    CGRect scene_bounds =
      scene ? scene.coordinateSpace.bounds
            : (window ? window.bounds : UIScreen.mainScreen.bounds);
    if (scene_bounds.size.width > 0 && scene_bounds.size.height > 0) {
      wd.width  = (int)scene_bounds.size.width;
      wd.height = (int)scene_bounds.size.height;
    }

    // Force the view to load now so create_context has a real UIView to bind.
    NEUIView* view = [vc neuiView];

    // Render context: the CG backend keys scale off the view's window/screen
    // (falls back to UIScreen.main when the view isn't in a window yet). Sized
    // to the scene-seeded frame dimensions above.
    auto* backend = xpl_host::platform_get_backend();
    if (backend && view) {
      wd.render_ctx = backend->create_context((__bridge void*)view,
                                              (uint32_t)wd.width,
                                              (uint32_t)wd.height);
    }

    CGFloat scale = scene ? scene.screen.scale : UIScreen.mainScreen.scale;
    if (scale <= 0) scale = 1.0;
    wd.dpi = (uint32_t)(96.0 * scale + 0.5);

    // +1 retain handed back as the native handle; released via __bridge_transfer
    // in platform_destroy_window.
    wd.native_handle = (__bridge_retained void*)window;
  }
}

} // namespace

// ---------------------------------------------------------------------------

namespace xpl_host
{
  // -------------------------------------------------------------------------
  // Lifecycle / window management.

  void platform_init()
  {
    // UIApplicationMain owns app bootstrap on iOS; nothing to register here.
    // Seed the theme palette from the current appearance so frames created
    // before the first view lays out already paint with system colours. Live
    // light/dark flips are delivered to NEUIView::traitCollectionDidChange:
    // (iOS has no global appearance notification, unlike macOS).
    neui_detail::ensure_theme_provider_ios();

    // Scale the host-painted widgets' DEFAULT text + layout metrics to the iOS
    // Dynamic-Type body size so they match the native UIKit controls beside them
    // AND follow the user's Larger-Text / accessibility setting (live -
    // recomputed on every content-size change in NEUIView::traitCollectionDidChange:).
    // The canonical painted default is 12px; at the default "Large" category
    // body~17pt -> scale ~1.42, preserving the previous look. Process-wide;
    // desktop hosts never touch it so they stay at 1.0 (byte-for-byte unchanged).
    // Client font/metric attrs still override the scaled default.
    neui_detail::recompute_painted_ui_scale_ios();

    // Install the iOS-real NEUI_API_METRICS seams (UIFont measurement + the
    // frame view's safeAreaInsets) into the shared vtable. Desktop platforms
    // leave the shared desktop defaults in place.
    neui_detail::metrics_measure_seam()   = &metrics_measure_text_xpl_ios;
    neui_detail::metrics_safe_area_seam() = &metrics_safe_area_xpl_ios;
  }

  neui_render_backend_t* platform_get_backend()
  {
    return neui_cg_backend::get_backend();
  }

  void platform_create_appwindow(Session* session, uint32_t widget_index,
                                  WidgetData& wd)
  {
    install_window_and_context(session, widget_index, wd);
  }

  // No in-process borderless plugin window on iOS (no DAW embedding).
  void platform_create_plugwindow(Session* /*session*/, uint32_t /*widget_index*/,
                                   WidgetData& /*wd*/) {}

  void platform_create_dialog(Session* session, uint32_t widget_index,
                               WidgetData& wd, void* owner_native)
  {
    // Build the dialog's own window-scoped view tree, then present its root VC
    // modally on the owner's root VC. iOS modal presentation already blocks
    // interaction with the presenter for the default presentation styles, so
    // NEUI_ATTR_MODAL is satisfied by the OS. UNLIKE the desktop contract,
    // widgets->show(dialog) does NOT block the thread here - it presents and
    // returns; the client dismisses via widgets->destroy.
    install_window_and_context(session, widget_index, wd);

    if (@available(iOS 13.0, *)) {
      if (owner_native) {
        UIWindow* owner = (__bridge UIWindow*)owner_native;
        UIViewController* owner_vc = owner.rootViewController;
        UIWindow* dlg = (__bridge UIWindow*)wd.native_handle;
        UIViewController* dlg_vc = dlg.rootViewController;
        if (owner_vc && dlg_vc) {
          // formSheet keeps the owner partially visible + input-blocked behind
          // the sheet, the closest analogue to a desktop modal dialog.
          dlg_vc.modalPresentationStyle = UIModalPresentationFormSheet;
          [owner_vc presentViewController:dlg_vc animated:YES completion:nil];
        }
      }
    }
  }

  void platform_destroy_window(WidgetData& wd)
  {
    if (!wd.native_handle) return;

    // Tear down the render context before the view goes away.
    auto* backend = xpl_host::platform_get_backend();
    if (backend && wd.render_ctx) {
      // The owning Session releases per-ctx asset uploads; mirror the macOS
      // windowWillClose: teardown order.
      backend->destroy_context(wd.render_ctx);
      wd.render_ctx = nullptr;
    }

    // Release the +1 retain installed in install_window_and_context. If this
    // window's root VC was presented modally (DIALOG), dismiss it first.
    UIWindow* w = (__bridge_transfer UIWindow*)wd.native_handle;
    wd.native_handle = nullptr;

    if (@available(iOS 13.0, *)) {
      UIViewController* vc = w.rootViewController;
      if (vc.presentingViewController)
        [vc.presentingViewController dismissViewControllerAnimated:NO completion:nil];
    }
    w.hidden = YES;
    // ARC releases the local UIWindow* on scope exit.
  }

  void platform_show_window(void* native_handle)
  {
    if (!native_handle) return;
    UIWindow* w = (__bridge UIWindow*)native_handle;
    // A dialog window's content was presented modally at create time; for a
    // top-level appwindow this makes it key + visible.
    [w makeKeyAndVisible];
  }

  void platform_hide_window(void* native_handle)
  {
    if (!native_handle) return;
    UIWindow* w = (__bridge UIWindow*)native_handle;
    w.hidden = YES;
  }

  void platform_set_window_enabled(void* /*native_handle*/, bool /*enabled*/)
  {
    // iOS modal presentation blocks the presenter automatically; no per-window
    // enable/disable toggle is needed (or available).
  }

  void platform_activate_window(void* native_handle)
  {
    if (!native_handle) return;
    UIWindow* w = (__bridge UIWindow*)native_handle;
    [w makeKeyAndVisible];
  }

  void platform_set_window_title(void* /*native_handle*/, const char* /*text*/)
  {
    // iOS windows have no title bar. The scene title (UIWindowScene.title) is
    // surfaced only in the app switcher / Stage Manager; not wired in v1.
  }

  void platform_set_window_pos(void* /*native_handle*/,
                                int /*x*/, int /*y*/, int /*w*/, int /*h*/,
                                uint32_t /*dpi*/)
  {
    // On iOS a UIWindow fills its scene; the client cannot position/size it.
    // The view controller's bounds drive layout instead (see RESIZE handling).
  }

  void platform_post_close(void* /*native_handle*/)
  {
    // No user-driven window close on iOS (the system manages scenes); the
    // client tears windows down via widgets->destroy -> platform_destroy_window.
  }

  float platform_get_scale_factor(void* native_handle)
  {
    if (@available(iOS 13.0, *)) {
      if (native_handle) {
        UIWindow* w = (__bridge UIWindow*)native_handle;
        if (w.windowScene && w.windowScene.screen)
          return (float)w.windowScene.screen.scale;
      }
    }
    return (float)UIScreen.mainScreen.scale;
  }

  void platform_invalidate(void* native_handle)
  {
    NEUIView* view = neui_view_for_window(native_handle);
    [view setNeedsDisplay];
  }

  // UIApplicationMain owns the run loop on iOS; neui never owns / stops it.
  bool platform_run() { return true; }
  bool platform_pump_once() { return true; }
  // No nested pump on iOS - modals use UIKit presentation, not a blocking loop.
  bool platform_run_modal_until(bool* /*keep_running*/) { return true; }

  // DAW-embedding seams (NEUI_API_EMBED). Not supported on iOS yet - an AUv3
  // app-extension embeds through UIKit view-controller containment, a
  // different mechanism from the desktop parent-handle model. The parent is
  // stored (harmless; the frame create path ignores it) so set_parent's
  // validation contract matches the other platforms; fd / pump are inert,
  // same as win32 / macOS where the host's own loop services the frame.
  void platform_set_embed_parent(Session* session, uint32_t widget_index,
                                 void* native_parent)
  {
    if (!session) return;
    auto* wd = session->get_widget(widget_index);
    if (wd) wd->embed_parent = reinterpret_cast<uintptr_t>(native_parent);
  }
  int  platform_embed_event_fd(void* /*native_handle*/) { return -1; }
  void platform_embed_pump_and_tick(void* /*native_handle*/) {}

  // -------------------------------------------------------------------------
  // Menu bar.
  //
  // iOS has no AppKit-style global menu bar on iPhone, so the host surfaces a
  // frame's MENUBAR child as a native UIMenu opened from a hamburger UIButton in
  // the frame's top inset (see NEUIView::refreshHamburger + menu_ios.h). This is
  // deliberately NOT the Linux in-frame band: platform_menubar_in_frame() stays
  // false so Session::paint_menubar (the host-drawn cascading dropdown painter)
  // never runs on iOS. The top inset is reserved via platform_frame_extra_top_inset
  // instead, which paints nothing.
  //
  // The menu MODEL still lives in MenubarWidget (built by tree t_add). For that
  // to populate, platform_menubar_create must return a non-null sentinel (like
  // Linux) - otherwise t_add bails on `!mb.hmenu`. The structural mutators are
  // no-ops; the UIMenu is rebuilt lazily from the model on platform_menubar_refresh
  // (which t_add / t_remove / set_shortcut call with the owning frame handle).
  bool  platform_menubar_in_frame() { return false; }

  // Reserve the status-bar/notch safe area, plus a hamburger band when the
  // frame carries a menubar. Painted by nothing - the hamburger is a real
  // UIButton subview. 0 contributions from this seam keep the desktop hosts
  // unchanged (they return 0).
  int   platform_frame_extra_top_inset(void* frame_native_handle, bool has_menubar)
  {
    int top = safe_area_top_for_window(frame_native_handle);
    return top + (has_menubar ? kHamburgerBandH : 0);
  }

  // Non-null sentinel so t_add populates the MenubarWidget model (mirrors the
  // Linux platform_menubar_create). The value is never dereferenced.
  void* platform_menubar_create(uint32_t /*widget_id*/) { return reinterpret_cast<void*>(0x1); }
  void  platform_menubar_destroy(void* /*hmenu*/) {}

  // attach / refresh carry the frame native handle - rebuild that frame's
  // hamburger menu from the (now-updated) model.
  void  platform_menubar_attach(void* frame, void* /*hmenu*/)
  {
    NEUIView* view = neui_view_for_window(frame);
    [view refreshHamburger];
  }
  void  platform_menubar_refresh(void* frame)
  {
    NEUIView* view = neui_view_for_window(frame);
    [view refreshHamburger];
  }

  // The model mutators are no-ops on iOS - the UIMenu is rebuilt wholesale from
  // the MenubarWidget model on refresh (above), so per-item native bookkeeping
  // isn't needed.
  void* platform_menubar_add_popup(void* /*hmenu*/, const char* /*text*/)               { return reinterpret_cast<void*>(0x1); }
  void  platform_menubar_add_item(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_add_separator(void* /*hmenu*/, uint32_t /*cmd*/)               {}
  void  platform_menubar_remove_popup(void* /*hmenu*/, void* /*sub*/)                   {}
  void  platform_menubar_remove_item(void* /*hmenu*/, uint32_t /*cmd*/)                 {}
  void  platform_menubar_enable_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*en*/)    {}
  void  platform_menubar_enable_popup(void* /*hmenu*/, void* /*sub*/, bool /*en*/)      {}
  // The UIMenu is rebuilt from the model on refresh, which re-reads checked.
  void  platform_menubar_check_item(void* /*hmenu*/, uint32_t /*cmd*/, bool /*chk*/)    {}
  void  platform_menubar_set_item_text(void* /*hmenu*/, uint32_t /*cmd*/, const char* /*t*/) {}
  void  platform_menubar_set_item_shortcut(void* /*hmenu*/, uint32_t /*cmd*/,
                                            uint32_t /*mods*/, uint32_t /*key*/)        {}

  void platform_set_window_icon(WidgetData& /*wd*/, const char* /*path*/) {}
  void platform_apply_size_constraints(void* /*nh*/, int /*minw*/, int /*minh*/,
                                        int /*maxw*/, int /*maxh*/) {}

  // Image loading. Delegates to the shared CGImageSource loader
  // (hosts/shared/ios/image_loader_ios.h) - same ImageIO decode + BGRA8-premul
  // normalisation as macOS, with an iOS-bundle resolution fallback so bundled
  // PNGs resolve from the .app Resources root. Allocation is new[], freed by
  // platform_free_image's delete[] (matching free_image_bgra8).
  uint8_t* platform_load_image(const char* path,
                                uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_ios(path, width_out, height_out);
  }

  void platform_free_image(uint8_t* pixels) { delete[] pixels; }

  // System clipboard. Delegates to hosts/shared/ios/clipboard_ios.h
  // (UIPasteboard.generalPasteboard). PRIMARY selection is X11-only -> no-op.
  bool platform_clipboard_set_text(const char* utf8, uint32_t length)
  {
    return neui_detail::clipboard_set_text_ios(utf8, length);
  }
  int  platform_clipboard_get_text(char* buf, int buflen)
  {
    return neui_detail::clipboard_get_text_ios(buf, buflen);
  }
  bool platform_clipboard_has_text()
  {
    return neui_detail::clipboard_has_text_ios();
  }
  void platform_clipboard_set_primary(const char* /*u*/, uint32_t /*l*/)      {}
  int  platform_clipboard_get_primary(char* /*b*/, int /*n*/)                 { return 0; }
  bool platform_clipboard_write_item(const neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_write_item_ios(item);
  }
  bool platform_clipboard_read_item(neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_read_item_ios(item);
  }

  // Drag & drop. iOS uses gesture-driven interactions on the content view (see
  // NEUIView installDragDrop + the UIDrop/UIDragInteractionDelegate methods).
  // register/unregister add/remove both interactions; the per-widget
  // set_drop_target + DRAG_SOURCE-behavior gating lives in the delegate
  // callbacks via the shared dnd_dispatch + behavior machinery.
  bool platform_dnd_register_window(void* native_handle, void* /*session_ptr*/,
                                     uint32_t /*frame_widget_id*/)
  {
    NEUIView* view = neui_view_for_window(native_handle);
    if (!view) return false;
    [view installDragDrop];
    return true;
  }
  void platform_dnd_unregister_window(void* native_handle)
  {
    NEUIView* view = neui_view_for_window(native_handle);
    if (!view) return;
    NSArray<id<UIInteraction>>* existing = [view.interactions copy];
    for (id<UIInteraction> i in existing) {
      if ([i isKindOfClass:[UIDropInteraction class]] ||
          [i isKindOfClass:[UIDragInteraction class]])
        [view removeInteraction:i];
    }
  }

  // SEMANTIC DIVERGENCE: iOS cannot start a drag from arbitrary code - a UIDrag
  // must originate from a UIDragInteraction delegate responding to the system
  // long-press-drag gesture. So neui's blocking begin_drag() contract does NOT
  // hold on iOS: it returns NONE immediately. A widget becomes draggable by
  // carrying a DRAG_SOURCE behavior asset, which the UIDragInteraction delegate
  // (itemsForBeginningSession:) resolves on the gesture - the drag is then
  // entirely gesture-driven (documented in include/neui/d/dnd.h).
  uint32_t platform_dnd_begin_drag(void* /*native_handle*/,
                                     neui_detail::DataItem* /*item*/,
                                     uint32_t /*allowed_actions*/,
                                     void* /*preview_native*/,
                                     int /*hot_x*/, int /*hot_y*/)            { return 0; }
  // No standalone preview surface: the UIDragInteraction builds its own drag
  // preview from the source view (v1). A custom-image preview would attach a
  // UIDragPreview in previewForLiftingItem:; deferred. Documented no-op.
  void*    platform_make_drag_preview(const uint8_t* /*bgra_premul*/,
                                       uint32_t /*w_px*/, uint32_t /*h_px*/,
                                       float /*scale*/)                       { return nullptr; }

  // No mouse cursor on touch (iPad pointer styles are a later milestone).
  void     platform_set_cursor(int /*kind*/)                                  {}

  // No pointer warping: iOS (touch input) has no mouse cursor to pin, so relative
  // (unbounded) pointer mode reports unsupported and NEUI_API_POINTER's
  // begin_relative returns false. A client's drag then behaves as an ordinary
  // bounded one rather than silently doing nothing.
  bool platform_supports_relative_pointer()                                   { return false; }
  bool platform_begin_relative_pointer(void*, int*, int*)                     { return false; }
  void platform_end_relative_pointer(void*, int, int)                         {}

  // Client timers (NEUI_API_TIMER). Same shape as the macOS seam: session-
  // scoped, so it hangs off the main runloop rather than off a view, and it
  // joins NSRunLoopCommonModes so the tick survives UIKit tracking loops
  // (a scroll / drag in progress must not freeze a client animation).
  static std::unordered_map<Session*, NSTimer*>& ios_session_timers()
  {
    // Immortal for the same reason as the macOS registry: ~Session runs during
    // static teardown AFTER this TU's statics would have been destroyed.
    static auto* m = new std::unordered_map<Session*, NSTimer*>();
    return *m;
  }

  // iOS: touch coords come raw from locationInView: and the view sizing
  // ignores the zoom, so scaling paint alone would hit-test in a different
  // space than it draws. Inert here rather than broken - deferred, not denied.
  bool platform_supports_ui_scale() { return false; }
  // Not wired: the iOS touch path has no tree-popup hit-test, so the cascade
  // would paint over the UI with taps falling through it and nothing able to
  // dismiss it (there is no Esc). A UIMenu / UIContextMenuInteraction bridge is
  // the right answer here, not a port of the pointer-shaped hit-testing.
  bool platform_supports_tree_popup() { return false; }

  void platform_timer_start(Session* session, uint32_t interval_ms)
  {
    if (!session || interval_ms == 0) return;
    platform_timer_stop(session);
    NSTimer* t = [NSTimer timerWithTimeInterval:(double)interval_ms / 1000.0
                                        repeats:YES
                                          block:^(NSTimer*) {
      auto& m = ios_session_timers();
      if (m.find(session) == m.end()) return;   // torn down between fires
      session->tick_client_timers();
    }];
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    ios_session_timers()[session] = t;
  }

  void platform_timer_stop(Session* session)
  {
    if (!session) return;
    auto& m = ios_session_timers();
    auto it = m.find(session);
    if (it == m.end()) return;
    [it->second invalidate];
    m.erase(it);
  }

  // Toast animation heartbeat. The toast is painted inside the frame's NEUIView
  // (shared Session::paint_toast, topmost in the paint pass), so we just kick
  // that view's CADisplayLink, which calls setNeedsDisplay each vsync until the
  // toast lifetime elapses (paint_toast then calls platform_stop_toast_animation).
  void platform_start_toast_animation(void* native_handle)
  {
    NEUIView* view = neui_view_for_window(native_handle);
    [view toastStart];
  }
  void platform_stop_toast_animation(void* native_handle)
  {
    NEUIView* view = neui_view_for_window(native_handle);
    [view toastStop];
  }
  // Monotonic clock (ms) for the shared toast phase math. CACurrentMediaTime is
  // mach_absolute_time-backed (seconds since boot, unaffected by wall-clock
  // changes); the shared code only ever reads differences, so the absolute
  // origin is irrelevant.
  uint64_t platform_now_ms()
  {
    return static_cast<uint64_t>(CACurrentMediaTime() * 1000.0);
  }

  // Modal message box. UIAlertController is ASYNC on iOS (it can't block + return
  // the chosen button synchronously like Win32 / macOS), so message_box_ios
  // presents the alert and returns a sentinel (NEUI_MB_IOS_PENDING) immediately;
  // result-to-client delivery is deferred. Resolve the presenting VC from the
  // frame's UIWindow.rootViewController; if a modal is already up, present on top
  // of it (presentedViewController). 0 = failure per contract (no VC to present).
  int platform_message_box(void* native_handle, const char* text,
                           const char* caption, uint32_t flags)
  {
    if (!native_handle) return 0;
    if (@available(iOS 13.0, *)) {
      UIWindow* w = (__bridge UIWindow*)native_handle;
      UIViewController* presenter = w.rootViewController;
      // If the root VC is already presenting (e.g. a DIALOG), walk to the
      // top-most presented VC so the alert isn't swallowed.
      while (presenter.presentedViewController)
        presenter = presenter.presentedViewController;
      return neui_detail::message_box_ios(presenter, text, caption, flags);
    }
    return 0;
  }

} // namespace xpl_host

// ---------------------------------------------------------------------------
// App-level menu-bar contribution hooks (iPad system menu bar) for the xpl host.
// Twin of the native host's neui_ios_native_* hooks (hosts/ios/window.mm). See
// hosts/ios/application_ios.mm for why the contribution moved to the app level
// (a content view's -buildMenuWithBuilder: is unreliably consulted; UIApplication
// is always in the responder chain). NEUIApplication calls whichever pair is
// linked; both gate internally so calling both is harmless.

namespace {
  // Frontmost realized xpl frame (visible, has a UIWindow native_handle) that
  // carries a MENUBAR child, across every live session. Prefers the key window.
  xpl_host::MenubarWidget* xpl_ios_frontmost_menubar(xpl_host::Session** out_sess,
                                                     uint32_t* out_frame_idx)
  {
    if (out_sess)      *out_sess      = nullptr;
    if (out_frame_idx) *out_frame_idx = 0;
    xpl_host::MenubarWidget* best_mb    = nullptr;
    xpl_host::Session*       best_sess  = nullptr;
    uint32_t                 best_frame = 0;
    bool                     best_is_key = false;

    uint32_t n = xpl_host::session_count();
    for (uint32_t sid = 1; sid <= n; ++sid) {
      xpl_host::Session* s = xpl_host::session_by_id(sid);
      if (!s) continue;
      uint32_t idx = s->_widgets.child(0);
      while (idx != 0) {
        if (s->_widgets.exists(idx)) {
          xpl_host::WidgetData& fw = s->_widgets[idx];
          if (fw.is_frame() && fw.visible && fw.native_handle) {
            xpl_host::MenubarWidget* mb = s->frame_menubar(idx);
            if (mb) {
              bool is_key = false;
              if (@available(iOS 13.0, *)) {
                UIWindow* w = (__bridge UIWindow*)fw.native_handle;
                is_key = w.isKeyWindow;
              }
              if (!best_mb || (is_key && !best_is_key)) {
                best_mb     = mb;
                best_sess   = s;
                best_frame  = idx;
                best_is_key = is_key;
              }
            }
          }
        }
        idx = s->_widgets.next(idx);
      }
    }
    if (out_sess)      *out_sess      = best_sess;
    if (out_frame_idx) *out_frame_idx = best_frame;
    return best_mb;
  }
}

extern "C" unsigned long neui_ios_xpl_build_menubar_menus(
    id<UIMenuBuilder> builder, id key_cmd_target, SEL key_cmd_sel)
{
  if (@available(iOS 13.0, *)) {
    if (!neui_detail::menu_ios_system_menubar_available()) return 0;
    xpl_host::Session* sess = nullptr;
    xpl_host::MenubarWidget* mb = xpl_ios_frontmost_menubar(&sess, nullptr);
    if (!mb || !sess) return 0;
    xpl_host::Session* s = sess;
    auto pick = [s](uint32_t cmd_id) { if (s) s->dispatch_menu_event(cmd_id); };
    // Merge popups matching a standard menu (File/Edit/View/...) into it; insert
    // non-matching ones as new top-level menus - avoids duplicating UIKit's
    // pre-created empty standard menus. Shared with the native host.
    unsigned long merged = 0, top = 0;
    neui_detail::menu_ios_contribute_menubar(
        builder, *mb, mb->widget_id, pick, key_cmd_target, key_cmd_sel,
        &merged, &top);
    // Diagnostic mirrors the native hook so either host produces the same
    // [neui-menu] signal (captured by simctl launch --console).
    printf("[neui-menu] buildMenuWithBuilder(xpl): avail=1 menubar=1 merged=%lu top=%lu\n",
           merged, top);
    fflush(stdout);
    return merged + top;
  }
  return 0;
}

extern "C" bool neui_ios_xpl_menu_key_command(UIKeyCommand* cmd)
{
  uint32_t key  = neui_detail::ios_input_to_neui_key(cmd.input);
  uint32_t mods = neui_detail::ios_modifiers_to_neui(cmd.modifierFlags);
  if (key == NEUI_KEY_NONE) return false;
  xpl_host::Session* sess = nullptr;
  xpl_host::MenubarWidget* mb = xpl_ios_frontmost_menubar(&sess, nullptr);
  if (!mb || !sess) return false;
  return sess->try_menubar_accel(key, mods);
}
