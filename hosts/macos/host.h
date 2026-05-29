#pragma once

#include <neui/neui.h>
#include "../shared/tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/theme_palette.h"
#include "../shared/behavior_runtime.h"
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
    void*    userdata    = nullptr;
    uint32_t owner_index = 0;

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
    // tree_items maps neui id → data; tree_items_ordered preserves insertion
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

    // Widget API (subset filled per step in plans/native-macos-host.md).
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

    // Session-scoped clipboard item store backing the item-based half of
    // NEUI_API_CLIPBOARD (read / create_item / write / item_*_format).
    // v1 round-trips text/plain only. Mirror of the win32 host.
    neui_detail::ClipboardItemStore _clipboard_items;
  };

  // Process-wide session registry (defined in host.mm). Slot index + 1 is
  // the public session id; 0 is reserved for "invalid".
  extern std::vector<std::unique_ptr<Session>> sessions;

} // namespace macos_host
