#pragma once

#include <neui/neui.h>
#include "../shared/tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/edit_history.h"
#include "../shared/theme_palette.h"
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
    void*    userdata  = nullptr;
    uint32_t widget_id = 0;
    std::string text;

    // Native window handle (APPWINDOW / PLUGWINDOW / DIALOG).
    void* native_handle = nullptr;
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
      return px >= abs_x && px < abs_x + width &&
             py >= abs_y && py < abs_y + height;
    }

    // Called before this widget is removed from the tree.
    // Override to release type-specific platform resources (menus, etc.).
    virtual void on_destroy(Session* s) {}

    // Type classification - avoids strcmp in hot paths.
    virtual bool is_frame()   const { return false; }
    virtual bool is_menubar() const { return false; }
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

  // APPWINDOW / PLUGWINDOW - owns native_handle, render_ctx, dpi (in base)
  class FrameWidget : public WidgetData {
  public:
    bool is_frame() const override { return true; }
  };

  // Structural / display-only widgets - no extra fields, use base paint
  class LabelWidget     : public WidgetData {};
  class ButtonWidget    : public WidgetData {
  public:
    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx, bool is_focused) override;
  };

  // SECTION - non-interactive visual container. Paints a colored backdrop
  // behind its children; optionally draws `text` as a header label at the
  // top (alignment via NEUI_ATTR_ALIGN_TEXT). Background color from
  // NEUI_ATTR_BACKGROUND, falling back to ColorRole::panel_bg.
  class SectionWidget   : public WidgetData {
  public:
    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx, bool is_focused) override;
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

  // KNOB - circular value control with -135°..+135° sweep. Self-painted via
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
    uint32_t scroll_offset   = 0;
    bool     sb_dragging     = false;
    int      sb_drag_start_y = 0;
    uint32_t sb_drag_start_offset = 0;

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

    // Number of item rows that fit in the drop area (widget height minus collapsed bar).
    int max_drop_visible() const;
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

    // Scroll / scrollbar state (parallels ListItemsWidget).
    uint32_t scroll_offset        = 0;
    bool     sb_dragging          = false;
    int      sb_drag_start_y      = 0;
    uint32_t sb_drag_start_offset = 0;

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

    void paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
               bool is_focused) override;
    // Painted after the recursive child-widget descent so z>=0 compound
    // layers can sit above children. Default implementation no-op; only
    // CustomDrawWidget overrides because only CUSTOMDRAW carries compound
    // layers today. (Method declared on WidgetData via a hook in host.h.)
    void paint_after_children(neui_render_backend_t* backend,
                                neui_render_ctx_t ctx,
                                bool is_focused) override;
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
      bool        is_separator   = false;
      void*       userdata       = nullptr;
      std::string text;
      // Typed shortcut binding. shortcut_key == NEUI_KEY_NONE → no shortcut.
      uint32_t    shortcut_mods  = 0;
      uint32_t    shortcut_key   = 0;
      // Cached display label ("Ctrl+S") computed from mods/key. Stored so
      // make_menu_text doesn't need to reformat on every refresh.
      std::string shortcut;
      // Built-in command binding (neui_command_t). 0 = no built-in routing.
      uint32_t    menu_cmd       = 0;
    };
    std::unordered_map<uint32_t, MenuItemData> menu_items;
    std::unordered_map<uint32_t, uint32_t>     menu_cmd_map;       // cmd_id → neui item id
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

    // Called by the platform layer when WM_PAINT / equivalent fires.
    void paint_frame(neui_render_ctx_t ctx, uint32_t parent_index);

    // Notify the render context that the native window has been resized.
    void resize_render_ctx(uint32_t widget_index, uint32_t w, uint32_t h);

    // Called when the window's DPI changes.
    void on_dpi_changed(uint32_t widget_index, uint32_t new_dpi);

    uint32_t get_session_id() const { return _session_id; }

    // Software hit-test: returns the deepest visible widget with emit_events=true
    // whose bounds contain (x, y). Returns 0 if no match.
    uint32_t widget_at(float x, float y, uint32_t parent_idx);

    // Update the logically focused widget, firing focus events as needed.
    void set_focus(uint32_t new_idx);

    // Update the hovered widget, firing mouse enter/leave events.
    void set_hovered(uint32_t new_idx);

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

    // Per-session clipboard item store and (optional) listener registration.
    neui_detail::ClipboardItemStore _clipboard_items;
    neui_clipboard_client_t*        _clipboard_client          = nullptr;
    uint32_t                        _clipboard_listener_handle = 0;

    // Optional menu-item validation callback. Polled at WM_INITMENUPOPUP.
    neui_menu_client_t*             _menu_client               = nullptr;

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
