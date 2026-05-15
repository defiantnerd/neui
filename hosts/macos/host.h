#pragma once

#include <neui/neui.h>
#include "../shared/tree.h"
#include "../shared/attrs.h"
#include "../shared/clipboard_item.h"
#include "../shared/theme_palette.h"

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

    Tree<WidgetData>      _widgets;
    neui_widget_client_t* _client_widget_api = nullptr;
    neui_client_t*        _client            = nullptr;
    void*                 _token             = nullptr;
    uint32_t              _session_id        = 0;
  };

  // Process-wide session registry (defined in host.mm). Slot index + 1 is
  // the public session id; 0 is reserved for "invalid".
  extern std::vector<std::unique_ptr<Session>> sessions;

} // namespace macos_host
