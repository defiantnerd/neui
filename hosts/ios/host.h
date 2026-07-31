#pragma once

// Native iOS host (neui.host.ios) - UIKit.
//
// MILESTONE 7. The native counterpart to hosts/macos/host.h, trimmed to the
// agreed v1 core subset (LABEL / BUTTON / INPUTBOX / MULTILINE / CHECKBOX /
// CHECKBOX3 / SLIDER / IMAGE / SECTION / CUSTOMDRAW + APPWINDOW / DIALOG /
// MENUBAR). GRID / TREEVIEW / COMBOBOX / LISTBOX / TABVIEW / DnD are phase-2
// stubs (see the TODO(ios phase 2) markers in widgets.mm / window.mm).
//
// Structure mirrors the macOS native host: Session + WidgetData + a slot-reused
// session registry. Native-handle fields are void* so this header is includable
// from pure C++ TUs; the .mm files bridge-cast through them. AppKit -> UIKit:
//   NSWindow*  -> UIWindow*   (frames; +1 retained via __bridge_retained)
//   NSView*    -> UIView*     (child controls / painted views)
//
// The shared portable logic (tree, attrs, asset store, paint helpers,
// behavior runtime, section scroll) is reused verbatim - identical to every
// other host.

#include <neui/neui.h>
#include "../shared/tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/theme_palette.h"
#include "../shared/behavior_runtime.h"
#include "../shared/widget_section_scroll.h"
#include "../shared/grid_model.h"
#include "../shared/widget_tabview.h"
#include "asset_manager_ios.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define NEUI_HOST_IOS "neui.host.ios"

namespace ios_host
{
  using neui_detail::Tree;

  void register_host();

  class Session;

  // Per-widget data - flat struct mirroring hosts/macos/host.h::WidgetData,
  // trimmed to the v1 core subset. Native handles are void* (bridge-cast in
  // the .mm files).
  class WidgetData
  {
  public:
    uint32_t    index = 0;
    const char* type  = nullptr;
    int x = 0, y = 0, width = 0, height = 0;
    bool isroot      = false;
    bool visible     = true;
    bool emit_events = false;
    bool enabled     = true;
    // Hover / press flags consumed by CUSTOMDRAW compound layer state filters.
    // pressed is set by the painted view's touch path; hovered only ever set
    // under an iPad pointer (no hover on touch).
    bool hovered     = false;
    bool pressed     = false;
    // DnD drop-target flag + accepted MIMEs. Stored so the API round-trips;
    // actual UIDropInteraction wiring is phase 2.
    bool drop_target = false;
    std::vector<std::string> accepted_mimes;

    void*    userdata    = nullptr;
    uint32_t owner_index = 0;

    // Native handles.
    //   native_window  - UIWindow*  (frames only; +1 retained)
    //   native_control - UIView* / UIControl* for child widgets / painted views
    void* native_window  = nullptr;
    void* native_control = nullptr;

    // Per-painted-widget CG render context (frame content view + each painted
    // child view own one). nullptr for native-control leaves.
    neui_render_ctx_t render_ctx = nullptr;
    uint32_t          dpi        = 96;

    Session* session    = nullptr;
    uint32_t session_id = 0;
    uint32_t widget_id  = 0;
    std::string text;

    std::unique_ptr<neui_detail::AttrBag> attrs;

    // NEUI_W_IMAGE source (see hosts/macos/host.h).
    neui_asset_t image_asset       = asset_none;
    bool         image_asset_owned = false;

    // CUSTOMDRAW compound + behavior assets.
    neui_asset_t compound_asset = asset_none;
    neui_asset_t                                  behavior_asset = asset_none;
    std::unique_ptr<neui_detail::BehaviorRuntime> behavior_rt;

    // LISTBOX / COMBOBOX items (model only in v1 - those types are stubbed).
    struct ItemEntry { std::string text; void* userdata = nullptr; };
    std::vector<ItemEntry> items;
    uint32_t               selected_item = UINT32_MAX;  // NEUI_ITEM_NONE

    // MENUBAR tree model (drives the hamburger UIMenu, built from the model in
    // window.mm via menu_ios.h). Mirror of the macOS host's tree_items.
    struct TreeNode {
      uint32_t    parent_id = 0;
      std::string text;
      void*       userdata  = nullptr;
      bool        enabled   = true;
      // TREEVIEW expand/collapse state (ignored for MENUBAR items). Lives on
      // the model so it survives a UITableView reloadData. Mirror of the xpl
      // host's TreeNode::expanded.
      bool        expanded  = false;
      // MENUBAR items only: UIMenu element checkmark (UIMenuElementStateOn).
      bool        checked   = false;
      uint32_t    menu_cmd  = 0;
      uint32_t    shortcut_mods = 0;
      uint32_t    shortcut_key  = NEUI_KEY_NONE;
    };
    std::unordered_map<uint32_t, TreeNode> tree_items;
    std::vector<uint32_t>                  tree_items_ordered;
    uint32_t                               next_tree_id      = 1;
    uint32_t                               selected_tree_item = UINT32_MAX;

    // TREEVIEW (native UITableView) flattened visible-row model: one entry per
    // currently-visible row (a parent + its expanded descendants), in display
    // order. tree_id is the model item id; depth drives the cell indent; has_kids
    // drives the disclosure chevron. Rebuilt from tree_items / expanded state by
    // tree_rebuild_visible_rows_ios on every mutation + expand toggle, so the
    // data source + didSelectRowAt agree on the row->item mapping across reloads.
    struct TreeVisRow { uint32_t tree_id; int depth; bool has_kids; };
    std::vector<TreeVisRow> tree_vis_rows;

    // SECTION scrolling state (lazy). The shared section-scroll runtime is used
    // exactly as on the other hosts; v1 wires the model + clip but the inertial
    // wheel/scrollbar drag are touch-driven follow-ups.
    std::unique_ptr<neui_detail::SectionScrollState> section_scroll_state;
    neui_detail::SectionLayout                       section_last_layout{};

    // GRID model (lazy). The native painted GRID reuses the shared grid_model.h
    // + widget_paint_grid.h + scroll_kinetics.h verbatim, exactly like the macOS
    // native host (which is the behavioral template). Allocated on first
    // grid_api touch / first paint.
    std::unique_ptr<neui_detail::GridModel> grid_model;
    // Inner body container view for a SECTION (UIView*, +1 retained), at the
    // body rect with layer.masksToBounds so children clip below the chip band.
    void* section_body_view = nullptr;

    // TABVIEW (NEUI_W_TABVIEW) state. Mirror of the macOS native host's
    // WidgetData tabview block. `tab_selected` is the active tab index (clamped
    // to the page count each paint); `tab_chips` caches the most recent chip
    // layout so a chip-tap can hit-test it; `tab_edge` is the resolved strip
    // edge from NEUI_ATTR_TAB_POSITION. A TABPAGE is a chip-less SECTION (it
    // reuses the section_* machinery + carries section_body_view); the parent
    // TABVIEW paints the chip strip + shows/hides pages.
    int                               tab_selected = 0;
    std::vector<neui_detail::TabChip> tab_chips;
    neui_detail::TabEdge              tab_edge = neui_detail::TabEdge::Top;
    std::vector<float>                tab_label_widths; // cached chip-label measurements
    uint64_t                          tab_label_sig = 0; // signature the cache was measured at
    // Last selection / page count page geometry was applied for, so a plain
    // repaint can skip the (subtree-reflowing) re-apply when nothing that
    // affects page geometry changed.
    int                               tab_applied_selected = -1;
    int                               tab_applied_count    = -1;
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

    // Tree manipulation (pure C++, defined in widgets.mm).
    neui_widget_t widget_create(neui_widget_t parent, const char* type,
                                int x, int y, int width, int height,
                                void* userdata);
    void          widget_destroy(neui_widget_t widget);
    void          widget_show(neui_widget_t widget);

    // Invalidate every CUSTOMDRAW widget whose compound_asset matches.
    void invalidate_widgets_with_compound(uint32_t asset_id);

    // Theme flip (dark/light): mark EVERY live widget dirty (so nested painted
    // views inside SECTION / TABVIEW containers repaint, not just the content
    // view's direct subviews) and re-apply the palette-derived colours that
    // UIKit can't auto-update (CGColor border snapshots). Defined in window.mm
    // because it touches UIView. Mirrors the xpl host's whole-frame repaint on
    // a flip; caller gates it on NEUI_ATTR_FOLLOW_SYSTEM_THEME.
    void invalidate_all_for_theme_change();

    // Walk parents to the nearest scrolling SECTION ancestor + scroll to bring
    // the widget into view (NEUI_API_SCROLL::ensure_visible). Defined in
    // window.mm.
    void ensure_widget_visible(uint32_t widget_idx);

    Tree<WidgetData>      _widgets;
    neui_widget_client_t* _client_widget_api = nullptr;
    neui_client_t*        _client            = nullptr;
    void*                 _token             = nullptr;
    uint32_t              _session_id        = 0;

    IOSAssetManager           _asset_manager;
    neui_detail::DataItemStore _data_items;

    // Session-level attribute bag (NEUI_API_ATTRS set_session_int /
    // get_session_int). Backs host-reserved session keys such as
    // NEUI_IOS_CHECKBOX_STYLE; lazily allocated on first set.
    std::unique_ptr<neui_detail::AttrBag> _session_attrs;

    // DnD dispatch state. The dispatch_dnd_* path is wired via a
    // UIDropInteraction on the frame's content view (window.mm); the shared
    // dnd_dispatch state machine maintains these (mirror of the macOS host).
    uint32_t _current_drop_target  = UINT32_MAX;
    uint32_t _current_drop_abs_x   = 0;   // frame-local top-left of the target
    uint32_t _current_drop_abs_y   = 0;
    uint32_t _last_accepted_action = 0;
    bool     _in_dnd_dispatch      = false;
    bool     _drag_source_active   = false;

    // ---- DnD dispatch (mirror of the macOS native host) --------------------
    // The frame's content view (the sole UIDropInteraction owner) fires these
    // as the user drags; the framework hit-tests the widget tree in software.
    // x/y are logical px relative to the frame's client-area top-left.
    uint32_t dispatch_dnd_enter(uint32_t frame_widget_idx, int x, int y,
                                const char* const* formats, uint32_t count,
                                uint32_t suggested, uint32_t buttonmap);
    uint32_t dispatch_dnd_move (uint32_t frame_widget_idx, int x, int y,
                                const char* const* formats, uint32_t count,
                                uint32_t suggested, uint32_t buttonmap);
    void     dispatch_dnd_leave();
    uint32_t dispatch_dnd_drop (uint32_t frame_widget_idx, int x, int y,
                                const char* const* formats, uint32_t count,
                                uint32_t suggested, uint32_t buttonmap,
                                neui_detail::DataItem* drop_item);

    // Deepest visible+enabled drop_target under (frame_x, frame_y) whose
    // accepted_mimes intersects `formats`, falling back to the frame itself.
    // Accumulates parent-relative coords on the fly (mirror of
    // find_drop_target_in_frame_macos). Returns the widget index (0 = none)
    // and its frame-local top-left via out params.
    uint32_t find_drop_target_in_frame_ios(uint32_t frame_widget_idx,
                                           int frame_x, int frame_y,
                                           const char* const* formats,
                                           uint32_t formats_count,
                                           int& out_abs_x, int& out_abs_y);

    // Adapter members consumed by the shared dnd_dispatch_* templates.
    uint32_t dnd_find_target(uint32_t frame_widget_idx, int x, int y,
                             const char* const* formats, uint32_t count,
                             int& out_abs_x, int& out_abs_y)
    {
      return find_drop_target_in_frame_ios(frame_widget_idx, x, y,
                                           formats, count, out_abs_x, out_abs_y);
    }
    void dnd_send_event(uint32_t widget_idx, uint32_t event_type,
                        int frame_x, int frame_y, int abs_x, int abs_y,
                        const char* const* formats, uint32_t count,
                        uint32_t suggested, uint32_t buttonmap,
                        neui_data_item_t data_item);

    // iOS drag-source resolution (mirror of the xpl host). Resolve the widget
    // under (frame_x, frame_y) carrying a DRAG_SOURCE behavior asset; copy its
    // DataItem (from the drag_data_key attr) into `out_item` + report
    // allowed_actions. Returns the widget index (0 = not a drag source).
    uint32_t dnd_resolve_drag_source(uint32_t frame_widget_idx,
                                     int frame_x, int frame_y,
                                     neui_detail::DataItem* out_item,
                                     uint32_t& out_allowed_actions);
    // Write the negotiated DnD action back to a DRAG_SOURCE behavior's
    // result_attr (+ ATTR_CHANGED), mirroring the desktop result feedback.
    void dnd_report_drag_result(uint32_t widget_idx, uint32_t action);

    // Optional grid-cell-edit validation callback (fetched once; unused in v1
    // since GRID is stubbed).
    neui_grid_client_t* _grid_client = nullptr;

    // Optional client resource provider (NEUI_API_RESOURCE_CLIENT). Asked for
    // bytes before this host tries the bundle / disk. The live binding used by
    // the load paths is _asset_manager.resource_provider().
    neui_resource_client_t* _resource_client = nullptr;
  };

  // Process-wide session registry (defined in host.mm).
  extern std::vector<std::unique_ptr<Session>> sessions;

} // namespace ios_host
