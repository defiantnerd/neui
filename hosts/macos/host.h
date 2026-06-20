#pragma once

#include <neui/neui.h>
#include "../shared/tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/theme_palette.h"
#include "../shared/behavior_runtime.h"
#include "../shared/grid_model.h"
#include "../shared/widget_section_scroll.h"
#include "../shared/widget_tabview.h"
#include "asset_manager_macos.h"

#include <memory>
#include <string>
#include <vector>

#define NEUI_HOST_MACOS "neui.host.macos"

namespace macos_host
{
  using neui_detail::Tree;

  // Register the native macOS host with the neui core registry. Idempotent.
  // Called from neui_register_macoshost() (the forced-symbol-reference
  // function the example links to so the static lib's TU's actually pull in).
  void register_host();

  class Session;

  // Widget data - flat struct, mirror of hosts/win32/host.h::WidgetData.
  // Per-widget native handles are stored as void* so this header is includable
  // from .cpp callers; the .mm files cast through __bridge.
  class WidgetData
  {
  public:
    uint32_t    index = 0;
    const char* type  = nullptr;
    int x = 0, y = 0, width = 0, height = 0;
    bool isroot      = false;
    bool visible     = true;
    bool emit_events = false;
    // Stored only - no visual / input effect on macOS yet (TODO: wire
    // NSControl setEnabled: / per-painted-view dim).
    bool enabled     = true;
    // Hover / press flags consumed by CUSTOMDRAW compound layer state
    // filters (NEUI_LAYER_STATE_HOVERED / _PRESSED). Set by
    // NEUINativePaintedView's mouseEntered: / mouseExited: / mouseDown: /
    // mouseUp:. Native-control views leave these untouched (compound only
    // attaches to CUSTOMDRAW).
    bool hovered     = false;
    bool pressed     = false;
    // True = this widget accepts drag&drop drops. Default false. Set via
    // NEUI_API_DND set_drop_target. Independent of `enabled`.
    bool drop_target = false;
    // Optional MIME allow-list for drop hit-testing. Empty = "accept any".
    std::vector<std::string> accepted_mimes;
    void*    userdata    = nullptr;
    uint32_t owner_index = 0;
    // For modal DIALOG frames: true while widget_show is blocking in a
    // nested NSEvent pump. Cleared by the NSWindow's windowWillClose:
    // delegate so the pump exits and widget_show returns.
    bool     modal_pump_active = false;

    // Native handles. Filled in step 3 onward.
    //   native_window  - NSWindow*  (frames only; +1 retained via __bridge_retained)
    //   native_control - NSView* / NSControl* for child widgets
    //   native_scroll  - NSScrollView* hosting LISTBOX / TREEVIEW / MULTILINE
    void* native_window  = nullptr;
    void* native_control = nullptr;
    void* native_scroll  = nullptr;

    Session* session    = nullptr;
    uint32_t session_id = 0;
    uint32_t widget_id  = 0;
    std::string text;

    std::unique_ptr<neui_detail::AttrBag> attrs;

    // NEUI_W_IMAGE source: a bitmap asset resolved against the session's
    // _asset_manager (same handle space as NEUI_API_ASSETS). Set either by
    // a file path via set_text (allocates an internally-owned slot) or by a
    // client-supplied handle via set_asset. image_asset_owned == true means
    // this slot was allocated internally and must be released on re-set /
    // set_asset / destroy; false means the client owns it (we just drop the
    // reference). asset_none = no source. Mirror of the win32 host.
    neui_asset_t image_asset       = asset_none;
    bool         image_asset_owned = false;

    // CUSTOMDRAW (NEUI_W_CUSTOMDRAW): optional compound asset. When set
    // (non-asset_none and resolves to a NEUI_ASSET_KIND_COMPOUND in this
    // session's _asset_manager), the painted-view drawRect: renders the
    // compound's layer stack and suppresses the client's WIDGET_PAINT
    // event. asset_none = no compound; client paints via WIDGET_PAINT.
    neui_asset_t compound_asset = asset_none;

    // CUSTOMDRAW: optional behavior asset (NEUI_ASSET_KIND_BEHAVIOR). When
    // attached, the framework dispatches mouse / key / wheel events through
    // hosts/shared/behavior_runtime.h, which mutates target attrs in this
    // widget's AttrBag. See <neui/d/behavior.h>. Per-widget drag state is
    // lazy.
    neui_asset_t                                  behavior_asset = asset_none;
    std::unique_ptr<neui_detail::BehaviorRuntime> behavior_rt;

    // LISTBOX / COMBOBOX items. Used by widgets.mm's items api + by the
    // NSTableViewDataSource / NSPopUpButton population in window.mm.
    struct ItemEntry { std::string text; void* userdata = nullptr; };
    std::vector<ItemEntry> items;
    uint32_t               selected_item = UINT32_MAX;  // NEUI_ITEM_NONE

    // TREEVIEW state. Mirror of the xpl host's TreeviewWidget data:
    // tree_items maps neui id -> data; tree_items_ordered preserves insertion
    // order (NSOutlineView's child:ofItem: uses it for stable indices).
    struct TreeNode {
      uint32_t    parent_id = 0;
      std::string text;
      void*       userdata  = nullptr;
      bool        enabled   = true;
      // Menubar items only: a built-in command (NEUI_CMD_*) bound via
      // tree->set_menu_cmd. On activation, the menu pick routes this to the
      // focused widget first; 0 = no binding (client gets TREE_ITEM_ACTIVATED).
      uint32_t    menu_cmd  = 0;
    };
    std::unordered_map<uint32_t, TreeNode> tree_items;
    std::vector<uint32_t>                  tree_items_ordered;
    uint32_t                               next_tree_id      = 1;
    uint32_t                               selected_tree_item = UINT32_MAX;

    // GRID (NEUI_W_GRID) state - column model, row data, scroll state,
    // selection, column-resize / scrollbar drag state. Lazy-allocated;
    // every other widget pays a single pointer.
    std::unique_ptr<neui_detail::GridModel> grid_model;

    // SECTION scrolling state. Allocated lazily when
    // NEUI_ATTR_SCROLL_MODE != "none" via section_refresh_scroll_state_macos.
    // section_last_layout is the layout cached during the most recent
    // paint pass; mouse / wheel handlers on the painted view read it
    // for hit-testing, scrollbar drag, and kinetics.
    std::unique_ptr<neui_detail::SectionScrollState> section_scroll_state;
    neui_detail::SectionLayout                       section_last_layout{};

    // Inner body view for a scrolling SECTION (NEUISectionBodyView*, retained
    // via __bridge_retained). Sits at the section's body rect; the section's
    // tree children parent to it rather than the section painted view, so
    // AppKit's default subview clipping confines them to the body and they
    // can't overpaint the title-chip band or scrollbar gutter. Null for
    // non-scrolling sections (and every non-SECTION widget). Mirror of the
    // win32 host's section_body_hwnd.
    void*                                            section_body_view = nullptr;

    // TABVIEW (NEUI_W_TABVIEW) state. Mirror of the xpl host's TabViewWidget.
    // `tab_selected` is the active tab index (clamped to the page count each
    // paint); `tab_chips` caches the most recent chip layout so mouseDown can
    // hit-test it; `tab_edge` is the resolved strip edge from
    // NEUI_ATTR_TAB_POSITION. A TABPAGE is a chip-less SECTION (it reuses the
    // section_* machinery + carries section_scroll_state / section_body_view);
    // the parent TABVIEW paints the chip strip + shows/hides pages.
    int                              tab_selected = 0;
    std::vector<neui_detail::TabChip> tab_chips;
    neui_detail::TabEdge             tab_edge = neui_detail::TabEdge::Top;
    std::vector<float>               tab_label_widths; // cached chip-label measurements
    uint64_t                         tab_label_sig = 0; // signature the cache was measured at
    // Last selection / page count that page geometry was applied for, so a
    // plain repaint can skip the (subtree-reflowing) re-apply when nothing that
    // affects page geometry changed.
    int                              tab_applied_selected = -1;
    int                              tab_applied_count    = -1;
  };

  class Session
  {
  public:
    Session(neui_client_t* client, void* token);
    ~Session();

    void  set_session_id(neui_session_t id) { _session_id = id.session; }
    void* get_token() const                  { return _token; }
    uint32_t session_id() const              { return _session_id; }

    bool run();
    void endsession();

    bool dispatch_event(neui_event_t* event);

    // Walk parents of widget_idx to find the nearest scrolling SECTION
    // ancestor and scroll it to bring the widget into view. Used by the
    // public NEUI_API_SCROLL::ensure_visible. Per-widget focus is not
    // emitted on the macOS native host today (Tier B deferred), so the
    // Tab-into-off-screen-child auto-scroll is wired only on win32 + xpl.
    void ensure_widget_visible(uint32_t widget_idx);

    // Widget API.
    // Step 3 wires create / destroy / show for APPWINDOW; later widget
    // types extend the show / destroy switch in widgets.mm + window.mm.
    neui_widget_t widget_create(neui_widget_t parent, const char* type,
                                int x, int y, int width, int height,
                                void* userdata);
    void          widget_destroy(neui_widget_t widget);
    void          widget_show(neui_widget_t widget);

    // Invalidate every CUSTOMDRAW widget in this session whose
    // compound_asset.id matches `asset_id`. Called from the compound API
    // mutators so visible widgets repaint when their backing compound
    // changes. asset_id is the full 32-bit handle id (session<<16 | slot).
    void invalidate_widgets_with_compound(uint32_t asset_id);

    Tree<WidgetData>      _widgets;
    neui_widget_client_t* _client_widget_api = nullptr;
    neui_client_t*        _client            = nullptr;
    void*                 _token             = nullptr;
    uint32_t              _session_id        = 0;

    // Session-scoped asset table backing the public neui_asset_api_t
    // (NEUI_API_ASSETS). Loaded outside the paint loop; per-paint GPU
    // upload happens lazily inside the painter draw_asset thunk.
    // Released on session destroy via clear().
    MacOSAssetManager     _asset_manager;

    // Session-scoped data-item store. Backs the item-based half of
    // NEUI_API_CLIPBOARD (read / create_item / write / item_*_format) and
    // (later) transient DnD drop payloads.
    neui_detail::DataItemStore _data_items;

    // DnD dispatch state. See hosts/crossplatform/host.h. The
    // NEUINativeContentView's NSDraggingDestination protocol methods
    // call dispatch_dnd_* on this Session as the user drags.
    uint32_t _current_drop_target  = UINT32_MAX;
    uint32_t _last_accepted_action = 0;
    bool     _in_dnd_dispatch      = false;
    bool     _drag_source_active   = false;
    // Frame-local top-left of the current drop target, cached so MOVE / LEAVE
    // produce widget-local coords without re-walking the tree. Mirror of the
    // win32 native host.
    int      _current_drop_abs_x   = 0;
    int      _current_drop_abs_y   = 0;

    // Walk the frame's descendants for the deepest visible+enabled drop_target
    // under (frame_x, frame_y) whose accepted_mimes intersect `formats`, else
    // fall back to the frame itself. Returns the widget index (0 = none) and,
    // via out params, that widget's frame-local top-left. Mirror of
    // hosts/win32/host.cpp::find_drop_target_in_frame_w32.
    uint32_t find_drop_target_in_frame_macos(uint32_t frame_widget_idx,
                                             int frame_x, int frame_y,
                                             const char* const* formats,
                                             uint32_t formats_count,
                                             int& out_abs_x, int& out_abs_y);

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
    uint32_t dnd_find_target(uint32_t frame_widget_idx, int x, int y,
                              const char* const* formats, uint32_t count,
                              int& out_abs_x, int& out_abs_y)
    {
      return find_drop_target_in_frame_macos(frame_widget_idx, x, y,
                                              formats, count,
                                              out_abs_x, out_abs_y);
    }
    void dnd_send_event(uint32_t widget_idx, uint32_t event_type,
                         int frame_x, int frame_y, int abs_x, int abs_y,
                         const char* const* formats, uint32_t count,
                         uint32_t suggested, uint32_t buttonmap,
                         neui_data_item_t data_item);  // defined in host.mm

    // Optional grid-cell-edit validation callback (NEUI_API_GRID_CLIENT).
    // Fetched once at session create time; called when the user commits a
    // grid in-place cell edit.
    neui_grid_client_t* _grid_client = nullptr;
  };

  // Process-wide session registry (defined in host.mm). Slot index + 1 is
  // the public session id; 0 is reserved for "invalid".
  extern std::vector<std::unique_ptr<Session>> sessions;

} // namespace macos_host
