#pragma once

#include <neui/neui.h>
#include "../shared/cursor_kind.h"
#include "../shared/relative_pointer.h"
#include "../shared/tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/edit_history.h"
#include "../shared/theme_palette.h"
#include "../shared/timer_table.h"
#include "../shared/behavior_runtime.h"
#include "../shared/grid_model.h"
#include "../shared/widget_section_scroll.h"
#include "../shared/widget_tabview.h"
#include "asset_manager.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstring>

#define NEUI_HOST_CROSSPLATFORM "neui.host.crossplatform"

namespace xpl_host
{
  class Session;

  // Defined per platform in platform_*.cpp; declared here (rather than pulling
  // in platform.h, which itself depends on this header) so WidgetData::ui_scale
  // can gate the zoom on platforms that divide input by it.
  bool platform_supports_ui_scale();

  // ---- Painted row metrics -------------------------------------------------
  // The row pitches the LISTBOX / COMBOBOX / TREEVIEW paint code lays out with,
  // and the in-frame menubar band height. Defined in host.cpp beside that paint
  // code; declared here so the accessibility adapter (a11y_adapter.cpp) reports
  // the same row rectangles the paint walk actually drew instead of keeping a
  // second copy of the numbers, which would drift the first time either is
  // touched. All logical px, already through scaled_painted_metric.
  int list_row_height();
  int tree_row_height();
  int menubar_band_height();
  int scrollbar_gutter_width();

  // -------------------------------------------------------------------------
  // Base widget class - holds all common fields and the virtual interface.
  // Derived classes add type-specific fields and override virtual methods.
  // The class is intentionally kept named WidgetData so that platform.h
  // (which forward-declares it) requires no changes.
  class WidgetData
  {
  public:
    uint32_t    index      = 0;
    const char* type       = nullptr;
    int x = 0, y = 0, width = 0, height = 0;
    // Frame-local absolute position, recomputed top-down each paint by
    // paint_widgets_recursive. Used for hit-test and any non-paint site
    // that needs the widget's position in frame coordinates (event mouse
    // coords are frame-local, so widget-local conversion subtracts these).
    // Valid after the first paint; before that they read 0 and any hit-
    // test happens to land on the frame's top-left, which is acceptable
    // because input cannot arrive before the window is first painted.
    int abs_x = 0, abs_y = 0;
    bool isroot      = false;
    bool visible     = false;
    bool emit_events = false;
    bool tab_stop    = false;   // if true, TAB / SHIFT+TAB can move focus here
    // True = receives input + paints normally; false = paints dimmed
    // (push_alpha(0.5) bracket around paint) and is skipped by widget_at
    // hit-test + focus_next tab traversal. Default true.
    bool enabled     = true;
    // Mouse-state flags consumed by compound layer state filters
    // (NEUI_LAYER_STATE_HOVERED / _PRESSED). hovered = cursor is currently
    // inside this widget's bounds (last set_hovered target). pressed =
    // a mouse button went down on this widget and hasn't yet released
    // (capture-style: stays true while cursor moves out, clears on UP).
    bool hovered     = false;
    bool pressed     = false;
    // True = this widget accepts drag&drop drops. Default false. Set via
    // NEUI_API_DND set_drop_target. Independent of `enabled` - a disabled
    // widget is skipped during drag hit-testing the same way it is during
    // mouse hit-testing.
    bool drop_target = false;
    // Optional MIME allow-list for drop hit-testing. Empty = "accept any".
    // Strings owned per-widget (copied at set_accepted_formats time).
    std::vector<std::string> accepted_mimes;
    void*    userdata  = nullptr;
    uint32_t widget_id = 0;
    std::string text;

    // Native window handle (APPWINDOW / PLUGWINDOW / DIALOG).
    void* native_handle = nullptr;
    // DAW embedding: when non-zero, a PLUGWINDOW is created inside this
    // foreign (DAW-provided) native parent instead of as its own top-level.
    // Per-platform meaning: Win32 = parent HWND (child is WS_CHILD); macOS =
    // parent NSView* (the frame roots in an NEUIView subview, no NSWindow);
    // Linux/X11 = parent Window id (child window over a dedicated Display
    // connection, no neui-owned event loop). Set via platform_set_embed_parent
    // / the public NEUI_API_EMBED set_parent before widget_show. 0 =
    // standalone top-level.
    uintptr_t embed_parent = 0;
    // Owned native icon (HICON on Win32) installed on the frame's HWND.
    // Tracked here so it can be DestroyIcon'd on replace / widget destroy.
    void* native_icon   = nullptr;
    neui_render_ctx_t render_ctx = nullptr;
    uint32_t dpi = 96;
    // FRAMES only: set the first time paint_frame runs for this frame. Read by
    // Session::ensure_abs_positions to tell whether the SECTION / TABVIEW body
    // layout caches (which only the paint path computes) exist yet.
    bool painted_once = false;
    // FRAMES only: the widget tree under this frame changed since the last
    // paint, so a cached SECTION / TABVIEW body rect may be missing or stale.
    // Set by Session::mark_layout_dirty from the structural mutations; cleared
    // by paint_frame. `painted_once` alone is not enough - a section created
    // AFTER the last paint has an empty cache in a frame that has painted.
    bool layout_dirty = false;

    // ---- Zoom (NEUI_ATTR_UI_SCALE) ------------------------------------------
    // The user zoom for a FRAME, clamped, 1.0 when unset. Read live from the
    // AttrBag rather than cached so a set_float takes effect immediately and
    // there is no second copy to keep in sync.
    //
    // Coordinate contract: x/y/width/height and everything else on this
    // struct stay LOGICAL px at 96 DPI at every zoom - the zoom only exists
    // (a) as a renderer transform during paint and (b) as a multiply when
    // converting to/from native/physical units in the platform layer. Keep it
    // out of the logical numbers or the menubar inset, hit-test rects and
    // client rect all fall out of sync.
    float ui_scale() const
    {
      // Inert on a platform layer that does not divide input by the zoom (iOS,
      // null): scaling paint without scaling hit-testing is worse than not
      // zooming, so the attr does nothing there rather than half-working.
      if (!platform_supports_ui_scale()) return 1.0f;
      float z = attrs ? attrs->get_float(NEUI_ATTR_UI_SCALE, 1.0f) : 1.0f;
      if (!(z > 0.0f)) return 1.0f;              // 0 / negative / NaN -> off
      if (z < NEUI_UI_SCALE_MIN) return NEUI_UI_SCALE_MIN;
      if (z > NEUI_UI_SCALE_MAX) return NEUI_UI_SCALE_MAX;
      return z;
    }

    // Physical pixels per logical pixel for this frame: the monitor's DPI
    // ratio times the user zoom. THE conversion constant for the platform
    // layer - every logical->native multiply and native->logical divide
    // should go through this rather than open-coding dpi/96.
    float logical_to_physical() const
    {
      return (static_cast<float>(dpi) / 96.0f) * ui_scale();
    }

    // For DIALOG frames: tree index of the owner frame (APPWINDOW or DIALOG).
    // Set via widgets->set_owner before show. 0 means no owner. While the
    // dialog is shown, the owner is input-blocked.
    uint32_t owner_index = 0;

    // Back-pointer to owning session (set at creation time).
    Session* session    = nullptr;
    uint32_t session_id = 0;

    // Per-widget attribute bag (lazy-allocated).
    std::unique_ptr<neui_detail::AttrBag> attrs;

    virtual ~WidgetData() = default;

    // Paint this widget into the given render context.
    // Base implementation: fill_rect + draw_text + focus outline (draw_rect).
    virtual void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                       bool is_focused);

    // Optional second paint pass invoked by paint_widgets_recursive AFTER
    // descending into this widget's children. Used by CUSTOMDRAW + compound
    // to paint z>=0 layers above the child-widget pass. Default no-op.
    virtual void paint_after_children(neui_render_backend_t* /*backend*/,
                                       neui_render_ctx_t /*ctx*/,
                                       bool /*is_focused*/) {}

    // Return true if the key event was consumed (do not forward to client).
    // Called for the currently focused widget.
    virtual bool on_keydown(uint32_t keycode, uint32_t modifiers) { return false; }
    virtual bool on_keychar(uint32_t codepoint, uint32_t modifiers) { return false; }

    // Called when a mouse event was not consumed by the client.
    // event->data.mouse is already populated. Return true if handled.
    virtual bool on_mouse_event(neui_event_t* event) { return false; }

    // Returns true if (px, py) is inside this widget's interactive area.
    // (px, py) are frame-local logical pixels (the same space as mouse
    // event coords). Default: full bounding rect using the cached absolute
    // position. Override to restrict (e.g. ComboBox top bar only).
    virtual bool hit_test(float px, float py) const {
      return px >= static_cast<float>(abs_x) && px < static_cast<float>(abs_x + width) &&
             py >= static_cast<float>(abs_y) && py < static_cast<float>(abs_y + height);
    }

    // Called before this widget is removed from the tree.
    // Override to release type-specific platform resources (menus, etc.).
    virtual void on_destroy(Session* s) {}

    // Called when this widget's logical focus state changes, BEFORE the
    // corresponding NEUI_EVENT_WIDGET_FOCUS reaches the client. Default
    // no-op. Override to flush transient state that depends on focus -
    // e.g. the GRID's in-place cell editor commits on focus loss so Tab
    // doesn't leave a stale editor open over a non-focused widget.
    virtual void on_focus_change(bool /*gained*/) {}

    // Type classification - avoids strcmp in hot paths.
    virtual bool is_frame()   const { return false; }
    // "This widget IS its containing frame's menu bar." True for NEUI_W_MENUBAR
    // ONLY. Everything that makes a widget a menu BAR keys off this: the native
    // HMENU / NSMenu created at widget_create, SetMenu / [NSApp setMainMenu:] at
    // widget_show, the reserved in-frame band on Linux, frame_menubar_index, and
    // registration in Session::_menubars (accelerator translation).
    virtual bool is_menubar() const { return false; }
    // "This widget stores the MenubarWidget menu-item model." True for both
    // NEUI_W_MENUBAR and NEUI_W_POPUPMENU. The distinction matters: the tree API
    // (add / remove / clear / set_shortcut / set_checked / set_menu_cmd /
    // set_enabled) dynamic_cast<MenubarWidget&> on the strength of this
    // predicate, and the layout / paint / hit-test / tab walks must skip both -
    // but only a real menu bar may be attached to a frame as its menu.
    // Splitting these two is not cosmetic: a POPUPMENU that answers yes to
    // is_menubar() replaces the frame's actual menu bar on show().
    virtual bool is_menu_model() const { return false; }
    // Editable text surfaces (INPUTBOX / MULTILINE). Lets the platform layer
    // target X11 middle-click PRIMARY paste at the right widget. insert_text
    // inserts UTF-8 at the caret (replacing any selection); returns true if it
    // consumed (false = not editable / readonly / empty).
    virtual bool is_text_input() const { return false; }
    virtual bool insert_text(const std::string& /*utf8*/) { return false; }
    // GRID widgets expose their scroll model so the platform layer can drive
    // pixel-precise smooth scrolling / rubber-band (macOS). nullptr otherwise.
    virtual neui_detail::GridModel* grid_model_ptr() { return nullptr; }
    // The widget's attached behavior asset, if any (CUSTOMDRAW). Lets the
    // DnD drag-source resolution (iOS) check for a DRAG_SOURCE handler without
    // downcasting. asset_none for every other widget type.
    virtual neui_asset_t behavior_asset_id() const { return asset_none; }
    // Scrolling SECTION exposes its scroll state so the paint walk +
    // hit-tester can offset + clip child positioning. nullptr otherwise
    // (non-scrolling SECTION, or any non-SECTION widget).
    virtual neui_detail::SectionScrollState* scroll_state_ptr() { return nullptr; }

    // True while this widget is mid scrollbar-thumb drag (a BUTTON_DOWN landed
    // on the thumb and the drag rides the MOUSE_MOVE path). Default false;
    // widgets with an internal scrollbar (LISTBOX / TREEVIEW / GRID / scrolling
    // SECTION) override to expose their `sb_dragging` flag. Used by the iOS
    // touch-pan recognizer to stay inert when a touch is already driving the
    // thumb directly - so a thumb drag is not also turned into a kinetic pan.
    virtual bool scrollbar_dragging() const { return false; }

    // Fire NEUI_EVENT_SCROLL_CHANGED if the widget's scroll position
    // changed since the last notification. SECTION overrides; other
    // widgets are inert. Called from every commit path (wheel, scrollbar
    // drag, spring-back tick, programmatic scroll API) so the platform
    // layers don't have to downcast.
    virtual void notify_scroll_changed() {}
    // Cached layout from the last SECTION paint - widget-local body rect +
    // scrollbar visibility. Returns non-null for ANY SECTION (scrolling
    // or not) so the paint walk can apply the body_y offset uniformly
    // (children's coords are body-relative, so chip "none" auto-expands
    // the visible area into the former chip band). Reads as zeros until
    // the first paint.
    virtual const neui_detail::SectionLayout* section_layout_ptr() const { return nullptr; }
    bool is_dialog() const { return type && !strcmp(type, NEUI_W_DIALOG); }

    // Try to perform a built-in command (neui_command_t) on this widget.
    // Returns true if the widget consumed/performed it. Default: false.
    // Text widgets override to handle UNDO/REDO/CUT/COPY/PASTE/SELECT_ALL/DELETE.
    virtual bool perform_command(uint32_t cmd) { (void)cmd; return false; }

    // Non-mutating peer to perform_command. Returns true if this widget
    // would handle `cmd` if asked. Used by WM_INITMENUPOPUP to gray
    // menu items whose bound command can't reach a consumer right now.
    // Default returns false; widgets that override perform_command
    // typically also override this with the same supported-cmd switch.
    virtual bool can_perform_command(uint32_t cmd) const {
      (void)cmd; return false;
    }

    // IME composition pipeline. The platform layer (e.g. WM_IME_* on Win32,
    // NSTextInputClient on macOS later) translates native composition events
    // into these calls on the focused widget. utf8 / byte_len carry the
    // composition or result string in UTF-8; caret_byte is the byte offset
    // of the IME-reported caret inside utf8 (UPDATE only). per_byte_attrs,
    // if non-null on UPDATE, is an array of byte_len attribute bytes (one
    // per UTF-8 byte) drawn from the CompAttr values below; widgets use it
    // to render per-clause coloured / thicker underlines. Default returns
    // false; only InputBox / Multiline override.
    enum CompKind { COMP_START = 0, COMP_UPDATE = 1, COMP_RESULT = 2, COMP_END = 3 };
    enum CompAttr {
      COMP_ATTR_INPUT               = 0,  // currently being typed (no conversion yet)
      COMP_ATTR_TARGET_CONVERTED    = 1,  // selected segment under conversion (highlight)
      COMP_ATTR_CONVERTED           = 2,  // already-converted segment
      COMP_ATTR_TARGET_NOTCONVERTED = 3,  // selected but not yet converted
      COMP_ATTR_INPUT_ERROR         = 4,  // invalid input
    };
    virtual bool on_composition(int kind, const char* utf8,
                                int byte_len, int caret_byte,
                                const uint8_t* per_byte_attrs) {
      (void)kind; (void)utf8; (void)byte_len; (void)caret_byte; (void)per_byte_attrs;
      return false;
    }

    // Caret rect in widget-local logical pixels (origin at widget's top-left).
    // Returns false if the widget has no caret to report (default). Used by
    // the platform layer to position the IME candidate window. The render
    // ctx is supplied so widgets can call backend->measure_text if needed.
    virtual bool caret_rect_local(neui_render_backend_t* backend,
                                  neui_render_ctx_t ctx,
                                  float* out_x, float* out_y, float* out_h) {
      (void)backend; (void)ctx; (void)out_x; (void)out_y; (void)out_h;
      return false;
    }
  protected:
    void repaint();
  };

  // -------------------------------------------------------------------------
  // Derived widget classes

  // Transient "toast" overlay attached to a top-level frame. Owned by
  // FrameWidget and painted on top of all other content during the
  // frame's paint pass. Animation: fly-in from above + fade-in, hold,
  // fade-out + slide-up. start_ms is a monotonic timestamp captured when
  // the toast was created (milliseconds since session start). The
  // platform layer drives a 16 ms repaint heartbeat while `active`.
  struct ToastState {
    bool        active        = false;
    std::string text;
    int         width         = 0;   // logical px, including padding
    int         height        = 0;
    int         line_h        = 0;   // line height used for the slide gap
    uint64_t    start_ms      = 0;   // monotonic ms at fly-in t=0
    // Cached duration constants - so the platform timer tick can read
    // total lifetime without recomputing the phase math.
    uint32_t    fade_in_ms    = 200;
    uint32_t    hold_ms       = 2000;
    uint32_t    fade_out_ms   = 400;
  };

  // APPWINDOW / PLUGWINDOW - owns native_handle, render_ctx, dpi (in base)
  class FrameWidget : public WidgetData {
  public:
    bool is_frame() const override { return true; }
    // True while a modal DIALOG is blocking inside platform_run_modal_until.
    // Set in widget_show; cleared by the destroy path so the pump exits.
    //
    // Held INDIRECTLY, and that is load-bearing rather than a style choice: the
    // pump polls this flag for as long as the dialog is up, while the documented
    // way OUT of a modal dialog is for a client callback to destroy the dialog
    // WIDGET - which frees this FrameWidget with the pump still polling. A plain
    // member meant the poll read freed memory, and whether the loop then exited
    // came down to what the allocator happened to leave in the byte: it survived
    // by luck on the client-destroy path, and hung outright when the destroy came
    // from a callback dispatched during the dialog's own WM_DESTROY. widget_show
    // keeps a share alive across the pump, so the poll always sees the value
    // end_modal actually wrote.
    std::shared_ptr<bool> modal_pump_flag = std::make_shared<bool>(false);
    bool modal_pump_active() const { return modal_pump_flag && *modal_pump_flag; }
    // For a modal DIALOG: the widget that held focus when the dialog was shown,
    // restored when it closes. Focus is session-global and key routing goes by
    // it rather than by which window the key arrived at, so a dialog MUST take
    // focus off its input-blocked owner - otherwise typing into the dialog edits
    // the window the dialog just blocked. Restoring on close is the other half:
    // without it the user is left with focus nowhere after dismissing a dialog.
    uint32_t prev_focus = 0;
    // The instance id `prev_focus`'s slot had when it was saved. Tree slots are
    // recycled, so a bare slot is not enough: the widget that held focus can be
    // destroyed while the dialog is up and a new one created into its slot, and
    // restoring by slot alone would hand focus to a widget the user never
    // touched. Same guard, same counter as the accessibility node ids.
    uint32_t prev_focus_gen = 0;
    // Active toast overlay (at most one per frame; replace-on-second-call).
    ToastState toast;
  };

  // NEUI_W_POPUPSURFACE - a frameless overlay that may leave its owner frame.
  //
  // Deliberately a FRAME KIND rather than a child widget, and that is the whole
  // design: a frame already owns a coordinate origin, a render context and the
  // root of a child subtree, so the two possible backings differ only in whether
  // this widget carries a native_handle.
  //
  //   desktop backing  - native_handle from platform_create_popup_surface: an
  //                      owned, borderless, non-activating platform window that
  //                      may extend past the owner (win32 / macOS / X11).
  //   in-frame backing  - no native_handle: the owner's paint_frame composites
  //                      this subtree into the owner's surface, clipped to its
  //                      client rect (iOS / WASM / LVGL / null - and not written
  //                      yet, see plans/popup-surface.md).
  //
  // Either way the SAME paint walk and the SAME hit-test walk run over the
  // children, so a GRID or a scrolling SECTION inside a popup behaves
  // identically under both - which the previous popup code (a bespoke paint pass
  // in paint_popup_menu / paint_tree_popup) could never do, and which is why a
  // client had to hand-roll menu rows into a CUSTOMDRAW to get a popup outside
  // the frame.
  //
  // Unlike the other frame kinds this one is NOT shown by widget_show (which has
  // no idea where to put it) - NEUI_API_POPUP::open places and shows it. The
  // stack of open surfaces lives on the Session; see _popup_surfaces.
  class PopupSurfaceWidget : public FrameWidget {
  public:
    // The frame that owns this surface for z-order / activation / dismissal, and
    // the widget it was anchored to, as recorded at open time. Both are tree
    // slots in the SAME session; 0 when closed.
    //
    // owner_index (on the base) is deliberately NOT reused for this: it carries
    // DIALOG modality semantics (widget_set_owner gates on is_dialog, and a
    // modal owner is input-BLOCKED), which is the opposite of what a popup
    // wants - a press on the owner must dismiss the popup, not be swallowed by
    // a disabled window.
    uint32_t popup_owner  = 0;
    uint32_t popup_anchor = 0;
    // Placement recorded from open(), so a re-place (owner moved, zoom changed)
    // can redo the same arithmetic without the client asking again.
    int  popup_side    = 0;    // neui_popup_side_t
    int  popup_off_x   = 0;    // logical px in the anchor's local space
    int  popup_off_y   = 0;
  };

  // Structural / display-only widgets - no extra fields, use base paint
  class LabelWidget     : public WidgetData {};
  class ButtonWidget    : public WidgetData {
  public:
    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
  };

  // SECTION - visual container. Paints a colored backdrop behind its
  // children; optionally draws `text` as a header label at the top
  // (alignment via NEUI_ATTR_ALIGN_TEXT). Background color from
  // NEUI_ATTR_BACKGROUND, falling back to ColorRole::panel_bg. When
  // NEUI_ATTR_SCROLL_MODE != "none", lazy-allocates a scroll state and
  // becomes a scrolling container: children are clipped to the body and
  // translated by the current scroll offset.
  class SectionWidget   : public WidgetData {
  public:
    std::unique_ptr<neui_detail::SectionScrollState> scroll_state;
    // Layout from the last paint - cached every paint regardless of
    // scroll state. Used by paint_widgets_recursive to translate
    // descendants by body_y (children's coords are body-relative, so
    // "none" alignment expands the visible area into the chip band) and
    // by widget_at_recursive to clip child hits to the body rect.
    neui_detail::SectionLayout last_layout{};

    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx, bool is_focused) override;
    bool on_mouse_event(neui_event_t* event) override;
    neui_detail::SectionScrollState* scroll_state_ptr() override {
      return scroll_state.get();
    }
    bool scrollbar_dragging() const override {
      return scroll_state &&
             (scroll_state->vert_drag.active || scroll_state->horz_drag.active);
    }
    const neui_detail::SectionLayout* section_layout_ptr() const override {
      return &last_layout;
    }
    // Refresh scroll_state allocation from NEUI_ATTR_SCROLL_MODE. Call from
    // widget_show + attr_changed. Allocates on first scrolling mode; cheap
    // no-op otherwise.
    void refresh_scroll_state();
    // Fire NEUI_EVENT_SCROLL_CHANGED iff scroll_x/y changed since last
    // notification. Idempotent, cheap; called from every commit path
    // (wheel, scrollbar drag, spring-back tick, programmatic scroll API).
    void notify_scroll_changed() override;
    // Programmatic external commit: writes (nx, ny) into the scroll
    // state, resets the per-axis kinetics integrator so a later wheel
    // event doesn't snap back, repaints, and fires SCROLL_CHANGED.
    // Public so the NEUI_API_SCROLL implementations (set_scroll,
    // ensure_visible) can drive it without touching the protected
    // repaint() seam. Clamps to the current legal scroll range.
    void external_commit(int nx, int ny);
  protected:
    // Effective header chip text / alignment. TabPageWidget overrides these
    // to suppress the chip entirely - a tab page is a chip-less scrolling
    // section whose `text` is the tab label drawn by the parent TABVIEW, not
    // a section header band.
    virtual std::string section_header_text()  const { return text; }
    virtual const char* section_header_align() const {
      return attrs ? attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
    }
  };

  // TABPAGE - one tab's content container. A chip-less scrolling SECTION
  // (its scroll body + child clipping + kinetics are entirely inherited);
  // the `text` is the tab label the parent TABVIEW paints in the strip.
  class TabPageWidget : public SectionWidget {
  protected:
    std::string section_header_text()  const override { return std::string(); }
    const char* section_header_align() const override { return "none"; }
  };

  // TABVIEW - tabbed container. Draws the chip strip (one chip per TABPAGE
  // child) + optional whole-area background + optional tab-outline border,
  // shows the selected page (sized to the content body rect) and hides the
  // rest, and fires NEUI_EVENT_TAB_DESELECTED / _SELECTED on every change.
  // Exposes its content rect via section_layout_ptr so the shared paint walk
  // offsets + clips the active page to the body automatically.
  class TabViewWidget : public WidgetData {
  public:
    int selected = 0;
    neui_detail::SectionLayout last_layout{};      // content rect (child positioning + clip)
    std::vector<neui_detail::TabChip> chips;        // cached for hit-testing
    neui_detail::TabEdge edge = neui_detail::TabEdge::Top;
    std::vector<float> label_widths;                // cached chip-label measurements
    uint64_t           label_sig = 0;               // signature the cache was measured at

    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx, bool is_focused) override;
    bool on_mouse_event(neui_event_t* event) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    const neui_detail::SectionLayout* section_layout_ptr() const override { return &last_layout; }

    // Collect the TABPAGE child indices in creation (tab) order.
    void collect_pages(std::vector<uint32_t>& out) const;
    // Switch selection; fires deselect(old) then select(new) BEFORE swapping
    // page visibility + repaint, so client handlers update the new page first.
    void select_tab(int new_index);
    // Size the active page to the content body rect; toggle page visibility.
    void apply_page_geometry();
  };

  // MULTILINE - multi-line text editor with caret + selection + vertical scroll.
  // Newlines in the stored text string are physical line breaks ('\n').
  class MultilineWidget : public WidgetData {
  public:
    int  cursor_pos = 0;  // byte offset
    int  sel_anchor = 0;  // byte offset

    // Vertical scrolling state.
    uint32_t scroll_offset        = 0;  // top row index
    bool     sb_dragging          = false;
    int      sb_drag_start_y      = 0;
    uint32_t sb_drag_start_offset = 0;
    bool scrollbar_dragging() const override { return sb_dragging; }

    // Cached line-start byte offsets (the result of ml_line_starts(text)),
    // consumed by paint() so it doesn't rescan the whole buffer every frame.
    // _ls_dirty is set on every text mutation (and external set_text), so the
    // cache is rebuilt exactly when the line structure can have changed.
    // Input handlers still build fresh starts for their own use - they run
    // once per keystroke, not once per frame, so the per-frame paint cost is
    // the one worth eliminating.
    // Cache holds VISUAL line starts: logical lines (split on '\n') when wrap
    // is off, plus soft word-wrap breaks when NEUI_ATTR_LINE_WRAP is set. The
    // cache key is (text dirty, wrap on/off, wrap width, font size) so it
    // rebuilds on edits, resize, font change, or toggling wrap; computing the
    // wrapped layout needs measure_text, so it is only redone when one of
    // those actually changes.
    std::vector<int> _ls_cache;
    bool             _ls_dirty   = true;
    bool             _cache_wrap = false;
    float            _cache_w    = -1.0f;
    float            _cache_font = -1.0f;
    const std::vector<int>& cached_line_starts();
    void mark_lines_dirty() { _ls_dirty = true; }
    bool wrap_enabled() const {
      return attrs && attrs->get_int(NEUI_ATTR_LINE_WRAP, 0) != 0;
    }

    // Reused per-line scratch for paint's draw_text slice (draw_text wants a
    // NUL-terminated pointer). assign() reuses capacity, so painting N visible
    // lines costs at most one (amortized) allocation instead of one per line.
    std::string _paint_scratch;

    neui_detail::EditHistory history;

    // IME composition state. Active only between COMP_START and COMP_END.
    // While `composing`, `text` is not mutated; composition_text is rendered
    // overlaid at the caret with an underline. Pre-state snapshot is captured
    // at COMP_START so the result-string commit can push a single undo entry.
    // composition_attrs, if non-empty, is one CompAttr value per UTF-8 byte
    // of composition_text - paint uses it to draw per-clause coloured/thicker
    // underlines (single underline if absent).
    bool                      composing             = false;
    std::string               composition_text;          // UTF-8
    int                       composition_caret     = 0; // byte offset within composition_text
    std::vector<uint8_t>      composition_attrs;
    neui_detail::EditState    composition_pre_state;

    void paint(neui_render_backend_t*, neui_render_ctx_t, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_keychar(uint32_t codepoint, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;
    bool perform_command(uint32_t cmd) override;
    bool can_perform_command(uint32_t cmd) const override;
    bool is_text_input() const override { return true; }
    bool insert_text(const std::string& utf8) override;
    bool on_composition(int kind, const char* utf8, int byte_len, int caret_byte,
                        const uint8_t* per_byte_attrs) override;
    bool caret_rect_local(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                          float* out_x, float* out_y, float* out_h) override;
  };

  // INPUTBOX - cursor, selection, overwrite mode
  class InputBoxWidget : public WidgetData {
  public:
    int  cursor_pos     = 0;     // byte offset of the caret (moving end)
    int  sel_anchor     = 0;     // byte offset of the fixed end;
                                 // equals cursor_pos when no active selection
    bool overwrite_mode = false; // false = insert (thin cursor), true = block cursor

    neui_detail::EditHistory history;

    // IME composition state - see MultilineWidget for the contract.
    bool                      composing             = false;
    std::string               composition_text;          // UTF-8
    int                       composition_caret     = 0; // byte offset within composition_text
    std::vector<uint8_t>      composition_attrs;
    neui_detail::EditState    composition_pre_state;

    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
               bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_keychar(uint32_t codepoint, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;
    bool perform_command(uint32_t cmd) override;
    bool can_perform_command(uint32_t cmd) const override;
    bool is_text_input() const override { return true; }
    bool insert_text(const std::string& utf8) override;
    bool on_composition(int kind, const char* utf8, int byte_len, int caret_byte,
                        const uint8_t* per_byte_attrs) override;
    bool caret_rect_local(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                          float* out_x, float* out_y, float* out_h) override;
  };

  // CHECKBOX / CHECKBOX3
  class CheckboxWidget : public WidgetData {
  public:
    int check_state = 0;  // NEUI_CHECK_*
    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
      bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;

    bool on_mouse_event(neui_event_t* event) override;
  };

  // SLIDER - linear value control (horizontal default; vertical via
  // NEUI_ATTR_ORIENTATION="vertical"). Self-painted via the backend; the
  // current value lives in the NEUI_PARAM_VALUE float attribute.
  class SliderWidget : public WidgetData {
  public:
    bool  is_vertical    = false;   // resolved from NEUI_ATTR_ORIENTATION at show
    bool  dragging       = false;
    int   drag_start_pos = 0;       // pixel coord at drag start (x or y)
    float drag_start_val = 0.0f;    // value at drag start

    void paint(neui_render_backend_t*, neui_render_ctx_t, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;
  };

  // KNOB - circular value control with -135deg..+135deg sweep. Self-painted via
  // the shared paint_knob helper; current value in NEUI_PARAM_VALUE.
  // Mouse interaction: angular drag - moving the cursor around the knob
  // centre rotates the value. Per-pixel sensitivity is the natural
  // 1/radius (small circles around the centre move the value faster than
  // wide ones), which is the desired feel for an audio-plugin knob.
  class KnobWidget : public WidgetData {
  public:
    bool  dragging        = false;
    float drag_prev_angle = 0.0f;  // last cursor angle (rad) relative to centre (rotational mode)
    // Unsnapped accumulator carried across drag samples. Without this, every
    // frame's small angular delta would be rounded to zero by the per-set
    // snap when NEUI_ATTR_STEPS is configured, making the drag feel "stuck"
    // until a single big motion crossed a half-step threshold.
    float drag_continuous = 0.0f;
    // Cached NEUI_ATTR_KNOB_MODE at mouse-down so the per-frame mouse-move
    // path doesn't pay the attribute-lookup cost. Live changes to the attr
    // take effect on the NEXT drag.
    int   drag_mode       = 0;
    int   drag_prev_x     = 0;     // previous mouse X (horizontal slider mode)
    int   drag_prev_y     = 0;     // previous mouse Y (vertical slider mode)

    void paint(neui_render_backend_t*, neui_render_ctx_t, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;
  };

  // LISTBOX
  class ListItemsWidget : public WidgetData {
  public:
    struct Item { std::string text; void* userdata = nullptr; };
    std::vector<Item> items;
    uint32_t selected_item   = UINT32_MAX;
    // Row under the cursor (for hover highlight in paint). UINT32_MAX = none.
    // Distinct from ComboBoxWidget::hover_item, which has commit semantics.
    uint32_t hover_row       = UINT32_MAX;
    uint32_t scroll_offset   = 0;
    bool     sb_dragging     = false;
    int      sb_drag_start_y = 0;
    uint32_t sb_drag_start_offset = 0;
    bool scrollbar_dragging() const override { return sb_dragging; }

    void paint(neui_render_backend_t*, neui_render_ctx_t, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;

  protected:
    // Number of rows visible in the scrollable area. Overridden by ComboBoxWidget
    // to use only the drop portion of the widget, not the full height.
    virtual int visible_rows() const;
  };

  // COMBOBOX - collapsed single-line view with a popup overlay
  class ComboBoxWidget : public ListItemsWidget {
  public:
    // Highlighted row inside the open overlay. Mouse hover and keyboard
    // navigation move it; the actual selection (selected_item) is committed
    // only on click or Enter. UINT32_MAX = no hover (use selected_item).
    uint32_t hover_item = UINT32_MAX;

    void paint(neui_render_backend_t*, neui_render_ctx_t, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;
    bool hit_test(float px, float py) const override;
    int  visible_rows() const override;   // returns max_drop_visible()

    // Called by Session::paint_frame after all normal widgets to draw the overlay on top.
    void paint_overlay(neui_render_backend_t*, neui_render_ctx_t);

    // Height of the collapsed combobox bar. This is the client-supplied widget
    // height (the client lays out only the collapsed control); falls back to a
    // sane default when the client passed a non-positive height.
    int collapsed_h() const;

    // Number of item rows shown in the open drop list: min(item count,
    // NEUI_ATTR_COMBO_MAX_VISIBLE [default 10]). Beyond this the list scrolls.
    // Independent of the widget height (the drop list is sized from the items,
    // not from leftover widget space).
    int max_drop_visible() const;

    // Pixel width of the open drop list. NEUI_ATTR_COMBO_DROP_WIDTH overrides;
    // otherwise the widest entry (+ padding + scrollbar column when the list
    // overflows) drives it. Never narrower than the collapsed bar. Needs the
    // backend for text measurement (may be null -> falls back to bar width).
    int drop_width(neui_render_backend_t* backend) const;

    // Open drop-list rectangle in frame-local absolute coords. The single
    // source of truth shared by paint_overlay + the Session combo hit-test
    // handlers, so painting and input always agree. Opens below the collapsed
    // bar by default; flips above it when the list would overflow the frame's
    // bottom edge but fits above (falls back to the roomier side, then clips).
    struct OverlayRect { float x, y, w, h; };
    OverlayRect overlay_rect(neui_render_backend_t* backend) const;
  };

  // TREEVIEW
  class TreeviewWidget : public WidgetData {
  public:
    struct TreeItem {
      uint32_t    parent_id = 0;
      std::string text;
      void*       userdata  = nullptr;
      bool        enabled   = true;
      bool        expanded  = false;  // shows children when true
      std::string shortcut;
    };
    std::unordered_map<uint32_t, TreeItem> tree_items;
    // IDs of all tree items in insertion order; sibling order (first_child /
    // next_sibling) is derived from this list, not from map iteration.
    std::vector<uint32_t>                  tree_items_ordered;
    uint32_t next_tree_id       = 1;
    uint32_t selected_tree_item = UINT32_MAX;
    // Visible-row index (into flatten_visible result) under the cursor for the
    // hover highlight in paint. UINT32_MAX = none.
    uint32_t hover_row          = UINT32_MAX;

    // Scroll / scrollbar state (parallels ListItemsWidget).
    uint32_t scroll_offset        = 0;
    bool     sb_dragging          = false;
    int      sb_drag_start_y      = 0;
    uint32_t sb_drag_start_offset = 0;
    bool scrollbar_dragging() const override { return sb_dragging; }

    // Flattened representation of one visible row.
    struct VisRow { uint32_t id; int depth; bool has_children; };

    // Returns the currently visible rows in display order, honouring
    // expanded / collapsed ancestors.
    std::vector<VisRow> flatten_visible() const;

    // Returns true if the given item has at least one child.
    bool has_children(uint32_t id) const;

    void paint(neui_render_backend_t*, neui_render_ctx_t, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;
  };

  // IMAGE - displays an image asset. Two source modes (last-set-wins,
  // mutually clearing): the legacy text field holds a file path OR
  // `asset` holds a pre-loaded NEUI_API_ASSETS handle. set_text and
  // set_asset on this widget clear the opposite source.
  class ImageWidget : public WidgetData {
  public:
    neui_asset_t asset = asset_none;
    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
               bool is_focused) override;
  };

  // CUSTOMDRAW - client-rendered surface. Emits NEUI_EVENT_WIDGET_PAINT
  // each paint with backend + ctx + size + focus; the framework wraps the
  // dispatch in push_transform / push_clip(widget bounds) / pop_clip /
  // pop_transform so the client can't corrupt sibling widget rendering.
  // Standard mouse/key events still flow to the client through the normal
  // event path (emit_events auto-set in widgets.cpp).
  //
  // When `compound_asset` references a NEUI_ASSET_KIND_COMPOUND asset, the
  // framework paints that compound (layers in (z, insertion) order) and
  // does NOT fire WIDGET_PAINT. Clients pick either declarative compound
  // or imperative WIDGET_PAINT for a given widget, not both.
  class CustomDrawWidget : public WidgetData {
  public:
    neui_asset_t compound_asset = asset_none;
    // Behavior asset attached via widgets->set_asset (kind-routed). When
    // set, the framework consults the asset's handler list before the
    // client's onevent for mouse / key / wheel events on this widget,
    // turning the CUSTOMDRAW into a fully interactive painted control.
    // See <neui/d/behavior.h>.
    neui_asset_t behavior_asset = asset_none;
    neui_asset_t behavior_asset_id() const override { return behavior_asset; }
    // Lazy per-widget drag state. Allocated on first MOUSE_BUTTON_DOWN
    // that lands on a drag-kind handler.
    std::unique_ptr<neui_detail::BehaviorRuntime> behavior_rt;

    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
               bool is_focused) override;
    // Painted after the recursive child-widget descent so z>=0 compound
    // layers can sit above children. Default implementation no-op; only
    // CustomDrawWidget overrides because only CUSTOMDRAW carries compound
    // layers today. (Method declared on WidgetData via a hook in host.h.)
    void paint_after_children(neui_render_backend_t* backend,
                                neui_render_ctx_t ctx,
                                bool is_focused) override;

    // Behavior dispatch hooks. Returns true if a behavior handler
    // consumed the event. Called by Session::dispatch_mouse_event /
    // Session::handle_input_key before the legacy on_mouse_event /
    // on_keydown forwards (which CustomDrawWidget itself doesn't
    // override - the behavior path is the only one).
    bool on_mouse_event(neui_event_t* event) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
  };

  // GRID - scrollable multi-column table. Cells are paint-state (not
  // widgets) - a 10000 x 8 grid is one widget. Two focus modes via
  // NEUI_ATTR_GRID_CELL_FOCUS: row-focus (default) or cell-focus with
  // a (row, col) cursor. See include/neui/d/grid.h.
  class GridWidget : public WidgetData {
  public:
    neui_detail::GridModel model;

    void paint(neui_render_backend_t*, neui_render_ctx_t, bool is_focused) override;
    bool on_keydown(uint32_t keycode, uint32_t modifiers) override;
    bool on_keychar(uint32_t codepoint, uint32_t modifiers) override;
    bool on_mouse_event(neui_event_t* event) override;
    void on_focus_change(bool gained) override;
    neui_detail::GridModel* grid_model_ptr() override { return &model; }
    bool scrollbar_dragging() const override {
      return model.vert_drag.active || model.horz_drag.active;
    }
  };

  // MENUBAR - native menu bar handle + all item bookkeeping
  class MenubarWidget : public WidgetData {
  public:
    void* hmenu = nullptr;

    struct MenuItemData {
      void*       parent_hmenu   = nullptr;
      void*       submenu        = nullptr;
      uint32_t    cmd_id         = 0;
      uint32_t    parent_item_id = 0;
      bool        enabled        = true;
      bool        checked        = false;   // checkmark (leaf menu items only)
      bool        is_separator   = false;
      void*       userdata       = nullptr;
      std::string text;
      // Typed shortcut binding. shortcut_key == NEUI_KEY_NONE -> no shortcut.
      uint32_t    shortcut_mods  = 0;
      uint32_t    shortcut_key   = 0;
      // Cached display label ("Ctrl+S") computed from mods/key. Stored so
      // make_menu_text doesn't need to reformat on every refresh.
      std::string shortcut;
      // Built-in command binding (neui_command_t). 0 = no built-in routing.
      uint32_t    menu_cmd       = 0;
    };
    std::unordered_map<uint32_t, MenuItemData> menu_items;
    std::unordered_map<uint32_t, uint32_t>     menu_cmd_map;       // cmd_id -> neui item id
    std::vector<uint32_t>                      menu_item_ids_ordered;
    uint32_t                                   next_menu_item_id = 1;
    uint32_t                                   next_menu_cmd_id  = 0x8000;
    // Owned accelerator table (HACCEL on Win32, void* opaque elsewhere).
    void*                                      native_accel = nullptr;

    bool is_menubar()    const override { return true; }
    bool is_menu_model() const override { return true; }
    // "This widget owns a native HMENU / NSMenu that the platform_menubar_*
    // seam manipulates." A real menu bar does; a POPUPMENU does not - the xpl
    // host paints its cascade itself, and every platform_menubar_* call on a
    // popup would either mutate a menu the user can see (macOS / win32 attach
    // theirs to the app) or be a no-op (Linux). Gating on this keeps the SHARED
    // part - the item model - and drops the native part.
    virtual bool uses_native_menu() const { return true; }
    void on_destroy(Session* s) override;
  };

  // NEUI_W_POPUPMENU - the model behind widgets->popup_tree_menu.
  //
  // Reuses MenubarWidget's ITEM MODEL wholesale: same menu_items map, same
  // tree->add / set_shortcut / set_checked / set_menu_cmd handling, same cascade
  // layout and painting (mb_build_columns / paint_menu_columns are already
  // origin-agnostic). What it deliberately does NOT reuse is menu-BAR-ness:
  //
  //   is_menubar()       false - so widget_create does not build a native menu,
  //                              widget_show does not attach it to the frame
  //                              (that would replace the app's real menu bar),
  //                              frame_menubar_index skips it, and it never
  //                              lands in Session::_menubars.
  //   uses_native_menu() false - so tree->add / remove / clear touch the item
  //                              model only, never platform_menubar_*.
  //
  // Consequences worth knowing: a popup's per-item shortcuts are LABELS, not
  // accelerators (nothing translates them - bind the real accelerator on the
  // menu bar), and a pick reports as a single NEUI_EVENT_ITEM_SELECTED rather
  // than going through dispatch_menu_event, whose _menubars scan would match a
  // menu bar holding the same (per-widget) cmd id.
  //
  // Has no on-screen presence of its own: no events, never painted except while
  // Session::_tree_popup_active names it.
  class PopupMenuWidget : public MenubarWidget {
  public:
    PopupMenuWidget() { emit_events = false; }
    bool is_menubar()       const override { return false; }
    bool uses_native_menu() const override { return false; }
  };

  // -------------------------------------------------------------------------

  class Session
  {
  public:
    Session(neui_client_t* client, void* token);
    ~Session();

    void  set_session_id(neui_session_t id) { _session_id = id.session; }
    void* get_token() const                 { return _token; }

    bool run();
    void endsession();

    // Widget API
    neui_widget_t widget_create(neui_widget_t parent, const char* type,
                                int x, int y, int width, int height, void* userdata);
    void          widget_destroy(neui_widget_t widget);
    void          widget_show(neui_widget_t widget);
    void          widget_hide(neui_widget_t widget);
    void          widget_set_pos(neui_widget_t widget, int x, int y, int width, int height);
    void          widget_set_size(neui_widget_t widget, int width, int height);
    void          widget_set_emit_events(neui_widget_t widget, bool enabled);
    void          widget_set_text(neui_widget_t widget, const char* text);
    int           widget_get_text(neui_widget_t widget, char* buf, int buflen);
    neui_widget_t widget_get_first_child(neui_widget_t widget);
    neui_widget_t widget_get_next_sibling(neui_widget_t widget);
    void          widget_set_focus(neui_widget_t widget);
    void          widget_set_owner(neui_widget_t dialog, neui_widget_t owner);
    void          widget_set_check(neui_widget_t widget, neui_check_state_t state);
    neui_check_state_t widget_get_check(neui_widget_t widget);
    void*         widget_get_native_handle(neui_widget_t widget);

    // Items API stubs
    void     items_clear(neui_widget_t widget);
    uint32_t items_add(neui_widget_t widget, const char* text, void* userdata);
    void     items_remove(neui_widget_t widget, uint32_t index);
    uint32_t items_count(neui_widget_t widget);
    int      items_get_text(neui_widget_t widget, uint32_t index, char* buf, int buflen);
    void     items_set_text(neui_widget_t widget, uint32_t index, const char* text);
    void*    items_get_userdata(neui_widget_t widget, uint32_t index);
    uint32_t items_get_selected(neui_widget_t widget);
    void     items_set_selected(neui_widget_t widget, uint32_t index);

    // Tree API stubs
    neui_item_t tree_add(neui_widget_t widget, neui_item_t parent,
                         const char* text, void* userdata);
    void        tree_remove(neui_widget_t widget, neui_item_t item);
    void        tree_clear(neui_widget_t widget);
    int         tree_get_text(neui_widget_t widget, neui_item_t item,
                               char* buf, int buflen);
    void        tree_set_text(neui_widget_t widget, neui_item_t item,
                               const char* text);
    void*       tree_get_userdata(neui_widget_t widget, neui_item_t item);
    void        tree_set_enabled(neui_widget_t widget, neui_item_t item, bool enabled);
    bool        tree_get_enabled(neui_widget_t widget, neui_item_t item);
    void        tree_set_shortcut(neui_widget_t widget, neui_item_t item,
                                   const char* shortcut);
    neui_item_t tree_get_first_child(neui_widget_t widget, neui_item_t parent);
    neui_item_t tree_get_next_sibling(neui_widget_t widget, neui_item_t item);
    neui_item_t tree_get_selected(neui_widget_t widget);
    void        tree_set_selected(neui_widget_t widget, neui_item_t item);

    // Internal helpers
    WidgetData* get_widget(uint32_t index);
    bool        dispatch_event(neui_event_t* event);

    // Client timers (NEUI_API_TIMER). The table + deadline math are portable
    // (hosts/shared/timer_table.h); these wrap it with the platform tick.
    uint32_t timer_add(uint32_t interval_ms);
    bool     timer_remove(uint32_t timer_id);
    bool     timer_set_interval(uint32_t timer_id, uint32_t interval_ms);
    // Called by the platform layer on every native tick.
    void     tick_client_timers();
    // Platform hook: the native tick could not be armed, so drop the cached
    // "armed at" interval and let the next add/remove try again.
    void     notify_timer_arm_failed() { _timer_native_interval = 0; }
  private:
    void     sync_timer_tick();
  public:

    // Route a menu command (Win32 WM_COMMAND) to NEUI_EVENT_TREE_ITEM_ACTIVATED.
    // Searches every registered menu BAR for the cmd_id - which is why it must
    // not be used for a POPUPMENU pick: next_menu_cmd_id restarts at 0x8000 per
    // widget, so ids collide across menus and the first bar holding a matching
    // id wins. A popup calls dispatch_menu_command directly with its own widget.
    bool dispatch_menu_event(uint32_t cmd_id);
    // Route `cmd_id` within ONE menu-model widget. Returns false if that widget
    // has no such command.
    bool dispatch_menu_command(MenubarWidget& mb, uint32_t cmd_id);

    // Walk up the widget tree from widget_index and return the first
    // ancestor's native_handle (the frame HWND). Returns nullptr if none.
    void* find_parent_native_handle(uint32_t widget_index);

    // Logical client height (px) of the frame owning widget_index - i.e. the
    // height field of the first ancestor carrying a native_handle, kept in
    // sync with the window client area on WM_SIZE. Returns 0 when no frame
    // ancestor is found (callers treat 0 as "unknown / no constraint").
    int frame_client_height(uint32_t widget_index);

    // Move keyboard focus to the next (forward=true) or previous (false) widget
    // that has both tab_stop=true and visible=true, WITHIN one frame. Wraps
    // around inside that frame; never moves focus into a different window.
    //
    // `frame_hint` is the frame whose native surface delivered the key - the
    // platform layer always knows it, and PASSING IT IS STRONGLY PREFERRED: it
    // is the only correct answer when no widget is focused yet. 0 falls back to
    // the focused widget's frame, then to the first realized frame.
    void focus_next(bool forward, uint32_t frame_hint = 0);

    // Recompute every widget's cached frame-local abs_x/abs_y under `frame_index`
    // WITHOUT painting. Normally these are a by-product of the paint walk, which
    // leaves them at 0 until the frame's first paint - fine for hit-testing
    // (input can't arrive that early) but not for an out-of-band positional
    // query such as an accessibility provider's. Uses the same origin arithmetic
    // as the paint walk; does not recompute SECTION / TABVIEW body layout, which
    // needs a live render context (see child_origin_of in host.cpp).
    void refresh_abs_positions(uint32_t frame_index);

    // refresh_abs_positions, but safe on a frame whose cached layout may not
    // exist yet: forces one synchronous paint first when the frame has never
    // painted or its tree changed since it last did. THE entry point for
    // out-of-band positional queries; prefer it over refresh_abs_positions
    // unless you know the frame has already painted since its last change.
    //
    // Returns false when the positions could NOT be made valid (window hidden /
    // unmapped / not yet realized, so the forced paint did not happen). In that
    // case the cached geometry is deliberately left ALONE rather than recomputed
    // from empty caches - stale beats wrong - so a caller must not treat the
    // cached values as fresh.
    //
    // Returns false, changing nothing, when called FROM INSIDE A PAINT: the
    // forced paint would re-enter paint_frame on the same render context, and a
    // client can reach here from a WIDGET_PREUPDATE handler, which runs mid-
    // paint. Enforced rather than documented (in_paint() below) because the
    // failure mode is a corrupted frame or a crash inside a backend, not a
    // wrong number - and an accessibility query that answers "not available"
    // for one frame costs nothing.
    bool ensure_abs_positions(uint32_t frame_index);

    // True while paint_frame is on the stack for ANY frame of this session.
    // Set for the whole of paint_frame, so it also covers the client callbacks
    // the paint makes (WIDGET_PREUPDATE, WIDGET_PAINT), which is the only way
    // client code can be running mid-paint.
    bool in_paint() const { return _in_paint > 0; }

    // Mark the frame owning `widget_index` as needing a paint before its cached
    // layout can be trusted. Called from structural mutations.
    void mark_layout_dirty(uint32_t widget_index);

    // Move focus OUT of `root_index`'s subtree if that is where it currently is.
    //
    // Called from every path that makes a widget unreachable while it may hold
    // focus: destroy, hide, and a TABVIEW page being deselected. Without it
    // _focused_widget keeps naming something the user can neither see nor reach -
    // a dead keyboard at best, and at worst keystrokes landing on an invisible
    // control, or (after a destroy) focus silently transferring to whatever
    // widget is next created into the recycled tree slot, with no focus event
    // fired for it.
    //
    // `try_next` picks the next tab stop in the same frame, which is right when
    // the widget still EXISTS and is merely out of sight (hide / tab switch).
    // A destroy passes false: the tree is mid-mutation, and choosing an arbitrary
    // neighbour on behalf of the client is a UX decision the framework should not
    // make. Either way a proper focus-lost event is dispatched, so a client is
    // told rather than left guessing.
    void focus_leave_subtree(uint32_t root_index, bool try_next);

    // True when `widget_index` is `root_index` or one of its descendants.
    bool is_in_subtree(uint32_t widget_index, uint32_t root_index) const;

    // End a modal DIALOG's blocking show: drop the pump flag so widget_show
    // unwinds, and give focus back to whatever held it when the dialog opened.
    //
    // Called from every platform's dialog-teardown path, because BOTH ways a
    // modal dialog can end have to do the same two things: the client destroying
    // it, and the USER closing the window (which unwinds the pump without any
    // widget_destroy running at all - focus would otherwise be left on a control
    // inside a closed window, i.e. a dead keyboard). The restore is validated
    // against the saved instance id, so a recycled slot is not mistaken for the
    // widget that held focus.
    void end_modal(uint32_t frame_index);

    // Tree slot of the FRAME (root child) owning `widget_index`, or 0 if there
    // is none. `widget_index` may itself be a frame.
    uint32_t frame_of(uint32_t widget_index) const;

    // Handle an editing key for the currently focused widget.
    // Returns true if the key was consumed and should not be forwarded.
    bool handle_input_key(neui_event_type_t type, uint32_t keycode, uint32_t modifiers);

    // Dispatch a mouse event to a widget: sends to the client first; if the
    // client returns false (did not consume), forwards to widget->on_mouse_event().
    // Respects emit_events - does nothing if the widget has emit_events=false.
    void dispatch_mouse_event(uint32_t widget_idx, neui_event_t* ev);

    // Dispatch a wheel event with ancestor bubbling: tries widget_idx first,
    // then walks up the parent chain until a widget consumes the event
    // (on_mouse_event returns true) or there are no more ancestors. The
    // client's onevent fires once per ancestor tried; if any of them
    // consumes via the client, bubbling stops. Returns whether any widget
    // ended up consuming.
    //
    // stop_before (optional): bubbling stops BEFORE reaching this widget -
    // neither it nor its ancestors are tried. The platform layer uses this
    // to give the widgets inside a scrolling SECTION first refusal on the
    // wheel, then feeds the SECTION's kinetics itself when nothing below
    // consumed.
    bool dispatch_wheel_event(uint32_t widget_idx, neui_event_t* ev,
                               uint32_t stop_before = 0);

    // Called by the platform layer when WM_PAINT / equivalent fires.
    void paint_frame(neui_render_ctx_t ctx, uint32_t parent_index);

    // The 0xAARRGGBB colour paint_frame clears the frame to (per-frame
    // NEUI_ATTR_BACKGROUND override, else the theme frame_bg under the frame's
    // effective palette). Self-contained: applies and restores the palette
    // override. Used by the platform WM_ERASEBKGND handler so areas exposed
    // before D2D's next frame match the painted background instead of black.
    uint32_t frame_clear_color(uint32_t parent_index);

    // Notify the render context that the native window has been resized.
    void resize_render_ctx(uint32_t widget_index, uint32_t w, uint32_t h);

    // Called when the window's DPI changes.
    void on_dpi_changed(uint32_t widget_index, uint32_t new_dpi);

    uint32_t get_session_id() const { return _session_id; }

    // Software hit-test: returns the deepest visible widget with emit_events=true
    // whose bounds contain (x, y). Returns 0 if no match.
    uint32_t widget_at(float x, float y, uint32_t parent_idx);

    // Update the logically focused widget, firing focus events as needed.
    // Also auto-scrolls any enclosing scrolling SECTION so the newly-
    // focused widget is visible (Tab traversal into off-screen children).
    void set_focus(uint32_t new_idx);

    // Walk parents of widget_idx to find the nearest scrolling SECTION
    // ancestor and scroll it to bring the widget into view. No-op if
    // there's no scrolling ancestor or the widget is already visible.
    // Called from set_focus and the public NEUI_API_SCROLL::ensure_visible.
    void ensure_widget_visible(uint32_t widget_idx);

    // ---- Standalone tree popup (widgets->popup_tree_menu) -------------------
    // Show `menu_idx` (a NEUI_W_POPUPMENU) anchored at `anchor_idx` + (x, y) in
    // the anchor's local logical px. Returns false for a bad / non-POPUPMENU
    // widget or an empty menu. Asynchronous: the pick arrives later as
    // NEUI_EVENT_ITEM_SELECTED on the menu widget.
    bool show_tree_popup(uint32_t anchor_idx, int x, int y, uint32_t menu_idx);
    void close_tree_popup();
    // Close the popup if the widget being destroyed is the popup itself, its
    // frame, or an ancestor of either. Called from widget_destroy BEFORE the
    // subtree goes away, so _tree_popup_active can never name a dead widget -
    // which would otherwise leave an invisible modal grab swallowing every
    // click for the rest of the frame's life.
    void close_tree_popup_if_within(uint32_t subtree_root);
    // Repaint the frame hosting an open tree popup, if `menu_idx` is that popup.
    // Item-model mutations (text / enabled / checked / shortcut) change what the
    // open cascade should look like - and its column widths - so only a repaint
    // makes them visible. A no-op unless that exact popup is on screen.
    void refresh_open_tree_popup(uint32_t menu_idx);
    void paint_tree_popup(neui_render_ctx_t ctx, uint32_t frame_index);
    // Return true only when the popup actually consumed the input. A click on a
    // DIFFERENT frame dismisses the popup and returns false, so the click still
    // reaches that frame - a menu losing its grab does not eat the click that
    // took it away.
    bool handle_tree_popup_click(uint32_t frame_index, float lx, float ly);
    bool handle_tree_popup_hover(uint32_t frame_index, float lx, float ly);
    // Esc dismisses. Returns true if the key was consumed.
    bool handle_tree_popup_key(uint32_t keycode);
    // A press consumed by the popup arms this; the platform layer's button-UP
    // path calls it to swallow the matching release exactly once, so the widget
    // under the dismissed popup does not also see an UP (and synthesise a
    // CLICK). Mirrors LinuxWindow::swallow_release, but lives on the Session
    // because the popup works on all three platforms.
    bool tree_popup_take_release();
    // Called from every platform's button-DOWN path BEFORE the popup is offered
    // the press. Guarantees the armed flag can never outlive its own gesture: if
    // the paired release never arrives - drag off the window and release outside,
    // which on win32 means no WM_LBUTTONUP at all because a consumed press takes
    // no capture - the flag would otherwise swallow the UP of an unrelated later
    // click, leaving that widget with a DOWN and no UP/CLICK, _pressed_widget
    // stuck, and (on KNOB / SLIDER) a GESTURE_BEGIN with no GESTURE_END.
    void tree_popup_discard_pending_release() { _tree_popup_swallow_release = false; }

    // ---- Popup surfaces (NEUI_W_POPUPSURFACE / NEUI_API_POPUP) ---------------
    // See PopupSurfaceWidget for why this is a frame kind. Everything below is
    // portable: the platform layer contributes the window, the work area and the
    // outside-press notification, and nothing else.

    // Place + show `surface_idx` anchored to `anchor_idx` + (off_x, off_y) in the
    // anchor's local logical px, on `side` (neui_popup_side_t). Re-places an
    // already-open surface. False = nothing shown (bad widget, no frame, empty).
    bool open_popup_surface(uint32_t surface_idx, uint32_t anchor_idx,
                            int off_x, int off_y, int side);
    // Close `surface_idx` and every level opened above it, reporting `reason`
    // (neui_popup_dismiss_reason_t) for it and CASCADE for the deeper levels.
    void close_popup_surface(uint32_t surface_idx, uint32_t reason);
    void close_all_popup_surfaces(uint32_t reason);
    // Close every level deeper than `depth` (0 = keep only the first level).
    void close_popup_surfaces_deeper_than(size_t depth, uint32_t reason);
    // Close any surface whose SURFACE, ANCHOR or OWNER is inside the subtree
    // being destroyed. Called from widget_destroy BEFORE the subtree goes, and
    // it has to check all three: a popup surface is its own root child, so the
    // owner frame's subtree does not contain it, and destroying the editor while
    // a picker is open would otherwise leave a live window over the DAW with a
    // dead owner - plus a grab still swallowing every click.
    void close_popup_surfaces_if_within(uint32_t subtree_root);

    bool popup_surface_open() const { return !_popup_surfaces.empty(); }
    // Depth of `frame_idx` in the open stack, or -1 when it is not a level of it.
    int  popup_surface_depth(uint32_t frame_idx) const;

    // The input gate. While a stack is open, the widgets UNDER it must stop
    // behaving like a live UI - and that suppression, not event delivery, is what
    // an OS pointer capture was ever buying. Doing it here instead means one
    // portable code path shared by both backings, and it leaves the owner window
    // ENABLED (a press reaches us and is interpreted, rather than being swallowed
    // by a disabled window the way a modal dialog's owner is).
    //
    // Call from a platform button-DOWN path with the frame the press arrived at,
    // BEFORE normal hit-testing. Returns true when the press was consumed:
    //   - frame is not part of the stack -> close everything, swallow. The press
    //     that dismisses is not also delivered to what is underneath, matching
    //     every OS menu; one click must not both close a picker and move a knob.
    //   - frame is a SHALLOWER level than the deepest -> close the deeper levels
    //     and return false, so the press still reaches that level's widgets
    //     (clicking a parent menu row while a submenu is open re-targets).
    //   - frame is the deepest level -> false, ordinary dispatch.
    bool popup_gate_press(uint32_t frame_idx);
    // The same decision as popup_gate_press's "outside" branch, for a backing
    // that cannot make it from the window identity alone.
    //
    // X11 needs this: an XGrabPointer with owner_events=True reports a press
    // that landed outside EVERY window of ours against the GRAB window - which
    // is the popup itself - with coordinates outside that window's bounds. Left
    // to popup_gate_press, that press reads as "inside the stack" and nothing
    // ever dismisses, which is the exact opposite of what the grab is for.
    // The platform decides outside-ness from the coordinates; the dismissal and
    // the release-swallow bookkeeping stay here, shared.
    bool popup_gate_press_outside();

    // Same for hover / move: true = swallow. Without this, moving off the popup
    // and across the owner lights up hover on every widget it passes, which is
    // the one thing that reads as "not a menu".
    bool popup_gate_hover(uint32_t frame_idx);
    // Same for the wheel: true = swallow. A scrolling SECTION or GRID under an
    // open popup must not scroll away beneath it, and on a KNOB / SLIDER the cost
    // is worse than cosmetic - an ungated notch fires the whole
    // GESTURE_BEGIN / VALUE_CHANGED / GESTURE_END triple, which in a DAW is an
    // automation write the user never saw.
    //
    // Unlike the press gate this does NOT dismiss: a wheel is not in the
    // documented trigger list (<neui/d/popup.h>), and a stack that vanished on an
    // accidental trackpad glide would be worse than one that ignores it. And
    // unlike the hover gate it leaves _hovered_widget alone, because a wheel does
    // not move the pointer.
    //
    // One method rather than the predicate open-coded at each platform's wheel
    // entry - there are four of them across three platforms (win32, macOS, and
    // Linux twice for core Button 4-7 plus the XI2 smooth path), which is exactly
    // the shape wheel_direction.h exists to stop repeating.
    bool popup_gate_wheel(uint32_t frame_idx);
    // Keyboard, for an open stack. true = consumed; the caller must not run its
    // normal routing. Called for KEYDOWN / KEYUP / KEYCHAR (for KEYCHAR,
    // `keycode` is the codepoint, matching every platform's char dispatch).
    //
    // The key arrives at the OWNER's window - a popup surface never takes
    // activation (inside a DAW that would read as the plugin editor losing
    // focus), so it is not first responder / focused HWND / X focus owner and
    // cannot be sent keys directly. This gate is therefore a RETARGET, not a
    // pass-through, and it makes the same promise for the keyboard that
    // popup_gate_press makes for the mouse: while a popup is open, input must not
    // reach the widgets underneath it. It used to fall through for everything but
    // Escape, so typing with a menu open edited the text field behind it.
    //
    // Three outcomes, in order:
    //   - Escape (KEYDOWN only): close the whole stack, consume.
    //   - focus already inside the DEEPEST level: return false and get out of the
    //     way. _focused_widget is a SESSION index and every platform's key
    //     dispatch reads it without asking which frame it belongs to, so the
    //     ordinary path already delivers into the popup - an INPUTBOX inside one
    //     really does type, non-key window and all.
    //   - anything else: deliver the key to the deepest SURFACE (so a
    //     client-drawn menu can implement its own navigation) and consume.
    bool popup_gate_key(neui_event_type_t type, uint32_t keycode,
                        uint32_t modifiers);
    // True when an open stack is taking keys away from the owner - i.e. a popup
    // is open and focus is not inside its deepest level. The predicate behind the
    // gate's third case, exposed because the platform layers' TEXT INPUT paths
    // (win32 IME composition, AppKit's insertText: / setMarkedText:) are not
    // keystrokes and cannot route through the gate, yet must not compose into the
    // field under an open popup either.
    bool popup_diverts_keys() const;
    // The host's own arrow / Home / End / Page / Enter walk over the item list a
    // client DECLARES on the surface (NEUI_ATTR_NAV_COUNT / _INDEX / _PAGE /
    // _WRAP). Runs only after the client's own handler declined the key, so it
    // is a default rather than a policy - see popup_gate_key. Inert unless a
    // count is declared, which is what keeps it out of the way of every popup
    // that does its own thing. Arithmetic in hosts/shared/popup_nav.h.
    void popup_navigate(uint32_t surface_idx, uint32_t keycode);
    // The button-UP peer of popup_gate_press, same contract and same reason as
    // tree_popup_take_release: a swallowed press must swallow its own release or
    // the widget under the dismissed popup sees an UP with no DOWN and
    // synthesises a CLICK.
    bool popup_take_release();
    void popup_discard_pending_release() { _popup_surface_swallow_release = false; }

    // The box a popup anchored to `anchor_idx` will be clamped to, logical px.
    // Desktop backing = the work area of the monitor the owner is on; in-frame
    // backing = the owner's client area. (0, 0) for a bad anchor.
    void popup_clamp_size(uint32_t anchor_idx, int* out_w, int* out_h);
    // True when such a popup gets its own OS window (and so may leave the frame).
    bool popup_escapes_frame(uint32_t anchor_idx);

    // Update the hovered widget, firing mouse enter/leave events.
    void set_hovered(uint32_t new_idx);

    // ---- Mouse cursor (NEUI_ATTR_CURSOR) --------------------------------
    // The effective cursor kind for `widget_idx`: the nearest ancestor with an
    // explicit NEUI_ATTR_CURSOR wins, and NEUI_CURSOR_DEFAULT when nobody sets
    // one. Inheritance is resolved HERE rather than in the platform layer, so
    // each platform only ever sees a concrete shape.
    int  resolve_cursor_for(uint32_t widget_idx) const;

    // Resolve the cursor for the currently hovered widget (honouring any
    // active override) and push it to the platform if it changed. Cheap and
    // idempotent - safe to call from hover changes, attr writes, and paint.
    void refresh_cursor();

    // Transient internal override, for cursor feedback that is a function of
    // POSITION rather than of which widget is hovered - the GRID's header
    // column-resize divider, which is one shape over a 6 px band inside a
    // single widget. NEUI_CURSOR_DEFAULT clears the override and falls back to
    // the hovered widget's own NEUI_ATTR_CURSOR, which is why an internal
    // caller can no longer just set DEFAULT to mean "arrow": that would stomp
    // a client's cursor.
    //
    // `owner_idx` is the widget the override belongs to. It exists so the
    // override cannot leak: an override is positional WITHIN one widget, so
    // set_hovered drops it as soon as the pointer moves to a different widget.
    // The exception is a drag - while the owner holds the press, the pointer
    // may legitimately be outside it (a column-resize drag continues past the
    // GRID's edge) and the override must survive.
    void set_cursor_override(uint32_t owner_idx, int kind);

    // Drop an override whose owner the pointer has left. Called from
    // set_hovered before the cursor is re-resolved; see set_cursor_override
    // for why a mid-drag owner is exempt.
    void drop_cursor_override_on_hover_change(uint32_t new_idx);

    // Clear hover / override state that points at a destroyed widget slot, then
    // re-resolve. Called after a widget subtree is destroyed, because no
    // pointer-leave event arrives when a widget is destroyed out from under a
    // stationary pointer - and a cursor="none" widget would otherwise leave the
    // pointer hidden process-wide.
    void forget_dead_hover();

    // ---- Relative (unbounded) pointer mode (NEUI_API_POINTER) -----------
    // Enter / leave relative mode for a drag on `widget_idx`. begin returns
    // false when the platform has no warping seam, the widget is invalid, or
    // the mode is already active (not reference-counted - a second begin
    // without an end is a bug, not a nesting request).
    bool begin_relative_pointer(uint32_t widget_idx);
    void end_relative_pointer();
    bool is_relative_pointer() const { return _relative.active; }

    // End relative mode if the subtree rooted at `subtree_root` contains the
    // widget that owns it. MUST be called BEFORE the subtree is destroyed, while
    // the owning frame's native handle is still alive: end_relative_pointer
    // hands that handle to the platform (X11 dereferences it to reach the
    // Display), so ending afterwards is a use-after-free.
    //
    // Relying on dispatch_relative_motion's own liveness check is NOT enough -
    // that only runs when a motion event arrives, and once the owning frame's
    // view/window is gone no motion ever arrives again. The mode would stay on
    // forever with the cursor decoupled from the device and hidden, which is a
    // machine-wide condition rather than a UI glitch.
    void end_relative_pointer_if_within(uint32_t subtree_root);

    // Feed a raw device delta (LOGICAL px) from the platform's motion handler
    // while relative mode is active. Advances the virtual position and
    // dispatches a MOUSE_MOVE carrying it, so an existing drag handler sees
    // ordinary absolute coordinates that are simply no longer screen-bounded.
    void dispatch_relative_motion(float dx, float dy, uint32_t buttonmap);

    // Restore the OS default cursor if this session had changed it. Called from
    // ~Session: a hidden pointer (NEUI_CURSOR_NONE) survives the session that
    // hid it, and on macOS [NSCursor hide]/unhide is a balanced counter, so an
    // unbalanced hide is permanent for the whole host process.
    void release_cursor();

    // Update the captured (pressed) widget. Mirrors set_hovered's role
    // for the press flag: clears the previous widget's `pressed`, sets
    // the new widget's `pressed`, and invalidates either side when it
    // has a compound asset with state-filtered layers.
    void set_pressed(uint32_t new_idx);

    // Toast overlay. Configures the FrameWidget's toast state with the
    // given multi-line text (\n separates lines), measures geometry from
    // the current default font, and starts the platform animation
    // heartbeat. `parent_window_idx` must be a frame widget index;
    // non-frame or unknown indices are silently dropped.
    void toast_show(uint32_t parent_window_idx, const char* text);

    // Paint the toast overlay over the given frame (called from
    // paint_frame after every other overlay so the toast sits on top).
    // Reads the frame's ToastState to compute the current animation
    // phase. Stops itself once the toast lifetime expires.
    void paint_toast(neui_render_ctx_t ctx, uint32_t frame_widget_idx);

    // Returns true if (lx, ly) lands on the active toast for the given
    // frame; when it does, jumps the toast to the start of its fade-out
    // phase so the user gets an immediate visual dismiss. Called from
    // the platform layer's mouse-down handler before widget hit-testing
    // so the toast can absorb the click. lx / ly are frame-local
    // logical pixels (the same space as widget abs_x/abs_y).
    bool handle_toast_click(uint32_t frame_widget_idx, float lx, float ly);

    // Popup menu overlay (used by widgets->popup_menu). Blocking - runs a
    // nested message loop until the user picks an item or dismisses.
    // anchor_idx is the widget the popup is anchored to; (lx, ly) are in
    // its local logical coordinates. Returns 1-based pick index, or 0 if
    // dismissed. items contains the displayed strings ("-" = separator).
    int open_popup_menu(uint32_t anchor_idx, int lx, int ly,
                        const std::vector<std::string>& items);
    // Paint the popup menu overlay. Called from paint_frame after the
    // combo overlay so the popup sits on top of everything else.
    void paint_popup_menu(neui_render_ctx_t ctx);
    // Mouse + key hooks for the popup. Called before normal widget
    // dispatch when _popup_active. lx/ly are in frame-local logical px.
    bool handle_popup_click(float lx, float ly);
    bool handle_popup_hover(float lx, float ly);
    bool handle_popup_key(uint32_t keycode);

    // Combo-overlay management.
    // open_combo adjusts scroll so the selected item is visible, then invalidates.
    void open_combo(uint32_t widget_idx);
    void close_combo();
    // Called from WM_LBUTTONDOWN before normal hit-testing when a combo is open.
    // Returns true if the click was consumed (always true when a combo is open).
    // Sets _combo_sb_dragging when the click landed on the overlay scrollbar thumb.
    bool handle_combo_click(float lx, float ly);
    // Called from WM_MOUSEWHEEL before normal dispatch when a combo overlay is open.
    // Returns true if the wheel event was consumed by the overlay.
    bool handle_combo_wheel(float lx, float ly, int delta);
    // Called from WM_MOUSEMOVE when a combo overlay scrollbar drag is in progress.
    // Returns true if the move was consumed by the drag.
    bool handle_combo_scroll_drag(float ly);

    // Called from WM_MOUSEMOVE while a combo overlay is open: updates the
    // highlighted item to the row under the cursor, without firing
    // ITEM_SELECTED. Mouse outside the overlay item area leaves the highlight
    // unchanged. Returns true if the cursor is over the overlay (caller should
    // suppress normal hit-testing in that case).
    bool handle_combo_hover(float lx, float ly);

    // ---- In-frame menubar (Linux / any platform_menubar_in_frame() host) ----
    // The host draws the menubar itself as a band at the top of the frame's
    // client area, with cascading dropdowns. Win32 (HMENU) / macOS (NSMenu)
    // use the OS menu and never reach this code (frame_top_inset returns 0,
    // paint_menubar early-returns, the platform input layer never calls the
    // handlers).

    // The visible MENUBAR child of a frame (nullptr if none), and the
    // popup-open effective-enabled verdict for a menu item (own flag AND
    // focused-widget-can-perform for built-ins AND menu-client validate).
    MenubarWidget* frame_menubar(uint32_t frame_index);
    bool           menu_item_enabled(const MenubarWidget& mb, uint32_t item_id);

    // Reserved top-band height (logical px) for `frame_index`: the menubar
    // band height when this platform draws menubars in-frame AND the frame has
    // a visible MENUBAR child, else 0. The shared paint walk offsets children
    // down by this; the Linux RESIZE path subtracts it from the reported size.
    int  frame_top_inset(uint32_t frame_index);

    // Usable content area of a widget in its own coordinate space (logical px).
    // For a frame this excludes any in-frame menubar band (origin (0, inset),
    // size (w, h-inset)); otherwise (0, 0, width, height). Out-pointers may be
    // NULL. Backs widgets->get_client_rect and the toast anchor.
    void widget_client_rect(uint32_t widget_index,
                            int* x, int* y, int* w, int* h);

    // Paint the menubar band (+ any open cascading dropdowns) for the frame.
    // Called from paint_frame after the child walk. No-op when the frame has
    // no menubar or this platform uses a native menu.
    void paint_menubar(neui_render_ctx_t ctx, uint32_t frame_index);

    // Input hooks, called from the platform layer before normal dispatch.
    // (lx, ly) are frame-local logical px. handle_menubar_click returns true
    // when the click is owned by the menubar (in the band, in an open
    // dropdown, or a click-outside that dismisses an open menu). hover/key are
    // only meaningful while a menu is open and return true when consumed.
    bool handle_menubar_click(uint32_t frame_index, float lx, float ly);
    bool handle_menubar_hover(uint32_t frame_index, float lx, float ly);
    // Closed-menu hover: highlight the top-level band label under the cursor
    // so it's an easy mouse target. Returns true while the cursor is over the
    // band (no widgets live there); false below it (caller continues normal
    // hover). Clears the highlight when the cursor leaves the band.
    bool handle_menubar_band_hover(uint32_t frame_index, float lx, float ly);
    bool handle_menubar_key(uint32_t keycode, uint32_t modifiers);
    void close_menubar_menu();

    // Match a key/modifier combo against every menubar item's shortcut and,
    // on a hit, route it through dispatch_menu_event (built-in command first,
    // then client TREE_ITEM_ACTIVATED). Returns true if a shortcut matched
    // (the key is then consumed). Mirrors the Win32 HACCEL path, which is
    // MSG-based and so does not run on Linux.
    bool try_menubar_accel(uint32_t keycode, uint32_t modifiers);

    // ---- Accessibility support (consumed by a11y_adapter.cpp) ---------------

    // One on-screen menu element: a MENUBAR band label, or a row of an open
    // dropdown / popup cascade. Frame-local logical px, matching widget
    // abs_x/abs_y. `text` points into the owning MenubarWidget::menu_items and
    // is therefore only valid until that item model is next mutated.
    struct MenuElementRect
    {
      uint32_t    item_id     = 0;
      uint32_t    parent_item = 0;   // 0 = a top-level band label
      int         x = 0, y = 0, w = 0, h = 0;
      bool        enabled     = false;
      bool        checked     = false;
      bool        has_submenu = false;
      bool        separator   = false;
      bool        expanded    = false;  // this item's submenu is currently open
      const char* text        = nullptr;
      const char* shortcut    = nullptr;  // display label, "" when none
    };

    // Collect the on-screen geometry of `menu_idx`'s items. Appends to `out`.
    //
    // Which elements exist depends on what is actually on screen, because that
    // is what an AT should see:
    //   - MENUBAR, in-frame platforms only (Linux): the band labels always, plus
    //     the rows of an open cascade. On win32 / macOS the menu is a real
    //     HMENU / NSMenu that the OS already exposes to the AT, so this reports
    //     NOTHING rather than publishing a duplicate the user would hear twice.
    //   - POPUPMENU: the open cascade rows while Session::_tree_popup_active
    //     names it, nothing otherwise (it has no resting visual presence).
    //
    // Needs the owning frame's render context for text measurement, exactly as
    // the click / hover handlers already do out of band (see tp_claim); returns
    // nothing when the frame has no context yet.
    //
    // CONTRACT: a NEUI_API_MENU_CLIENT::validate implementation must not mutate
    // the session. This runs the same per-item enable verdict paint runs, which
    // means it can call back into client code - but unlike paint it can be
    // reached from an out-of-band accessibility query at an arbitrary runloop
    // point. A validate that destroyed a widget or edited the menu would
    // invalidate the `text` / `shortcut` pointers below, and the WidgetData
    // references the caller is holding, mid-walk.
    void collect_menu_elements(uint32_t menu_idx,
                               std::vector<MenuElementRect>& out);

    // Per-widget-INSTANCE accessibility id. Widget ids are
    // (session << 16 | slot) and slots are recycled with no staleness detection,
    // while both UI Automation and NSAccessibility hold element references
    // across long spans. The adapter stamps this into every A11yNodeId so a
    // reference to a destroyed widget resolves to nothing instead of silently
    // answering with whatever widget later took the slot.
    //
    // The value comes from a PROCESS-WIDE monotonic counter, assigned once when
    // the widget is created. A per-session counter is not enough: `destroy()`
    // drops a whole Session without walking its widgets, and create_session
    // reuses the freed slot WITH THE SAME SESSION ID - so a per-session counter
    // restarting at 0 would let an id minted in the old incarnation validate
    // against an unrelated widget in the new one, which is exactly the
    // wrong-answer-instead-of-no-answer failure this exists to prevent.
    // Assigning at create (rather than bumping at destroy) also means there is
    // one call site instead of two and no way to free a slot without
    // invalidating it: the next occupant simply gets a value nobody has seen.
    //
    // 0 means "no instance id", which no live widget has - so a stale id whose
    // slot is now empty also fails to resolve.
    uint32_t a11y_generation(uint32_t slot) const
    {
      return slot < _a11y_generation.size() ? _a11y_generation[slot] : 0;
    }
    void a11y_assign_generation(uint32_t slot)
    {
      if (slot >= _a11y_generation.size())
        _a11y_generation.resize(slot + 1, 0);
      _a11y_generation[slot] = next_a11y_instance_id();
    }
    // Wraps after 2^32 widget creations in one process, which no session-based
    // UI reaches; the counter starts at 1 so 0 stays reserved.
    static uint32_t next_a11y_instance_id()
    {
      static uint32_t s_next = 1;
      return s_next++;
    }

    // Cache key for a platform provider's node tree.
    //
    // Providers are PULL, not push (plans/accessibility.md §4.1): the tree is
    // built on query behind this counter, never eagerly on change. An AT asks
    // for children, then for each child's role / name / value / frame, so
    // rebuilding per query would be O(n) rebuilds for one traversal - but a
    // cache with no invalidation would answer from a tree that no longer
    // matches the window.
    //
    // PAINT IS THE INVALIDATION SIGNAL. Anything that changes what the window
    // SHOWS also repaints it, so bumping this once per paint_frame covers every
    // visible mutation - text, items, selection, scroll, value, geometry -
    // without a bump at each of the several hundred mutation sites, which is
    // where a hand-wired scheme would rot. The non-visible changes an AT still
    // has to hear about (focus, a client's own notify()) bump it explicitly.
    //
    // Consequence worth knowing: a frame animating at 60 Hz rebuilds on every
    // query. That is the cheap direction of the trade (POD nodes, and only
    // while an AT is actually querying) and it is never wrong.
    uint32_t a11y_revision() const { return _a11y_revision; }
    void bump_a11y_revision() { ++_a11y_revision; }

    // Backs NEUI_API_A11Y::is_active. Set by a platform provider the first time
    // it answers a query. macOS offers no "an AT attached" signal, so "has
    // anything queried us" is the only honest answer - and the public header
    // says so, and says not to gate correctness on it.
    bool a11y_queried() const { return _a11y_queried; }
    void mark_a11y_queried() { _a11y_queried = true; }

    // One accessibility increment / decrement on a value widget, honouring the
    // real-world step declared via NEUI_API_A11Y::set_value_range. False when no
    // step was declared - the caller then falls back to the arrow-key path, so
    // keyboard and AT agree whenever the client named no step. Implemented in
    // host.cpp next to the value helpers, because it has to raise the same
    // GESTURE_BEGIN / VALUE_CHANGED / GESTURE_END triple a keypress does.
    bool a11y_step_value(uint32_t slot, bool up);

    // Write a normalized [0..1] value on behalf of an AT (UIA's
    // RangeValue::SetValue). Raises the same GESTURE_BEGIN / VALUE_CHANGED /
    // GESTURE_END triple a drag or a keypress does, which is what a DAW needs to
    // record the edit as one automation gesture. False when the widget cannot
    // take a value.
    bool a11y_set_value_user(uint32_t slot, float normalized);

  public:
    neui_detail::Tree<WidgetData>  _widgets;
    neui_detail::AssetManager      _asset_manager;
    neui_widget_client_t*  _client_widget_api = nullptr;
    neui_client_t*         _client            = nullptr;
    void*                  _token             = nullptr;
    uint32_t               _session_id        = 0;
    neui_render_backend_t* _backend           = nullptr;

    uint32_t _hovered_widget = 0;
    uint32_t _pressed_widget = 0;
    uint32_t _focused_widget = 0;

    // Cursor state - see resolve_cursor_for / refresh_cursor /
    // set_cursor_override above. _cursor_applied is the last kind actually
    // pushed to the platform (-1 = never), so a hover walk that resolves to
    // the same shape costs nothing; on Linux each push is a server round-trip
    // per window, so the dedupe is load-bearing, not just tidy.
    int      _cursor_override       = NEUI_CURSOR_DEFAULT;
    int      _cursor_applied        = -1;
    uint32_t _cursor_override_owner = 0;

    // Relative (unbounded) pointer mode. The virtual-position bookkeeping is
    // portable (hosts/shared/relative_pointer.h, Tier-1 tested); the anchor is
    // whatever opaque coordinates the platform handed back at begin, kept here
    // only to pass straight back at end.
    neui_detail::RelativePointer _relative;
    // Last pointer position seen by dispatch_mouse_event, FRAME-local logical
    // px. Only used to seed a relative drag from its real press point.
    float _last_mouse_frame_x = 0.0f;
    float _last_mouse_frame_y = 0.0f;
    bool  _last_mouse_valid   = false;
    int  _relative_anchor_x = 0;
    int  _relative_anchor_y = 0;
    void* _relative_native  = nullptr;

    // Client timers. _timer_native_interval caches what the platform tick is
    // currently armed at, so we only re-arm when the shortest interval moves.
    neui_detail::TimerTable            _timers;
    uint32_t                           _timer_native_interval = 0;
    // (The re-entrancy guard lives in TimerTable::tick_and_dispatch, so the
    // dispatch walk and its guard stay together and stay testable.)
    bool     _os_focused     = true;  // frame currently has OS keyboard focus
    uint32_t _open_combo     = 0;   // tree index of the currently open ComboBoxWidget, or 0

    // The zoom of the frame currently being painted (NEUI_ATTR_UI_SCALE), 1.0
    // outside paint_frame. Set for the duration of a frame paint so the
    // painters handed to widget paint code can report the true device scale
    // and so a device-pixel CUSTOMDRAW can undo the zoom transform.
    float    _paint_zoom     = 1.0f;
    float    paint_zoom() const { return _paint_zoom; }

    // Popup-menu overlay state. _popup_active gates the nested message
    // loop in open_popup_menu; mouse + key hooks above check it before
    // routing to normal widget dispatch.
    bool                     _popup_active     = false;
    bool                     _popup_running    = false;  // controls the nested pump
    int                      _popup_picked     = 0;
    int                      _popup_hover      = -1;
    int                      _popup_x_abs      = 0;     // logical px, frame-local
    int                      _popup_y_abs      = 0;
    std::vector<std::string> _popup_items;

    // In-frame menubar overlay state (see the handle_menubar_* methods).
    // _menu_open gates the band/dropdown interaction; _menu_bar is the open
    // menubar's widget index; _menu_path is the chain of open submenu item ids
    // (path[0] = the open top-level popup, path[1] = an open submenu within it,
    // ...), so cascades nest arbitrarily; _menu_hover_item is the menu item id
    // currently under the cursor (0 = none) for row highlighting. Geometry is
    // recomputed on demand from these (no stored rects), like the popup overlay.
    bool                  _menu_open        = false;
    uint32_t              _menu_bar         = 0;
    std::vector<uint32_t> _menu_path;
    uint32_t              _menu_hover_item  = 0;
    // Top-level band label under the cursor while the menu is CLOSED (hover
    // feedback before opening; 0 = none). When a menu is open, highlighting
    // uses _menu_path / _menu_hover_item instead.
    uint32_t              _menu_band_hover  = 0;

    // Standalone tree popup (widgets->popup_tree_menu). Borrows _menu_path /
    // _menu_hover_item for the open cascade, so exactly one menu cascade can be
    // open at a time - which is also true of the OS menus this mirrors.
    // _tree_popup_x/_y are the anchor in FRAME-local logical px. The three
    // platform layers test _tree_popup_active in their mouse paths, exactly as
    // they already do for _popup_active / _menu_open.
    bool                  _tree_popup_active = false;
    uint32_t              _tree_popup_menu   = 0;   // POPUPMENU widget index
    uint32_t              _tree_popup_frame  = 0;
    int                   _tree_popup_x      = 0;
    int                   _tree_popup_y      = 0;
    // Set when a press was consumed by the popup; see tree_popup_take_release.
    bool                  _tree_popup_swallow_release = false;

    // Open popup surfaces (NEUI_W_POPUPSURFACE), outermost level first. Tree
    // slots of the PopupSurfaceWidget frames; the owner / anchor / placement for
    // each level live on the widget itself.
    //
    // A vector rather than a single slot because a cascade is the normal case (a
    // menu opening a submenu opening a colour picker), and the whole cascade
    // shares ONE dismissal decision: a press outside closes all of it, a press on
    // level N closes everything deeper. A client cannot coordinate that across
    // several surfaces, so the host owns it.
    std::vector<uint32_t> _popup_surfaces;
    // Set when popup_gate_press swallowed a press; consumed by popup_take_release.
    //
    // (No re-entrancy flag guards the close path, deliberately: closing a level
    // POPS IT FROM THIS VECTOR BEFORE any teardown or client dispatch, so the
    // stack strictly shrinks and a client that closes another surface from its
    // NEUI_EVENT_POPUP_DISMISSED handler simply pops more. A blocking guard would
    // have been worse than nothing - re-entering from a handler that DESTROYS
    // widgets would then leave a level in the stack naming a dead one.)
    bool                  _popup_surface_swallow_release = false;

    // Overlay scrollbar drag state (managed by handle_combo_click / handle_combo_scroll_drag).
    bool     _combo_sb_dragging       = false;
    int      _combo_sb_drag_start_y   = 0;
    uint32_t _combo_sb_drag_start_off = 0;

    // Indices of MENUBAR widgets for WM_COMMAND routing.
    std::vector<uint32_t> _menubars;

    // Per-slot accessibility instance id - see a11y_generation() above.
    // Indexed by tree slot, grown on demand; slot 0 (the root sentinel) is
    // never handed out so its entry stays 0.
    std::vector<uint32_t> _a11y_generation;

    // Provider cache key + first-query flag - see a11y_revision() above.
    uint32_t _a11y_revision = 1;
    bool     _a11y_queried  = false;

    // paint_frame nesting depth - see in_paint(). A counter rather than a bool
    // because one frame's paint can legitimately be on the stack while another
    // frame paints (a client PREUPDATE handler that shows a dialog), and a bool
    // would clear the guard on the inner paint's exit.
    int _in_paint = 0;

    // Try to consume a Win32 MSG via this session's menubar accelerator
    // tables. Implemented in widgets.cpp; called from platform_run().
    // Takes/returns void* to keep windows.h out of this header.
    bool try_translate_accel(void* msg_ptr);

    // Invoke a built-in command (neui_command_t) on the focused widget or
    // a specific one. Returns true if the widget consumed it.
    bool invoke_focused_command(uint32_t cmd);
    bool invoke_command(neui_widget_t widget, uint32_t cmd);
    // Non-mutating peer to invoke_focused_command - returns true if the
    // currently focused widget would handle `cmd`. Used by the menubar's
    // WM_INITMENUPOPUP handler to gray menu items whose bound command
    // can't reach a consumer right now.
    bool can_focused_perform_command(uint32_t cmd);

    // Per-session data-item store. Backs the item-based half of
    // NEUI_API_CLIPBOARD and (transient) DnD drop payloads.
    neui_detail::DataItemStore _data_items;

    // DnD dispatch state. _current_drop_target is the widget currently
    // receiving DND_ENTER/MOVE events (UINT32_MAX = none). _last_accepted_action
    // is the value the client last passed to dnd_api->accept(); the platform
    // layer reads this back when reporting the cursor effect to the OS.
    // _in_dnd_dispatch is true only while we're inside a DnD dispatch
    // callback so dnd_api->accept can recognise valid calls.
    // _drag_source_active is true while a begin_drag OS drag loop spins;
    // it blocks recursive begin_drag from non-DnD callbacks (timers,
    // animation ticks) firing in idle gaps during the active drag.
    uint32_t _current_drop_target  = UINT32_MAX;
    uint32_t _last_accepted_action = 0;  // neui_dnd_action_t
    bool     _in_dnd_dispatch      = false;
    bool     _drag_source_active   = false;
    // Frame-local top-left of `_current_drop_target`, captured at the
    // last find. The shared dispatch templates use it so MOVE / LEAVE on
    // the same widget produce widget-local coords without re-walking.
    int      _current_drop_abs_x   = 0;
    int      _current_drop_abs_y   = 0;

    // Drop hit-test + event dispatch. `frame_widget_idx` identifies the
    // top-level frame whose IDropTarget / NSDraggingDestination fired the
    // OS callback; the dispatcher walks descendants (and the frame itself)
    // for the deepest drop_target whose accepted_mimes intersects
    // `formats`, and fires the corresponding NEUI_EVENT_DND_*.
    // frame_local_x / _y are logical pixels relative to the frame's
    // client-area top-left. Returns the cached accepted action (0 = NONE).
    uint32_t dispatch_dnd_enter(uint32_t frame_widget_idx,
                                 int frame_local_x, int frame_local_y,
                                 const char* const* formats, uint32_t formats_count,
                                 uint32_t suggested_action,
                                 uint32_t buttonmap);
    uint32_t dispatch_dnd_move (uint32_t frame_widget_idx,
                                 int frame_local_x, int frame_local_y,
                                 const char* const* formats, uint32_t formats_count,
                                 uint32_t suggested_action,
                                 uint32_t buttonmap);
    void     dispatch_dnd_leave();
    uint32_t dispatch_dnd_drop (uint32_t frame_widget_idx,
                                 int frame_local_x, int frame_local_y,
                                 const char* const* formats, uint32_t formats_count,
                                 uint32_t suggested_action,
                                 uint32_t buttonmap,
                                 neui_detail::DataItem* drop_item);

    // Adapter members consumed by the shared dnd_dispatch_* templates
    // (hosts/shared/dnd_dispatch.h) backing the dispatch_dnd_* bodies.
    // Defined in host.cpp: the find wraps the hit_test-based walker and
    // reports the matched widget's cached abs_x/abs_y; the send builds
    // the NEUI_EVENT_DND_* payload with widget-local coords.
    uint32_t dnd_find_target(uint32_t frame_widget_idx, int x, int y,
                              const char* const* formats, uint32_t count,
                              int& out_abs_x, int& out_abs_y);
    void dnd_send_event(uint32_t widget_idx, uint32_t event_type,
                         int frame_x, int frame_y, int abs_x, int abs_y,
                         const char* const* formats, uint32_t count,
                         uint32_t suggested, uint32_t buttonmap,
                         neui_data_item_t data_item);

    // iOS drag-source resolution. iOS drags are gesture-driven (a
    // UIDragInteraction delegate responds to the system long-press-drag),
    // not initiated programmatically via begin_drag - so the interaction
    // hit-tests the widget under the gesture and asks here whether it is a
    // drag source. A widget is a drag source when it carries a DRAG_SOURCE
    // behavior asset; this resolves that handler's DataItem (from the
    // drag_data_key attr) + allowed_actions. Returns the deepest such widget
    // under (frame_local_x, frame_local_y), or 0 if none. The resolved
    // DataItem id lives in this Session's _data_items store (transient -
    // the caller copies its bytes into an NSItemProvider, then it may be
    // released). `out_*` are only written when a drag source is found.
    uint32_t dnd_resolve_drag_source(uint32_t frame_widget_idx,
                                      int frame_local_x, int frame_local_y,
                                      neui_detail::DataItem* out_item,
                                      uint32_t& out_allowed_actions);

    // Write the negotiated DnD action back to a DRAG_SOURCE behavior's
    // result_attr (+ fire ATTR_CHANGED + invalidate), mirroring the desktop
    // DRAG_SOURCE result feedback. Called from the UIDragInteraction
    // session-did-end delegate once iOS reports the drop operation. No-op if
    // the widget has no DRAG_SOURCE handler or no result_attr configured.
    void dnd_report_drag_result(uint32_t widget_idx, uint32_t action);

    // Optional menu-item validation callback. Polled at WM_INITMENUPOPUP.
    neui_menu_client_t*             _menu_client               = nullptr;

    // Optional grid-cell-edit validation callback. Called when the user
    // commits an in-place cell edit (ENTER inside the editor).
    neui_grid_client_t*             _grid_client               = nullptr;

    // System-theme listener handle. The xpl host always tracks the system
    // theme; on_theme_changed invalidates every frame so paint pulls the
    // updated palette.
    uint32_t                        _theme_listener_handle     = 0;

    // Optional client-side theme-change callback (NEUI_API_THEME_CLIENT).
    neui_theme_client_t*            _theme_client              = nullptr;

    // Per-session attribute storage (NEUI_ATTR_THEME_MODE etc.).
    neui_detail::AttrBag            _session_attrs;

    // Effective palette derived from the system palette + this session's
    // NEUI_ATTR_THEME_MODE. Pointed at by the global
    // active_palette_override_ptr so paint code reads our effective
    // palette via current_palette() without needing a Session pointer.
    neui_detail::Palette            _effective_palette{};

    // Snapshot of _effective_palette taken at session creation and
    // refreshed only when NEUI_ATTR_THEME_MODE changes - not on system
    // theme flips. paint_frame swaps the global override to this for
    // frames whose NEUI_ATTR_FOLLOW_SYSTEM_THEME is 0 / unset, so those
    // frames render a stable palette independent of the OS theme.
    neui_detail::Palette            _frozen_palette{};

    // Recompute _effective_palette. When `from_mode_change` is true the
    // call originated from a NEUI_ATTR_THEME_MODE flip, so _frozen_palette
    // is refreshed and ALL frames are invalidated (even FOLLOW=0). When
    // false the call originated from a system theme listener and only
    // FOLLOW=1 frames are touched.
    void on_theme_changed(bool from_mode_change = false);
    void recompute_effective_palette();

    // Public accessor used by entry points that need to scope the
    // process-wide palette override to THIS session before reading
    // current_palette() (e.g. the platform frame-creation path).
    const neui_detail::Palette* effective_palette_ptr() const
    { return &_effective_palette; }
  };

  // -------------------------------------------------------------------------
  void register_host();

} // namespace xpl_host
