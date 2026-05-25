#pragma once

#include <neui/neui.h>
#include "tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/theme_palette.h"
#include "asset_manager_w32.h"
#include <memory>
#include <string>
#include <unordered_map>

// Pull in windows.h explicitly before windowsx.h (windowsx.h does not auto-include it)
#define NOMINMAX
#define WINDOWS_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <CommCtrl.h>

#define NEUI_HOST_WIN32 "neui.host.win32"

namespace win32_host {

  using neui_detail::Tree;

  // Register the Win32 host with the neui core registry.
  // Must be called before main() - WinMain does this automatically.
  void register_host();

  class Session;
  class WidgetData;  // forward - needed by paint_fn_t typedef below

  // Paint function for self-painted widgets hosted by the shared
  // "neui.painted" window class. Called from PaintedWndProc on WM_PAINT
  // after begin_frame and before end_frame. `wd` is the widget being
  // painted; w/h are the client-area dimensions in logical pixels;
  // focused is true when the HWND has OS keyboard focus.
  typedef void (*paint_fn_t)(struct neui_render_backend* backend,
                              void*                       ctx,    // neui_render_ctx_t
                              float w, float h,
                              WidgetData&                 wd,
                              bool                        focused);

  // Optional message hook for self-painted widgets. Called from
  // PaintedWndProc for mouse / wheel / focus / button events. Allows
  // widget-specific behaviour (e.g. knob drag-to-value) to live host-side
  // without growing a virtual widget hierarchy in the win32 host.
  // Return value is currently unused (reserved for future "consumed" flag).
  typedef void (*painted_msg_fn_t)(WidgetData& wd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);

  // Widget implementation data structure
  class WidgetData {
  public:
    uint32_t index = 0;
    const char* type = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool isroot = false;
    bool visible = true;
    bool emit_events = false;
    // True = HWND is left WS_ENABLED; false = EnableWindow(hwnd, FALSE).
    // Default true. When the HWND has not yet been created (deferred),
    // the flag is stored here and applied in create_child_windows.
    bool enabled = true;
    void* userdata = nullptr;
    // For DIALOG frames: tree index of the owner frame. 0 = no owner.
    uint32_t owner_index = 0;
    // Win32-specific
    HWND hwnd = nullptr;
    HFONT hfont = nullptr;   // DPI-scaled font; owned by frame windows (APPWINDOW/PLUGWINDOW)
    // Optional per-widget custom font, built from NEUI_ATTR_FONT_FAMILY /
    // NEUI_ATTR_FONT_SIZE / NEUI_ATTR_FONT_WEIGHT. When non-null the native
    // control is sent WM_SETFONT(custom_hfont) instead of the parent
    // frame's hfont. font_signature caches a hash of (family, size, weight,
    // dpi) so ensure_custom_font can no-op when nothing relevant changed.
    HFONT    custom_hfont    = nullptr;
    uint64_t font_signature  = 0;
    HICON native_icon = nullptr;  // owned icon for frame windows; freed in widget_destroy
    Session* session = nullptr;
    uint32_t session_id = 0;   // stored as uint32_t for packing math; wrap as neui_session_t when returning to public API
    uint32_t dpi = 96;
    bool mouse_tracked = false;
    bool has_subclass = false;
    wchar_t pending_surrogate = 0;  // high surrogate waiting for its low pair in WM_CHAR
    uint32_t widget_id = 0;    // stored as uint32_t for packing math; wrap as neui_widget_t when returning to public API
    std::string text;          // copy of the last set_text value; applied on HWND creation if set before show
    struct PendingItem { std::string text; void* userdata; };
    std::vector<PendingItem> pending_items;     // items queued before HWND exists
    uint32_t pending_selection = UINT32_MAX;   // selection queued before HWND exists (UINT32_MAX = none)
    int      pending_check = -1;               // check state queued before HWND exists (-1 = none)

    // Menu bar (NEUI_W_MENUBAR): HMENU created immediately; no HWND
    HMENU hmenu = nullptr;

    // Image (NEUI_W_IMAGE): sole widget-local source is the asset handle.
    // Two source kinds, mutually exclusive:
    //   * Client-supplied (widget_set_asset): client owns the handle and
    //     calls assets->destroy when done. image_asset_owned = false.
    //   * Widget-owned-from-path (widget_set_text): the widget allocated
    //     a W32AssetManager slot internally and must release it on
    //     re-set / set_asset / widget_destroy. image_asset_owned = true.
    // Invariant: image_asset_owned == true implies image_asset != asset_none
    // and the slot lives in this session's _asset_manager.
    neui_asset_t image_asset       = asset_none;
    bool         image_asset_owned = false;

    // CUSTOMDRAW (NEUI_W_CUSTOMDRAW): optional compound asset. When set
    // (non-asset_none and resolves to a NEUI_ASSET_KIND_COMPOUND in this
    // session's _asset_manager), the painted-widget WM_PAINT path renders
    // the compound's layer stack and suppresses the client's WIDGET_PAINT
    // event. asset_none = no compound; client paints via WIDGET_PAINT.
    neui_asset_t compound_asset    = asset_none;

    // Treeview item data
    struct TreeItemData {
      HTREEITEM   hitem    = nullptr;
      void*       userdata = nullptr;
      std::string text;
      bool        enabled  = true;
    };
    struct PendingTreeItem { uint32_t neui_id; uint32_t parent_neui_id; };
    std::unordered_map<uint32_t, TreeItemData>    tree_items;          // neui id → data
    std::unordered_map<uintptr_t, uint32_t>       tree_items_reverse;  // HTREEITEM (as uintptr_t) → neui id
    std::vector<PendingTreeItem>                  pending_tree_items;  // items buffered before HWND exists
    uint32_t                                      next_tree_id = 1;    // 0 reserved for root sentinel

    // Menu item data
    struct MenuItemData {
      HMENU       parent_hmenu  = nullptr;  // HMENU this item belongs to
      HMENU       submenu       = nullptr;  // non-null if this item opens a sub-menu (popup)
      UINT        cmd_id        = 0;        // 0 for popup items
      uint32_t    parent_item_id = 0;       // parent neui_item_t id; 0 = direct child of menu bar
      bool        enabled       = true;
      bool        is_separator  = false;
      void*       userdata      = nullptr;
      std::string text;
      // Typed shortcut binding. shortcut_key == NEUI_KEY_NONE → no shortcut.
      uint32_t    shortcut_mods = 0;
      uint32_t    shortcut_key  = 0;
      std::string shortcut;     // formatted display label, derived from mods/key
      // Built-in command binding (neui_command_t). 0 = no built-in routing.
      uint32_t    menu_cmd      = 0;
    };
    std::unordered_map<uint32_t, MenuItemData>    menu_items;            // neui id → data
    std::unordered_map<UINT, uint32_t>            menu_cmd_map;          // cmd_id → neui item id
    std::vector<uint32_t>                         menu_item_ids_ordered; // insertion order for navigation
    uint32_t                                      next_menu_item_id = 1;
    // Win32 WM_COMMAND wParam carries only LOWORD; cmd_ids must stay <= 0xFFFF
    // or they alias other items on dispatch. Control IDs live in [1, 0x7FFF]
    // (the lower half of the 16-bit space; see CreateChildHwnd). Menu cmd_ids
    // live in [0x8000, 0xFFFF] (the upper half) and are recycled via the free
    // list below so a churning menubar can't exhaust the upper half.
    UINT                                          next_menu_cmd_id  = 0x8000;
    std::vector<UINT>                             free_menu_cmd_ids;     // reusable cmd_ids freed by tree_remove
    // Owned accelerator table for this menubar (HACCEL).
    HACCEL                                        native_accel = nullptr;

    // Per-widget attribute bag (lazy-allocated).
    std::unique_ptr<neui_detail::AttrBag>         attrs;

    // Self-painted widget seam (NEUI_W_KNOB and future painted widgets).
    // For widgets hosted on the shared "neui.painted" class, paint_fn does
    // the actual drawing each WM_PAINT and painted_msg_fn handles
    // widget-specific input (e.g. knob drag). Both null for native-control
    // widgets.
    neui_render_ctx_t paint_ctx       = nullptr;
    paint_fn_t        paint_fn        = nullptr;
    painted_msg_fn_t  painted_msg_fn  = nullptr;

    // SECTION-only: brush matching the section's body fill, used by
    // PaintedWndProc's WM_CTLCOLORSTATIC / WM_CTLCOLORBTN so STATIC text
    // children (labels, checkbox/radio text) paint over the section bg
    // instead of the system default. Recreated lazily when the resolved
    // ARGB changes (palette flip or NEUI_ATTR_BACKGROUND edit). Freed in
    // widget_destroy.
    HBRUSH    section_ctl_bg_brush      = nullptr;
    uint32_t  section_ctl_bg_brush_argb = 0;

    // Drag state for value-bearing painted widgets. KNOB uses prev_angle
    // (frame-to-frame angular delta) and continuous (an unsnapped
    // accumulator that survives across STEPS-snap rounding). prev_x / prev_y
    // are the previous-frame cursor positions used by the vertical /
    // horizontal slider modes of the KNOB (NEUI_ATTR_KNOB_MODE). paint_drag_mode
    // caches the resolved mode at mouse-down so WM_MOUSEMOVE doesn't pay the
    // attribute-lookup cost; live changes apply on the next drag.
    bool  paint_dragging        = false;
    int   paint_drag_mode       = 0;
    int   paint_drag_prev_x     = 0;
    int   paint_drag_prev_y     = 0;
    float paint_drag_prev_angle = 0.0f;  // last cursor angle (rad) relative to centre (rotational mode)
    float paint_drag_continuous = 0.0f;  // continuous unsnapped value during drag
  };

  class Session {
  public:
    Session() = default;
    Session(neui_client_t* client, void* token);
    ~Session();

    void set_session_id(neui_session_t id) { _session_id = id.session; }
    void* get_token() const { return _token; }

    bool run();
    void endsession();

    // Widget API member functions
    neui_widget_t widget_create(neui_widget_t parent, const char* type, int x, int y, int width, int height, void* userdata);
    void widget_destroy(neui_widget_t widget);
    void widget_show(neui_widget_t widget);
    void widget_hide(neui_widget_t widget);
    void widget_set_pos(neui_widget_t widget, int x, int y, int width, int height);
    void widget_set_size(neui_widget_t widget, int width, int height);
    void widget_set_emit_events(neui_widget_t widget, bool enabled);
    void widget_set_text(neui_widget_t widget, const char* text);
    int  widget_get_text(neui_widget_t widget, char* buf, int buflen);
    neui_widget_t widget_get_first_child(neui_widget_t widget);
    neui_widget_t widget_get_next_sibling(neui_widget_t widget);
    void widget_set_focus(neui_widget_t widget);
    void               widget_set_owner(neui_widget_t dialog, neui_widget_t owner);
    void               widget_set_check(neui_widget_t widget, neui_check_state_t state);
    neui_check_state_t widget_get_check(neui_widget_t widget);
    void*              widget_get_native_handle(neui_widget_t widget);
    void               widget_set_asset(neui_widget_t widget, neui_asset_t asset);

    // Items API
    void     items_clear       (neui_widget_t widget);
    uint32_t items_add         (neui_widget_t widget, const char* text, void* userdata);
    void     items_remove      (neui_widget_t widget, uint32_t index);
    uint32_t items_count       (neui_widget_t widget);
    int      items_get_text    (neui_widget_t widget, uint32_t index, char* buf, int buflen);
    void     items_set_text    (neui_widget_t widget, uint32_t index, const char* text);
    void*    items_get_userdata(neui_widget_t widget, uint32_t index);
    uint32_t items_get_selected(neui_widget_t widget);
    void     items_set_selected(neui_widget_t widget, uint32_t index);

    // Tree API
    neui_item_t tree_add         (neui_widget_t widget, neui_item_t parent, const char* text, void* userdata);
    void        tree_remove      (neui_widget_t widget, neui_item_t item);
    void        tree_clear       (neui_widget_t widget);
    int         tree_get_text    (neui_widget_t widget, neui_item_t item, char* buf, int buflen);
    void        tree_set_text    (neui_widget_t widget, neui_item_t item, const char* text);
    void*       tree_get_userdata(neui_widget_t widget, neui_item_t item);
    void        tree_set_enabled (neui_widget_t widget, neui_item_t item, bool enabled);
    bool        tree_get_enabled (neui_widget_t widget, neui_item_t item);
    void        tree_set_shortcut(neui_widget_t widget, neui_item_t item, uint32_t modifiers, uint32_t key);
    neui_item_t tree_get_first_child (neui_widget_t widget, neui_item_t parent);
    neui_item_t tree_get_next_sibling(neui_widget_t widget, neui_item_t item);
    neui_item_t tree_get_selected(neui_widget_t widget);
    void        tree_set_selected(neui_widget_t widget, neui_item_t item);

    // Internal helpers
    WidgetData* get_widget(uint32_t index);
    bool dispatch_event(neui_event_t* event);  // returns client's onevent return value
    void create_child_windows(uint32_t parent_index);
    // Reposition + resize every descendant HWND of `parent_index` to match
    // `new_dpi` (in physical px = LogicalToPhysical(logical, new_dpi)),
    // refresh painted widgets' D2D context DPI, and rebuild section
    // regions. wd.dpi is updated on every descendant so subsequent
    // create_child_windows calls have the right scaling. Used by the
    // frame's WM_DPICHANGED handler.
    void cascade_dpi(uint32_t parent_index, UINT new_dpi);
    uint32_t get_dpi_for_widget(uint32_t index);
    HWND find_parent_hwnd(uint32_t widget_index);     // walk up tree to find nearest HWND
    HFONT find_parent_hfont(uint32_t widget_index);   // walk up tree to find nearest ancestor HFONT (frame font)
    bool dispatch_menu_event(UINT cmd_id);             // route WM_COMMAND (lParam==0) to menu items

    // Try to consume a message via this session's menubar accelerator
    // tables. Called from the message pump for each session before
    // TranslateMessage. Returns true if any accelerator handled the
    // message (caller should skip TranslateMessage / DispatchMessage).
    bool try_translate_accel(MSG* msg);

    // Invoke a built-in command (neui_command_t) on the focused HWND or
    // a specific widget. Returns true if the target consumed it.
    bool invoke_focused_command(uint32_t cmd);
    bool invoke_command(neui_widget_t widget, uint32_t cmd);
    // Non-mutating peer to invoke_focused_command - returns true if the
    // currently focused HWND would handle `cmd`. Used by WM_INITMENUPOPUP
    // to gray menu items whose bound command can't reach a consumer.
    bool can_focused_perform_command(uint32_t cmd);

    // Walk the widget tree and DestroyWindow every frame whose
    // owner_index points at owner_idx. Each owned frame's WM_DESTROY
    // re-enters synchronously. The owner's hwnd should be cleared
    // before calling so the owned dialogs' "re-enable owner" path
    // is skipped. Used for auto-close-children-when-owner-closes.
    void close_owned_frames(uint32_t owner_idx);

    // Auto-disable items in `popup` whose bound built-in command can't
    // reach a consumer right now. Called from WM_INITMENUPOPUP.
    void update_menu_popup(HMENU popup);

    // System-theme tracking. on_theme_changed is invoked from the shared
    // theme provider (after UISettings::ColorValuesChanged has been
    // marshalled to the UI thread). It walks frames flagged with
    // NEUI_ATTR_FOLLOW_SYSTEM_THEME and re-applies DWM dark mode +
    // invalidates so WM_CTLCOLOR / NM_CUSTOMDRAW / paint pull fresh
    // palette values.
    void on_theme_changed();

    // True if the frame owning `wd` has NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1.
    // Climbs the parent chain to the nearest frame.
    bool frame_follows_theme(WidgetData* wd);

    // Invalidate every CUSTOMDRAW widget in this session whose
    // compound_asset.id matches `asset_id`. Called from the compound API
    // mutators so visible widgets repaint when their backing compound
    // changes. asset_id is the full 32-bit handle id (session<<16 | slot).
    void invalidate_widgets_with_compound(uint32_t asset_id);

  public:
    // Per-session clipboard item store and (optional) listener registration.
    neui_detail::ClipboardItemStore _clipboard_items;
    neui_clipboard_client_t*        _clipboard_client          = nullptr;
    uint32_t                        _clipboard_listener_handle = 0;

    // Optional menu-item validation callback. Polled at WM_INITMENUPOPUP.
    neui_menu_client_t*             _menu_client               = nullptr;

    // System-theme listener handle (singleton listener in
    // theme_provider_win32.h). Registered in ctor, unregistered in dtor.
    uint32_t                        _theme_listener_handle     = 0;

    // Optional client-side theme-change callback. Fired from
    // on_theme_changed after the framework has invalidated frames so the
    // client can refresh its own custom drawing.
    neui_theme_client_t*            _theme_client              = nullptr;

    // Per-session attribute storage (NEUI_ATTR_THEME_MODE etc.). Session
    // attrs are independent of the per-widget AttrBag.
    neui_detail::AttrBag            _session_attrs;

    // Session-scoped asset table backing the public neui_asset_api_t
    // (NEUI_API_ASSETS). Loaded outside the paint loop; per-paint GPU
    // upload happens lazily inside paint_customdraw_w32's draw_asset
    // thunk. Released on session destroy via clear().
    W32AssetManager                 _asset_manager;

    // Effective palette derived from the system palette + this session's
    // NEUI_ATTR_THEME_MODE. Recomputed in recompute_effective_palette()
    // whenever the system theme or the mode changes; pointed at by the
    // process-wide active_palette_override_ptr so current_palette()
    // returns it for this session's paint paths.
    neui_detail::Palette            _effective_palette{};

    // Recompute _effective_palette from the current system palette and
    // the session's NEUI_ATTR_THEME_MODE; refresh the palette override.
    void recompute_effective_palette();

    // Public accessor used by entry points that need to scope the
    // process-wide palette override to THIS session before reading
    // current_palette() (apply_theme_to_frame_w32, WM_CTLCOLOR brush
    // lookup, etc.). See ScopedPaletteOverride.
    const neui_detail::Palette* effective_palette_ptr() const
    { return &_effective_palette; }

    // Read-only view of the owning session id. Used by widget-id packing /
    // validation in widgets.cpp.
    uint32_t session_id() const { return _session_id; }

  protected:
    Tree<WidgetData> _widgets;
    neui_widget_client_t* _client_widget_api = nullptr;
    neui_client_t* _client = nullptr;
    void* _token = nullptr;
    uint32_t _session_id = 0;   // stored as uint32_t for packing math; wrap as neui_session_t when returning to public API
    std::vector<uint32_t> _menubars;  // widget indices of MENUBAR widgets, for WM_COMMAND routing
  };

}
