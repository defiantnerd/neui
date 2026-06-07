// Native macOS host - Session lifecycle + interface routing + register_host.
//
// Step 1 scaffold. Mirror of hosts/win32/host.cpp. The actual session
// implementation (NSWindow lifecycle, message pump, widget management)
// lands in steps 3+ per plans/native-macos-host.md.

#include "host.h"
#include "../shared/dnd_dispatch.h"

#include <cstring>

namespace macos_host
{
  // Process-wide session registry. Slot index + 1 is the public session id;
  // 0 is reserved for "invalid". Same shape as hosts/win32/host.cpp.
  std::vector<std::unique_ptr<Session>> sessions;

  // API tables defined in widgets.mm.
  extern neui_widget_api_t    widgets_api;
  extern neui_items_api_t     items_api;
  extern neui_tree_api_t      tree_api;
  extern neui_attr_api_t      attrs_api;
  extern neui_clipboard_api_t clipboard_api;
  extern neui_commands_api_t  commands_api;
  extern neui_asset_api_t     asset_api;
  extern neui_compound_api_t  compound_api;
  extern neui_behavior_api_t  behavior_api;
  extern neui_grid_api_t      grid_api;
  extern neui_dnd_api_t       dnd_api;

  // -------------------------------------------------------------------------

  Session::Session(neui_client_t* client, void* token)
    : _client(client), _token(token)
  {
    if (_client && _client->get_interface) {
      _client_widget_api = static_cast<neui_widget_client_t*>(
        _client->get_interface(token, NEUI_API_WIDGETS));
      _grid_client = static_cast<neui_grid_client_t*>(
        _client->get_interface(token, NEUI_API_GRID_CLIENT));
    }
  }

  Session::~Session() = default;

  // Session::run lives in window.mm (drives [NSApp run] + quit-on-last-frame).
  void Session::endsession() {}

  bool Session::dispatch_event(neui_event_t* event)
  {
    if (_client_widget_api && _client_widget_api->onevent)
      return _client_widget_api->onevent(_token, event);
    return false;
  }

  // -------------------------------------------------------------------------
  // DnD dispatch. NEUINativeContentView is the single NSDraggingDestination
  // for the frame (child painted views / native controls don't register their
  // own destinations); the framework hit-tests the neui widget tree to find
  // the deepest drop_target under the cursor, matching the win32 native + xpl
  // hosts. Coordinates arriving here are frame-local (content view is
  // isFlipped=YES, so already top-down).

  // MIME matching shared with the other hosts (dnd_dispatch.h).
  using neui_detail::dnd_formats_match;

  // (x, y) are frame-local; (abs_x, abs_y) is the target widget's frame-local
  // top-left, so the event carries widget-local coords. Mirror of the win32
  // host's send_dnd_event_internal.
  static void send_dnd_event_macos(Session* s, uint32_t widget_idx,
                                    neui_event_type_t type,
                                    int x, int y,
                                    int abs_x, int abs_y,
                                    const char* const* formats,
                                    uint32_t formats_count,
                                    uint32_t suggested, uint32_t buttonmap,
                                    neui_data_item_t data_item)
  {
    if (!s) return;
    auto* client_api = s->_client_widget_api;
    if (!client_api || !client_api->onevent) return;
    if (!s->_widgets.exists(widget_idx)) return;
    auto& wd = s->_widgets[widget_idx];
    neui_event_t ev = {};
    ev.type = type;
    ev.data.dnd.widget        = { wd.widget_id };
    ev.data.dnd.x             = x - abs_x;
    ev.data.dnd.y             = y - abs_y;
    ev.data.dnd.buttonmap     = buttonmap;
    ev.data.dnd.formats       = formats;
    ev.data.dnd.formats_count = formats_count;
    ev.data.dnd.data          = data_item;
    ev.data.dnd.suggested_action = suggested;
    s->_in_dnd_dispatch = true;
    client_api->onevent(s->_token, &ev);
    s->_in_dnd_dispatch = false;
  }

  // Recursive descendant walker. Stack-passes parent abs coords so each
  // widget's frame-local top-left is parent_abs + wd.x / wd.y. Updates
  // out_idx / out_abs_x / out_abs_y as deeper matches are found. Mirror of
  // hosts/win32/host.cpp::find_drop_target_descendants_w32.
  static void find_drop_target_descendants_macos(
      neui_detail::Tree<WidgetData>& widgets,
      uint32_t parent_idx,
      int parent_abs_x, int parent_abs_y,
      int frame_x, int frame_y,
      const char* const* formats, uint32_t formats_count,
      uint32_t& out_idx, int& out_abs_x, int& out_abs_y)
  {
    uint32_t idx = widgets.child(parent_idx);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& wd = widgets[idx];
        if (wd.visible) {
          int abs_x = parent_abs_x + wd.x;
          int abs_y = parent_abs_y + wd.y;
          if (frame_x >= abs_x && frame_x < abs_x + wd.width &&
              frame_y >= abs_y && frame_y < abs_y + wd.height) {
            if (wd.enabled && wd.drop_target &&
                dnd_formats_match(wd.accepted_mimes, formats,
                                         formats_count)) {
              out_idx   = idx;
              out_abs_x = abs_x;
              out_abs_y = abs_y;
            }
            find_drop_target_descendants_macos(widgets, idx,
                                                abs_x, abs_y,
                                                frame_x, frame_y,
                                                formats, formats_count,
                                                out_idx, out_abs_x, out_abs_y);
          }
        }
      }
      idx = widgets.next(idx);
    }
  }

  uint32_t Session::find_drop_target_in_frame_macos(uint32_t frame_widget_idx,
                                                     int frame_x, int frame_y,
                                                     const char* const* formats,
                                                     uint32_t formats_count,
                                                     int& out_abs_x,
                                                     int& out_abs_y)
  {
    out_abs_x = 0;
    out_abs_y = 0;
    if (frame_widget_idx == 0 || frame_widget_idx == UINT32_MAX) return 0;
    if (!_widgets.exists(frame_widget_idx)) return 0;

    // Walk descendants first (deepest match wins).
    uint32_t deepest = 0;
    int deepest_abs_x = 0, deepest_abs_y = 0;
    find_drop_target_descendants_macos(_widgets, frame_widget_idx,
                                        0, 0, frame_x, frame_y,
                                        formats, formats_count,
                                        deepest, deepest_abs_x, deepest_abs_y);
    if (deepest != 0) {
      out_abs_x = deepest_abs_x;
      out_abs_y = deepest_abs_y;
      return deepest;
    }

    // Fallback: the frame itself (its client-area origin is (0,0)).
    auto& frame_wd = _widgets[frame_widget_idx];
    if (frame_wd.drop_target &&
        dnd_formats_match(frame_wd.accepted_mimes, formats,
                                 formats_count)) {
      return frame_widget_idx;
    }
    return 0;
  }

  // Adapter for the shared dispatch templates - forwards to the
  // file-static send_dnd_event_macos above.
  void Session::dnd_send_event(uint32_t widget_idx, uint32_t event_type,
                                int frame_x, int frame_y, int abs_x, int abs_y,
                                const char* const* formats, uint32_t count,
                                uint32_t suggested, uint32_t buttonmap,
                                neui_data_item_t data_item)
  {
    send_dnd_event_macos(this, widget_idx,
                         static_cast<neui_event_type_t>(event_type),
                         frame_x, frame_y, abs_x, abs_y,
                         formats, count, suggested, buttonmap, data_item);
  }

  // ENTER / re-target / MOVE / LEAVE / DROP state machine shared with the
  // other hosts (hosts/shared/dnd_dispatch.h); only the hit-test walker
  // (find_drop_target_in_frame_macos above) and the event-send plumbing
  // stay macOS-local, reached via the dnd_find_target / dnd_send_event
  // adapter members in host.h.

  uint32_t Session::dispatch_dnd_enter(uint32_t frame_widget_idx,
                                        int x, int y,
                                        const char* const* formats,
                                        uint32_t count,
                                        uint32_t suggested,
                                        uint32_t buttonmap)
  {
    return neui_detail::dnd_dispatch_enter(this, frame_widget_idx, x, y,
                                            formats, count,
                                            suggested, buttonmap);
  }

  uint32_t Session::dispatch_dnd_move(uint32_t frame_widget_idx,
                                       int x, int y,
                                       const char* const* formats,
                                       uint32_t count,
                                       uint32_t suggested,
                                       uint32_t buttonmap)
  {
    return neui_detail::dnd_dispatch_move(this, frame_widget_idx, x, y,
                                           formats, count,
                                           suggested, buttonmap);
  }

  void Session::dispatch_dnd_leave()
  {
    neui_detail::dnd_dispatch_leave(this);
  }

  uint32_t Session::dispatch_dnd_drop(uint32_t /*frame_widget_idx*/,
                                       int x, int y,
                                       const char* const* formats,
                                       uint32_t count,
                                       uint32_t suggested,
                                       uint32_t buttonmap,
                                       neui_detail::DataItem* drop_item)
  {
    return neui_detail::dnd_dispatch_drop(this, x, y, formats, count,
                                           suggested, buttonmap, drop_item);
  }

  // -------------------------------------------------------------------------
  // neui_api_t shim functions.

  static neui_session_t create_session(neui_client_t* client, void* token)
  {
    for (size_t i = 0; i < sessions.size(); ++i) {
      if (!sessions[i]) {
        sessions[i] = std::make_unique<Session>(client, token);
        neui_session_t s = { static_cast<uint32_t>(i + 1) };
        sessions[i]->set_session_id(s);
        return s;
      }
    }
    sessions.push_back(std::make_unique<Session>(client, token));
    neui_session_t s = { static_cast<uint32_t>(sessions.size()) };
    sessions.back()->set_session_id(s);
    return s;
  }

  static void* destroy(neui_session_t session)
  {
    uint32_t i = session.session - 1;
    if (i < sessions.size() && sessions[i]) {
      void* tok = sessions[i]->get_token();
      sessions[i] = nullptr;
      return tok;
    }
    return nullptr;
  }

  static void* get_interface(neui_session_t /*sess*/, const char* iface)
  {
    if (!iface) return nullptr;
    if (!strcmp(iface, NEUI_API_WIDGETS))   return &widgets_api;
    if (!strcmp(iface, NEUI_API_ITEMS))     return &items_api;
    if (!strcmp(iface, NEUI_API_TREE))      return &tree_api;
    if (!strcmp(iface, NEUI_API_ATTRS))     return &attrs_api;
    if (!strcmp(iface, NEUI_API_CLIPBOARD)) return &clipboard_api;
    if (!strcmp(iface, NEUI_API_COMMANDS))  return &commands_api;
    if (!strcmp(iface, NEUI_API_ASSETS))    return &asset_api;
    if (!strcmp(iface, NEUI_API_COMPOUND))  return &compound_api;
    if (!strcmp(iface, NEUI_API_BEHAVIOR))  return &behavior_api;
    if (!strcmp(iface, NEUI_API_GRID))      return &grid_api;
    if (!strcmp(iface, NEUI_API_DND))       return &dnd_api;
    return nullptr;
  }

  static bool run_fn(neui_session_t session)
  {
    uint32_t i = session.session - 1;
    if (i < sessions.size() && sessions[i]) return sessions[i]->run();
    return true;
  }

  static void endsession_fn(neui_session_t session)
  {
    uint32_t i = session.session - 1;
    if (i < sessions.size() && sessions[i]) sessions[i]->endsession();
  }

  // pump_once is global to the message queue, not per-session - like the
  // win32 host, we accept the session arg for ABI consistency and ignore it.
  static bool pump_once_fn(neui_session_t /*session*/) { return true; }

  // -------------------------------------------------------------------------
  // Registration.

  void register_host()
  {
    static neui_api_t base_api = {
      NEUI_VERSION,
      create_session,
      destroy,
      get_interface,
      run_fn,
      endsession_fn,
      pump_once_fn,
    };
    neui_register(NEUI_HOST_MACOS, &base_api);
  }

} // namespace macos_host

// -------------------------------------------------------------------------
// Compound invalidation walk. Mirror of
// hosts/win32/host.cpp::Session::invalidate_widgets_with_compound -
// walks every CUSTOMDRAW widget owned by this session and marks any
// whose compound_asset.id matches as needing display. The actual
// [view setNeedsDisplay:YES] call has to cross into AppKit, but we
// stay in pure C++ here by writing through native_control directly.

@class NSView;

namespace macos_host
{
  // Forward decl - defined in widgets.mm / window.mm where AppKit is
  // imported. Pure C++ TU here forwards through it.
  extern void mark_widget_dirty_for_paint(WidgetData& wd);

  void Session::invalidate_widgets_with_compound(uint32_t asset_id)
  {
    auto order = _widgets.release_order();
    for (uint32_t i : order) {
      if (i == 0 || !_widgets.exists(i)) continue;
      WidgetData& wd = _widgets[i];
      if (!wd.type) continue;
      if (strcmp(wd.type, NEUI_W_CUSTOMDRAW) != 0) continue;
      if (wd.compound_asset.id != asset_id) continue;
      mark_widget_dirty_for_paint(wd);
    }
  }

} // namespace macos_host

// Forced-symbol-reference for the example's link line. Mirror of
// neui_register_xplhost() in hosts/crossplatform/host.cpp.
extern "C" void neui_register_macoshost()
{
  macos_host::register_host();
}
