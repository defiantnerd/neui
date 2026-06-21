// Native iOS host - UIKit lifecycle + painted views + native control creation
// + the paint pass. The UIKit-coupled half of the host (the C++ API tables
// live in widgets.mm).
//
// MILESTONE 7. Structural mirror of hosts/macos/window.mm, scoped to the v1
// core subset:
//   APPWINDOW / DIALOG  -> a UIWindow bound to the active UIWindowScene whose
//                          rootViewController hosts a NEUINativeIOSContentView. The
//                          content view paints the frame background + reserves
//                          a top inset (safe area + hamburger band).
//   LABEL/BUTTON/INPUTBOX/MULTILINE/CHECKBOX[3]/SLIDER -> native UIKit controls
//                          (UILabel / UIButton / UITextField / UITextView /
//                          UISwitch / UISlider), added as subviews with frames
//                          in logical points (UIView is natively top-left /
//                          Y-down, so no flip - simpler than the macOS
//                          isFlipped path).
//   IMAGE/KNOB/CUSTOMDRAW/SECTION -> a NEUINativeIOSPaintedView whose drawRect: binds
//                          the CG backend via set_current_frame and dispatches
//                          to the SHARED paint helpers (paint_section /
//                          paint_knob / widget_paint_compound / draw_asset),
//                          verbatim with the other hosts.
//   GRID/LISTBOX/TREEVIEW/TABVIEW/TABPAGE -> phase-2 painted placeholders (a
//                          flat panel-bg fill + a TODO label-less rect); see
//                          the TODO(ios phase 2) markers.
//
// Touch input -> synthesized mouse stream, mirroring the M2 xpl pattern. The
// native controls handle their own touches + fire the client events through
// UIControl target/action; the painted views forward touches as
// MOUSE_BUTTON_DOWN/MOVE/UP/CLICK for CUSTOMDRAW + the behavior runtime.

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>   // objc_setAssociatedObject - keeps a UITableView's
                           // data source/delegate alive for the table's lifetime

#include "host.h"
#include "../shared/compound.h"
#include "../shared/widget_paint_section.h"
#include "../shared/widget_paint_knob.h"
#include "../shared/widget_paint_compound.h"
#include "../shared/widget_paint_grid.h"
#include "../shared/widget_paint_tabview.h"
#include "../shared/widget_tabview_host.h"
#include "../shared/scrollbar.h"
#include "../shared/text_edit.h"
#include "../shared/edit_history.h"
#include "../shared/painter.h"
#include "../shared/behavior_runtime.h"
#include "../../backends/cg/cg_backend.h"

#include "../shared/ios/theme_provider_ios.h"
#include "../shared/ios/image_loader_ios.h"
#include "../shared/ios/menu_ios.h"
#include "../shared/ios/message_box_ios.h"
#include "../shared/ios/keys_ios.h"
#include "../shared/ios/checkbox_image_ios.h"
#include "../shared/ios/clipboard_ios.h"
#include "../shared/ios/dnd_ios.h"
#include "../shared/dnd_modifier_suggest.h"
#include "../shared/metrics.h"

#include <cstring>
#include <functional>
#include <vector>

// Headless menu diagnostics. The iPad menu-bar / hamburger code paths print a
// one-line verdict the verification orchestrator greps from the device console
// (`devicectl` / `simctl launch --console`). They fire on every UIKit menu
// rebuild (focus change, keyboard attach, resize), so they're OFF by default -
// build with -DNEUI_IOS_MENU_DIAG=1 to re-enable for an orchestrated run.
#ifndef NEUI_IOS_MENU_DIAG
#define NEUI_IOS_MENU_DIAG 0
#endif
#if NEUI_IOS_MENU_DIAG
#define NEUI_MENU_DIAG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define NEUI_MENU_DIAG(...) ((void)0)
#endif

using neui_detail::ColorRole;
using neui_detail::color;

@class NEUINativeIOSPaintedView;

// Container for a SWITCH-style 2-state CHECKBOX: a UILabel (left, fills the
// remaining width) + a UISwitch pinned to the right edge, laid out
// iOS-Settings-style. Used only when NEUI_IOS_CHECKBOX_STYLE == SWITCH (the
// default); the GLYPH style + CHECKBOX3 keep the bare UIButton path. The
// container IS the widget's native_control, so geometry / enabled / set_text /
// the check get/set branch on [native_control isKindOfClass:] to reach these
// subviews. First-responder is forwarded to the switch so Tab traversal +
// w_set_focus still work. Declared up here (full interface) because the
// namespace-scoped helpers below (set_native_text / check / enabled) touch its
// properties.
@interface NEUIIOSCheckboxSwitchView : UIView
@property (nonatomic, strong) UILabel*  theLabel;
@property (nonatomic, strong) UISwitch* theSwitch;
@end

@implementation NEUIIOSCheckboxSwitchView
- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    _theLabel = [[UILabel alloc] init];
    _theLabel.textColor = UIColor.labelColor;
    [self addSubview:_theLabel];
    _theSwitch = [[UISwitch alloc] init];
    [self addSubview:_theSwitch];
  }
  return self;
}
- (void)layoutSubviews
{
  [super layoutSubviews];
  // Pin the switch to the right edge (it has a fixed intrinsic ~51x31); the
  // label fills the remaining left space, vertically centred against the switch.
  CGSize sw = [_theSwitch intrinsicContentSize];
  CGFloat h = self.bounds.size.height, w = self.bounds.size.width;
  _theSwitch.frame = CGRectMake(w - sw.width, (h - sw.height) * 0.5f, sw.width, sw.height);
  const CGFloat gap = 8;
  CGFloat lw = (w - sw.width - gap);
  if (lw < 0) lw = 0;
  _theLabel.frame = CGRectMake(0, 0, lw, h);
}
// Forward first-responder to the switch so Tab traversal + w_set_focus reach a
// real responder (a plain UIView refuses first responder).
- (BOOL)canBecomeFirstResponder { return [_theSwitch canBecomeFirstResponder]; }
- (BOOL)becomeFirstResponder    { return [_theSwitch becomeFirstResponder]; }
@end

namespace ios_host {
  // Forward decls of the painter draw thunk + helpers defined later in this TU
  // but referenced by the views.
  void NEUI_ABI ios_painter_draw_asset_thunk(void* host_token,
                                             neui_render_backend_t* backend,
                                             neui_render_ctx_t ctx,
                                             neui_asset_t asset,
                                             float x, float y, float w, float h,
                                             uint32_t frame,
                                             uint32_t tint);
  static WidgetData* widget_for_id(uint32_t widget_id, Session** out_sess);
  static void paint_widget_into_ctx(Session* s, WidgetData& wd,
                                    neui_render_backend_t* backend,
                                    float w, float h);
  void mark_widget_dirty_for_paint(WidgetData& wd);
  // LISTBOX / TREEVIEW (native UITableView) helpers defined later in this TU.
  void tree_rebuild_visible_rows_ios(WidgetData& wd);
  void reload_native_tree_selection_ios(WidgetData& wd);
  static int frame_top_inset_ios(Session* s, uint32_t frame_idx);
  void dispatch_metrics_changed_ios(Session* s, uint32_t frame_idx);
  static const char* section_effective_align_ios(WidgetData& wd);
  static const char* section_effective_text_ios(WidgetData& wd);
  void frame_refresh_hamburger_ios(WidgetData& frame);
  static bool is_painted_type_ios(const char* type);
  void section_reposition_children_ios(WidgetData& sec);
  void section_notify_scroll_changed_ios(WidgetData& sec);
  void section_apply_layout_changes_ios(WidgetData& sec);
  void section_ensure_body_view_ios(WidgetData& sec);
  bool invoke_focused_command_ios(uint32_t cmd);
  // Hardware-keyboard accelerator dispatch: match a (key, mods) chord against
  // the frame's MENUBAR model + fire it (built-in command first, else
  // TREE_ITEM_ACTIVATED). Returns true if a menu item matched. Defined below.
  bool try_menubar_accel_ios(Session* s, uint32_t frame_idx, uint32_t key, uint32_t mods);
  // Find the MENUBAR child of a frame (model-only widget driving the menu).
  WidgetData* frame_menubar_ios(Session* s, uint32_t frame_idx);
  // Find the FRONTMOST realized frame (visible APPWINDOW/PLUGWINDOW/DIALOG with a
  // live UIWindow) that carries a MENUBAR child, across every live session in
  // ios_host::sessions. Prefers the key window's frame. Returns the owning
  // Session + the frame's WidgetData via out params and the MENUBAR WidgetData
  // as the result (nullptr = no eligible frame). Used by the app-level
  // -buildMenuWithBuilder: (NEUIApplication), which - unlike a content view - is
  // always in the responder chain so its contribution actually reaches the iPad
  // system menu bar.
  WidgetData* frontmost_menubar_ios(Session** out_sess, WidgetData** out_frame);

  // Adapter exposing the native MENUBAR's TreeNode model in the shape
  // menu_ios.h's UIMenu builder templates expect (parent_item_id / cmd_id /
  // is_separator / shortcut text + typed shortcut_key/mods / enabled). The
  // native host stores menu items as TreeNodes keyed by tree id; this flattens
  // them so ONE builder serves both the hamburger UIButton and the iPad system
  // menu bar (-buildMenuWithBuilder:), exactly like the xpl host's MenubarWidget.
  // cmd_id == the tree id, so dispatch maps it straight back via dispatch_menu_
  // item_ios.
  struct IosMenuItemAdapter {
    uint32_t    parent_item_id = 0;
    std::string text;
    bool        is_separator   = false;
    uint32_t    cmd_id         = 0;
    bool        enabled        = true;
    std::string shortcut;
    uint32_t    shortcut_mods  = 0;
    uint32_t    shortcut_key   = NEUI_KEY_NONE;
  };
  struct IosMenubarAdapter {
    std::vector<uint32_t>                          menu_item_ids_ordered;
    std::unordered_map<uint32_t, IosMenuItemAdapter> menu_items;
  };
  IosMenubarAdapter build_menubar_adapter_ios(WidgetData& mb);
  // Dispatch a single menubar item (built-in command first, else client
  // TREE_ITEM_ACTIVATED). Defined below; declared here for the content view's
  // hamburger pick + system-menu-bar contribution above its definition.
  void dispatch_menu_item_ios(Session* s, WidgetData& mb, uint32_t item_id);

  // Resolve the painted widget under a content-view-local point (used by the
  // content view's hover + key routing to reach CUSTOMDRAW / KNOB widgets).
  WidgetData* painted_widget_at_ios(Session* s, uint32_t frame_idx, float lx, float ly);
  // Best-effort Tab traversal: cycle first responder among the frame's
  // focusable native controls in creation order. Defined below.
  bool focus_next_native_ios(Session* s, uint32_t frame_idx, bool forward);

  // GRID (NEUI_W_GRID). The shared grid_model.h + widget_paint_grid.h +
  // scroll_kinetics.h carry the model + paint + math; these are the iOS glue
  // (mirror of the macOS native host's grid_painted_msg_macos family). The
  // painted view drives them from its touch / pan / key path.
  neui_detail::GridModel& ios_grid_ensure_model(WidgetData& wd);
  // Funnelled non-scroll input kind (touch-tap / hardware key).
  enum class IosGridMsg { Down, DblClick, Up, Key };
  void ios_grid_dispatch_msg(WidgetData& wd, IosGridMsg kind, float lx, float ly,
                             uint32_t keycode, uint32_t mods);
  void ios_grid_char(WidgetData& wd, uint32_t cp);
  // iOS-only rubber-band feel (matches platform_ios.mm: +50% overscroll range,
  // ~2.5x spring-back time). Applied per-grid on the model's ScrollKinetics.
  void ios_grid_apply_scroll_feel(WidgetData& wd);
  // The SECTION twin: same feel on both per-axis kinetics integrators.
  void section_apply_scroll_feel(WidgetData& wd);

  // TABVIEW (NEUI_W_TABVIEW) runtime helpers. The chip-strip geometry +
  // selection model are the shared widget_tabview.h / widget_tabview_host.h;
  // these are the iOS glue (mirror of the macOS native host's
  // tabview_*_macos family). The painted view's touch path drives select; the
  // paint pass + the TABS API drive collect/apply.
  void tabview_collect_pages_ios(WidgetData& tv, std::vector<uint32_t>& out);
  void tabview_select_ios(WidgetData& tv, int new_index);
  void tabview_apply_page_geometry_ios(WidgetData& tv);
}

// Height (logical px) of the hamburger band reserved below the safe area when a
// frame carries a MENUBAR child. Matches the xpl iOS platform layer.
static constexpr int kHamburgerBandH = 44;

// ---------------------------------------------------------------------------
// NEUINativeIOSPaintedView - one per IMAGE / KNOB / CUSTOMDRAW / SECTION (and phase-2
// placeholder painted types). drawRect: binds the CG backend + dispatches to
// the shared paint helpers. UIView is natively Y-down, matching the renderer
// convention - no flip needed.

API_AVAILABLE(ios(13.0))
@interface NEUINativeIOSPaintedView : UIView <UIContextMenuInteractionDelegate,
                                             UIGestureRecognizerDelegate>
{
@public
  uint32_t          widget_id;
  neui_render_ctx_t render_ctx;
@private
  // GRID touch-scroll (the platform_ios.mm pattern reproduced on the native
  // view): a UIPanGestureRecognizer feeds grid_scroll_wheel; a CADisplayLink
  // synthesizes the release-momentum glide + drives grid_scroll_bounce_step.
  UIPanGestureRecognizer* _grid_pan;
  CADisplayLink*          _grid_link;
  CGPoint                 _grid_pan_last;
  CGPoint                 _grid_scroll_vel;
  bool                    _grid_momentum;
  // DEFERRED-TAP: a tap selects, a swipe scrolls without selecting (mirror of
  // platform_ios.mm). A GRID touch defers its DOWN; the pan claims a swipe and
  // discards the pending tap, while a stationary lift synthesizes Down->Up.
  bool                    _grid_tap_pending;
  bool                    _grid_consumed_by_scroll;
  CGPoint                 _grid_tap_point;
  // TABVIEW deferred chip tap: record on touchesBegan:, commit on a stationary
  // lift in touchesEnded: (cleared by a swipe in touchesMoved:).
  bool                    _tab_tap_pending;
  CGPoint                 _tab_tap_point;
}
// KNOB-only: install a long-press context menu offering "Reset to default"
// (iOS has no right-click; this is the touch-idiomatic equivalent of the
// desktop KNOB reset popup). Idempotent.
- (void)installKnobResetMenu;
// GRID + scrolling SECTION / TABPAGE: install the pan recognizer that drives
// touch-scroll. Idempotent.
- (void)installGridScroll;
// Scrolling SECTION / TABPAGE touch-scroll (shares the pan recognizer + display
// link + momentum ivars with the GRID path; a painted view is one or the other).
- (void)handleSectionScrollPan:(UIPanGestureRecognizer*)g;
- (void)sectionFeedDX:(double)dx dy:(double)dy began:(bool)began changed:(bool)changed
                ended:(bool)ended momentum:(bool)momentum momentumEnded:(bool)momentumEnded;
- (void)sectionScrollTick;
- (bool)sectionBounceStep;
// Stop the shared momentum / spring-back display link (also called externally
// when a section flips out of a scrolling mode mid-glide).
- (void)gridLinkStop;
@end

@implementation NEUINativeIOSPaintedView

- (BOOL)isOpaque { return NO; }   // SECTION lets the frame bg show through

// Belt-and-suspenders self-invalidate on appearance flip. The content view's
// Session-tree walk is the primary fix (it also re-themes native controls),
// but a painted view nested deep in a SECTION/TABVIEW container receives its
// own -traitCollectionDidChange: too; repaint so we never show a stale-palette
// straggler even if the tree walk path changes. Palette refresh is owned by
// the content view (idempotent), so we only need setNeedsDisplay here.
- (void)traitCollectionDidChange:(UITraitCollection*)previous
{
  [super traitCollectionDidChange:previous];
  if (@available(iOS 13.0, *)) {
    if (previous && previous.userInterfaceStyle == self.traitCollection.userInterfaceStyle)
      return;
    [self setNeedsDisplay];
  }
}

- (void)drawRect:(CGRect)rect
{
  (void)rect;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !render_ctx) return;
  CGContextRef cg = UIGraphicsGetCurrentContext();
  if (!cg) return;

  CGSize sz = self.bounds.size;
  neui_cg_backend::set_current_frame(render_ctx, (void*)cg,
                                     (float)sz.width, (float)sz.height);

  auto* backend = neui_cg_backend::get_backend();
  if (!backend) return;
  ios_host::paint_widget_into_ctx(sess, *wd, backend,
                                  (float)sz.width, (float)sz.height);
}

// Touch -> synthesized mouse stream for CUSTOMDRAW + behavior runtime
// (KNOB drag etc.). Native controls don't use this path. Single-pointer.
- (bool)isGrid
{
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  return wd && wd->type && !strcmp(wd->type, NEUI_W_GRID);
}

// A scrolling SECTION / TABPAGE: drives section_scroll_wheel_kinetic from the
// shared pan recognizer (mutually exclusive with isGrid - a painted view is
// one or the other). GRID owns grid_model; a section owns section_scroll_state.
- (bool)isSectionScroll
{
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  return wd && !wd->grid_model && wd->section_scroll_state != nullptr;
}

// Resolve the TABVIEW WidgetData for this painted view, or nullptr. Used by the
// touch path to hit-test the chip strip. Mirror of the macOS native host's
// -tabviewInputWidget.
- (ios_host::WidgetData*)tabviewInputWidget
{
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !wd->enabled || !wd->type) return nullptr;
  if (strcmp(wd->type, NEUI_W_TABVIEW) != 0) return nullptr;
  return wd;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !wd->emit_events) { [super touchesBegan:touches withEvent:event]; return; }
  UITouch* t = touches.anyObject; if (!t) return;
  CGPoint p = [t locationInView:self];

  // TABVIEW: a tap on a chip selects that tab. Like GRID this uses a deferred
  // tap (record now, commit on a stationary lift) so a stray swipe across the
  // strip doesn't flip tabs; the page content lives in separate child views
  // below the strip, so a touch landing on the TABVIEW view itself is on the
  // chip strip / body gutter. Synthesize nothing else for a TABVIEW.
  if (wd->type && !strcmp(wd->type, NEUI_W_TABVIEW)) {
    _tab_tap_pending = true;
    _tab_tap_point   = p;
    return;
  }

  // GRID: DEFERRED-TAP model (a tap selects, a swipe scrolls). Record the
  // pending tap and emit NOTHING yet - the synthesized Down/Up runs on a
  // stationary lift in touchesEnded:, while the pan recognizer claims a swipe
  // and discards the pending tap (handleGridScrollPan: .changed). The pan also
  // pulls keyboard focus to the view for hardware-keyboard nav.
  if (wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
    if ([self canBecomeFirstResponder] && !self.isFirstResponder)
      [self becomeFirstResponder];
    [self gridLinkStop];   // a fresh touch interrupts any momentum glide
    _grid_consumed_by_scroll = false;
    _grid_tap_pending        = true;
    _grid_tap_point          = p;
    return;
  }

  // Scrolling SECTION / TABPAGE: the pan recognizer drives scrolling. The
  // section view itself has no tap action (chip taps land on TABVIEW, content
  // taps on child subviews), so swallow the touch - synthesize no mouse stream.
  if ([self isSectionScroll]) {
    [self gridLinkStop];   // a fresh touch interrupts any momentum glide
    _grid_consumed_by_scroll = false;
    return;
  }

  wd->pressed = true;
  neui_event_t ev = {};
  ev.type = NEUI_EVENT_MOUSE_BUTTON_DOWN;
  ev.data.mouse.widget = { wd->widget_id };
  ev.data.mouse.x = (int)p.x; ev.data.mouse.y = (int)p.y;
  ev.data.mouse.buttonmap = NEUI_MK_LBUTTON;
  sess->dispatch_event(&ev);
  [self setNeedsDisplay];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !wd->emit_events) return;
  // GRID: the pan recognizer drives scrolling; a deferred GRID touch never
  // synthesizes MOUSE_MOVE (it stays a tap or the pan claims it as a scroll).
  if (wd->type && !strcmp(wd->type, NEUI_W_GRID)) return;
  // Scrolling SECTION / TABPAGE: same - the pan drives scroll, no MOUSE_MOVE.
  if ([self isSectionScroll]) return;
  // TABVIEW: a moved finger is a swipe, not a chip tap - discard the pending
  // tap so the lift selects nothing.
  if (wd->type && !strcmp(wd->type, NEUI_W_TABVIEW)) {
    if (UITouch* mt = touches.anyObject) {
      CGPoint mp = [mt locationInView:self];
      if (std::fabs(mp.x - _tab_tap_point.x) > 10.0 ||
          std::fabs(mp.y - _tab_tap_point.y) > 10.0)
        _tab_tap_pending = false;
    }
    return;
  }
  UITouch* t = touches.anyObject; if (!t) return;
  CGPoint p = [t locationInView:self];
  neui_event_t ev = {};
  ev.type = NEUI_EVENT_MOUSE_MOVE;
  ev.data.mouse.widget = { wd->widget_id };
  ev.data.mouse.x = (int)p.x; ev.data.mouse.y = (int)p.y;
  ev.data.mouse.buttonmap = NEUI_MK_LBUTTON;
  sess->dispatch_event(&ev);
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !wd->emit_events) return;
  UITouch* t = touches.anyObject; if (!t) return;
  CGPoint p = [t locationInView:self];

  // TABVIEW: a stationary lift on a chip selects that tab (fires the shared
  // deselect/select events + swaps page visibility). Off a chip, or after a
  // swipe (pending cleared in touchesMoved:), nothing happens.
  if (auto* tv = [self tabviewInputWidget]) {
    if (_tab_tap_pending) {
      _tab_tap_pending = false;
      int hit = neui_detail::tabview_chip_hit(tv->tab_chips.data(),
                                               (int)tv->tab_chips.size(),
                                               (float)p.x, (float)p.y);
      if (hit >= 0) ios_host::tabview_select_ios(*tv, hit);
    }
    return;
  }

  // GRID: a stationary lift = a tap -> synthesize Down (+ Up) at the start
  // point so the row selects + the click ladder fires, exactly as a desktop
  // click. If the pan claimed this touch as a swipe, the pending tap was
  // already discarded - synthesize nothing (the swipe scrolled, didn't select).
  if (wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
    if (_grid_consumed_by_scroll) { _grid_consumed_by_scroll = false; _grid_tap_pending = false; return; }
    if (_grid_tap_pending) {
      _grid_tap_pending = false;
      ios_host::ios_grid_dispatch_msg(*wd, ios_host::IosGridMsg::Down,
                                      (float)_grid_tap_point.x, (float)_grid_tap_point.y, 0, 0);
      ios_host::ios_grid_dispatch_msg(*wd, ios_host::IosGridMsg::Up,
                                      (float)_grid_tap_point.x, (float)_grid_tap_point.y, 0, 0);
    }
    return;
  }

  // Scrolling SECTION / TABPAGE: the lift is handled by the pan recognizer's
  // Ended state (momentum / spring-back); nothing to synthesize here.
  if ([self isSectionScroll]) { _grid_consumed_by_scroll = false; return; }

  bool was_pressed = wd->pressed;
  wd->pressed = false;
  neui_event_t up = {};
  up.type = NEUI_EVENT_MOUSE_BUTTON_UP;
  up.data.mouse.widget = { wd->widget_id };
  up.data.mouse.x = (int)p.x; up.data.mouse.y = (int)p.y;
  sess->dispatch_event(&up);
  bool inside = (p.x >= 0 && p.y >= 0 && p.x < self.bounds.size.width &&
                 p.y < self.bounds.size.height);
  if (was_pressed && inside) {
    neui_event_t click = {};
    click.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
    click.data.mouse.widget = { wd->widget_id };
    click.data.mouse.x = (int)p.x; click.data.mouse.y = (int)p.y;
    sess->dispatch_event(&click);
  }
  [self setNeedsDisplay];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)touches; (void)event;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (wd && wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
    _grid_tap_pending        = false;
    _grid_consumed_by_scroll = false;
    return;
  }
  if (wd && wd->type && !strcmp(wd->type, NEUI_W_TABVIEW)) {
    _tab_tap_pending = false;
    return;
  }
  if (wd && !wd->grid_model && wd->section_scroll_state) {
    _grid_consumed_by_scroll = false;
    return;
  }
  if (wd) { wd->pressed = false; [self setNeedsDisplay]; }
}

// GRID becomes a first responder so a hardware keyboard's arrows / page / home
// / end / return reach the shared grid nav (pressesBegan: below). Touch-only
// devices never need this; it is a no-op there.
- (BOOL)canBecomeFirstResponder { return [self isGrid] ? YES : NO; }

// Hardware-keyboard nav for a focused GRID (P2). Mirror of the macOS native
// keyDown: GRID branch: a recognised keycode runs the shared grid Key dispatch;
// printable characters feed the in-place cell editor when active. Unhandled
// presses fall through to the responder chain.
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event
{
  if (![self isGrid]) { [super pressesBegan:presses withEvent:event]; return; }
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !wd->enabled) { [super pressesBegan:presses withEvent:event]; return; }
  if (@available(iOS 13.4, *)) {
    [self gridLinkStop];   // user input cancels any spring-back animation
    bool handled = false;
    for (UIPress* press in presses) {
      UIKey* key = press.key;
      if (!key) continue;
      uint32_t mods    = neui_detail::ios_modifiers_to_neui(key.modifierFlags);
      uint32_t keycode = neui_detail::ios_keycode_to_neui((uint16_t)key.keyCode);
      if (keycode != 0) {
        ios_host::ios_grid_dispatch_msg(*wd, ios_host::IosGridMsg::Key, 0, 0, keycode, mods);
        handled = true;
      }
      // Printable text into the live cell editor (Ctrl/Cmd held = shortcut, not
      // text). The editor must be active for ios_grid_char to do anything.
      if (!(mods & NEUI_KMOD_CTRL) && key.characters.length > 0) {
        for (NSUInteger i = 0; i < key.characters.length; ++i) {
          uint32_t cp = (uint32_t)[key.characters characterAtIndex:i];
          if (!neui_detail::ios_is_printable_codepoint(cp)) continue;
          ios_host::ios_grid_char(*wd, cp);
          handled = true;
        }
      }
    }
    if (!handled) [super pressesBegan:presses withEvent:event];
    return;
  }
  [super pressesBegan:presses withEvent:event];
}

// --- KNOB reset menu (M7b) -------------------------------------------------
// iOS has no right-click, so the desktop KNOB "Reset to default" popup
// (NEUI_EVENT_MOUSE_RBUTTON_DOWN -> popup_menu) becomes a long-press
// UIContextMenuInteraction. The reset path is the SAME the other hosts use:
// clamp NEUI_PARAM_DEFAULT, write it to NEUI_PARAM_VALUE, fire VALUE_CHANGED,
// repaint.

- (void)installKnobResetMenu
{
  if (@available(iOS 13.0, *)) {
    for (id<UIInteraction> it in self.interactions)
      if ([it isKindOfClass:[UIContextMenuInteraction class]]) return;  // already installed
    [self addInteraction:[[UIContextMenuInteraction alloc] initWithDelegate:self]];
  }
}

- (void)resetKnobToDefault
{
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !wd->enabled) return;
  if (!wd->type || strcmp(wd->type, NEUI_W_KNOB) != 0) return;
  float def = wd->attrs ? wd->attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f) : 0.0f;
  if (def < 0) def = 0; if (def > 1) def = 1;
  neui_detail::ensure_attrs(wd->attrs).set_float(NEUI_PARAM_VALUE, def);
  neui_event_t ev = {};
  ev.type = NEUI_EVENT_VALUE_CHANGED;
  ev.data.value.widget = { wd->widget_id };
  ev.data.value.value  = def;
  sess->dispatch_event(&ev);
  [self setNeedsDisplay];
}

- (UIContextMenuConfiguration*)contextMenuInteraction:(UIContextMenuInteraction*)interaction
                       configurationForMenuAtLocation:(CGPoint)location
    API_AVAILABLE(ios(13.0))
{
  (void)interaction; (void)location;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !wd->enabled || !wd->type || strcmp(wd->type, NEUI_W_KNOB) != 0)
    return nil;  // disabled knob / non-knob: no menu
  __weak NEUINativeIOSPaintedView* weakSelf = self;
  return [UIContextMenuConfiguration
            configurationWithIdentifier:nil
                        previewProvider:nil
                         actionProvider:^UIMenu*(NSArray<UIMenuElement*>* suggested) {
    (void)suggested;
    UIAction* reset = [UIAction actionWithTitle:@"Reset to default"
                                          image:nil
                                     identifier:nil
                                        handler:^(__kindof UIAction* action) {
      (void)action;
      [weakSelf resetKnobToDefault];
    }];
    return [UIMenu menuWithTitle:@"" children:@[ reset ]];
  }];
}

// --- GRID touch-scroll (reproduces hosts/crossplatform/platform_ios.mm) -----
// A UIPanGestureRecognizer feeds the SAME shared kinetics the desktop wheel
// path uses (grid_scroll_wheel, as a pixel-precise delta so rubber-band +
// momentum engage), and a CADisplayLink synthesizes the release-momentum glide
// + drives grid_scroll_bounce_step. GRID kinetics are vertical-only, so only
// the pan's dy is fed (matching the macOS grid_model_ptr branch).

// Momentum tuning (matches platform_ios.mm's kScroll* constants).
static const double kIosGridMomentumDecay  = 0.95;
static const double kIosGridMomentumCutoff = 6.0;   // points/sec

- (void)installGridScroll
{
  if (_grid_pan) return;
  _grid_pan = [[UIPanGestureRecognizer alloc] initWithTarget:self
                                                      action:@selector(handleGridScrollPan:)];
  _grid_pan.delegate = self;
  _grid_pan.maximumNumberOfTouches = 1;
  [self addGestureRecognizer:_grid_pan];
}

// Feed one vertical pixel increment + gesture-phase flags to the grid kinetics.
- (void)gridFeedDY:(double)dy began:(bool)began changed:(bool)changed
             ended:(bool)ended momentum:(bool)momentum momentumEnded:(bool)momentumEnded
{
  using namespace neui_detail;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !wd->grid_model) return;
  auto& m   = *wd->grid_model;
  auto  cfg = grid_read_config(wd->attrs.get());
  GridViewport vp = grid_compute_viewport(m, wd->width, wd->height, cfg.row_h, cfg.header_h);
  GridWheelInput in;
  in.precise        = true;   // finger == precise/pixel -> rubber-band on
  in.delta_px       = dy;
  in.phase_began    = began;
  in.phase_changed  = changed;
  in.phase_ended    = ended;
  in.momentum       = momentum;
  in.momentum_ended = momentumEnded;
  GridWheelAction act = grid_scroll_wheel(m, vp, cfg.row_h, in);
  if (act.changed)      [self setNeedsDisplay];
  if (act.start_bounce) [self gridLinkStart];
}

- (void)handleGridScrollPan:(UIPanGestureRecognizer*)g
{
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd) return;
  // The pan recognizer is shared: a painted view is either a GRID or a
  // scrolling SECTION / TABPAGE. Route the section case to its own kinetics.
  if (!wd->grid_model) {
    if (wd->section_scroll_state) [self handleSectionScrollPan:g];
    return;
  }
  switch (g.state) {
    case UIGestureRecognizerStateBegan: {
      [self gridLinkStop];
      ios_host::ios_grid_apply_scroll_feel(*wd);   // iOS rubber-band feel
      _grid_pan_last = [g translationInView:self];
      [self gridFeedDY:0.0 began:true changed:false ended:false
              momentum:false momentumEnded:false];
      break;
    }
    case UIGestureRecognizerStateChanged: {
      // First real movement: this touch is a swipe, not a tap - discard the
      // pending deferred tap so touchesEnded: selects nothing.
      if (!_grid_consumed_by_scroll) {
        _grid_consumed_by_scroll = true;
        _grid_tap_pending        = false;
      }
      CGPoint t = [g translationInView:self];
      // Natural-scroll: a finger moving DOWN (t.y increasing) reveals content
      // above = position decreases. neui's delta_px is subtracted, so pass the
      // incremental finger motion straight through.
      double dy = (double)(t.y - _grid_pan_last.y);
      _grid_pan_last = t;
      [self gridFeedDY:dy began:false changed:true ended:false
              momentum:false momentumEnded:false];
      break;
    }
    case UIGestureRecognizerStateEnded:
    case UIGestureRecognizerStateCancelled:
    case UIGestureRecognizerStateFailed: {
      [self gridFeedDY:0.0 began:false changed:false ended:true
              momentum:false momentumEnded:false];
      CGPoint v = [g velocityInView:self];
      if (std::fabs(v.y) > kIosGridMomentumCutoff) {
        _grid_scroll_vel = v;
        _grid_momentum   = true;
        [self gridLinkStart];
      } else {
        _grid_scroll_vel = CGPointZero;
        _grid_momentum   = false;
        // A slow release may still have left an overscroll to settle.
        if ([self gridBounceStep]) [self gridLinkStart];
      }
      break;
    }
    default: break;
  }
}

- (void)gridLinkStart
{
  if (_grid_link) return;
  _grid_link = [CADisplayLink displayLinkWithTarget:self selector:@selector(gridScrollTick:)];
  [_grid_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)gridLinkStop
{
  if (!_grid_link) return;
  [_grid_link invalidate];
  _grid_link = nil;
  _grid_momentum   = false;
  _grid_scroll_vel = CGPointZero;
}

// One spring-back step (grid_scroll_bounce_step). Returns true while animating.
- (bool)gridBounceStep
{
  using namespace neui_detail;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !wd->grid_model) return false;
  auto& m   = *wd->grid_model;
  auto  cfg = grid_read_config(wd->attrs.get());
  GridViewport vp = grid_compute_viewport(m, wd->width, wd->height, cfg.row_h, cfg.header_h);
  bool more = grid_scroll_bounce_step(m, vp, cfg.row_h);
  [self setNeedsDisplay];
  return more;
}

- (void)gridScrollTick:(CADisplayLink*)link
{
  (void)link;
  // The display link is shared with the section path (same _grid_link / momentum
  // ivars). Route a section view to its own per-axis momentum + spring-back.
  if ([self isSectionScroll]) { [self sectionScrollTick]; return; }
  // 1) Momentum glide: feed the decaying release velocity as a momentum delta
  //    so the kinetics carry the fling past the lift + rubber-stretch at edges.
  if (_grid_momentum) {
    const double dt = 1.0 / 60.0;
    double dy = (double)_grid_scroll_vel.y * dt;
    _grid_scroll_vel.y = (CGFloat)((double)_grid_scroll_vel.y * kIosGridMomentumDecay);
    bool spent = (std::fabs(_grid_scroll_vel.y) <= kIosGridMomentumCutoff);
    [self gridFeedDY:dy began:false changed:false ended:false
            momentum:true momentumEnded:spent];
    if (spent) _grid_momentum = false;
  }
  // 2) Rubber-band spring-back. Stop the link once both are settled.
  bool animating = [self gridBounceStep];
  if (!animating && !_grid_momentum) [self gridLinkStop];
}

// --- scrolling SECTION / TABPAGE touch-scroll ------------------------------
// The SECTION twin of the GRID pan path: feeds the pan's per-axis pixel
// increments into the shared section_scroll_wheel_kinetic (rubber-band +
// momentum), repositions the section's native child subviews + repaints the
// scrollbars on each commit, and runs the same release-momentum + spring-back
// on the shared CADisplayLink. Unlike a GRID, a section can scroll on both
// axes, so dx and dy are both fed (each gated on its enabled axis).

- (void)sectionFeedDX:(double)dx dy:(double)dy began:(bool)began changed:(bool)changed
                ended:(bool)ended momentum:(bool)momentum momentumEnded:(bool)momentumEnded
{
  using namespace neui_detail;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !wd->section_scroll_state) return;
  auto& st = *wd->section_scroll_state;
  const SectionLayout& L = wd->section_last_layout;
  bool phase = began || ended || momentum || momentumEnded;
  bool any_changed = false, any_bounce = false;
  // Vertical axis.
  if (section_axis_has_v(st.axis) && (dy != 0.0 || phase)) {
    ScrollWheelInput in;
    in.precise = true; in.delta_px = dy;
    in.phase_began = began; in.phase_changed = changed; in.phase_ended = ended;
    in.momentum = momentum; in.momentum_ended = momentumEnded;
    ScrollWheelAction a = section_scroll_wheel_kinetic(st, L, in, /*axis_h=*/false);
    any_changed |= a.changed; any_bounce |= a.start_bounce;
  }
  // Horizontal axis.
  if (section_axis_has_h(st.axis) && (dx != 0.0 || phase)) {
    ScrollWheelInput in;
    in.precise = true; in.delta_px = dx;
    in.phase_began = began; in.phase_changed = changed; in.phase_ended = ended;
    in.momentum = momentum; in.momentum_ended = momentumEnded;
    ScrollWheelAction a = section_scroll_wheel_kinetic(st, L, in, /*axis_h=*/true);
    any_changed |= a.changed; any_bounce |= a.start_bounce;
  }
  if (any_changed) {
    ios_host::section_reposition_children_ios(*wd);
    [self setNeedsDisplay];
    ios_host::section_notify_scroll_changed_ios(*wd);
  }
  if (any_bounce) [self gridLinkStart];
}

- (void)handleSectionScrollPan:(UIPanGestureRecognizer*)g
{
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !wd->section_scroll_state) return;
  switch (g.state) {
    case UIGestureRecognizerStateBegan: {
      [self gridLinkStop];
      ios_host::section_apply_scroll_feel(*wd);   // iOS rubber-band feel
      _grid_pan_last = [g translationInView:self];
      [self sectionFeedDX:0.0 dy:0.0 began:true changed:false ended:false
                 momentum:false momentumEnded:false];
      break;
    }
    case UIGestureRecognizerStateChanged: {
      _grid_consumed_by_scroll = true;   // a swipe, not a tap
      CGPoint t = [g translationInView:self];
      // Natural-scroll: incremental finger motion is subtracted by the kinetics,
      // so pass it straight through (content follows the finger).
      double dx = (double)(t.x - _grid_pan_last.x);
      double dy = (double)(t.y - _grid_pan_last.y);
      _grid_pan_last = t;
      [self sectionFeedDX:dx dy:dy began:false changed:true ended:false
                 momentum:false momentumEnded:false];
      break;
    }
    case UIGestureRecognizerStateEnded:
    case UIGestureRecognizerStateCancelled:
    case UIGestureRecognizerStateFailed: {
      [self sectionFeedDX:0.0 dy:0.0 began:false changed:false ended:true
                 momentum:false momentumEnded:false];
      // Only a real lift (Ended) flings; a system-cancelled / failed gesture
      // stops dead - an interrupted pan must not synthesize release momentum.
      CGPoint v = (g.state == UIGestureRecognizerStateEnded)
                    ? [g velocityInView:self] : CGPointZero;
      if (std::fabs(v.x) > kIosGridMomentumCutoff ||
          std::fabs(v.y) > kIosGridMomentumCutoff) {
        _grid_scroll_vel = v;
        _grid_momentum   = true;
        [self gridLinkStart];
      } else {
        _grid_scroll_vel = CGPointZero;
        _grid_momentum   = false;
        if ([self sectionBounceStep]) [self gridLinkStart];
      }
      break;
    }
    default: break;
  }
}

// One spring-back step per enabled axis. Returns true while still animating.
- (bool)sectionBounceStep
{
  using namespace neui_detail;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !wd->section_scroll_state) return false;
  auto& st = *wd->section_scroll_state;
  const SectionLayout& L = wd->section_last_layout;
  bool more = false;
  if (section_axis_has_v(st.axis)) more |= section_scroll_bounce_step(st, L, false);
  if (section_axis_has_h(st.axis)) more |= section_scroll_bounce_step(st, L, true);
  ios_host::section_reposition_children_ios(*wd);
  [self setNeedsDisplay];
  return more;
}

- (void)sectionScrollTick
{
  // Per-axis momentum glide (decaying release velocity), then spring-back. Stop
  // the shared link once both axes are settled.
  if (_grid_momentum) {
    const double dt = 1.0 / 60.0;
    double dx = (double)_grid_scroll_vel.x * dt;
    double dy = (double)_grid_scroll_vel.y * dt;
    _grid_scroll_vel.x = (CGFloat)((double)_grid_scroll_vel.x * kIosGridMomentumDecay);
    _grid_scroll_vel.y = (CGFloat)((double)_grid_scroll_vel.y * kIosGridMomentumDecay);
    bool spent = (std::fabs(_grid_scroll_vel.x) <= kIosGridMomentumCutoff &&
                  std::fabs(_grid_scroll_vel.y) <= kIosGridMomentumCutoff);
    [self sectionFeedDX:dx dy:dy began:false changed:false ended:false
               momentum:true momentumEnded:spent];
    if (spent) _grid_momentum = false;
  }
  bool animating = [self sectionBounceStep];
  if (!animating && !_grid_momentum) [self gridLinkStop];
}

// The pan coexists with the touch-* handlers (touchesBegan: latches the
// deferred tap; the pan claims a swipe on .changed).
- (BOOL)gestureRecognizer:(UIGestureRecognizer*)g
shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer*)other
{
  (void)g; (void)other;
  return YES;
}

- (void)willMoveToWindow:(UIWindow*)newWindow
{
  [super willMoveToWindow:newWindow];
  if (!newWindow) [self gridLinkStop];   // don't outlive the view
}
@end

// ---------------------------------------------------------------------------
// NEUINativeIOSContentView - the frame's root view. Paints the frame background +
// reserves the top inset; hosts the hamburger UIButton when a MENUBAR exists.
// Child widgets (native controls + painted views) are added as subviews.

@interface NEUINativeIOSContentView : UIView <UIDropInteractionDelegate,
                                              UIDragInteractionDelegate>
{
@public
  ios_host::Session* session;
  uint32_t           widget_index;
  neui_render_ctx_t  render_ctx;
@private
  UIButton*          _hamburger;
  CADisplayLink*     _toast_link;
  // iPad pointer hover (M7b). Fires only for a pointing device, never a finger,
  // so the "no hover on touch" divergence holds. nil until installEnhancedInput.
  UIHoverGestureRecognizer* _hover;
  // Painted widget currently under the iPad pointer (frame slot, 0 = none), so
  // hover transitions toggle the painted view's hovered flag + repaint it.
  uint32_t           _hover_widget;
  // Most recent UIDragInteraction drag-source widget index (for the
  // session-did-end result_attr write-back). 0 when no drag in flight.
  uint32_t           _drag_source_widget;
}
- (void)refreshHamburger;
- (void)toastStart;
- (void)toastStop;
- (void)installEnhancedInput;
// Attach the UIDrop/UIDragInteraction (drag&drop). Idempotent.
- (void)installDragDrop;
@end

@implementation NEUINativeIOSContentView

- (BOOL)isOpaque { return YES; }

- (void)drawRect:(CGRect)rect
{
  (void)rect;
  if (!session || !render_ctx) return;
  CGContextRef cg = UIGraphicsGetCurrentContext();
  if (!cg) return;
  CGSize sz = self.bounds.size;
  neui_cg_backend::set_current_frame(render_ctx, (void*)cg,
                                     (float)sz.width, (float)sz.height);
  auto* backend = neui_cg_backend::get_backend();
  if (!backend) return;
  // The frame background. Children paint themselves into their own subviews.
  uint32_t bg = color(ColorRole::frame_bg);
  if (session->_widgets.exists(widget_index) && session->_widgets[widget_index].attrs) {
    int ov = session->_widgets[widget_index].attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
    if (ov != 0) bg = (uint32_t)ov;
  }
  if (backend->begin_frame) backend->begin_frame(render_ctx, bg);
  if (backend->end_frame)   backend->end_frame(render_ctx);
}

- (void)traitCollectionDidChange:(UITraitCollection*)previous
{
  [super traitCollectionDidChange:previous];
  if (@available(iOS 13.0, *)) {
    // Dynamic Type (content-size category) change. Independent of the light/dark
    // flip below (same userInterfaceStyle), so handle it FIRST and UNCONDITIONALLY
    // (not gated on NEUI_ATTR_FOLLOW_SYSTEM_THEME - the user's text-size setting
    // applies to every frame). Recompute the painted-UI scale, then run the SAME
    // full-tree invalidate the theme path uses so painted widgets nested inside
    // SECTION / TABVIEW containers re-scale (they re-read painted_ui_scale + their
    // GridModel config from attrs at paint time, so a repaint suffices). Native
    // controls with adjustsFontForContentSizeCategory self-update.
    if (previous && session && session->_widgets.exists(widget_index) &&
        previous.preferredContentSizeCategory !=
            self.traitCollection.preferredContentSizeCategory) {
      neui_detail::recompute_painted_ui_scale_ios();
      [self setNeedsDisplay];
      session->invalidate_all_for_theme_change();
      // Notify the client so it can re-run its responsive layout against the
      // new metrics (control heights / margins / measured text widths grow with
      // the Dynamic Type scale). Same dispatch path as RESIZE.
      ios_host::dispatch_metrics_changed_ios(session, widget_index);
    }

    if (previous && previous.userInterfaceStyle == self.traitCollection.userInterfaceStyle)
      return;
    // Refresh the palette FIRST so every view that repaints below reads the
    // new colours (refresh is idempotent + no-ops if nothing actually flipped).
    neui_detail::refresh_theme_palette_ios();
    [self setNeedsDisplay];
    if (!session || !session->_widgets.exists(widget_index)) return;
    // Match the xpl host's policy: only force-repaint frames that opted into
    // following the system appearance. Frames without the attr keep whatever
    // they were showing (no auto-invalidate on a flip).
    auto& fw = session->_widgets[widget_index];
    if (!fw.attrs || fw.attrs->get_int(NEUI_ATTR_FOLLOW_SYSTEM_THEME, 0) == 0) return;
    // Invalidate the ENTIRE widget tree - the old shallow self.subviews loop
    // missed painted views nested inside SECTION body / TABVIEW page containers,
    // and never re-themed native controls. The Session walk reaches every
    // widget + re-applies palette-derived native-control colours.
    session->invalidate_all_for_theme_change();
  }
}

- (void)refreshHamburger
{
  if (!session || !session->_widgets.exists(widget_index)) return;
  // Find a MENUBAR child of the frame.
  ios_host::WidgetData* mb = nullptr;
  uint32_t c = session->_widgets.child(widget_index);
  while (c != 0) {
    if (session->_widgets.exists(c)) {
      auto& cw = session->_widgets[c];
      if (cw.type && !strcmp(cw.type, NEUI_W_MENUBAR)) { mb = &cw; break; }
    }
    c = session->_widgets.next(c);
  }
  // Hamburger-visibility rule (menu_ios_hamburger_should_hide): hide the in-frame
  // hamburger only where the OS provides a navigable system menu bar AND it's
  // reachable - iPad on iPadOS 26+ AND windowed (Stage Manager / Split View),
  // where the menubar tree is contributed via -buildMenuWithBuilder: below. Show
  // it everywhere else - full-screen iPad 26 (the menu bar exists but can't be
  // swiped down), iPad < 26, iPhone (no menu bar) - so the menu stays reachable.
  // The window drives the full-screen test; re-evaluated on resize
  // (reportResizeIfChanged). Mirrors the xpl host's refreshHamburger gate.
  bool hide = neui_detail::menu_ios_hamburger_should_hide(self.window);
  // Log the verdict so the iPad-sim version split (and full-screen vs windowed)
  // is observable (diagnostic build only - see NEUI_IOS_MENU_DIAG; the
  // full-screen query is evaluated only when the diagnostic is enabled).
  NEUI_MENU_DIAG("[neui-menu] hamburger: menubar_avail=%d fullscreen=%d shown=%d\n",
                 neui_detail::menu_ios_system_menubar_available() ? 1 : 0,
                 neui_detail::menu_ios_window_is_fullscreen(self.window) ? 1 : 0,
                 (mb && !hide) ? 1 : 0);
  if (hide) {
    if (_hamburger) { [_hamburger removeFromSuperview]; _hamburger = nil; }
    if (@available(iOS 13.0, *)) [UIMenuSystem.mainSystem setNeedsRebuild];
    return;
  }

  if (!mb) {
    if (_hamburger) { [_hamburger removeFromSuperview]; _hamburger = nil; }
    return;
  }

  if (!_hamburger) {
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    if (@available(iOS 13.0, *)) {
      UIImage* img = [UIImage systemImageNamed:@"line.3.horizontal"];
      if (img) [b setImage:img forState:UIControlStateNormal];
      else     [b setTitle:@"☰" forState:UIControlStateNormal];
    } else {
      [b setTitle:@"☰" forState:UIControlStateNormal];
    }
    if (@available(iOS 14.0, *)) b.showsMenuAsPrimaryAction = YES;
    [self addSubview:b];
    _hamburger = b;
  }

  // Build a UIMenu from the menubar tree model via the shared adapter
  // (build_menubar_adapter_ios) + the menu_ios.h builder. Hamburger path: no
  // key-command target (it's a UIButton popover, not a keyboard surface), so
  // shortcuts show as discoverabilityTitle only and picks route via `pick`.
  ios_host::Session* sess = session;
  ios_host::WidgetData* mbp = mb;
  if (@available(iOS 14.0, *)) {
    ios_host::IosMenubarAdapter adapter = ios_host::build_menubar_adapter_ios(*mbp);
    uint32_t mb_widget_id = mbp->widget_id;
    auto pick = [sess, mb_widget_id](uint32_t item_id) {
      if (!sess) return;
      uint32_t i = mb_widget_id & 0xffff;
      if (!sess->_widgets.exists(i)) return;
      auto& wd = sess->_widgets[i];
      ios_host::dispatch_menu_item_ios(sess, wd, item_id);
    };
    _hamburger.menu = neui_detail::menu_ios_build_uimenu(adapter, @"Menu", pick);
  }
  [self setNeedsLayout];
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  if (_hamburger) {
    CGFloat top = 0;
    if (@available(iOS 11.0, *)) top = self.safeAreaInsets.top;
    const CGFloat pad = 6;
    _hamburger.frame = CGRectMake(pad, top + (kHamburgerBandH - 36) * 0.5f, 44, 36);
  }
}

- (void)toastTick:(CADisplayLink*)link { (void)link; [self setNeedsDisplay]; }
- (void)toastStart
{
  if (_toast_link) return;
  _toast_link = [CADisplayLink displayLinkWithTarget:self selector:@selector(toastTick:)];
  [_toast_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}
- (void)toastStop
{
  if (!_toast_link) return;
  [_toast_link invalidate];
  _toast_link = nil;
}
- (void)willMoveToWindow:(UIWindow*)newWindow
{
  [super willMoveToWindow:newWindow];
  if (!newWindow) [self toastStop];
}

// ---------------------------------------------------------------------------
// Hardware-keyboard accelerators (M7b). The native host has no logical-focus
// Session machinery (native controls own their own keys + the system keyboard),
// so the content view only needs to publish the menubar accelerators + route
// keys to painted CUSTOMDRAW / KNOB widgets, which UIKit otherwise never feeds.
//
// DOUBLE-DISPATCH SPLIT (mirrors platform_ios.mm M6): UIKeyCommand owns the
// menubar accelerators - UIKit matches -keyCommands BEFORE delivering
// pressesBegan: for the same chord, so a matched accelerator never reaches
// pressesBegan:; presses own everything else (key input to painted widgets).

- (BOOL)canBecomeFirstResponder { return YES; }

- (NSArray<UIKeyCommand*>*)keyCommands
{
  if (!session || !session->_widgets.exists(widget_index)) return nil;
  ios_host::WidgetData* mb = ios_host::frame_menubar_ios(session, widget_index);
  if (!mb) return nil;
  // DOUBLE-FIRE SPLIT: when the iPad system menu bar is active the menubar items
  // are contributed via -buildMenuWithBuilder: as UIKeyCommand-bearing elements
  // that own + show + fire the accelerators, so -keyCommands yields them to the
  // menu bar (single dispatch point). iPhone / no menu bar -> -keyCommands stays
  // the accelerator surface (unchanged from M7b).
  if (neui_detail::menu_ios_system_menubar_available()) return nil;
  if (@available(iOS 13.0, *)) {
    NSMutableArray<UIKeyCommand*>* cmds = [NSMutableArray array];
    for (uint32_t id : mb->tree_items_ordered) {
      auto it = mb->tree_items.find(id);
      if (it == mb->tree_items.end()) continue;
      const auto& node = it->second;
      if (node.text == "-") continue;
      if (node.shortcut_key == NEUI_KEY_NONE) continue;
      NSString* input = neui_detail::menu_ios_key_to_input(node.shortcut_key);
      if (!input) continue;
      UIKeyModifierFlags flags = neui_detail::menu_ios_mods_to_uikit(node.shortcut_mods);
      UIKeyCommand* kc = [UIKeyCommand keyCommandWithInput:input
                                             modifierFlags:flags
                                                    action:@selector(handleMenuKeyCommand:)];
      NSString* title = [NSString stringWithUTF8String:node.text.c_str()];
      if (title) kc.discoverabilityTitle = title;
      [cmds addObject:kc];
    }
    return cmds.count ? cmds : nil;
  }
  return nil;
}

- (void)handleMenuKeyCommand:(UIKeyCommand*)cmd
{
  if (!session) return;
  uint32_t key  = neui_detail::ios_input_to_neui_key(cmd.input);
  uint32_t mods = neui_detail::ios_modifiers_to_neui(cmd.modifierFlags);
  if (key == NEUI_KEY_NONE) return;
  ios_host::try_menubar_accel_ios(session, widget_index, key, mods);
}

// ---------------------------------------------------------------------------
// System menu bar contribution moved to the APP level (NEUIApplication, see
// hosts/ios/application_ios.mm + the neui_ios_native_build_menubar_menus hook in
// this TU). A UIView's -buildMenuWithBuilder: is only consulted while that view
// is in the ACTIVE responder chain, which is unreliable on a freshly-launched
// app with no first responder - on a real iPad NOTHING appeared. UIApplication
// is always in the chain, so the contribution lives there now. The content view
// no longer overrides -buildMenuWithBuilder: (it would double-contribute). The
// other menu plumbing stays here: -keyCommands (iPhone / no-bar accelerators),
// -handleMenuKeyCommand: (UIKeyCommand routing), the hamburger build, and the
// setNeedsRebuild triggers (refreshHamburger / widget_show / menu mutation).

// Route hardware key presses to the painted widget under the pointer / the last
// hovered painted widget, since the native host has no focus model. Native
// UITextField / UITextView get system-keyboard text input directly from UIKit,
// so this only needs to reach CUSTOMDRAW / KNOB.
- (ios_host::WidgetData*)keyTargetPainted
{
  if (!session) return nullptr;
  if (_hover_widget != 0 && session->_widgets.exists(_hover_widget)) {
    auto& wd = session->_widgets[_hover_widget];
    if (wd.type && (!strcmp(wd.type, NEUI_W_CUSTOMDRAW) || !strcmp(wd.type, NEUI_W_KNOB)))
      return &wd;
  }
  return nullptr;
}

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event
{
  if (!session) { [super pressesBegan:presses withEvent:event]; return; }
  if (@available(iOS 13.4, *)) {
    bool handled_any = false;
    ios_host::WidgetData* target = [self keyTargetPainted];
    for (UIPress* press in presses) {
      UIKey* key = press.key;
      if (!key) continue;
      uint32_t mods    = neui_detail::ios_modifiers_to_neui(key.modifierFlags);
      uint32_t keycode = neui_detail::ios_keycode_to_neui((uint16_t)key.keyCode);
      // Menubar Command-accelerators never arrive here (UIKit matches
      // -keyCommands first), so no double-fire guard is needed.
      // Tab cycles first responder among the frame's native tab stops (best
      // effort - UIKit has no nextKeyView loop; the focus engine owns the rest).
      if (keycode == NEUI_KEY_TAB) {
        if (ios_host::focus_next_native_ios(session, widget_index, !(mods & NEUI_KMOD_SHIFT)))
          handled_any = true;
        continue;
      }
      if (!target || !target->emit_events) continue;
      if (keycode != 0) {
        neui_event_t ev = {};
        ev.type     = NEUI_EVENT_KEYDOWN;
        ev.data.key = { { target->widget_id }, keycode, mods };
        session->dispatch_event(&ev);
        handled_any = true;
      }
      if (!(mods & NEUI_KMOD_CTRL) && key.characters.length > 0) {
        for (NSUInteger i = 0; i < key.characters.length; ++i) {
          uint32_t cp = (uint32_t)[key.characters characterAtIndex:i];
          if (!neui_detail::ios_is_printable_codepoint(cp)) continue;
          neui_event_t ev = {};
          ev.type     = NEUI_EVENT_KEYCHAR;
          ev.data.key = { { target->widget_id }, cp, mods };
          session->dispatch_event(&ev);
          handled_any = true;
        }
      }
    }
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
    ios_host::WidgetData* target = [self keyTargetPainted];
    for (UIPress* press in presses) {
      UIKey* key = press.key;
      if (!key || !target || !target->emit_events) continue;
      uint32_t keycode = neui_detail::ios_keycode_to_neui((uint16_t)key.keyCode);
      if (keycode == 0) continue;
      uint32_t mods = neui_detail::ios_modifiers_to_neui(key.modifierFlags);
      neui_event_t ev = {};
      ev.type     = NEUI_EVENT_KEYUP;
      ev.data.key = { { target->widget_id }, keycode, mods };
      session->dispatch_event(&ev);
      handled_any = true;
    }
    if (!handled_any) [super pressesEnded:presses withEvent:event];
    return;
  }
  [super pressesEnded:presses withEvent:event];
}

// ---------------------------------------------------------------------------
// iPad pointer hover (M7b). A UIHoverGestureRecognizer fires only for a
// pointing device (trackpad / mouse), never for a finger, so it drives the
// hovered flag on painted CUSTOMDRAW / KNOB widgets without breaking the
// "no hover on touch" divergence. Touch stays intact (handled per-painted-view).

- (void)installEnhancedInput
{
  if (@available(iOS 13.4, *)) {
    if (!_hover) {
      _hover = [[UIHoverGestureRecognizer alloc]
                 initWithTarget:self action:@selector(handleHover:)];
      [self addGestureRecognizer:_hover];
    }
  }
}

- (void)setHoverWidget:(uint32_t)idx
{
  if (idx == _hover_widget) return;
  if (_hover_widget != 0 && session && session->_widgets.exists(_hover_widget)) {
    auto& prev = session->_widgets[_hover_widget];
    prev.hovered = false;
    ios_host::mark_widget_dirty_for_paint(prev);
  }
  _hover_widget = idx;
  if (idx != 0 && session && session->_widgets.exists(idx)) {
    auto& wd = session->_widgets[idx];
    wd.hovered = true;
    ios_host::mark_widget_dirty_for_paint(wd);
  }
}

- (void)handleHover:(UIHoverGestureRecognizer*)g API_AVAILABLE(ios(13.0))
{
  if (!session) return;
  switch (g.state) {
    case UIGestureRecognizerStateBegan:
    case UIGestureRecognizerStateChanged: {
      CGPoint p = [g locationInView:self];
      ios_host::WidgetData* hit = ios_host::painted_widget_at_ios(
          session, widget_index, (float)p.x, (float)p.y);
      [self setHoverWidget:hit ? hit->index : 0];
      break;
    }
    default:
      [self setHoverWidget:0];
      break;
  }
}

// ---------------------------------------------------------------------------
// Drag & drop. The content view is the sole UIDropInteraction (drop-target) +
// UIDragInteraction (drag-source) for the frame; the framework hit-tests the
// widget tree in software via Session::dispatch_dnd_* / dnd_resolve_drag_source
// (host.mm). Mirror of the macOS native host's NEUINativeContentView, which is
// the sole NSDraggingDestination. Content-view-local coords == frame-local
// logical px (children are positioned at wd.x/wd.y, same as the touch / hover
// hit-test paths).

- (void)installDragDrop
{
  for (id<UIInteraction> i in self.interactions) {
    if ([i isKindOfClass:[UIDropInteraction class]] ||
        [i isKindOfClass:[UIDragInteraction class]])
      return;  // idempotent
  }
  UIDropInteraction* drop = [[UIDropInteraction alloc] initWithDelegate:self];
  [self addInteraction:drop];
  UIDragInteraction* drag = [[UIDragInteraction alloc] initWithDelegate:self];
  drag.allowsSimultaneousRecognitionDuringLift = YES;
  [self addInteraction:drag];
}

// ---- UIDropInteractionDelegate (drop target) ----

- (BOOL)dropInteraction:(UIDropInteraction*)interaction
       canHandleSession:(id<UIDropSession>)dropSession
{
  (void)interaction;
  if (!session) return NO;
  NSMutableArray<NSItemProvider*>* providers = [NSMutableArray array];
  for (UIDragItem* it in dropSession.items)
    if (it.itemProvider) [providers addObject:it.itemProvider];
  return neui_detail::dnd_session_has_known_type_ios(providers) ? YES : NO;
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
  CGPoint p = [dropSession locationInView:self];
  uint32_t suggested = neui_detail::dnd_suggest_action(
      NEUI_DND_ACTION_COPY | NEUI_DND_ACTION_MOVE | NEUI_DND_ACTION_LINK,
      false, false);

  uint32_t accepted;
  if (session->_current_drop_target == UINT32_MAX) {
    accepted = session->dispatch_dnd_enter(widget_index, (int)p.x, (int)p.y,
                                           ml.ptrs.data(),
                                           (uint32_t)ml.ptrs.size(),
                                           suggested, 0);
  } else {
    accepted = session->dispatch_dnd_move(widget_index, (int)p.x, (int)p.y,
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

  neui_detail::DataItem item;
  neui_detail::dnd_read_drop_item_ios(providers, item);
  auto ml = neui_detail::dnd_collect_mimes_ios(providers);
  CGPoint p = [dropSession locationInView:self];
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

  neui_detail::DataItem item;
  uint32_t allowed = 0;
  uint32_t w = session->dnd_resolve_drag_source(widget_index,
                                                (int)p.x, (int)p.y,
                                                &item, allowed);
  if (w == 0) return @[];

  NSItemProvider* provider = neui_detail::dnd_item_provider_for_item_ios(item);
  if (!provider) return @[];

  _drag_source_widget = w;
  UIDragItem* di = [[UIDragItem alloc] initWithItemProvider:provider];
  di.localObject = @(w);
  return @[ di ];
}

- (void)dragInteraction:(UIDragInteraction*)interaction
                session:(id<UIDragSession>)dragSession
    didEndWithOperation:(UIDropOperation)operation
{
  (void)interaction;
  if (!session) return;
  uint32_t action = NEUI_DND_ACTION_NONE;
  switch (operation) {
    case UIDropOperationCopy: action = NEUI_DND_ACTION_COPY; break;
    case UIDropOperationMove: action = NEUI_DND_ACTION_MOVE; break;
    default:                  action = NEUI_DND_ACTION_NONE; break;
  }
  uint32_t w = _drag_source_widget;
  for (UIDragItem* di in dragSession.items) {
    if ([di.localObject isKindOfClass:[NSNumber class]]) {
      w = (uint32_t)[(NSNumber*)di.localObject unsignedIntValue];
      break;
    }
  }
  if (w != 0) session->dnd_report_drag_result(w, action);
  _drag_source_widget = 0;
}
@end

// ---------------------------------------------------------------------------
// NEUINativeIOSViewController - root VC. Drives RESIZE on bounds / safe-area changes.

@interface NEUINativeIOSViewController : UIViewController
{
@public
  ios_host::Session* session;
  uint32_t           widget_index;
@private
  int _last_w, _last_h, _last_inset;
}
@end

@implementation NEUINativeIOSViewController
- (void)loadView
{
  NEUINativeIOSContentView* v = [[NEUINativeIOSContentView alloc] initWithFrame:CGRectZero];
  v->session = session;
  v->widget_index = widget_index;
  v.contentScaleFactor = UIScreen.mainScreen.scale;
  [v installEnhancedInput];   // iPad pointer hover (no-op on iOS < 13.4)
  [v installDragDrop];        // drag&drop (UIDrop/UIDragInteraction)
  self.view = v;
  neui_detail::refresh_theme_palette_ios();
}
- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];
  // Become first responder so hardware-keyboard accelerators (-keyCommands) +
  // painted-widget key presses (pressesBegan:) reach the content view. A native
  // text control taking the keyboard transparently steals first-responder for
  // its own editing while it is focused, and reinstates this on resign.
  [self.view becomeFirstResponder];
  // This frame's view entered the responder chain; on iPad request a menu-bar
  // rebuild so its -buildMenuWithBuilder: contribution shows immediately. No-op
  // on iPhone (no system menu bar).
  if (neui_detail::menu_ios_system_menubar_available()) {
    if (@available(iOS 13.0, *)) [UIMenuSystem.mainSystem setNeedsRebuild];
  }
}
- (NEUINativeIOSContentView*)contentView
{
  return [self.view isKindOfClass:[NEUINativeIOSContentView class]] ? (NEUINativeIOSContentView*)self.view : nil;
}
- (void)reportResizeIfChanged
{
  if (!session || !session->_widgets.exists(widget_index)) return;
  CGSize sz = self.view.bounds.size;
  int w = (int)sz.width, h = (int)sz.height;
  if (w <= 0 || h <= 0) return;
  int inset = ios_host::frame_top_inset_ios(session, widget_index);
  if (w == _last_w && h == _last_h && inset == _last_inset) return;
  _last_w = w; _last_h = h; _last_inset = inset;
  auto& wd = session->_widgets[widget_index];
  wd.width = w; wd.height = h;
  auto* backend = neui_cg_backend::get_backend();
  if (backend && backend->resize && wd.render_ctx)
    backend->resize(wd.render_ctx, (uint32_t)w, (uint32_t)h);
  neui_event_t ev = {};
  ev.type = NEUI_EVENT_RESIZE;
  ev.data.resize.widget = { wd.widget_id };
  ev.data.resize.width = w; ev.data.resize.height = h;
  session->dispatch_event(&ev);
  // The safe-area insets (and thus safe_area_insets / get_client_rect) change on
  // rotation + when the notch/status-bar inset first resolves, so notify the
  // client that the metrics changed too (alongside RESIZE).
  ios_host::dispatch_metrics_changed_ios(session, widget_index);
  // A bounds change can be a full-screen <-> windowed transition (entering/
  // leaving Stage Manager / Split View), which flips the hamburger-visibility
  // rule on iPad 26+. Re-evaluate so the hamburger appears when going full-screen
  // and disappears when windowing. refreshHamburger is cheap when nothing changed
  // (re-uses the existing button) and runs outside the layout pass, so no loop.
  // No-op on iPhone / iPad < 26 (rule is version-gated).
  [[self contentView] refreshHamburger];
  [self.view setNeedsDisplay];
}
- (void)viewDidLayoutSubviews { [super viewDidLayoutSubviews]; [self reportResizeIfChanged]; }
- (void)viewSafeAreaInsetsDidChange
{
  [super viewSafeAreaInsetsDidChange];
  [[self contentView] refreshHamburger];
  [self reportResizeIfChanged];
}
- (void)viewWillTransitionToSize:(CGSize)size
       withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
  [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
  [coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext> ctx) {
    (void)ctx; [self.view setNeedsDisplay];
  } completion:nil];
}
@end

// ---------------------------------------------------------------------------
// First-responder capture. UIWindow exposes no public firstResponder accessor;
// the documented idiom is to send a no-op action with a nil target so UIKit
// resolves it against the current first responder, which records itself. Used
// by invoke_focused_command_ios to reach the focused field editor's
// NSUndoManager for UNDO / REDO.

@interface NEUIFirstResponderProbe : NSObject
@property (nonatomic, weak) UIResponder* found;
- (void)findFirstResponder:(id)sender;
@end
@implementation NEUIFirstResponderProbe
- (void)findFirstResponder:(id)sender
{
  self.found = [sender isKindOfClass:[UIResponder class]] ? (UIResponder*)sender : nil;
}
@end

static UIResponder* neui_capture_first_responder_ios()
{
  NEUIFirstResponderProbe* probe = [[NEUIFirstResponderProbe alloc] init];
  [UIApplication.sharedApplication sendAction:@selector(findFirstResponder:)
                                          to:probe from:nil forEvent:nil];
  return probe.found;
}

// ---------------------------------------------------------------------------

namespace ios_host
{
  // Resolve a widget_id (session<<16 | slot) to its WidgetData* + session.
  static WidgetData* widget_for_id(uint32_t widget_id, Session** out_sess)
  {
    if (out_sess) *out_sess = nullptr;
    uint32_t sess_id = (widget_id >> 16) & 0xffff;
    uint32_t idx     = widget_id & 0xffff;
    if (sess_id == 0 || sess_id > sessions.size()) return nullptr;
    Session* s = sessions[sess_id - 1].get();
    if (!s || !s->_widgets.exists(idx)) return nullptr;
    if (out_sess) *out_sess = s;
    return &s->_widgets[idx];
  }

  API_AVAILABLE(ios(13.0))
  static UIWindowScene* active_window_scene()
  {
    UIWindowScene* fallback = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:[UIWindowScene class]]) continue;
      UIWindowScene* ws = (UIWindowScene*)scene;
      if (!fallback) fallback = ws;
      if (scene.activationState == UISceneActivationStateForegroundActive) return ws;
    }
    return fallback;
  }

  static NEUINativeIOSContentView* content_view_for_frame(WidgetData& frame)
  {
    if (!frame.native_window) return nil;
    UIWindow* w = (__bridge UIWindow*)frame.native_window;
    UIViewController* vc = w.rootViewController;
    if ([vc isKindOfClass:[NEUINativeIOSViewController class]])
      return [(NEUINativeIOSViewController*)vc contentView];
    return nil;
  }

  // Walk parents to the owning frame's WidgetData.
  static WidgetData* owning_frame(Session* s, uint32_t idx)
  {
    uint32_t cur = idx;
    while (cur != 0 && s->_widgets.exists(cur)) {
      auto& wd = s->_widgets[cur];
      if (wd.isroot) return &wd;
      cur = s->_widgets.get_parent(cur);
      if (cur == neui_detail::knone.id) break;
    }
    return nullptr;
  }

  // ---------------------------------------------------------------------------
  // NEUI_API_METRICS iOS-real seams (installed into the shared vtable at
  // register_host()). measure_text resolves a UIFont from family/size/weight
  // (same mapping as the cg backend + ios_dynamic_control_font) and measures
  // the string; safe_area_insets reads the frame view's safeAreaInsets, folding
  // the hamburger menu band into `top` so it stays consistent with
  // get_client_rect / frame_top_inset.

  // Resolve a UIFont for measurement. Empty family => system font; weight 0 =>
  // Regular, 700+ => Bold (matches the cg backend's CSS-weight mapping). size
  // <= 0 => the painted body default scaled by the Dynamic-Type scale (the same
  // value NEUI_METRIC_BODY_FONT_SIZE returns), so an unspecified size measures
  // at the current accessibility text size.
  static UIFont* metrics_font_ios(const char* family, float size_px, int weight)
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

  static int metrics_measure_text_ios(neui_session_t /*session*/, const char* text,
                                      const char* family, float size_px, int weight)
  {
    if (!text || !*text) return 0;
    UIFont* font = metrics_font_ios(family, size_px, weight);
    if (!font) return 0;
    NSString* str = [NSString stringWithUTF8String:text];
    if (!str) return 0;
    CGSize sz = [str sizeWithAttributes:@{ NSFontAttributeName : font }];
    return (int)(sz.width + 0.5f);
  }

  static void metrics_safe_area_ios(neui_session_t /*session*/, neui_widget_t frame,
                                    int* left, int* top, int* right, int* bottom)
  {
    if (left)   *left   = 0;
    if (top)    *top    = 0;
    if (right)  *right  = 0;
    if (bottom) *bottom = 0;
    Session* s = nullptr;
    WidgetData* fw = widget_for_id(frame.id, &s);
    if (!fw || !s || !fw->isroot) return;
    NEUINativeIOSContentView* cv = content_view_for_frame(*fw);
    if (!cv) return;
    if (@available(iOS 11.0, *)) {
      UIEdgeInsets ins = cv.safeAreaInsets;
      if (left)   *left   = (int)(ins.left + 0.5);
      if (right)  *right  = (int)(ins.right + 0.5);
      if (bottom) *bottom = (int)(ins.bottom + 0.5);
      // Fold the hamburger menu band into `top` so this matches the content
      // rect get_client_rect reports (frame_top_inset = safe-area top + band).
      if (top) {
        uint32_t fidx = fw->widget_id & 0xffff;
        *top = frame_top_inset_ios(s, fidx);
      }
    }
  }

  // Install the iOS-real measure_text + safe_area seams. Called once from each
  // iOS host's register_host(); idempotent (overwrites with the same pointers).
  void install_metrics_seams_ios()
  {
    neui_detail::metrics_measure_seam()   = &metrics_measure_text_ios;
    neui_detail::metrics_safe_area_seam() = &metrics_safe_area_ios;
  }

  // Dispatch NEUI_EVENT_METRICS_CHANGED to a frame's client (same path as
  // RESIZE). Fired when the Dynamic Type scale recomputes or the safe area /
  // rotation changes, so clients can re-run their responsive layout.
  void dispatch_metrics_changed_ios(Session* s, uint32_t frame_idx)
  {
    if (!s || !s->_widgets.exists(frame_idx)) return;
    auto& fw = s->_widgets[frame_idx];
    neui_event_t ev = {};
    ev.type                = NEUI_EVENT_METRICS_CHANGED;
    ev.data.metrics.widget = { fw.widget_id };
    ev.data.metrics.ui_scale = neui_detail::painted_ui_scale();
    s->dispatch_event(&ev);
  }

  // Find the MENUBAR child of a frame (model-only widget driving the menu).
  WidgetData* frame_menubar_ios(Session* s, uint32_t frame_idx)
  {
    if (!s->_widgets.exists(frame_idx)) return nullptr;
    uint32_t c = s->_widgets.child(frame_idx);
    while (c != 0) {
      if (s->_widgets.exists(c)) {
        auto& cw = s->_widgets[c];
        if (cw.type && !strcmp(cw.type, NEUI_W_MENUBAR)) return &cw;
      }
      c = s->_widgets.next(c);
    }
    return nullptr;
  }

  // See the forward declaration: walk every live session for the frontmost
  // realized frame carrying a MENUBAR child, preferring the key window. The
  // app-level menu builder needs this because UIApplication is not bound to any
  // one frame's content view - it must locate "the frame whose menus the bar
  // should currently show" itself.
  WidgetData* frontmost_menubar_ios(Session** out_sess, WidgetData** out_frame)
  {
    if (out_sess)  *out_sess  = nullptr;
    if (out_frame) *out_frame = nullptr;
    WidgetData* best_mb    = nullptr;
    WidgetData* best_frame = nullptr;
    Session*    best_sess  = nullptr;
    bool        best_is_key = false;

    for (auto& sp : sessions) {
      Session* s = sp.get();
      if (!s) continue;
      // Scan top-level (root) widgets - the frames - in this session.
      uint32_t idx = s->_widgets.child(0);
      while (idx != 0) {
        if (s->_widgets.exists(idx)) {
          WidgetData& fw = s->_widgets[idx];
          // A realized, visible frame (has a UIWindow). DIALOG counts too: when
          // a modal dialog with its own menubar is up it is the frontmost frame.
          if (fw.isroot && fw.visible && fw.native_window) {
            WidgetData* mb = frame_menubar_ios(s, idx);
            if (mb) {
              bool is_key = false;
              if (@available(iOS 13.0, *)) {
                UIWindow* w = (__bridge UIWindow*)fw.native_window;
                is_key = w.isKeyWindow;
              }
              // Prefer the key window's frame; otherwise take the first eligible
              // frame found (and let a later key window override it).
              if (!best_mb || (is_key && !best_is_key)) {
                best_mb     = mb;
                best_frame  = &fw;
                best_sess   = s;
                best_is_key = is_key;
              }
            }
          }
        }
        idx = s->_widgets.next(idx);
      }
    }

    if (out_sess)  *out_sess  = best_sess;
    if (out_frame) *out_frame = best_frame;
    return best_mb;
  }

  // Flatten the native MENUBAR's TreeNode model into the menu_ios.h builder
  // shape (see IosMenubarAdapter). cmd_id == tree id so a UIAction pick routes
  // straight back through dispatch_menu_item_ios. Shared by the hamburger build
  // and the iPad system-menu-bar contribution so both surfaces stay in sync.
  IosMenubarAdapter build_menubar_adapter_ios(WidgetData& mb)
  {
    IosMenubarAdapter adapter;
    for (uint32_t id : mb.tree_items_ordered) {
      auto it = mb.tree_items.find(id);
      if (it == mb.tree_items.end()) continue;
      IosMenuItemAdapter mi;
      mi.parent_item_id = it->second.parent_id;
      mi.text           = it->second.text;
      mi.is_separator   = (it->second.text == "-");
      mi.cmd_id         = id;     // route by tree id (dispatch maps it)
      mi.enabled        = it->second.enabled;
      mi.shortcut_mods  = it->second.shortcut_mods;
      mi.shortcut_key   = it->second.shortcut_key;
      // shortcut text left empty: on iPad the UIKeyCommand element shows the
      // trailing equivalent itself; the hamburger (iPhone) never displayed one.
      adapter.menu_items[id] = std::move(mi);
      adapter.menu_item_ids_ordered.push_back(id);
    }
    return adapter;
  }

  // Dispatch a single menubar item (built-in command first, then client
  // TREE_ITEM_ACTIVATED) - shared by the accelerator path + the hamburger pick.
  void dispatch_menu_item_ios(Session* s, WidgetData& mb, uint32_t item_id)
  {
    auto it = mb.tree_items.find(item_id);
    if (it == mb.tree_items.end()) return;
    if (it->second.menu_cmd != 0 && it->second.menu_cmd < NEUI_CMD_USER_BASE) {
      if (invoke_focused_command_ios(it->second.menu_cmd)) return;
    }
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_TREE_ITEM_ACTIVATED;
    ev.data.tree.widget = { mb.widget_id };
    ev.data.tree.item   = { item_id };
    s->dispatch_event(&ev);
  }

  bool try_menubar_accel_ios(Session* s, uint32_t frame_idx, uint32_t key, uint32_t mods)
  {
    if (!s || key == NEUI_KEY_NONE) return false;
    WidgetData* mb = frame_menubar_ios(s, frame_idx);
    if (!mb) return false;
    for (uint32_t id : mb->tree_items_ordered) {
      auto it = mb->tree_items.find(id);
      if (it == mb->tree_items.end()) continue;
      const auto& node = it->second;
      if (node.shortcut_key == key && node.shortcut_mods == mods && node.enabled) {
        dispatch_menu_item_ios(s, *mb, id);
        return true;
      }
    }
    return false;
  }

  WidgetData* painted_widget_at_ios(Session* s, uint32_t frame_idx, float lx, float ly)
  {
    // Deepest CUSTOMDRAW / KNOB descendant containing (lx, ly) in frame-local
    // coords. Simple parent-relative accumulation matching the native DnD
    // walker; v1 only needs the painted-input widgets (native controls handle
    // their own keys / hover via UIKit).
    WidgetData* best = nullptr;
    int best_depth = -1;
    std::function<void(uint32_t, int, int, int)> walk =
      [&](uint32_t idx, int ox, int oy, int depth) {
        uint32_t c = s->_widgets.child(idx);
        while (c != 0) {
          if (s->_widgets.exists(c)) {
            auto& cw = s->_widgets[c];
            if (cw.visible && cw.enabled) {
              int ax = ox + cw.x, ay = oy + cw.y;
              bool inside = lx >= ax && ly >= ay &&
                            lx < ax + cw.width && ly < ay + cw.height;
              bool painted_input = cw.type &&
                  (!strcmp(cw.type, NEUI_W_CUSTOMDRAW) || !strcmp(cw.type, NEUI_W_KNOB));
              if (inside && painted_input && depth > best_depth) {
                best = &cw; best_depth = depth;
              }
              if (inside) walk(c, ax, ay, depth + 1);
            }
          }
          c = s->_widgets.next(c);
        }
      };
    walk(frame_idx, 0, 0, 0);
    return best;
  }

  // Collect the frame's tab-stop native controls in creation (pre-order) order.
  // Tab stops: BUTTON / INPUTBOX / MULTILINE / CHECKBOX[3] / SLIDER (the native
  // leaves that can_become_first_responder), honouring NEUI_ATTR_TAB_STOP=0.
  static void collect_tab_stops_ios(Session* s, uint32_t idx,
                                    std::vector<UIView*>& out)
  {
    uint32_t c = s->_widgets.child(idx);
    while (c != 0) {
      if (s->_widgets.exists(c)) {
        auto& cw = s->_widgets[c];
        bool tab_ok = !cw.attrs || cw.attrs->get_int(NEUI_ATTR_TAB_STOP, 1) != 0;
        const char* t = cw.type;
        bool focusable = t && (!strcmp(t, NEUI_W_BUTTON) || !strcmp(t, NEUI_W_INPUTBOX) ||
                               !strcmp(t, NEUI_W_MULTILINE) || !strcmp(t, NEUI_W_CHECKBOX) ||
                               !strcmp(t, NEUI_W_CHECKBOX3) || !strcmp(t, NEUI_W_SLIDER));
        if (cw.native_control && cw.visible && cw.enabled && tab_ok && focusable) {
          UIView* v = (__bridge UIView*)cw.native_control;
          if ([v canBecomeFirstResponder]) out.push_back(v);
        }
        if (cw.native_control || (t && !strcmp(t, NEUI_W_SECTION)))
          collect_tab_stops_ios(s, c, out);
      }
      c = s->_widgets.next(c);
    }
  }

  bool focus_next_native_ios(Session* s, uint32_t frame_idx, bool forward)
  {
    if (!s || !s->_widgets.exists(frame_idx)) return false;
    std::vector<UIView*> stops;
    collect_tab_stops_ios(s, frame_idx, stops);
    if (stops.empty()) return false;
    // Find the current first responder in the list.
    NSInteger cur = -1;
    for (size_t i = 0; i < stops.size(); ++i)
      if (stops[i].isFirstResponder) { cur = (NSInteger)i; break; }
    NSInteger n = (NSInteger)stops.size();
    NSInteger next = (cur < 0) ? (forward ? 0 : n - 1)
                               : ((cur + (forward ? 1 : -1)) % n + n) % n;
    [stops[(size_t)next] becomeFirstResponder];
    return true;
  }

  static int frame_top_inset_ios(Session* s, uint32_t frame_idx)
  {
    if (!s->_widgets.exists(frame_idx)) return 0;
    auto& frame = s->_widgets[frame_idx];
    int top = 0;
    NEUINativeIOSContentView* cv = content_view_for_frame(frame);
    if (@available(iOS 11.0, *)) {
      if (cv) top = (int)(cv.safeAreaInsets.top + 0.5);
    }
    // Hamburger band if a MENUBAR child exists.
    bool has_menubar = false;
    uint32_t c = s->_widgets.child(frame_idx);
    while (c != 0) {
      if (s->_widgets.exists(c)) {
        auto& cw = s->_widgets[c];
        if (cw.type && !strcmp(cw.type, NEUI_W_MENUBAR)) { has_menubar = true; break; }
      }
      c = s->_widgets.next(c);
    }
    return top + (has_menubar ? kHamburgerBandH : 0);
  }

  void widget_client_rect_ios(Session* s, uint32_t widget_idx,
                              int* x, int* y, int* w, int* h)
  {
    if (!s->_widgets.exists(widget_idx)) return;
    auto& wd = s->_widgets[widget_idx];
    if (wd.isroot) {
      int inset = frame_top_inset_ios(s, widget_idx);
      int ch = wd.height - inset;
      if (ch < 0) ch = 0;   // early layout: inset can exceed a not-yet-sized frame
      if (x) *x = 0;
      if (y) *y = inset;
      if (w) *w = wd.width;
      if (h) *h = ch;
    } else {
      if (x) *x = 0;
      if (y) *y = 0;
      if (w) *w = wd.width;
      if (h) *h = wd.height;
    }
  }

  // -------------------------------------------------------------------------
  // SECTION helpers (mirror of the macOS section_* family; UIView clipping
  // replaces the NSView body container).

  // A TABPAGE is a chip-less SECTION: its `text` is the tab label (drawn by the
  // parent TABVIEW's chip strip, not a section header chip), and it carries no
  // band. Return "" / "none" so the section paint path draws no chip + the body
  // fills the whole page rect (band_h == 0). Mirror of the macOS native host's
  // section_effective_text / section_effective_align.
  static const char* section_effective_text_ios(WidgetData& wd)
  {
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE)) return "";
    return wd.text.c_str();
  }
  static const char* section_effective_align_ios(WidgetData& wd)
  {
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE)) return "none";
    const char* a = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
    return a ? a : "left";
  }
  static uint32_t section_resolve_bg_ios(WidgetData& wd)
  {
    int ov = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_BACKGROUND, 0) : 0;
    if (ov != 0) return (uint32_t)ov;
    return neui_detail::shade(color(ColorRole::frame_bg), neui_detail::SECTION_BG_LIFT);
  }

  void section_refresh_scroll_state_ios(WidgetData& wd)
  {
    const char* mode = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_SCROLL_MODE) : nullptr;
    auto axis = neui_detail::parse_section_scroll_mode(mode);
    bool want = (axis != neui_detail::SectionScrollAxis::None);
    if (want && !wd.section_scroll_state)
      wd.section_scroll_state = std::make_unique<neui_detail::SectionScrollState>();
    else if (!want && wd.section_scroll_state)
      wd.section_scroll_state.reset();
    wd.emit_events = want;  // scrolling sections take touches
    // Install the touch-scroll pan once the section is scrollable. Idempotent;
    // a later flip back to "none" leaves the recognizer installed but inert
    // (the pan handler no-ops when section_scroll_state is gone). The painted
    // view exists by the time this runs (realize sets native_control first; a
    // live scroll_mode change runs after show).
    if (wd.native_control) {
      id v = (__bridge id)wd.native_control;
      if ([v isKindOfClass:[NEUINativeIOSPaintedView class]]) {
        if (want) [(NEUINativeIOSPaintedView*)v installGridScroll];
        else      [(NEUINativeIOSPaintedView*)v gridLinkStop];  // cancel any glide
      }
    }
  }

  static UIView* section_child_container_ios(WidgetData& sec)
  {
    if (sec.section_body_view) return (__bridge UIView*)sec.section_body_view;
    if (sec.native_control)    return (__bridge UIView*)sec.native_control;
    return nil;
  }

  void section_apply_layout_changes_ios(WidgetData& sec)
  {
    if (!sec.native_control) return;
    const char* align = section_effective_align_ios(sec);
    const char* text  = section_effective_text_ios(sec);
    bool scrolling = (sec.section_scroll_state != nullptr);
    int band_h = scrolling ? 0
                           : neui_detail::section_band_h_for(text, sec.height, align);
    auto axis = scrolling
        ? neui_detail::parse_section_scroll_mode(
              sec.attrs ? sec.attrs->get_string(NEUI_ATTR_SCROLL_MODE) : nullptr)
        : neui_detail::SectionScrollAxis::None;

    int content_w = sec.width, content_h = sec.height;
    if (scrolling && sec.session) {
      int auto_w = 0, auto_h = 0;
      neui_detail::section_compute_auto_extent(sec.session->_widgets, sec.index,
                                               auto_w, auto_h);
      int ov_w = sec.attrs ? sec.attrs->get_int(NEUI_ATTR_CONTENT_WIDTH, 0) : 0;
      int ov_h = sec.attrs ? sec.attrs->get_int(NEUI_ATTR_CONTENT_HEIGHT, 0) : 0;
      content_w = ov_w > 0 ? ov_w : auto_w;
      content_h = ov_h > 0 ? ov_h : auto_h;
      sec.section_scroll_state->content_w = content_w;
      sec.section_scroll_state->content_h = content_h;
      // The kinetics (section_scroll_wheel_kinetic / _bounce_step) gate every
      // axis on st.axis; leaving it at the default None would make them
      // early-return and the section would never scroll. Keep it in sync with
      // the live scroll mode (re-parsed above each paint).
      sec.section_scroll_state->axis = axis;
    }
    sec.section_last_layout = neui_detail::compute_section_layout(
        sec.width, sec.height, band_h, content_w, content_h, axis);

    // Position the body container at the body rect.
    if (sec.section_body_view) {
      UIView* body = (__bridge UIView*)sec.section_body_view;
      const auto& L = sec.section_last_layout;
      body.frame = CGRectMake(L.body_x, L.body_y, L.body_w > 0 ? L.body_w : 1,
                              L.body_h > 0 ? L.body_h : 1);
    }
    section_reposition_children_ios(sec);
    mark_widget_dirty_for_paint(sec);
  }

  void section_ensure_body_view_ios(WidgetData& sec)
  {
    if (!sec.native_control || sec.section_body_view) return;
    bool scrolling = (sec.section_scroll_state != nullptr);
    int band_h = scrolling ? 0
                           : neui_detail::section_band_h_for(section_effective_text_ios(sec),
                                                             sec.height,
                                                             section_effective_align_ios(sec));
    if (!scrolling && band_h <= 0) return;  // chip-less, non-scrolling: none needed

    UIView* host = (__bridge UIView*)sec.native_control;
    UIView* body = [[UIView alloc] initWithFrame:CGRectZero];
    body.backgroundColor = UIColor.clearColor;
    body.clipsToBounds = YES;
    [host addSubview:body];
    sec.section_body_view = (__bridge_retained void*)body;

    // Re-parent existing children to the body container.
    if (sec.session) {
      uint32_t c = sec.session->_widgets.child(sec.index);
      while (c != 0) {
        if (sec.session->_widgets.exists(c)) {
          auto& cw = sec.session->_widgets[c];
          if (cw.native_control) {
            UIView* v = (__bridge UIView*)cw.native_control;
            [v removeFromSuperview];
            [body addSubview:v];
          }
        }
        c = sec.session->_widgets.next(c);
      }
    }
  }

  void section_reposition_children_ios(WidgetData& sec)
  {
    if (!sec.session || !sec.native_control) return;
    int sx = sec.section_scroll_state ? sec.section_scroll_state->scroll_x : 0;
    int sy = sec.section_scroll_state ? sec.section_scroll_state->scroll_y : 0;
    uint32_t c = sec.session->_widgets.child(sec.index);
    while (c != 0) {
      if (sec.session->_widgets.exists(c)) {
        auto& cw = sec.session->_widgets[c];
        if (cw.native_control) {
          UIView* v = (__bridge UIView*)cw.native_control;
          v.frame = CGRectMake(cw.x - sx, cw.y - sy, cw.width, cw.height);
        }
      }
      c = sec.session->_widgets.next(c);
    }
  }

  void section_notify_scroll_changed_ios(WidgetData& wd)
  {
    if (!wd.section_scroll_state || !wd.session) return;
    auto& st = *wd.section_scroll_state;
    if (st.scroll_x == st.last_notified_x && st.scroll_y == st.last_notified_y) return;
    st.last_notified_x = st.scroll_x; st.last_notified_y = st.scroll_y;
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_SCROLL_CHANGED;
    ev.data.scroll.widget = { wd.widget_id };
    ev.data.scroll.scroll_x = st.scroll_x;
    ev.data.scroll.scroll_y = st.scroll_y;
    wd.session->dispatch_event(&ev);
  }

  // -------------------------------------------------------------------------
  // TABVIEW runtime helpers. The geometry math + selection model live in the
  // shared widget_tabview.h / widget_tabview_host.h; here the iOS host
  // enumerates the TABPAGE children, sizes the selected page's UIView to the
  // content body rect, toggles page visibility, and fires the deselect/select
  // events. Mirror of the macOS native host's tabview_*_macos family.

  // Collect the TABVIEW's NEUI_W_TABPAGE child indices in creation (tab) order.
  void tabview_collect_pages_ios(WidgetData& tv, std::vector<uint32_t>& out)
  {
    neui_detail::tabview_collect_pages(tv.session, tv.index, out);
  }

  // Size the active page to the tabview's content body rect (from the most
  // recent NEUI_ATTR_TAB_POSITION / _STRIP_SIZE) + show it; hide the rest. A
  // page fills the body and its own children are body-relative (chip-less
  // section, body_y == 0). Reads NEUI_ATTR_TAB_POSITION fresh so a geometry
  // re-apply between paints (e.g. a page added post-show) stays correct.
  void tabview_apply_page_geometry_ios(WidgetData& tv)
  {
    if (!tv.session) return;
    std::vector<uint32_t> pages;
    tabview_collect_pages_ios(tv, pages);
    int count = (int)pages.size();
    if (count == 0) return;
    if (tv.tab_selected < 0)      tv.tab_selected = 0;
    if (tv.tab_selected >= count) tv.tab_selected = count - 1;

    // Use the content rect cached by the last paint (it accounts for the auto
    // vertical strip width, which needs label measurement). Before the first
    // paint it is zero - fall back to a no-strip layout so the page is sized
    // sensibly until the first paint corrects it.
    neui_detail::SectionLayout L = tv.section_last_layout;
    neui_detail::TabEdge edge_used = tv.tab_edge;
    if (L.body_w <= 0 && L.body_h <= 0) {
      const char* pos = tv.attrs ? tv.attrs->get_string(NEUI_ATTR_TAB_POSITION) : nullptr;
      auto tp = neui_detail::parse_tab_position(pos);
      edge_used = tp.edge;
      float strip = tv.attrs ? (float)tv.attrs->get_int(NEUI_ATTR_TAB_STRIP_SIZE, 0) : 0.0f;
      neui_detail::TabViewLayout tl =
        neui_detail::compute_tabview_layout((float)tv.width, (float)tv.height, tp.edge, strip);
      L.body_x = (int)tl.body_x; L.body_y = (int)tl.body_y;
      L.body_w = (int)tl.body_w; L.body_h = (int)tl.body_h;
    }

    // Carve the baseline / content-border insets out of the body so the page
    // does not paint over the strip painter's lines - identical math to the
    // win32 / macOS / crossplatform hosts (shared tabview_page_insets), so the
    // page's usable client area is the same on every platform.
    bool has_border = tv.attrs && tv.attrs->has(NEUI_ATTR_TAB_BORDER_COLOR) &&
                      tv.attrs->get_int(NEUI_ATTR_TAB_BORDER_COLOR, 0) != 0;
    float bw = tv.attrs ? (float)tv.attrs->get_int(NEUI_ATTR_TAB_BORDER_WIDTH, 0) : 0.0f;
    int it = 0, il = 0, ib = 0, ir = 0;
    neui_detail::tabview_page_insets(edge_used, has_border, bw, it, il, ib, ir);
    int body_pw = (int)L.body_w - il - ir; if (body_pw < 0) body_pw = 0;
    int body_ph = (int)L.body_h - it - ib; if (body_ph < 0) body_ph = 0;

    for (int i = 0; i < count; ++i) {
      auto& pw = tv.session->_widgets[pages[i]];
      bool active = (i == tv.tab_selected);
      pw.x = (int)L.body_x + il;
      pw.y = (int)L.body_y + it;
      pw.width  = body_pw;
      pw.height = body_ph;
      pw.visible = active;
      if (pw.native_control) {
        UIView* v = (__bridge UIView*)pw.native_control;
        v.frame = CGRectMake(pw.x, pw.y, pw.width, pw.height);
        v.hidden = !active;
      }
      // The page's own body view + children re-flow to the new size.
      section_apply_layout_changes_ios(pw);
    }
  }

  // Switch the active tab. If the selection actually changes, fire
  // NEUI_EVENT_TAB_DESELECTED (old) then _SELECTED (new) BEFORE swapping page
  // visibility + repainting (so a client handler can update the incoming page's
  // widgets first); only then swap page geometry + repaint. Mirror of the macOS
  // native host's tabview_select_macos.
  void tabview_select_ios(WidgetData& tv, int ni)
  {
    if (neui_detail::tabview_commit_selection(tv.session, tv.widget_id, tv.index,
                                              tv.tab_selected, ni)) {
      tabview_apply_page_geometry_ios(tv);
      mark_widget_dirty_for_paint(tv);
    }
  }

  // -------------------------------------------------------------------------
  // Paint dispatch for a painted widget. Mirror of the macOS painted view
  // drawRect: per-type branch, reusing the shared helpers verbatim.

  static void paint_widget_into_ctx(Session* s, WidgetData& wd,
                                    neui_render_backend_t* backend, float w, float h)
  {
    const char* type = wd.type;
    // A TABPAGE is a chip-less SECTION: it paints through the section path
    // (transparent clear, body fill, body-relative children). A TABVIEW clears
    // transparent so the shared paint_tabview helper draws the strip + body.
    bool is_section = type && (!strcmp(type, NEUI_W_SECTION) ||
                               !strcmp(type, NEUI_W_TABPAGE));
    bool is_tabview = type && !strcmp(type, NEUI_W_TABVIEW);

    uint32_t clear = (is_section || is_tabview) ? 0x00000000u
                                                : color(ColorRole::panel_bg);
    if (!is_section && !is_tabview && wd.attrs) {
      int ov = wd.attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
      if (ov != 0) clear = (uint32_t)ov;
    }
    if (backend->begin_frame) backend->begin_frame(wd.render_ctx, clear);
    bool dim = !wd.enabled;
    if (dim && backend->push_alpha) backend->push_alpha(wd.render_ctx, 0.5f);

    if (type && !strcmp(type, NEUI_W_IMAGE)) {
      if (wd.image_asset.id != asset_none.id) {
        auto* e = s->_asset_manager.get_slot(wd.image_asset.id & 0xffff);
        if (e && e->kind == NEUI_ASSET_KIND_BITMAP && e->scale > 0.0f) {
          float bw = (float)e->width_px / e->scale, bh = (float)e->height_px / e->scale;
          if (bw > 0 && bh > 0) {
            float sc = (bw / bh > w / h) ? (w / bw) : (h / bh);
            float dw = bw * sc, dh = bh * sc;
            float dx = (w - dw) * 0.5f, dy = (h - dh) * 0.5f;
            float rot = wd.attrs ? wd.attrs->get_float(NEUI_ATTR_ROTATION, 0.0f) : 0.0f;
            if (rot != 0.0f && backend->push_transform) {
              backend->push_transform(wd.render_ctx);
              backend->translate(wd.render_ctx, dx + dw * 0.5f, dy + dh * 0.5f);
              backend->rotate(wd.render_ctx, rot);
              backend->translate(wd.render_ctx, -dw * 0.5f, -dh * 0.5f);
              ios_painter_draw_asset_thunk(s, backend, wd.render_ctx, wd.image_asset,
                                           0, 0, dw, dh,
                                           neui_detail::k_draw_asset_whole, 0xFFFFFFFFu);
              backend->pop_transform(wd.render_ctx);
            } else {
              ios_painter_draw_asset_thunk(s, backend, wd.render_ctx, wd.image_asset,
                                           dx, dy, dw, dh,
                                           neui_detail::k_draw_asset_whole, 0xFFFFFFFFu);
            }
          }
        }
      }
    }
    else if (type && !strcmp(type, NEUI_W_KNOB)) {
      if (wd.emit_events) {
        neui_event_t pe = {};
        pe.type = NEUI_EVENT_WIDGET_PREUPDATE;
        pe.data.preupdate.widget = { wd.widget_id };
        s->dispatch_event(&pe);
      }
      float value = wd.attrs ? wd.attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f;
      int steps = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
      const char* pol = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_POLARITY) : nullptr;
      const char* vt  = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_VALUE_TEXT) : nullptr;
      neui_detail::paint_knob(backend, wd.render_ctx, 0, 0, w, h, value, false,
                              neui_detail::parse_knob_polarity(pol), steps, vt, wd.attrs.get());
    }
    else if (type && !strcmp(type, NEUI_W_CUSTOMDRAW)) {
      neui_painter painter{};
      painter.backend = backend;
      painter.ctx = wd.render_ctx;
      painter.host_token = s;
      painter.draw_asset_thunk = &ios_painter_draw_asset_thunk;
      neui_detail::CompoundAsset* ca = nullptr;
      if (wd.compound_asset.id != asset_none.id) {
        auto* e = s->_asset_manager.get_slot(wd.compound_asset.id & 0xffff);
        if (e && e->kind == NEUI_ASSET_KIND_COMPOUND) ca = e->compound.get();
      }
      if (backend->push_clip) backend->push_clip(wd.render_ctx, 0, 0, w, h);
      if (ca) {
        uint32_t state = neui_detail::compose_widget_state(wd.enabled, wd.hovered, wd.pressed);
        neui_detail::paint_compound_below(&painter, *ca, w, h, wd.attrs.get(), state);
        neui_detail::paint_compound_above(&painter, *ca, w, h, wd.attrs.get(), state);
      } else if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_WIDGET_PAINT;
        ev.data.paint.widget = { wd.widget_id };
        ev.data.paint.painter_api = &neui_detail::k_painter_api;
        ev.data.paint.p = &painter;
        ev.data.paint.width = w;
        ev.data.paint.height = h;
        ev.data.paint.focused = false;
        s->dispatch_event(&ev);
      }
      if (backend->pop_clip) backend->pop_clip(wd.render_ctx);
    }
    else if (is_section) {
      uint32_t bg = section_resolve_bg_ios(wd);
      const char* align = section_effective_align_ios(wd);
      uint32_t tc = color(ColorRole::text_primary);
      neui_detail::paint_section(backend, wd.render_ctx, 0, 0, w, h,
                                 section_effective_text_ios(wd), bg, align, tc, wd.attrs.get());
      if (wd.section_scroll_state) {
        section_apply_layout_changes_ios(wd);
        neui_detail::paint_section_scrollbars(backend, wd.render_ctx,
                                              wd.section_last_layout, *wd.section_scroll_state,
                                              color(ColorRole::scrollbar_separator),
                                              color(ColorRole::scrollbar_track),
                                              color(ColorRole::scrollbar_thumb));
      }
    }
    else if (type && !strcmp(type, NEUI_W_GRID)) {
      // GRID: the shared paint helper (sticky header, columns, rows, focus
      // band, dual scrollbars, cell-focus outline, cell-edit overlay) - reused
      // verbatim from the other hosts. Origin (0,0) widget-local; (w,h) is the
      // view bounds. The native host has no logical-focus model, so the GRID
      // reports focused when its painted view is first responder (drives the
      // focused-vs-unfocused border + selected-row tint).
      auto& m = ios_grid_ensure_model(wd);
      bool focused = false;
      if (wd.native_control) {
        UIView* v = (__bridge UIView*)wd.native_control;
        focused = v.isFirstResponder ? true : false;
      }
      neui_detail::paint_grid(backend, wd.render_ctx, 0, 0, w, h,
                              m, wd.attrs.get(), focused);
    }
    else if (is_tabview) {
      // TABVIEW: chip strip + body fill + tab-outline border, then size the
      // selected page to the body rect + hide the others. Mirror of the macOS
      // native host's drawRect: TABVIEW branch, reusing the shared
      // widget_paint_tabview.h / widget_tabview.h verbatim.
      const char* pos = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_TAB_POSITION) : nullptr;
      auto tp = neui_detail::parse_tab_position(pos);
      wd.tab_edge = tp.edge;

      std::vector<uint32_t> pages;
      tabview_collect_pages_ios(wd, pages);
      int count = (int)pages.size();
      if (wd.tab_selected >= count) wd.tab_selected = count > 0 ? count - 1 : 0;
      if (wd.tab_selected < 0)      wd.tab_selected = 0;

      // pw.text is a stable std::string for the duration of this paint, so
      // point labels straight at its c_str() (no per-paint copy) - matches the
      // other hosts.
      neui_detail::EffectiveFont ef =
        neui_detail::read_widget_font(wd.attrs.get(), neui_detail::TAB_CHIP_FONT);
      std::vector<const char*> labels(count, "");
      std::vector<uint32_t>    chip_bg(count, 0), chip_text(count, 0);
      for (int i = 0; i < count; ++i) {
        auto& pw = s->_widgets[pages[i]];
        labels[i] = pw.text.c_str();
        if (pw.attrs) {
          chip_bg[i]   = (uint32_t)pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_BG_COLOR, 0);
          chip_text[i] = (uint32_t)pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_TEXT_COLOR, 0);
        }
      }

      // Measure chip labels (chip widths + auto vertical strip) only when the
      // label set or font changed - measure_text is comparatively expensive and
      // this paints every frame. Cached widths keyed by the shared signature.
      uint64_t sig = neui_detail::tab_labels_signature(labels.data(), count,
                         ef.family.c_str(), ef.weight, ef.size);
      if (sig != wd.tab_label_sig || (int)wd.tab_label_widths.size() != count) {
        wd.tab_label_widths.assign(count, 0.0f);
        if (backend->measure_text) {
          neui_detail::push_widget_font(backend, wd.render_ctx, ef);
          for (int i = 0; i < count; ++i)
            wd.tab_label_widths[i] = backend->measure_text(wd.render_ctx, labels[i], -1, ef.size);
          neui_detail::pop_widget_font(backend, wd.render_ctx, ef);
        }
        wd.tab_label_sig = sig;
      }
      const float* widths = wd.tab_label_widths.data();

      float explicit_strip = wd.attrs
                      ? (float)wd.attrs->get_int(NEUI_ATTR_TAB_STRIP_SIZE, 0) : 0.0f;
      float strip = neui_detail::tab_resolve_strip_size(tp.edge, explicit_strip,
                                                        widths, count);
      neui_detail::TabViewLayout L =
        neui_detail::compute_tabview_layout(w, h, tp.edge, strip);

      wd.tab_chips.assign(count, neui_detail::TabChip{});
      if (count > 0 && tp.edge != neui_detail::TabEdge::None)
        neui_detail::layout_tab_chips(L, tp.edge, tp.align, widths,
                                       count, wd.tab_chips.data());

      // The active page's NEUI_ATTR_BACKGROUND drives body_bg so the active
      // chip reads as connected to its page (shared with macOS / win32 / xpl).
      const neui_detail::AttrBag* active_attrs =
        (count > 0) ? s->_widgets[pages[wd.tab_selected]].attrs.get() : nullptr;
      neui_detail::TabPaintColors tc =
        neui_detail::resolve_tab_paint_colors(wd.attrs.get(), active_attrs);

      neui_detail::paint_tabview(backend, wd.render_ctx,
                                 0.0f, 0.0f, w, h,
                                 L, tp.edge, wd.tab_chips.data(), count,
                                 wd.tab_selected,
                                 labels.data(), chip_bg.data(), chip_text.data(),
                                 tc.body_bg, tc.default_text, tc.inactive_chip_bg,
                                 tc.sep_color, tc.border_w, tc.strip_bg, tc.content_border,
                                 tc.chip_radius, wd.attrs.get());

      // Cache the content rect so page geometry (incl. an auto vertical strip)
      // stays consistent outside paint, then size the selected page + show it.
      // Re-apply only when something that affects geometry changed (body rect,
      // selection, or page count) - apply re-flows every page's subtree.
      neui_detail::SectionLayout prev = wd.section_last_layout;
      wd.section_last_layout = neui_detail::SectionLayout{};
      wd.section_last_layout.body_x = (int)L.body_x;
      wd.section_last_layout.body_y = (int)L.body_y;
      wd.section_last_layout.body_w = (int)L.body_w;
      wd.section_last_layout.body_h = (int)L.body_h;
      bool geom_changed = prev.body_x != wd.section_last_layout.body_x ||
                          prev.body_y != wd.section_last_layout.body_y ||
                          prev.body_w != wd.section_last_layout.body_w ||
                          prev.body_h != wd.section_last_layout.body_h ||
                          wd.tab_selected != wd.tab_applied_selected ||
                          count          != wd.tab_applied_count;
      if (geom_changed) {
        tabview_apply_page_geometry_ios(wd);
        wd.tab_applied_selected = wd.tab_selected;
        wd.tab_applied_count    = count;
      }
    }
    else {
      // Defensive fallback for any future painted type that reaches here
      // without its own branch: a flat panel-bg fill + a 1px border. LISTBOX /
      // TREEVIEW are now native UITableViews (not painted views), so they no
      // longer land here.
      if (backend->fill_rect) backend->fill_rect(wd.render_ctx, 0, 0, w, h,
                                                 color(ColorRole::panel_bg));
      if (backend->draw_rect) backend->draw_rect(wd.render_ctx, 0.5f, 0.5f, w - 1, h - 1,
                                                 color(ColorRole::border), 1.0f);
    }

    if (dim && backend->pop_alpha) backend->pop_alpha(wd.render_ctx);
    if (backend->end_frame) backend->end_frame(wd.render_ctx);
  }

  // The painter draw_asset thunk: shared per-(asset, ctx) GPU upload walk.
  void NEUI_ABI ios_painter_draw_asset_thunk(void* host_token,
                                             neui_render_backend_t* backend,
                                             neui_render_ctx_t ctx,
                                             neui_asset_t asset,
                                             float x, float y, float w, float h,
                                             uint32_t frame,
                                             uint32_t tint)
  {
    auto* s = static_cast<Session*>(host_token);
    if (!s || !backend || !ctx || asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    auto* entry = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!entry) return;
    // The dispatch helper owns the whole-vs-cell rule (k_draw_asset_whole
    // draws the whole bitmap; a frame index samples one filmstrip cell)
    // (hosts/shared/painter.h).
    neui_detail::painter_draw_entry_dispatch(backend, ctx, entry, frame,
                                             x, y, w, h, tint);
  }

  // -------------------------------------------------------------------------
  // GRID (NEUI_W_GRID) glue. Mirror of hosts/macos/window.mm's
  // grid_painted_msg_macos family + hosts/crossplatform/platform_ios.mm's
  // touch-pan scroll. The model + paint + math are the shared headers; this is
  // only the iOS-specific event -> model mutation -> repaint + event glue.

  // iOS-only rubber-band feel (live constants from platform_ios.mm): +50%
  // overscroll range, ~2.5x spring-back time. 1.0 defaults keep every other
  // host's feel; only an iOS grid touched here sees these.
  static constexpr double kIosGridOverscrollRangeScale = 1.5;
  static constexpr double kIosGridBounceRateScale      = 1.0 / 2.5;

  neui_detail::GridModel& ios_grid_ensure_model(WidgetData& wd)
  {
    if (!wd.grid_model)
      wd.grid_model = std::make_unique<neui_detail::GridModel>();
    return *wd.grid_model;
  }

  void ios_grid_apply_scroll_feel(WidgetData& wd)
  {
    auto& m = ios_grid_ensure_model(wd);
    m.scroll_kin.overscroll_range_scale = kIosGridOverscrollRangeScale;
    m.scroll_kin.bounce_rate_scale      = kIosGridBounceRateScale;
  }

  void section_apply_scroll_feel(WidgetData& wd)
  {
    if (!wd.section_scroll_state) return;
    auto& st = *wd.section_scroll_state;
    st.kin_v.overscroll_range_scale = kIosGridOverscrollRangeScale;
    st.kin_v.bounce_rate_scale      = kIosGridBounceRateScale;
    st.kin_h.overscroll_range_scale = kIosGridOverscrollRangeScale;
    st.kin_h.bounce_rate_scale      = kIosGridBounceRateScale;
  }

  static void ios_grid_repaint(WidgetData& wd) { mark_widget_dirty_for_paint(wd); }

  // Not yet wired into the iOS GRID paint/hit-test path (port is a core subset);
  // kept for when it is. [[maybe_unused]] so -Werror strict builds don't trip.
  [[maybe_unused]] static neui_detail::GridViewport ios_grid_viewport(WidgetData& wd)
  {
    auto& m   = ios_grid_ensure_model(wd);
    auto  cfg = neui_detail::grid_read_config(wd.attrs.get());
    return neui_detail::grid_compute_viewport(m, wd.width, wd.height,
                                              cfg.row_h, cfg.header_h);
  }

  // ---- Event firers (mirror the macOS grid_fire_* helpers) ----------------

  static bool ios_grid_fire_row_selected(WidgetData& wd, int row)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_SELECTED;
    ev.data.grid_row.widget.id = wd.widget_id;
    ev.data.grid_row.row       = row;
    return wd.session->dispatch_event(&ev);
  }
  static bool ios_grid_fire_cell_selected(WidgetData& wd, int row, int col)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_SELECTED;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row = row; ev.data.grid_cell.col = col;
    return wd.session->dispatch_event(&ev);
  }
  static bool ios_grid_fire_cell_clicked(WidgetData& wd, int row, int col)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_CLICKED;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row = row; ev.data.grid_cell.col = col;
    return wd.session->dispatch_event(&ev);
  }
  static void ios_grid_fire_row_activated(WidgetData& wd, int row)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_ACTIVATED;
    ev.data.grid_row.widget.id = wd.widget_id;
    ev.data.grid_row.row = row;
    wd.session->dispatch_event(&ev);
  }
  static void ios_grid_fire_sort_changed(WidgetData& wd, int col,
                                         neui_grid_sort_dir_t dir)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_SORT_CHANGED;
    ev.data.grid_sort.widget.id = wd.widget_id;
    ev.data.grid_sort.col = col; ev.data.grid_sort.dir = (int)dir;
    wd.session->dispatch_event(&ev);
  }
  static void ios_grid_fire_cell_edit_event(WidgetData& wd, neui_event_type_t t,
                                            int row, int col)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = t;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row = row; ev.data.grid_cell.col = col;
    wd.session->dispatch_event(&ev);
  }

  // Body-cell click ladder: ROW_SELECTED -> (cell_focus) CELL_SELECTED ->
  // CELL_CLICKED, each only firing if the prior wasn't consumed.
  static void ios_grid_click_ladder(WidgetData& wd, int row, int col)
  {
    auto& m   = ios_grid_ensure_model(wd);
    auto  cfg = neui_detail::grid_read_config(wd.attrs.get());
    m.selected_row = row;
    if (cfg.cell_focus) m.selected_col = col;
    if (ios_grid_fire_row_selected(wd, row)) return;
    if (cfg.cell_focus) {
      if (ios_grid_fire_cell_selected(wd, row, col)) return;
    }
    ios_grid_fire_cell_clicked(wd, row, col);
  }

  // ---- Cell-edit dispatch (P3) --------------------------------------------

  bool grid_try_begin_edit_ios(WidgetData& wd, int row, int col)
  {
    auto& m = ios_grid_ensure_model(wd);
    if (m.edit.active) return false;
    auto cfg = neui_detail::grid_read_config(wd.attrs.get());
    if (!neui_detail::grid_cell_edit_allowed(m, row, col, cfg.cell_focus))
      return false;
    neui_detail::grid_begin_edit(m, row, col);
    ios_grid_repaint(wd);
    ios_grid_fire_cell_edit_event(wd, NEUI_EVENT_GRID_CELL_EDIT_BEGIN, row, col);
    return true;
  }

  bool grid_commit_edit_ios(WidgetData& wd)
  {
    auto& m = ios_grid_ensure_model(wd);
    if (!m.edit.active) return false;
    int row = m.edit.row, col = m.edit.col;
    auto* client = wd.session ? wd.session->_grid_client : nullptr;
    const std::string proposed = m.edit.te.text;
    if (client && client->validate_cell) {
      neui_widget_t w{}; w.id = wd.widget_id;
      if (!client->validate_cell(wd.session->get_token(), w, row, col,
                                 proposed.c_str()))
        return false;
    }
    (void)neui_detail::grid_end_edit(m);
    auto& r = m.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = proposed;
    m.sort_dirty = true;
    ios_grid_repaint(wd);
    ios_grid_fire_cell_edit_event(wd, NEUI_EVENT_GRID_CELL_CHANGED, row, col);
    return true;
  }

  void grid_cancel_edit_ios(WidgetData& wd)
  {
    auto& m = ios_grid_ensure_model(wd);
    if (!m.edit.active) return;
    int row = m.edit.row, col = m.edit.col;
    (void)neui_detail::grid_end_edit(m);
    ios_grid_repaint(wd);
    ios_grid_fire_cell_edit_event(wd, NEUI_EVENT_GRID_CELL_EDIT_CANCEL, row, col);
  }

  // Insert one codepoint into the live grid editor (hardware-keyboard path).
  void ios_grid_char(WidgetData& wd, uint32_t cp)
  {
    auto& m = ios_grid_ensure_model(wd);
    if (!m.edit.active) return;
    if (cp < 0x20 || cp == 0x7F) return;
    char buf[4];
    int  n = neui_detail::te_encode_utf8(cp, buf);
    auto& te = m.edit.te;
    neui_detail::te_insert_utf8(te.text, te.cursor, te.sel_anchor,
                                te.overwrite, buf, n, &m.edit.history);
    ios_grid_repaint(wd);
  }

  // Funnelled non-scroll input. Touch tap -> Down/DblClick/Up; hardware key ->
  // Key. (Scrollbar-drag / header-divider-drag are pointer paths the touch tap
  // doesn't reach - a tap on a sortable HEADER cycles the sort; a tap on a Cell
  // runs the click ladder; a "double tap" on an editable cell opens the editor.
  // P3 note: column-resize drag is hardware-pointer only on iOS; deferred.)
  void ios_grid_dispatch_msg(WidgetData& wd, IosGridMsg kind, float lxf, float lyf,
                             uint32_t keycode, uint32_t mods)
  {
    using namespace neui_detail;
    if (!wd.session) return;
    auto& m   = ios_grid_ensure_model(wd);
    auto  cfg = grid_read_config(wd.attrs.get());
    GridViewport vp = grid_compute_viewport(m, wd.width, wd.height,
                                            cfg.row_h, cfg.header_h);
    int lx = (int)lxf, ly = (int)lyf;

    if (kind == IosGridMsg::Key) {
      // --- Edit-mode keys take priority over nav (hardware keyboard) ---
      if (m.edit.active) {
        auto& te   = m.edit.te;
        auto& hist = m.edit.history;
        const bool shift = (mods & NEUI_KMOD_SHIFT) != 0;
        const bool ctrl  = (mods & NEUI_KMOD_CTRL)  != 0;
        switch (keycode) {
        case NEUI_KEY_RETURN: grid_commit_edit_ios(wd); return;
        case NEUI_KEY_ESCAPE: grid_cancel_edit_ios(wd); return;
        case NEUI_KEY_LEFT:
          te_move_left (te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
          ios_grid_repaint(wd); return;
        case NEUI_KEY_RIGHT:
          te_move_right(te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
          ios_grid_repaint(wd); return;
        case NEUI_KEY_HOME:
          te_move_home (te.text, te.cursor, te.sel_anchor, shift, &hist);
          ios_grid_repaint(wd); return;
        case NEUI_KEY_END:
          te_move_end  (te.text, te.cursor, te.sel_anchor, shift, &hist);
          ios_grid_repaint(wd); return;
        case NEUI_KEY_BACK:
          te_backspace     (te.text, te.cursor, te.sel_anchor, ctrl, &hist);
          ios_grid_repaint(wd); return;
        case NEUI_KEY_DELETE:
          te_delete_forward(te.text, te.cursor, te.sel_anchor, ctrl, &hist);
          ios_grid_repaint(wd); return;
        case NEUI_KEY_A:
          if (ctrl) { te_select_all(te.text, te.cursor, te.sel_anchor, &hist); ios_grid_repaint(wd); }
          return;
        case NEUI_KEY_C:
          if (ctrl) {
            std::string sel = te_selected_text(te.text, te.cursor, te.sel_anchor);
            if (!sel.empty())
              neui_detail::clipboard_set_text_ios(sel.c_str(), (uint32_t)sel.size());
          }
          return;
        case NEUI_KEY_X:
          if (ctrl) {
            std::string sel = te_selected_text(te.text, te.cursor, te.sel_anchor);
            if (!sel.empty()) {
              neui_detail::clipboard_set_text_ios(sel.c_str(), (uint32_t)sel.size());
              hist.mark(EditState{ te.text, te.cursor, te.sel_anchor },
                        EditHistory::None, true);
              te_erase_selection(te.text, te.cursor, te.sel_anchor);
              ios_grid_repaint(wd);
            }
          }
          return;
        case NEUI_KEY_V:
          if (ctrl) {
            int n = neui_detail::clipboard_get_text_ios(nullptr, 0);
            if (n > 0) {
              std::vector<char> buf((size_t)n);
              neui_detail::clipboard_get_text_ios(buf.data(), n);
              std::string paste(buf.data(), (size_t)(n > 0 ? n - 1 : 0));
              te_paste(te.text, te.cursor, te.sel_anchor, paste,
                       /*strip_newlines=*/true, &hist);
              ios_grid_repaint(wd);
            }
          }
          return;
        case NEUI_KEY_Z:
          if (ctrl) {
            if (shift) te_redo(te.text, te.cursor, te.sel_anchor, hist);
            else       te_undo(te.text, te.cursor, te.sel_anchor, hist);
            ios_grid_repaint(wd);
          }
          return;
        case NEUI_KEY_Y:
          if (ctrl) { te_redo(te.text, te.cursor, te.sel_anchor, hist); ios_grid_repaint(wd); }
          return;
        default: return;  // swallow other keys while editing
        }
      }

      int n_rows = (int)m.rows.size();
      int n_cols = (int)m.columns.size();
      if (n_rows == 0) return;
      grid_ensure_sort_clean(m);

      int prev_row = m.selected_row;
      int prev_col = m.selected_col;
      int vis = grid_visible_rows(vp, cfg.row_h);
      if (vis < 1) vis = 1;
      bool handled = true;
      switch (keycode) {
      case NEUI_KEY_UP: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v - 1)); break;
      }
      case NEUI_KEY_DOWN: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v + 1)); break;
      }
      case NEUI_KEY_PAGEUP: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v - vis)); break;
      }
      case NEUI_KEY_PAGEDOWN: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? vis : (v + vis)); break;
      }
      case NEUI_KEY_HOME:
        if (cfg.cell_focus && !(mods & NEUI_KMOD_CTRL)) {
          m.selected_col = (n_cols > 0) ? 0 : -1;
          if (m.selected_row < 0) grid_set_selected_visual(m, 0);
        } else {
          grid_set_selected_visual(m, 0);
          if (cfg.cell_focus) m.selected_col = (n_cols > 0) ? 0 : -1;
        }
        break;
      case NEUI_KEY_END:
        if (cfg.cell_focus && !(mods & NEUI_KMOD_CTRL)) {
          m.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
          if (m.selected_row < 0) grid_set_selected_visual(m, n_rows - 1);
        } else {
          grid_set_selected_visual(m, n_rows - 1);
          if (cfg.cell_focus) m.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
        }
        break;
      case NEUI_KEY_LEFT:
        if (cfg.cell_focus) {
          if (m.selected_col > 0) m.selected_col--;
          else if (m.selected_col < 0 && n_cols > 0) m.selected_col = 0;
          if (m.selected_row < 0) m.selected_row = 0;
        } else {
          m.scroll_offset_x -= grid_horizontal_step_px(m);
          grid_clamp_scroll(m, vp, cfg.row_h);
          ios_grid_repaint(wd);
          return;
        }
        break;
      case NEUI_KEY_RIGHT:
        if (cfg.cell_focus) {
          if (m.selected_col < n_cols - 1) {
            if (m.selected_col < 0) m.selected_col = 0;
            else                     m.selected_col++;
          }
          if (m.selected_row < 0) m.selected_row = 0;
        } else {
          m.scroll_offset_x += grid_horizontal_step_px(m);
          grid_clamp_scroll(m, vp, cfg.row_h);
          ios_grid_repaint(wd);
          return;
        }
        break;
      case NEUI_KEY_RETURN: {
        int r = m.selected_row;
        if (r < 0) return;
        if (cfg.cell_focus && m.selected_col >= 0 &&
            grid_try_begin_edit_ios(wd, r, m.selected_col))
          return;
        ios_grid_fire_row_activated(wd, r);
        return;
      }
      default: handled = false; break;
      }
      if (!handled) return;
      m.scroll_px_offset = 0;   // keyboard nav snaps to exact row alignment
      if (cfg.cell_focus && m.selected_col >= 0)
        grid_ensure_cell_visible(m, vp, cfg.row_h, m.selected_row, m.selected_col);
      else
        grid_ensure_row_visible(m, vp, cfg.row_h, m.selected_row);
      if (m.selected_row != prev_row)
        ios_grid_fire_row_selected(wd, m.selected_row);
      if (cfg.cell_focus &&
          (m.selected_row != prev_row || m.selected_col != prev_col))
        ios_grid_fire_cell_selected(wd, m.selected_row, m.selected_col);
      ios_grid_repaint(wd);
      return;
    }

    // --- touch tap / double-tap (Down / DblClick) + Up ---
    if (kind == IosGridMsg::Down || kind == IosGridMsg::DblClick) {
      grid_ensure_sort_clean(m);
      GridHit hit = grid_hit_test(m, vp, cfg.row_h, wd.width, wd.height, lx, ly);

      // Edit-mode tap handling: a tap inside the editing cell keeps the editor
      // open; anywhere else commits (cancel-on-reject swallows the tap).
      if (m.edit.active) {
        bool on_editing_cell = (hit.region == GridHitRegion::Cell &&
                                hit.row == m.edit.row && hit.col == m.edit.col);
        if (on_editing_cell) return;
        if (!grid_commit_edit_ios(wd)) return;
      }

      switch (hit.region) {
      case GridHitRegion::Header:
        // A tap on a sortable header cycles the sort (touch has no Shift, so
        // always a primary-replace).
        if (kind == IosGridMsg::Down && grid_header_click_allowed(m, hit.col)) {
          neui_grid_sort_dir_t new_dir = grid_apply_header_click(m, hit.col, false);
          ios_grid_repaint(wd);
          ios_grid_fire_sort_changed(wd, hit.col, new_dir);
        }
        return;
      case GridHitRegion::VertScrollTrack:
      case GridHitRegion::HorzScrollTrack:
      case GridHitRegion::HeaderDivider:
        // Touch scroll is the pan; the scroll track / divider are position
        // readouts (iOS idiom). No-op on tap.
        return;
      case GridHitRegion::Cell: {
        if (kind == IosGridMsg::DblClick) {
          if (!grid_try_begin_edit_ios(wd, hit.row, hit.col))
            ios_grid_fire_row_activated(wd, hit.row);
        } else {
          const GridCellOverride* ov = grid_find_override(m, hit.row, hit.col);
          bool cell_dis = ov && ov->has_enabled && !ov->enabled;
          int prev_row = m.selected_row;
          m.selected_row = hit.row;
          if (cfg.cell_focus) m.selected_col = hit.col;
          if (cell_dis) {
            if (m.selected_row != prev_row)
              ios_grid_fire_row_selected(wd, hit.row);
          } else {
            ios_grid_click_ladder(wd, hit.row, hit.col);
          }
        }
        ios_grid_repaint(wd);
        return;
      }
      case GridHitRegion::BodyEmpty:
        if (m.selected_row != -1) {
          m.selected_row = -1;
          m.selected_col = -1;
          ios_grid_fire_row_selected(wd, -1);
          ios_grid_repaint(wd);
        }
        return;
      default:
        return;
      }
    }
    (void)keycode;
  }

  // -------------------------------------------------------------------------
  // mark_widget_dirty / enabled / geometry / text helpers (UIKit-coupled).

  void mark_widget_dirty_for_paint(WidgetData& wd)
  {
    if (!wd.native_control) {
      if (wd.native_window) {
        UIWindow* w = (__bridge UIWindow*)wd.native_window;
        [w.rootViewController.view setNeedsDisplay];
      }
      return;
    }
    UIView* v = (__bridge UIView*)wd.native_control;
    [v setNeedsDisplay];
  }

  // COMBOBOX subview layout constants (the title label + chevron are subviews of
  // the host UIButton so they can pin to opposite edges - see the create branch).
  // Tags let combobox_rebuild_menu_ios + theming find them via -viewWithTag:.
  static const NSInteger kComboTitleLabelTag = 0x4E55C0B1; // 'NUcobl'-ish, unique
  static const NSInteger kComboChevronTag    = 0x4E55C0B2;
  static const CGFloat   kComboLabelInset    = 10.0f;      // left/right edge inset
  static const CGFloat   kComboChevronSize   = 13.0f;      // chevron glyph box
  static const CGFloat   kComboChevronZone   = 32.0f;      // right inset for label

  // COMBOBOX (native UIButton + UIMenu): rebuild the pull-down menu + title to
  // track wd.items / wd.selected_item. iOS idiom for the macOS NSPopUpButton:
  // a UIButton with showsMenuAsPrimaryAction whose .menu is a UIMenu of one
  // UIAction per item (the selected item carries .state = .on, so UIKit draws a
  // checkmark next to it). The button title shows the current selection (or a
  // "Select…" placeholder when nothing is picked), mirroring the collapsed bar.
  //
  // DANGLING-CALLBACK SAFETY (this exact mistake std::bad_function_call-crashed
  // the hamburger before): each UIAction handler captures ONLY plain values by
  // value - the widget_id (uint32_t) + the item index - and re-resolves the
  // live Session / WidgetData via widget_for_id at TAP time. It never captures a
  // std::function, a C++ reference, or a pointer into a temporary, so a tap that
  // arrives after the menu was rebuilt (or the widget rebuilt) stays safe.
  //
  // No-op before the button exists (items added pre-show seed at create time via
  // this same helper). COMBO_* attrs (combo_max_visible / combo_drop_width) are
  // ignored here, exactly like the macOS NSPopUpButton: a UIMenu auto-sizes its
  // pop-out, so there is no collapsed-list width / row-count to tune.
  void combobox_rebuild_menu_ios(WidgetData& wd)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    if (![v isKindOfClass:[UIButton class]]) return;
    UIButton* btn = (UIButton*)v;

    // Current-selection title (placeholder when none / out of range). The title
    // lives on the left-pinned UILabel subview (NOT setTitle: - the button's own
    // title would group with any image + center, which is exactly the look we are
    // avoiding). The label is located by its known tag, set up in the create
    // branch; fall back to setTitle: only if it is somehow missing.
    NSString* title = @"Select…";
    if (wd.selected_item != NEUI_ITEM_NONE && wd.selected_item < wd.items.size())
      title = [NSString stringWithUTF8String:wd.items[wd.selected_item].text.c_str()];
    UILabel* lbl = (UILabel*)[btn viewWithTag:kComboTitleLabelTag];
    if ([lbl isKindOfClass:[UILabel class]])
      lbl.text = title;
    else
      [btn setTitle:title forState:UIControlStateNormal];

    if (@available(iOS 14.0, *)) {
      uint32_t widget_id = wd.widget_id;   // plain value, captured by copy
      NSMutableArray<UIMenuElement*>* children = [NSMutableArray array];
      for (size_t i = 0; i < wd.items.size(); ++i) {
        NSString* itext = [NSString stringWithUTF8String:wd.items[i].text.c_str()];
        uint32_t  index = (uint32_t)i;     // plain value, captured by copy
        UIAction* a = [UIAction actionWithTitle:itext
                                          image:nil
                                     identifier:nil
                                        handler:^(__kindof UIAction* action) {
          (void)action;
          // Re-resolve the live widget at tap time - never trust a captured ref.
          ios_host::Session* psess = nullptr;
          ios_host::WidgetData* pwd = ios_host::widget_for_id(widget_id, &psess);
          if (!pwd || !psess) return;
          if (index >= pwd->items.size()) return;
          pwd->selected_item = index;
          if (pwd->emit_events) {
            neui_event_t ev = {};
            ev.type             = NEUI_EVENT_ITEM_SELECTED;
            ev.data.item.widget = { pwd->widget_id };
            ev.data.item.index  = index;
            psess->dispatch_event(&ev);
          }
          // Title + checkmark follow the new selection.
          ios_host::combobox_rebuild_menu_ios(*pwd);
        }];
        if (wd.selected_item == index) a.state = UIMenuElementStateOn;
        [children addObject:a];
      }
      btn.menu = [UIMenu menuWithTitle:@"" children:children];
    }
  }

  // LISTBOX (native UITableView): reloadData so the rows track wd.items.
  // COMBOBOX (native UIButton + UIMenu): rebuild the pull-down + title.
  // Branch on the native control kind so the LISTBOX behavior stays intact.
  // No-op before the control exists (items added pre-show seed at creation).
  void reload_native_items_ios(WidgetData& wd)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    if ([v isKindOfClass:[UITableView class]]) [(UITableView*)v reloadData];
    else if ([v isKindOfClass:[UIButton class]]) combobox_rebuild_menu_ios(wd);
  }

  // LISTBOX: reflect the model selection into the UITableView (select + scroll
  // to it, or clear). Mirror of the macOS i_set_selected scrollRowToVisible.
  // COMBOBOX: rebuild the UIMenu so the title + checkmark track selected_item.
  // Branch on the native control kind so the LISTBOX behavior stays intact.
  void reload_native_item_selection_ios(WidgetData& wd)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    if ([v isKindOfClass:[UIButton class]]) { combobox_rebuild_menu_ios(wd); return; }
    if (![v isKindOfClass:[UITableView class]]) return;
    UITableView* tv = (UITableView*)v;
    if (wd.selected_item == NEUI_ITEM_NONE || wd.selected_item >= wd.items.size()) {
      NSIndexPath* sel = tv.indexPathForSelectedRow;
      if (sel) [tv deselectRowAtIndexPath:sel animated:NO];
      return;
    }
    NSIndexPath* ip = [NSIndexPath indexPathForRow:(NSInteger)wd.selected_item inSection:0];
    [tv selectRowAtIndexPath:ip animated:NO scrollPosition:UITableViewScrollPositionNone];
    [tv scrollToRowAtIndexPath:ip atScrollPosition:UITableViewScrollPositionNone animated:NO];
  }

  // TREEVIEW (native UITableView): re-flatten the visible-row model from the
  // expanded state, then reloadData. No-op before the table exists.
  void reload_native_tree_ios(WidgetData& wd)
  {
    tree_rebuild_visible_rows_ios(wd);
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    if ([v isKindOfClass:[UITableView class]]) [(UITableView*)v reloadData];
  }

  // TREEVIEW: reflect the model selection into the UITableView by finding the
  // visible row for selected_tree_item (no-op if it is off-screen under a
  // collapsed ancestor). Caller refreshes tree_vis_rows first.
  void reload_native_tree_selection_ios(WidgetData& wd)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    if (![v isKindOfClass:[UITableView class]]) return;
    UITableView* tv = (UITableView*)v;
    NSInteger row = -1;
    for (size_t r = 0; r < wd.tree_vis_rows.size(); ++r)
      if (wd.tree_vis_rows[r].tree_id == wd.selected_tree_item) { row = (NSInteger)r; break; }
    if (row < 0) {
      NSIndexPath* sel = tv.indexPathForSelectedRow;
      if (sel) [tv deselectRowAtIndexPath:sel animated:NO];
      return;
    }
    NSIndexPath* ip = [NSIndexPath indexPathForRow:row inSection:0];
    [tv selectRowAtIndexPath:ip animated:NO scrollPosition:UITableViewScrollPositionNone];
    [tv scrollToRowAtIndexPath:ip atScrollPosition:UITableViewScrollPositionNone animated:NO];
  }

  void apply_enabled_native_ios(WidgetData& wd)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    if ([v isKindOfClass:[NEUINativeIOSPaintedView class]]) {
      [v setNeedsDisplay];
    } else if ([v isKindOfClass:[NEUIIOSCheckboxSwitchView class]]) {
      // SWITCH-style checkbox: drive the inner switch's native enabled look +
      // dim the label to match.
      NEUIIOSCheckboxSwitchView* cv = (NEUIIOSCheckboxSwitchView*)v;
      cv.theSwitch.enabled = wd.enabled ? YES : NO;
      cv.theLabel.enabled  = wd.enabled ? YES : NO;
    } else if ([v isKindOfClass:[UIControl class]]) {
      ((UIControl*)v).enabled = wd.enabled ? YES : NO;
      // COMBOBOX: the title label + chevron are independent subviews, so the
      // button's own disabled dimming doesn't reach them - dim them by hand.
      if ([v isKindOfClass:[UIButton class]]) {
        UIView* lbl  = [v viewWithTag:kComboTitleLabelTag];
        UIView* chev = [v viewWithTag:kComboChevronTag];
        CGFloat a    = wd.enabled ? 1.0f : 0.5f;
        if (lbl)  lbl.alpha  = a;
        if (chev) chev.alpha = a;
      }
    } else if ([v isKindOfClass:[UITextView class]]) {
      ((UITextView*)v).editable = wd.enabled ? YES : NO;
    } else {
      v.userInteractionEnabled = wd.enabled ? YES : NO;
      v.alpha = wd.enabled ? 1.0f : 0.5f;
    }
  }

  // Re-apply the palette-derived colours a native control snapshotted at
  // creation time. The text / tint colours we set are DYNAMIC system UIColors
  // (UIColor.labelColor etc.), which UIKit re-resolves automatically when the
  // trait environment flips - so we leave those alone. The exception is
  // layer.borderColor: it is a CGColorRef, a STATIC snapshot resolved at
  // assignment, so it does NOT track the appearance flip. Re-assign it from
  // UIColor.separatorColor (resolved against the view's current traits) so
  // button / text-field / multiline borders match the new theme without a tap.
  void apply_theme_native_ios(WidgetData& wd)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    CGColorRef sep = UIColor.separatorColor.CGColor;
    if (@available(iOS 13.0, *))
      sep = [UIColor.separatorColor resolvedColorWithTraitCollection:v.traitCollection].CGColor;
    if ([v isKindOfClass:[UIButton class]] ||
        [v isKindOfClass:[UITextView class]] ||
        [v isKindOfClass:[UITableView class]]) {
      // BUTTON + MULTILINE + LISTBOX/TREEVIEW draw a 1px palette border via
      // layer.borderColor (a static CGColor snapshot that doesn't track the
      // appearance flip on its own).
      if (v.layer.borderWidth > 0) v.layer.borderColor = sep;
    }
  }

  void Session::invalidate_all_for_theme_change()
  {
    // Walk every live widget (not just the content view's direct subviews) so
    // painted views nested inside SECTION body containers / TABVIEW page
    // containers repaint too, and native controls re-apply palette-derived
    // colours. Mirror of invalidate_widgets_with_compound's full-tree walk.
    auto order = _widgets.release_order();
    for (uint32_t i : order) {
      if (i == 0 || !_widgets.exists(i)) continue;
      WidgetData& wd = _widgets[i];
      if (!wd.type) continue;
      apply_theme_native_ios(wd);
      mark_widget_dirty_for_paint(wd);
    }
  }

  static void resolve_parent_scroll_offset(Session* s, uint32_t idx, int& ox, int& oy)
  {
    ox = oy = 0;
    uint32_t p = s->_widgets.get_parent(idx);
    if (p != 0 && p != neui_detail::knone.id && s->_widgets.exists(p)) {
      auto& pw = s->_widgets[p];
      if (pw.section_scroll_state) {
        ox = pw.section_scroll_state->scroll_x;
        oy = pw.section_scroll_state->scroll_y;
      }
    }
  }

  void apply_geometry_native_ios(WidgetData& wd)
  {
    if (wd.native_window) {
      // A UIWindow fills its scene; nothing to position. Children re-layout
      // off RESIZE.
      return;
    }
    if (!wd.native_control) return;
    int ox = 0, oy = 0;
    if (wd.session) resolve_parent_scroll_offset(wd.session, wd.index, ox, oy);
    UIView* v = (__bridge UIView*)wd.native_control;
    v.frame = CGRectMake(wd.x - ox, wd.y - oy, wd.width, wd.height);
    if ([v isKindOfClass:[NEUINativeIOSPaintedView class]]) {
      NEUINativeIOSPaintedView* pv = (NEUINativeIOSPaintedView*)v;
      auto* backend = neui_cg_backend::get_backend();
      if (backend && backend->resize && pv->render_ctx)
        backend->resize(pv->render_ctx, (uint32_t)wd.width, (uint32_t)wd.height);
      [pv setNeedsDisplay];
    }
    if (wd.type && (!strcmp(wd.type, NEUI_W_SECTION) ||
                    !strcmp(wd.type, NEUI_W_TABPAGE)))
      section_apply_layout_changes_ios(wd);
    // TABVIEW self-resize: a repaint re-flows the chip strip + re-sizes the
    // selected page to the new content body rect (the paint pass calls
    // tabview_apply_page_geometry_ios when the body rect changed).
    else if (wd.type && !strcmp(wd.type, NEUI_W_TABVIEW))
      mark_widget_dirty_for_paint(wd);
  }

  static NSString* nsstr(const char* s) { return s ? [NSString stringWithUTF8String:s] : @""; }

  void set_window_title_ios(WidgetData& /*wd*/, const char* /*text*/)
  {
    // iOS windows have no title bar. (UIWindowScene.title is app-switcher only.)
  }

  void set_native_text_ios(WidgetData& wd, const char* text)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    NSString* t = nsstr(text);
    if ([v isKindOfClass:[NEUIIOSCheckboxSwitchView class]]) ((NEUIIOSCheckboxSwitchView*)v).theLabel.text = t;
    else if ([v isKindOfClass:[UILabel class]])        ((UILabel*)v).text = t;
    else if ([v isKindOfClass:[UITextField class]]) ((UITextField*)v).text = t;
    else if ([v isKindOfClass:[UITextView class]])  ((UITextView*)v).text = t;
    else if ([v isKindOfClass:[UIButton class]])    [(UIButton*)v setTitle:t forState:UIControlStateNormal];
  }

  int get_native_text_ios(WidgetData& wd, char* buf, int buflen)
  {
    if (!wd.native_control) return -1;
    UIView* v = (__bridge UIView*)wd.native_control;
    NSString* t = nil;
    if ([v isKindOfClass:[NEUIIOSCheckboxSwitchView class]]) t = ((NEUIIOSCheckboxSwitchView*)v).theLabel.text;
    else if ([v isKindOfClass:[UILabel class]])        t = ((UILabel*)v).text;
    else if ([v isKindOfClass:[UITextField class]]) t = ((UITextField*)v).text;
    else if ([v isKindOfClass:[UITextView class]])  t = ((UITextView*)v).text;
    else if ([v isKindOfClass:[UIButton class]])    t = [(UIButton*)v titleForState:UIControlStateNormal];
    else return -1;
    const char* utf8 = t ? t.UTF8String : "";
    int needed = (int)strlen(utf8) + 1;
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      memcpy(buf, utf8, (size_t)(n - 1)); buf[n - 1] = '\0';
    }
    return needed;
  }

  void set_native_check_ios(WidgetData& wd, neui_check_state_t state)
  {
    if (!wd.native_control) return;
    // Checkboxes are UIButtons driving an SF Symbol image (see create). Only the
    // CHECKBOX / CHECKBOX3 buttons carry a checkstate attr, so a plain BUTTON
    // never reaches here through w_set_check (the client only calls set_check on
    // a checkbox). Swap the glyph to match the requested state.
    if (wd.type && (!strcmp(wd.type, NEUI_W_CHECKBOX) || !strcmp(wd.type, NEUI_W_CHECKBOX3))) {
      UIView* v = (__bridge UIView*)wd.native_control;
      if ([v isKindOfClass:[NEUIIOSCheckboxSwitchView class]]) {
        // SWITCH style: drive theSwitch.on (a UISwitch has no indeterminate
        // state, so any non-unchecked state shows as on).
        ((NEUIIOSCheckboxSwitchView*)v).theSwitch.on = (state != NEUI_CHECK_UNCHECKED);
      } else if ([v isKindOfClass:[UIButton class]]) {
        // GLYPH style: swap the SF Symbol image to match the requested state.
        [(UIButton*)v setImage:neui_detail::checkbox_image_for_state_ios((int)state)
                      forState:UIControlStateNormal];
      }
    }
  }

  void set_native_float_ios(WidgetData& wd, const char* key, float value)
  {
    if (!key || !wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    if (!strcmp(key, NEUI_PARAM_VALUE) && [v isKindOfClass:[UISlider class]]) {
      float c = value < 0 ? 0 : (value > 1 ? 1 : value);
      ((UISlider*)v).value = c;
    } else if (!strcmp(key, NEUI_ATTR_ROTATION) && [v isKindOfClass:[NEUINativeIOSPaintedView class]]) {
      [v setNeedsDisplay];
    }
  }

  // Route a built-in command (NEUI_CMD_*) to the key window's first responder
  // via UIKit's standard editing actions. cut/copy/paste/select-all/delete map
  // to UIResponderStandardEditActions selectors (which UITextField / UITextView
  // implement); undo/redo go through the responder's NSUndoManager. Returns
  // true if something handled it. Mirror of invoke_focused_command_macos -
  // [UIApplication sendAction:to:nil...] walks the responder chain from the
  // first responder, the same contract as AppKit's [NSApp sendAction:to:nil].
  bool invoke_focused_command_ios(uint32_t cmd)
  {
    if (@available(iOS 13.0, *)) {
      if (cmd == NEUI_CMD_UNDO || cmd == NEUI_CMD_REDO) {
        // UIWindow has no public firstResponder accessor; capture it by sending
        // a no-op action through the responder chain (the receiver records
        // `sender`, which the chain resolves to the current first responder).
        UIResponder* r = neui_capture_first_responder_ios();
        NSUndoManager* um = nil;
        if (r && [r respondsToSelector:@selector(undoManager)])
          um = [(id)r undoManager];
        if (!um) return false;
        if (cmd == NEUI_CMD_UNDO) { if (!um.canUndo) return false; [um undo]; return true; }
        else                      { if (!um.canRedo) return false; [um redo]; return true; }
      }

      SEL sel = nil;
      switch (cmd) {
        case NEUI_CMD_CUT:        sel = @selector(cut:);       break;
        case NEUI_CMD_COPY:       sel = @selector(copy:);      break;
        case NEUI_CMD_PASTE:      sel = @selector(paste:);     break;
        case NEUI_CMD_SELECT_ALL: sel = @selector(selectAll:); break;
        case NEUI_CMD_DELETE:     sel = @selector(delete:);    break;
        default: return false;  // NONE / unknown / >= USER_BASE: client's job
      }
      return [UIApplication.sharedApplication sendAction:sel to:nil
                                                    from:nil forEvent:nil] ? true : false;
    }
    return false;
  }

  void frame_refresh_hamburger_ios(WidgetData& mb_or_frame)
  {
    // Resolve to the owning frame's content view + rebuild.
    Session* s = mb_or_frame.session;
    if (!s) return;
    WidgetData* frame = mb_or_frame.isroot ? &mb_or_frame
                                           : owning_frame(s, mb_or_frame.index);
    if (!frame) return;
    NEUINativeIOSContentView* cv = content_view_for_frame(*frame);
    [cv refreshHamburger];
  }

  void notify_toast_ios(WidgetData& frame, const char* text)
  {
    // v1: a simple transient UILabel overlay anchored at the content-area top.
    // (The shared Session::paint_toast overlay path is xpl-only; the native
    // host shows a lightweight UIKit toast instead.) TODO(ios phase 2): unify
    // with the shared ToastState phase math.
    if (!frame.native_window || !text) return;
    UIWindow* w = (__bridge UIWindow*)frame.native_window;
    UIView* root = w.rootViewController.view;
    if (!root) return;
    UILabel* toast = [[UILabel alloc] init];
    toast.text = nsstr(text);
    toast.numberOfLines = 0;
    toast.textAlignment = NSTextAlignmentCenter;
    toast.textColor = UIColor.whiteColor;
    toast.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.82];
    toast.layer.cornerRadius = 10;
    toast.clipsToBounds = YES;
    CGFloat top = 0;
    if (@available(iOS 11.0, *)) top = root.safeAreaInsets.top;
    CGFloat ww = root.bounds.size.width * 0.7;
    CGSize fit = [toast sizeThatFits:CGSizeMake(ww, 1000)];
    CGFloat bw = fit.width + 28, bh = fit.height + 18;
    toast.frame = CGRectMake((root.bounds.size.width - bw) * 0.5f, top + kHamburgerBandH + 10,
                             bw, bh);
    [root addSubview:toast];
    [UIView animateWithDuration:0.4 delay:1.8 options:0 animations:^{
      toast.alpha = 0.0;
    } completion:^(BOOL finished) { (void)finished; [toast removeFromSuperview]; }];
  }

  int notify_message_box_ios(WidgetData& frame, const char* text,
                             const char* caption, uint32_t flags)
  {
    if (!frame.native_window) return 0;
    if (@available(iOS 13.0, *)) {
      UIWindow* w = (__bridge UIWindow*)frame.native_window;
      UIViewController* presenter = w.rootViewController;
      while (presenter.presentedViewController) presenter = presenter.presentedViewController;
      return neui_detail::message_box_ios(presenter, text, caption, flags);
    }
    return 0;
  }

  // -------------------------------------------------------------------------
  // Native control creation + the WIDGET_PAINT target/action sink.

  // A single object instance per control whose action selectors fire the neui
  // events. The control's `tag` carries widget_id.
}

@interface NEUINativeIOSControlTarget : NSObject
+ (instancetype)shared;
- (void)buttonTapped:(UIButton*)b;
- (void)checkboxTapped:(UIButton*)b;
- (void)checkboxSwitchChanged:(UISwitch*)sw;
- (void)sliderChanged:(UISlider*)sl;
- (void)textChanged:(UITextField*)tf;
@end

@implementation NEUINativeIOSControlTarget
+ (instancetype)shared
{
  static NEUINativeIOSControlTarget* g = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ g = [[NEUINativeIOSControlTarget alloc] init]; });
  return g;
}
- (void)dispatchFor:(NSInteger)tag build:(void(^)(ios_host::Session*, ios_host::WidgetData&))build
{
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* wd = ios_host::widget_for_id((uint32_t)tag, &sess);
  if (!wd || !sess) return;
  build(sess, *wd);
}
- (void)buttonTapped:(UIButton*)b
{
  [self dispatchFor:b.tag build:^(ios_host::Session* s, ios_host::WidgetData& wd) {
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
    ev.data.mouse.widget = { wd.widget_id };
    s->dispatch_event(&ev);
  }];
}
- (void)checkboxTapped:(UIButton*)b
{
  [self dispatchFor:b.tag build:^(ios_host::Session* s, ios_host::WidgetData& wd) {
    // The button does not auto-toggle; we own the state machine (mirror of the
    // macOS native checkbox click handler). Read the cached state, advance by 2
    // (CHECKBOX) or 3 (CHECKBOX3 / tristate), swap the SF Symbol image, write
    // back, and fire CHECKBOX_CHANGED.
    bool tristate = wd.attrs && wd.attrs->get_int(NEUI_ATTR_TRISTATE, 0) != 0;
    int prev = wd.attrs ? wd.attrs->get_int("neui.ioshost.checkstate", NEUI_CHECK_UNCHECKED)
                        : NEUI_CHECK_UNCHECKED;
    int mod  = tristate ? 3 : 2;
    int next = (prev + 1) % mod;
    [b setImage:neui_detail::checkbox_image_for_state_ios(next) forState:UIControlStateNormal];
    neui_detail::ensure_attrs(wd.attrs).set_int("neui.ioshost.checkstate", next);

    neui_event_t ev = {};
    ev.type = NEUI_EVENT_CHECKBOX_CHANGED;
    ev.data.checkbox.widget = { wd.widget_id };
    ev.data.checkbox.state = (neui_check_state_t)next;
    s->dispatch_event(&ev);
  }];
}
- (void)checkboxSwitchChanged:(UISwitch*)sw
{
  [self dispatchFor:sw.tag build:^(ios_host::Session* s, ios_host::WidgetData& wd) {
    // SWITCH-style 2-state CHECKBOX: the UISwitch auto-toggles itself, so the
    // host just mirrors its on/off into the cached state + fires
    // CHECKBOX_CHANGED (CHECKED / UNCHECKED - a UISwitch has no indeterminate
    // state, so this path is never tri-state).
    int next = sw.on ? NEUI_CHECK_CHECKED : NEUI_CHECK_UNCHECKED;
    neui_detail::ensure_attrs(wd.attrs).set_int("neui.ioshost.checkstate", next);
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_CHECKBOX_CHANGED;
    ev.data.checkbox.widget = { wd.widget_id };
    ev.data.checkbox.state = (neui_check_state_t)next;
    s->dispatch_event(&ev);
  }];
}
- (void)sliderChanged:(UISlider*)sl
{
  [self dispatchFor:sl.tag build:^(ios_host::Session* s, ios_host::WidgetData& wd) {
    float v = sl.value; if (v < 0) v = 0; if (v > 1) v = 1;
    neui_detail::ensure_attrs(wd.attrs).set_float(NEUI_PARAM_VALUE, v);
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_VALUE_CHANGED;
    ev.data.value.widget = { wd.widget_id };
    ev.data.value.value = v;
    s->dispatch_event(&ev);
  }];
}
- (void)textChanged:(UITextField*)tf
{
  [self dispatchFor:tf.tag build:^(ios_host::Session* s, ios_host::WidgetData& wd) {
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_WIDGET_UPDATED;
    ev.data.preupdate.widget = { wd.widget_id };
    s->dispatch_event(&ev);
  }];
}
@end

namespace ios_host
{
  // Rebuild the TREEVIEW flattened visible-row model from tree_items +
  // per-node expanded state (root children first, then each expanded node's
  // children in insertion order). Mirror of the xpl host's
  // TreeviewWidget::flatten_visible(). Called on every tree mutation + expand
  // toggle so the UITableView data source + didSelect agree on row->item.
  void tree_rebuild_visible_rows_ios(WidgetData& wd)
  {
    wd.tree_vis_rows.clear();
    // children lookup preserving insertion order.
    std::unordered_map<uint32_t, std::vector<uint32_t>> kids;
    for (uint32_t id : wd.tree_items_ordered) {
      auto it = wd.tree_items.find(id);
      if (it == wd.tree_items.end()) continue;
      kids[it->second.parent_id].push_back(id);
    }
    wd.tree_vis_rows.reserve(wd.tree_items_ordered.size());
    struct Frame { uint32_t id; int depth; };
    std::vector<Frame> stack;
    // Push root children (parent_id == 0) in reverse so the first is popped first.
    auto rit = kids.find(0u);
    if (rit != kids.end())
      for (auto it = rit->second.rbegin(); it != rit->second.rend(); ++it)
        stack.push_back({ *it, 0 });
    while (!stack.empty()) {
      Frame f = stack.back();
      stack.pop_back();
      auto it = wd.tree_items.find(f.id);
      if (it == wd.tree_items.end()) continue;
      auto kit = kids.find(f.id);
      bool has_kids = (kit != kids.end() && !kit->second.empty());
      wd.tree_vis_rows.push_back({ f.id, f.depth, has_kids });
      if (has_kids && it->second.expanded)
        for (auto cit = kit->second.rbegin(); cit != kit->second.rend(); ++cit)
          stack.push_back({ *cit, f.depth + 1 });
    }
  }
} // namespace ios_host

// ---------------------------------------------------------------------------
// NEUIIOSListSource - per-LISTBOX UITableViewDataSource + Delegate. One per
// LISTBOX widget; the UITableView owns a strong reference via
// objc_setAssociatedObject. Rows mirror wd.items; a tap fires
// NEUI_EVENT_ITEM_SELECTED. Mirror of the macOS NEUINativeListSource.

@interface NEUIIOSListSource : NSObject<UITableViewDataSource, UITableViewDelegate>
{
@public
  uint32_t widget_id;
}
@end

@implementation NEUIIOSListSource

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section
{
  (void)tableView; (void)section;
  auto* wd = ios_host::widget_for_id(widget_id, nullptr);
  return wd ? (NSInteger)wd->items.size() : 0;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath
{
  static NSString* const k_id = @"neui.ios.listcell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:k_id];
  if (!cell)
    cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                  reuseIdentifier:k_id];
  auto* wd = ios_host::widget_for_id(widget_id, nullptr);
  NSString* text = @"";
  if (wd && indexPath.row >= 0 && (size_t)indexPath.row < wd->items.size())
    text = [NSString stringWithUTF8String:wd->items[(size_t)indexPath.row].text.c_str()];
  // UITableViewCell's default content configuration is Dynamic-Type-aware and
  // uses system label colours, so it auto-follows dark/light.
  if (@available(iOS 14.0, *)) {
    UIListContentConfiguration* cfg = [cell defaultContentConfiguration];
    cfg.text = text;
    cell.contentConfiguration = cfg;
  } else {
    cell.textLabel.text = text;
  }
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath
{
  (void)tableView;
  ios_host::Session* sess = nullptr;
  auto* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  uint32_t idx = (indexPath.row >= 0 && (size_t)indexPath.row < wd->items.size())
                   ? (uint32_t)indexPath.row : NEUI_ITEM_NONE;
  wd->selected_item = idx;
  if (!wd->emit_events) return;
  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_ITEM_SELECTED;
  ev.data.item.widget = { wd->widget_id };
  ev.data.item.index  = idx;
  sess->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUIIOSTreeSource - per-TREEVIEW UITableViewDataSource + Delegate. A
// TREEVIEW renders as a flattened list of currently-visible rows (parents +
// expanded descendants) with a depth indent + a disclosure chevron on rows
// that have children. Tapping a parent row toggles expand/collapse + reloads;
// any row tap fires NEUI_EVENT_TREE_ITEM_SELECTED; a tap on an
// already-selected leaf fires NEUI_EVENT_TREE_ITEM_ACTIVATED (the touch
// analogue of macOS double-click activation). The visible-row model lives on
// the WidgetData (tree_vis_rows) so it survives reloadData.

@interface NEUIIOSTreeSource : NSObject<UITableViewDataSource, UITableViewDelegate>
{
@public
  uint32_t widget_id;
}
@end

@implementation NEUIIOSTreeSource

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section
{
  (void)tableView; (void)section;
  auto* wd = ios_host::widget_for_id(widget_id, nullptr);
  return wd ? (NSInteger)wd->tree_vis_rows.size() : 0;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath
{
  static NSString* const k_id = @"neui.ios.treecell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:k_id];
  if (!cell)
    cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                  reuseIdentifier:k_id];
  auto* wd = ios_host::widget_for_id(widget_id, nullptr);
  NSString* text = @"";
  int  depth = 0;
  bool has_kids = false, expanded = false;
  if (wd && indexPath.row >= 0 && (size_t)indexPath.row < wd->tree_vis_rows.size()) {
    const auto& vr = wd->tree_vis_rows[(size_t)indexPath.row];
    depth    = vr.depth;
    has_kids = vr.has_kids;
    auto it = wd->tree_items.find(vr.tree_id);
    if (it != wd->tree_items.end()) {
      text     = [NSString stringWithUTF8String:it->second.text.c_str()];
      expanded = it->second.expanded;
    }
  }
  if (@available(iOS 14.0, *)) {
    UIListContentConfiguration* cfg = [cell defaultContentConfiguration];
    cfg.text = text;
    // Per-depth indent via the content config's directional inset (1 level =
    // 16 pt, matching the xpl host's TREE_INDENT).
    cfg.directionalLayoutMargins =
      NSDirectionalEdgeInsetsMake(0, 16 + depth * 16, 0, 8);
    cell.contentConfiguration = cfg;
  } else {
    cell.textLabel.text = text;
    cell.indentationLevel = depth;
    cell.indentationWidth = 16;
  }
  // Disclosure chevron on parent rows (SF Symbol, collapsed vs expanded). Leaf
  // rows have no accessory. The chevron is a non-interactive image (the row tap
  // toggles), so an accessoryView keeps the whole row tappable.
  if (has_kids) {
    if (@available(iOS 13.0, *)) {
      NSString* sym = expanded ? @"chevron.down" : @"chevron.right";
      UIImage* img = [UIImage systemImageNamed:sym];
      UIImageView* iv = [[UIImageView alloc] initWithImage:img];
      iv.tintColor = UIColor.secondaryLabelColor;
      [iv sizeToFit];
      cell.accessoryView = iv;
    } else {
      cell.accessoryView = nil;
    }
  } else {
    cell.accessoryView = nil;
  }
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath
{
  ios_host::Session* sess = nullptr;
  auto* wd = ios_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  if (indexPath.row < 0 || (size_t)indexPath.row >= wd->tree_vis_rows.size()) return;
  const auto vr = wd->tree_vis_rows[(size_t)indexPath.row];
  uint32_t id_v = vr.tree_id;

  // Parent row: toggle expand/collapse, rebuild the visible model, reload.
  // (Tapping the chevron or the row both reach here.)
  if (vr.has_kids) {
    auto it = wd->tree_items.find(id_v);
    if (it != wd->tree_items.end()) {
      it->second.expanded = !it->second.expanded;
      ios_host::tree_rebuild_visible_rows_ios(*wd);
      [tableView reloadData];
    }
  }

  // Activation: a tap on an already-selected leaf re-activates it (touch
  // analogue of the desktop double-click -> TREE_ITEM_ACTIVATED).
  bool reselect_leaf = !vr.has_kids && wd->selected_tree_item == id_v;
  wd->selected_tree_item = id_v;
  if (!wd->emit_events) {
    [tableView deselectRowAtIndexPath:indexPath animated:NO];
    return;
  }

  neui_event_t sel = {};
  sel.type             = NEUI_EVENT_TREE_ITEM_SELECTED;
  sel.data.tree.widget = { wd->widget_id };
  sel.data.tree.item   = { id_v };
  sess->dispatch_event(&sel);

  if (reselect_leaf) {
    neui_event_t act = {};
    act.type             = NEUI_EVENT_TREE_ITEM_ACTIVATED;
    act.data.tree.widget = { wd->widget_id };
    act.data.tree.item   = { id_v };
    sess->dispatch_event(&act);
  }
  // Keep the row highlighted (selection state), but a parent toggle reloaded
  // the table, so re-resolve nothing here - the model selection drives repaint.
  if (vr.has_kids)
    [tableView deselectRowAtIndexPath:indexPath animated:NO];
}

@end

namespace ios_host
{
  static NEUINativeIOSPaintedView* create_painted_view(WidgetData& wd)
  {
    NEUINativeIOSPaintedView* v = [[NEUINativeIOSPaintedView alloc]
                          initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)];
    v->widget_id = wd.widget_id;
    v.backgroundColor = UIColor.clearColor;
    v.clipsToBounds = YES;
    v.contentScaleFactor = UIScreen.mainScreen.scale;
    auto* backend = neui_cg_backend::get_backend();
    if (backend)
      v->render_ctx = backend->create_context((__bridge void*)v,
                                              (uint32_t)wd.width, (uint32_t)wd.height);
    // The paint pass (paint_widget_into_ctx) reads wd.render_ctx; keep the
    // WidgetData field in sync with the view ivar so begin_frame / fill_rect
    // target the bound context (a null wd.render_ctx silently no-ops every
    // backend draw).
    wd.render_ctx = v->render_ctx;
    // KNOB: long-press "Reset to default" (the touch analogue of the desktop
    // right-click reset popup).
    if (wd.type && !strcmp(wd.type, NEUI_W_KNOB)) {
      if (@available(iOS 13.0, *)) [v installKnobResetMenu];
    }
    // GRID: install the touch-scroll pan recognizer (deferred-tap + kinetics).
    if (wd.type && !strcmp(wd.type, NEUI_W_GRID))
      [v installGridScroll];
    return v;
  }

  // Dynamic-Type-aware font for a native control (UILabel / UIButton /
  // UITextField / UITextView). By default UIKit gives these the FIXED system
  // font (not Dynamic-Type-scaled), so without this they stay one size while the
  // user's Larger-Text / accessibility setting changes everything else. We hand
  // them the preferred BODY font and (in apply_native_font_ios below) set
  // adjustsFontForContentSizeCategory = YES so UIKit auto-rescales them live on a
  // content-size change - no manual font reset needed when the category flips.
  //
  // An explicit client NEUI_ATTR_FONT_SIZE still wins (Dynamic Type scales the
  // DEFAULT, which the client can override): we scale the client point-size
  // through UIFontMetrics so an explicit size ALSO follows Dynamic Type, keeping
  // it proportional to the body metric. NEUI_ATTR_FONT_FAMILY / _WEIGHT, when
  // set, pick the base font that metric then scales.
  static UIFont* ios_dynamic_control_font(WidgetData& wd)
  {
    const neui_detail::AttrBag* bag = wd.attrs.get();
    // No font attrs: the plain preferred body font (the common case).
    if (!bag ||
        (!bag->has(NEUI_ATTR_FONT_SIZE) && !bag->get_string(NEUI_ATTR_FONT_FAMILY) &&
         bag->get_int(NEUI_ATTR_FONT_WEIGHT, 0) == 0))
      return [UIFont preferredFontForTextStyle:UIFontTextStyleBody];

    // Build the base font from the explicit family / size / weight, then scale
    // it for the current content-size category via UIFontMetrics so the
    // explicit size still follows Dynamic Type.
    float size = bag->has(NEUI_ATTR_FONT_SIZE)
                   ? bag->get_float(NEUI_ATTR_FONT_SIZE, (float)[UIFont systemFontSize])
                   : (float)[UIFont systemFontSize];
    if (size <= 0.0f) size = (float)[UIFont systemFontSize];
    int weight = bag->get_int(NEUI_ATTR_FONT_WEIGHT, 0);

    UIFont* base = nil;
    if (const char* fam = bag->get_string(NEUI_ATTR_FONT_FAMILY); fam && fam[0]) {
      NSString* family = [NSString stringWithUTF8String:fam];
      base = family ? [UIFont fontWithName:family size:size] : nil;
    }
    if (!base) {
      // CSS weight 100..900 -> UIFontWeight (700+ Bold), matching the painted
      // backends' weight mapping; unknown family falls through to here too.
      UIFontWeight w = (weight >= 700) ? UIFontWeightBold : UIFontWeightRegular;
      base = [UIFont systemFontOfSize:size weight:w];
    }
    if (@available(iOS 11.0, *)) {
      // Scale the explicit size off the body metric so it grows / shrinks with
      // the content-size category like the default does.
      UIFontMetrics* m = [UIFontMetrics metricsForTextStyle:UIFontTextStyleBody];
      return [m scaledFontForFont:base];
    }
    return base;
  }

  // Apply the Dynamic-Type font to whichever native control kind `wd` realized,
  // and turn on UIKit's live auto-rescale. Safe to call on any native control;
  // a no-op for kinds without a settable text font (UISwitch, UISlider,
  // UIImageView, painted views). UITableView cells use UIListContentConfiguration
  // which is already Dynamic-Type-aware, so the tables self-scale.
  void apply_native_font_ios(WidgetData& wd)
  {
    if (!wd.native_control) return;
    UIView* v = (__bridge UIView*)wd.native_control;
    UIFont* f = ios_dynamic_control_font(wd);
    if ([v isKindOfClass:[UILabel class]]) {
      UILabel* l = (UILabel*)v;
      l.font = f;
      l.adjustsFontForContentSizeCategory = YES;
    } else if ([v isKindOfClass:[UITextField class]]) {
      UITextField* tf = (UITextField*)v;
      tf.font = f;
      tf.adjustsFontForContentSizeCategory = YES;
    } else if ([v isKindOfClass:[UITextView class]]) {
      UITextView* tv = (UITextView*)v;
      tv.font = f;
      tv.adjustsFontForContentSizeCategory = YES;
    } else if ([v isKindOfClass:[UIButton class]]) {
      UIButton* b = (UIButton*)v;
      b.titleLabel.font = f;
      b.titleLabel.adjustsFontForContentSizeCategory = YES;
      // COMBOBOX hosts a left-pinned title UILabel subview (NOT the button's own
      // titleLabel); scale it too so the collapsed bar follows Dynamic Type.
      UILabel* lbl = (UILabel*)[b viewWithTag:kComboTitleLabelTag];
      if ([lbl isKindOfClass:[UILabel class]]) {
        lbl.font = f;
        lbl.adjustsFontForContentSizeCategory = YES;
      }
    } else if ([v isKindOfClass:[NEUIIOSCheckboxSwitchView class]]) {
      // SWITCH-style CHECKBOX: scale its left label (the UISwitch has no text).
      NEUIIOSCheckboxSwitchView* cv = (NEUIIOSCheckboxSwitchView*)v;
      cv.theLabel.font = f;
      cv.theLabel.adjustsFontForContentSizeCategory = YES;
    }
  }

  // Create the native UIKit view for a child widget, add it to `container`.
  static void create_native_for_widget(Session* s, WidgetData& wd, UIView* container)
  {
    if (!container || wd.native_control || !wd.type) return;
    NEUINativeIOSControlTarget* tgt = [NEUINativeIOSControlTarget shared];
    NSInteger tag = (NSInteger)wd.widget_id;
    UIView* created = nil;

    if (!strcmp(wd.type, NEUI_W_LABEL)) {
      UILabel* l = [[UILabel alloc] initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)];
      l.text = nsstr(wd.text.c_str());
      l.textColor = UIColor.labelColor;
      created = l;
    }
    else if (!strcmp(wd.type, NEUI_W_BUTTON)) {
      UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
      b.frame = CGRectMake(wd.x, wd.y, wd.width, wd.height);
      [b setTitle:nsstr(wd.text.c_str()) forState:UIControlStateNormal];
      b.layer.borderWidth = 1.0;
      b.layer.borderColor = UIColor.separatorColor.CGColor;
      b.layer.cornerRadius = 6.0;
      b.tag = tag;
      [b addTarget:tgt action:@selector(buttonTapped:) forControlEvents:UIControlEventTouchUpInside];
      created = b;
    }
    else if (!strcmp(wd.type, NEUI_W_INPUTBOX)) {
      UITextField* tf = [[UITextField alloc] initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)];
      tf.text = nsstr(wd.text.c_str());
      tf.borderStyle = UITextBorderStyleRoundedRect;
      tf.tag = tag;
      [tf addTarget:tgt action:@selector(textChanged:) forControlEvents:UIControlEventEditingChanged];
      created = tf;
    }
    else if (!strcmp(wd.type, NEUI_W_MULTILINE)) {
      UITextView* tv = [[UITextView alloc] initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)];
      tv.text = nsstr(wd.text.c_str());
      tv.layer.borderWidth = 1.0;
      tv.layer.borderColor = UIColor.separatorColor.CGColor;
      tv.layer.cornerRadius = 4.0;
      created = tv;
    }
    else if (!strcmp(wd.type, NEUI_W_CHECKBOX) || !strcmp(wd.type, NEUI_W_CHECKBOX3)) {
      bool tristate = !strcmp(wd.type, NEUI_W_CHECKBOX3) ||
                      (wd.attrs && wd.attrs->get_int(NEUI_ATTR_TRISTATE, 0) != 0);
      // Style is a session-level knob read ONCE here at creation. CHECKBOX3
      // (tri-state) ALWAYS uses the square-glyph control - a UISwitch has no
      // indeterminate state - so it ignores the session style entirely.
      int style = NEUI_IOS_CHECKBOX_SWITCH;
      if (!tristate && s->_session_attrs)
        style = s->_session_attrs->get_int(NEUI_IOS_CHECKBOX_STYLE, NEUI_IOS_CHECKBOX_SWITCH);
      int st = wd.attrs ? wd.attrs->get_int("neui.ioshost.checkstate", NEUI_CHECK_UNCHECKED)
                        : NEUI_CHECK_UNCHECKED;

      if (!tristate && style == NEUI_IOS_CHECKBOX_SWITCH) {
        // SWITCH style (default for a 2-state CHECKBOX): a real green UISwitch
        // pinned to the right + a UILabel filling the left, iOS-Settings-style.
        // The switch auto-toggles; checkboxSwitchChanged: mirrors its on/off
        // into the host state + fires CHECKBOX_CHANGED.
        NEUIIOSCheckboxSwitchView* cv =
            [[NEUIIOSCheckboxSwitchView alloc]
                initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)];
        cv.theLabel.text = nsstr(wd.text.c_str());
        cv.theSwitch.on  = (st == NEUI_CHECK_CHECKED);
        cv.theSwitch.tag = tag;
        [cv.theSwitch addTarget:tgt action:@selector(checkboxSwitchChanged:)
               forControlEvents:UIControlEventValueChanged];
        created = cv;
      } else {
        // GLYPH style (opt-in for 2-state, mandatory for CHECKBOX3): a
        // borderless UIButton driving an SF Symbol glyph (leading) + the label
        // text (trailing), with the host owning the state machine - mirror of
        // the macOS native checkbox. This shows BOTH the label and a real
        // three-state indeterminate glyph (a bare UISwitch could do neither).
        // UIButton does not auto-toggle here; checkboxTapped: advances + redraws.
        UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
        b.frame = CGRectMake(wd.x, wd.y, wd.width, wd.height);
        [b setTitle:nsstr(wd.text.c_str()) forState:UIControlStateNormal];
        [b setTitleColor:UIColor.labelColor forState:UIControlStateNormal];
        b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
        // The glyph sits leading, the label trailing, in the system button's
        // default image+title layout - no edge-inset tuning (titleEdgeInsets /
        // contentEdgeInsets are deprecated + ignored on iOS 15+; the default
        // spacing reads fine for a checkbox row).
        [b setImage:neui_detail::checkbox_image_for_state_ios(st) forState:UIControlStateNormal];
        b.tag = tag;
        [b addTarget:tgt action:@selector(checkboxTapped:) forControlEvents:UIControlEventTouchUpInside];
        created = b;
      }
    }
    else if (!strcmp(wd.type, NEUI_W_SLIDER)) {
      UISlider* sl = [[UISlider alloc] initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)];
      sl.minimumValue = 0; sl.maximumValue = 1;
      sl.value = wd.attrs ? wd.attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f;
      sl.tag = tag;
      [sl addTarget:tgt action:@selector(sliderChanged:) forControlEvents:UIControlEventValueChanged];
      created = sl;
    }
    else if (!strcmp(wd.type, NEUI_W_COMBOBOX)) {
      // COMBOBOX -> a UIButton pull-down (iOS idiom for the macOS NSPopUpButton):
      // bordered/rounded, title pinned to the LEFT edge, disclosure chevron
      // pinned to the RIGHT edge, with showsMenuAsPrimaryAction so a single tap
      // pops the UIMenu. The button is the tappable host only - the title +
      // chevron are SUBVIEWS so they can sit at opposite edges (a UIButton's own
      // image+title group together and can't be spread, and the deprecated
      // *EdgeInsets are off-limits on the iOS 15 deploy target). The menu items
      // + title text are built by combobox_rebuild_menu_ios (also re-run on every
      // items_api mutation + selection change). COMBO_* attrs are ignored - a
      // UIMenu auto-sizes its pop-out, like NSPopUpButton.
      UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
      b.frame = CGRectMake(wd.x, wd.y, wd.width, wd.height);
      b.layer.borderWidth  = 1.0;
      b.layer.borderColor  = UIColor.separatorColor.CGColor;
      b.layer.cornerRadius = 6.0;

      // Title label: left edge with a 10pt inset, leaving ~32pt of right inset so
      // the text never runs under the chevron. Flexible width+height keeps it
      // filling the button as the frame is re-set on resize (apply_geometry).
      UILabel* lbl = [[UILabel alloc] initWithFrame:
          CGRectMake(kComboLabelInset, 0,
                     b.bounds.size.width - kComboLabelInset - kComboChevronZone,
                     b.bounds.size.height)];
      lbl.tag             = kComboTitleLabelTag;
      lbl.textColor       = UIColor.labelColor;          // dynamic - tracks theme
      lbl.font            = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
      lbl.lineBreakMode   = NSLineBreakByTruncatingTail;
      lbl.autoresizingMask =
          UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
      lbl.userInteractionEnabled = NO;                   // taps fall through to btn
      [b addSubview:lbl];

      if (@available(iOS 13.0, *)) {
        // Trailing disclosure chevron, pinned to the right edge + vertically
        // centered, mirroring the NSPopUpButton pull-down look.
        UIImage* chev = [UIImage systemImageNamed:@"chevron.up.chevron.down"];
        if (chev) {
          UIImageView* cv = [[UIImageView alloc] initWithImage:chev];
          cv.tag         = kComboChevronTag;
          cv.contentMode = UIViewContentModeScaleAspectFit;
          cv.tintColor   = UIColor.secondaryLabelColor;  // dynamic - tracks theme
          CGFloat cs     = kComboChevronSize;
          cv.frame = CGRectMake(b.bounds.size.width - kComboLabelInset - cs,
                                (b.bounds.size.height - cs) * 0.5f, cs, cs);
          cv.autoresizingMask =
              UIViewAutoresizingFlexibleLeftMargin |
              UIViewAutoresizingFlexibleTopMargin  |
              UIViewAutoresizingFlexibleBottomMargin;
          cv.userInteractionEnabled = NO;                // taps fall through to btn
          [b addSubview:cv];
        }
      }
      if (@available(iOS 14.0, *)) b.showsMenuAsPrimaryAction = YES;
      b.tag = tag;
      [container addSubview:b];
      wd.native_control = (__bridge_retained void*)b;
      // Sync any pre-show items + selection into the menu + title now (items are
      // typically added before show, exactly like the LISTBOX seed path).
      combobox_rebuild_menu_ios(wd);
      apply_native_font_ios(wd);
      if (!wd.enabled) apply_enabled_native_ios(wd);
      return;
    }
    else if (!strcmp(wd.type, NEUI_W_LISTBOX)) {
      // LISTBOX -> a plain UITableView. UITableView owns scrolling / momentum /
      // cell-reuse / Dynamic-Type / VoiceOver natively, so no painted-scroll or
      // deferred-tap plumbing is needed (unlike the painted GRID). Rows mirror
      // wd.items via the data source; the source is kept alive on the table.
      UITableView* tv = [[UITableView alloc]
          initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)
                  style:UITableViewStylePlain];
      tv.layer.borderWidth = 1.0;
      tv.layer.borderColor = UIColor.separatorColor.CGColor;
      tv.layer.cornerRadius = 4.0;
      NEUIIOSListSource* src = [[NEUIIOSListSource alloc] init];
      src->widget_id = wd.widget_id;
      tv.dataSource = src;
      tv.delegate   = src;
      objc_setAssociatedObject(tv, "NEUIIOSListSource", src,
                               OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      // Reflect any pre-show selection (items are typically added before show).
      if (wd.selected_item != NEUI_ITEM_NONE && wd.selected_item < wd.items.size())
        [tv selectRowAtIndexPath:[NSIndexPath indexPathForRow:(NSInteger)wd.selected_item
                                                    inSection:0]
                        animated:NO
                  scrollPosition:UITableViewScrollPositionNone];
      created = tv;
    }
    else if (!strcmp(wd.type, NEUI_W_TREEVIEW)) {
      // TREEVIEW -> a UITableView over the flattened visible-row model
      // (tree_rebuild_visible_rows_ios). Tapping a parent toggles expand; tap
      // fires TREE_ITEM_SELECTED. UITableView owns scrolling natively.
      UITableView* tv = [[UITableView alloc]
          initWithFrame:CGRectMake(wd.x, wd.y, wd.width, wd.height)
                  style:UITableViewStylePlain];
      tv.layer.borderWidth = 1.0;
      tv.layer.borderColor = UIColor.separatorColor.CGColor;
      tv.layer.cornerRadius = 4.0;
      NEUIIOSTreeSource* src = [[NEUIIOSTreeSource alloc] init];
      src->widget_id = wd.widget_id;
      tv.dataSource = src;
      tv.delegate   = src;
      objc_setAssociatedObject(tv, "NEUIIOSTreeSource", src,
                               OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      // Build the visible-row model from any tree items added before show.
      tree_rebuild_visible_rows_ios(wd);
      created = tv;
    }
    else if (is_painted_type_ios(wd.type)) {
      NEUINativeIOSPaintedView* pv = create_painted_view(wd);
      created = pv;
      // SECTION / TABPAGE: clip children to the rect + (when scrolling or
      // chip-bearing) parent them into an inner body view. A TABPAGE is a
      // chip-less SECTION, so it reuses the same section machinery verbatim.
      if (!strcmp(wd.type, NEUI_W_SECTION) || !strcmp(wd.type, NEUI_W_TABPAGE)) {
        pv.clipsToBounds = YES;
        wd.native_control = (__bridge_retained void*)pv;
        [container addSubview:pv];
        section_refresh_scroll_state_ios(wd);
        section_ensure_body_view_ios(wd);
        if (!wd.enabled) apply_enabled_native_ios(wd);
        return;
      }
      // TABVIEW: clips its (TABPAGE) children to its bounds; the selected page
      // is positioned to the content body rect each paint. No inner body view -
      // pages parent directly to the tabview view.
      if (!strcmp(wd.type, NEUI_W_TABVIEW))
        pv.clipsToBounds = YES;
    }

    if (created) {
      [container addSubview:created];
      wd.native_control = (__bridge_retained void*)created;
      // Dynamic-Type font + live auto-rescale for the text-bearing controls
      // (LABEL / BUTTON / INPUTBOX / MULTILINE / glyph-CHECKBOX / SWITCH-CHECKBOX);
      // a no-op for SLIDER / IMAGE / painted views (no settable text font).
      apply_native_font_ios(wd);
      if (!wd.enabled) apply_enabled_native_ios(wd);
    }
  }

  static bool is_painted_type_ios(const char* type)
  {
    // LISTBOX / TREEVIEW are NATIVE UITableViews (see create_native_for_widget),
    // not painted views - so they are intentionally excluded here.
    return type && (!strcmp(type, NEUI_W_IMAGE) || !strcmp(type, NEUI_W_KNOB) ||
                    !strcmp(type, NEUI_W_CUSTOMDRAW) || !strcmp(type, NEUI_W_SECTION) ||
                    !strcmp(type, NEUI_W_GRID) || !strcmp(type, NEUI_W_TABVIEW) ||
                    !strcmp(type, NEUI_W_TABPAGE));
  }

  // Pick the container view a child should parent to (the section body view if
  // the parent is a section with a body container, else the frame content view
  // or the parent's native view).
  static UIView* parent_container_for(Session* s, uint32_t idx)
  {
    uint32_t p = s->_widgets.get_parent(idx);
    if (p == 0 || p == neui_detail::knone.id || !s->_widgets.exists(p)) return nil;
    auto& pw = s->_widgets[p];
    if (pw.isroot) {
      NEUINativeIOSContentView* cv = content_view_for_frame(pw);
      return cv;
    }
    // SECTION / TABPAGE children parent into the inner body view (chip band /
    // scroll clip); a chip-less, non-scrolling one has none, so the helper
    // falls back to the painted view itself. TABVIEW pages + other children
    // parent directly to the parent's view.
    if (pw.type && (!strcmp(pw.type, NEUI_W_SECTION) ||
                    !strcmp(pw.type, NEUI_W_TABPAGE)))
      return section_child_container_ios(pw);
    if (pw.native_control)
      return (__bridge UIView*)pw.native_control;
    return nil;
  }

  // Recursively realize a frame's descendants once its content view exists.
  static void create_descendants_native(Session* s, uint32_t parent_idx, UIView* container)
  {
    uint32_t c = s->_widgets.child(parent_idx);
    while (c != 0) {
      if (s->_widgets.exists(c)) {
        auto& cw = s->_widgets[c];
        // MENUBAR is not a view (drives the hamburger); skip native creation.
        if (cw.type && strcmp(cw.type, NEUI_W_MENUBAR) != 0) {
          create_native_for_widget(s, cw, container);
          // Recurse into the child's container: a SECTION / TABPAGE body view
          // (chip / scroll clip) or its own view (TABVIEW + everything else).
          UIView* sub = nil;
          if (cw.type && (!strcmp(cw.type, NEUI_W_SECTION) ||
                          !strcmp(cw.type, NEUI_W_TABPAGE)))
            sub = section_child_container_ios(cw);
          else if (cw.native_control)
            sub = (__bridge UIView*)cw.native_control;
          if (sub) create_descendants_native(s, c, sub);
        }
      }
      c = s->_widgets.next(c);
    }
  }

  void realize_widget_ios(Session* s, uint32_t idx)
  {
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];
    if (wd.isroot || wd.native_control) return;
    if (wd.type && !strcmp(wd.type, NEUI_W_MENUBAR)) return;
    UIView* container = parent_container_for(s, idx);
    if (!container) return;  // frame not shown yet
    create_native_for_widget(s, wd, container);
    // Re-layout the parent container so a post-show child lands correctly:
    // a SECTION / TABPAGE re-flows its body; a TABVIEW re-flows the chip strip
    // + re-sizes its pages (a new TABPAGE needs a tab + geometry even when it
    // isn't the selected one).
    uint32_t p = s->_widgets.get_parent(idx);
    if (p != 0 && p != neui_detail::knone.id && s->_widgets.exists(p)) {
      auto& pw = s->_widgets[p];
      if (pw.type && (!strcmp(pw.type, NEUI_W_SECTION) ||
                      !strcmp(pw.type, NEUI_W_TABPAGE)))
        section_apply_layout_changes_ios(pw);
      else if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW)) {
        tabview_apply_page_geometry_ios(pw);
        mark_widget_dirty_for_paint(pw);
      }
    }
  }

  void release_native_window_ios(WidgetData& wd)
  {
    if (!wd.native_window) return;
    auto* backend = neui_cg_backend::get_backend();
    if (backend && wd.render_ctx) {
      backend->destroy_context(wd.render_ctx);
      wd.render_ctx = nullptr;
    }
    UIWindow* w = (__bridge_transfer UIWindow*)wd.native_window;
    wd.native_window = nullptr;
    if (@available(iOS 13.0, *)) {
      UIViewController* vc = w.rootViewController;
      if (vc.presentingViewController)
        [vc.presentingViewController dismissViewControllerAnimated:NO completion:nil];
    }
    w.hidden = YES;
  }

  void release_native_control_ios(WidgetData& wd)
  {
    if (wd.section_body_view) {
      UIView* body = (__bridge_transfer UIView*)wd.section_body_view;
      [body removeFromSuperview];
      wd.section_body_view = nullptr;
    }
    if (!wd.native_control) return;
    UIView* v = (__bridge_transfer UIView*)wd.native_control;
    wd.native_control = nullptr;
    if ([v isKindOfClass:[NEUINativeIOSPaintedView class]]) {
      NEUINativeIOSPaintedView* pv = (NEUINativeIOSPaintedView*)v;
      auto* backend = neui_cg_backend::get_backend();
      if (pv->render_ctx) {
        if (backend) wd.session->_asset_manager.release_context(pv->render_ctx, backend);
        if (backend && backend->destroy_context) backend->destroy_context(pv->render_ctx);
        pv->render_ctx = nullptr;
      }
      wd.render_ctx = nullptr;  // mirrored from the view ivar in create_painted_view
    }
    [v removeFromSuperview];
  }

  // Session::widget_show - the platform gate. Frames build their UIWindow +
  // content view + descendants; child widgets realize lazily via
  // realize_widget_ios at create time, so show on a non-frame is a no-op
  // beyond making it visible.
  void Session::widget_show(neui_widget_t widget)
  {
    uint32_t idx = widget.id & 0xffff;
    if (!_widgets.exists(idx)) return;
    auto& wd = _widgets[idx];

    if (!wd.isroot) {
      wd.visible = true;
      if (wd.native_control) ((__bridge UIView*)wd.native_control).hidden = NO;
      return;
    }

    bool is_dialog = wd.type && !strcmp(wd.type, NEUI_W_DIALOG);

    if (@available(iOS 13.0, *)) {
      if (!wd.native_window) {
        UIWindowScene* scene = active_window_scene();
        UIWindow* window = scene ? [[UIWindow alloc] initWithWindowScene:scene]
                                 : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        NEUINativeIOSViewController* vc = [[NEUINativeIOSViewController alloc] init];
        vc->session = this;
        vc->widget_index = idx;
        window.rootViewController = vc;

        // Seed the frame size from the scene bounds (a UIWindow fills its
        // scene), so the client's layout sees the true device size.
        CGRect sb = scene ? scene.coordinateSpace.bounds : window.bounds;
        if (sb.size.width > 0 && sb.size.height > 0) {
          wd.width = (int)sb.size.width;
          wd.height = (int)sb.size.height;
        }

        NEUINativeIOSContentView* cv = [vc contentView];
        auto* backend = neui_cg_backend::get_backend();
        if (backend && cv) {
          wd.render_ctx = backend->create_context((__bridge void*)cv,
                                                  (uint32_t)wd.width, (uint32_t)wd.height);
          cv->render_ctx = wd.render_ctx;
        }
        CGFloat scale = scene ? scene.screen.scale : UIScreen.mainScreen.scale;
        if (scale <= 0) scale = 1.0;
        wd.dpi = (uint32_t)(96.0 * scale + 0.5);
        wd.native_window = (__bridge_retained void*)window;

        // Build descendants now that the content view exists.
        create_descendants_native(this, idx, cv);
        [cv refreshHamburger];

        if (is_dialog && wd.owner_index != 0 && _widgets.exists(wd.owner_index)) {
          auto& owner = _widgets[wd.owner_index];
          if (owner.native_window) {
            UIWindow* ow = (__bridge UIWindow*)owner.native_window;
            UIViewController* ovc = ow.rootViewController;
            if (ovc) {
              vc.modalPresentationStyle = UIModalPresentationFormSheet;
              [ovc presentViewController:vc animated:YES completion:nil];
            }
          }
        } else {
          [window makeKeyAndVisible];
        }
      } else {
        UIWindow* w = (__bridge UIWindow*)wd.native_window;
        [w makeKeyAndVisible];
      }
    }
  }

} // namespace ios_host

// ---------------------------------------------------------------------------
// App-level menu-bar contribution hooks (iPad system menu bar).
//
// WHY app-level: UIKit builds the MAIN menu bar (and the press-and-hold-⌘ HUD)
// from the responder chain. A UIView's -buildMenuWithBuilder: is only consulted
// when that view is in the ACTIVE responder chain, which is unreliable on a
// freshly-launched app with no first responder - so the old content-view
// contribution never reached the bar on a real iPad. UIApplication is ALWAYS in
// the responder chain, so an app-level -buildMenuWithBuilder: (NEUIApplication,
// hosts/ios/application_ios.mm) is the authoritative contribution point.
//
// These two C hooks are the seam NEUIApplication calls into the native host (it
// lives in the application TU / neui-ioshost but must not drag host.h ObjC++
// into the application class). They are also weak-linkable so the application
// subclass can call the parallel xpl hooks when the xpl host is the one linked
// (see hosts/crossplatform/platform_ios.mm). Built from the SAME menu model +
// adapter as the hamburger so the two surfaces stay in sync.

// Build + insert the frontmost native frame's top-level menus into `builder`.
// `key_cmd_target` / `key_cmd_sel` receive shortcut-bearing leaves as
// UIKeyCommands so the bar shows + fires the equivalent from a hardware
// keyboard. Returns the number of top-level menus inserted (0 = nothing to
// contribute). Emits the headless orchestrator signal in a diagnostic build
// (NEUI_IOS_MENU_DIAG).
extern "C" unsigned long neui_ios_native_build_menubar_menus(
    id<UIMenuBuilder> builder, id key_cmd_target, SEL key_cmd_sel)
{
  if (@available(iOS 13.0, *)) {
    bool avail = neui_detail::menu_ios_system_menubar_available();
    ios_host::Session* sess = nullptr;
    ios_host::WidgetData* frame = nullptr;
    ios_host::WidgetData* mb =
        avail ? ios_host::frontmost_menubar_ios(&sess, &frame) : nullptr;
    unsigned long merged = 0, tops_n = 0;
    if (avail && mb && sess) {
      ios_host::Session* s = sess;
      uint32_t mb_widget_id = mb->widget_id;
      auto pick = [s, mb_widget_id](uint32_t item_id) {
        if (!s) return;
        uint32_t i = mb_widget_id & 0xffff;
        if (!s->_widgets.exists(i)) return;
        ios_host::dispatch_menu_item_ios(s, s->_widgets[i], item_id);
      };
      ios_host::IosMenubarAdapter adapter = ios_host::build_menubar_adapter_ios(*mb);
      // Merge popups matching a standard menu (File/Edit/View/...) into it;
      // insert non-matching ones as new top-level menus - avoids duplicating
      // UIKit's pre-created empty standard menus. Shared with the xpl host.
      neui_detail::menu_ios_contribute_menubar(
          builder, adapter, mb_widget_id, pick, key_cmd_target, key_cmd_sel,
          &merged, &tops_n);
    }
    // Diagnostic: prove the app-level override fires + the merge split. Captured
    // by `devicectl --console` / `simctl launch --console` (diagnostic build
    // only - see NEUI_IOS_MENU_DIAG).
    NEUI_MENU_DIAG("[neui-menu] buildMenuWithBuilder: avail=%d menubar=%d merged=%lu top=%lu\n",
                   avail ? 1 : 0, mb ? 1 : 0, merged, tops_n);
    return merged + tops_n;
  }
  return 0;
}

// Route a hardware-keyboard menu accelerator (a UIKeyCommand built by the hook
// above) to the frontmost native frame's MENUBAR. Returns true if it matched.
extern "C" bool neui_ios_native_menu_key_command(UIKeyCommand* cmd)
{
  uint32_t key  = neui_detail::ios_input_to_neui_key(cmd.input);
  uint32_t mods = neui_detail::ios_modifiers_to_neui(cmd.modifierFlags);
  if (key == NEUI_KEY_NONE) return false;
  ios_host::Session* sess = nullptr;
  ios_host::WidgetData* frame = nullptr;
  ios_host::WidgetData* mb = ios_host::frontmost_menubar_ios(&sess, &frame);
  if (!mb || !sess || !frame) return false;
  return ios_host::try_menubar_accel_ios(sess, frame->index, key, mods);
}
