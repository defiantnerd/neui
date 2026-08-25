#pragma once

#include <neui/neui.h>
#include "../shared/tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/edit_history.h"
#include "../shared/theme_palette.h"
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
    virtual bool is_menubar() const { return false; }
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
    bool modal_pump_active = false;
    // Active toast overlay (at most one per frame; replace-on-second-call).
    ToastState toast;
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

    bool is_menubar() const override { return true; }
    void on_destroy(Session* s) override;
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

    // Route a menu command (Win32 WM_COMMAND) to NEUI_EVENT_TREE_ITEM_ACTIVATED.
    bool dispatch_menu_event(uint32_t cmd_id);

    // Walk up the widget tree from widget_index and return the first
    // ancestor's native_handle (the frame HWND). Returns nullptr if none.
    void* find_parent_native_handle(uint32_t widget_index);

    // Logical client height (px) of the frame owning widget_index - i.e. the
    // height field of the first ancestor carrying a native_handle, kept in
    // sync with the window client area on WM_SIZE. Returns 0 when no frame
    // ancestor is found (callers treat 0 as "unknown / no constraint").
    int frame_client_height(uint32_t widget_index);

    // Move keyboard focus to the next (forward=true) or previous (false) widget
    // that has both tab_stop=true and visible=true. Wraps around.
    void focus_next(bool forward);

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

#ifdef NEUI_PLATFORM_LVGL
    // Retained-mode paint entries (LVGL Option C prototype). The LVGL
    // platform dispatches one widget at a time from per-object draw events;
    // these wrap the widget paint in the same palette-override bracket /
    // focus gating / PREUPDATE / disabled-dim mechanics paint_frame applies
    // during its whole-tree walk, so widget paint code sees an identical
    // environment on both paths.
    //  - after_children=false: PREUPDATE + wd.paint (parent-local coords)
    //  - after_children=true:  wd.paint_after_children (widget-local coords)
    void paint_widget_retained(neui_render_ctx_t ctx, uint32_t widget_index,
                               bool after_children);
    // The begin_frame(clear) substitute: fill the frame rect with the
    // frame's effective background colour.
    void paint_frame_background_retained(neui_render_ctx_t ctx,
                                         uint32_t frame_index);
    // Overlay pass drawn above every widget mirror (combo drop, popup menu,
    // toast), palette-bracketed like paint_frame's overlay tail.
    void paint_overlays_retained(neui_render_ctx_t ctx, uint32_t frame_index);
#endif

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

    // Update the hovered widget, firing mouse enter/leave events.
    void set_hovered(uint32_t new_idx);

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
    bool     _os_focused     = true;  // frame currently has OS keyboard focus
    uint32_t _open_combo     = 0;   // tree index of the currently open ComboBoxWidget, or 0

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

    // Overlay scrollbar drag state (managed by handle_combo_click / handle_combo_scroll_drag).
    bool     _combo_sb_dragging       = false;
    int      _combo_sb_drag_start_y   = 0;
    uint32_t _combo_sb_drag_start_off = 0;

    // Indices of MENUBAR widgets for WM_COMMAND routing.
    std::vector<uint32_t> _menubars;

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

    // Optional client resource provider (NEUI_API_RESOURCE_CLIENT). Asked for
    // bytes before the host tries the filesystem / embedded resources. Kept
    // here for symmetry with the other opt-in client interfaces; the live
    // binding used by the load paths is _asset_manager.resource_provider().
    neui_resource_client_t*         _resource_client           = nullptr;

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
