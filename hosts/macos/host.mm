// Native macOS host - Session lifecycle + interface routing + register_host.
//
// Step 1 scaffold. Mirror of hosts/win32/host.cpp. The actual session
// implementation (NSWindow lifecycle, message pump, widget management)
// lands in steps 3+ per plans/native-macos-host.md.

#include "host.h"

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
  // DnD dispatch. v1 reports drops at the frame level: NEUINativeContentView
  // is the NSDraggingDestination; the framework dispatches to the frame
  // widget if it has drop_target=true and accepted_mimes match. Child
  // NSViews (painted views / native controls) do not yet opt-in as
  // independent drop destinations.

  static bool dnd_formats_match_macos(const std::vector<std::string>& accepted,
                                       const char* const* formats,
                                       uint32_t formats_count)
  {
    if (accepted.empty()) return true;
    if (!formats || formats_count == 0) return false;
    for (auto& want : accepted) {
      for (uint32_t i = 0; i < formats_count; ++i) {
        if (formats[i] && want == formats[i]) return true;
      }
    }
    return false;
  }

  static void send_dnd_event_macos(Session* s, uint32_t widget_idx,
                                    neui_event_type_t type,
                                    int x, int y,
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
    ev.data.dnd.x             = x;
    ev.data.dnd.y             = y;
    ev.data.dnd.buttonmap     = buttonmap;
    ev.data.dnd.formats       = formats;
    ev.data.dnd.formats_count = formats_count;
    ev.data.dnd.data          = data_item;
    ev.data.dnd.suggested_action = suggested;
    s->_in_dnd_dispatch = true;
    client_api->onevent(s->_token, &ev);
    s->_in_dnd_dispatch = false;
  }

  uint32_t Session::dispatch_dnd_enter(uint32_t frame_widget_idx,
                                        int x, int y,
                                        const char* const* formats,
                                        uint32_t count,
                                        uint32_t suggested,
                                        uint32_t buttonmap)
  {
    uint32_t idx = 0;
    if (frame_widget_idx != 0 && _widgets.exists(frame_widget_idx)) {
      auto& wd = _widgets[frame_widget_idx];
      if (wd.drop_target &&
          dnd_formats_match_macos(wd.accepted_mimes, formats, count))
        idx = frame_widget_idx;
    }
    _current_drop_target = idx;
    _last_accepted_action = 0;
    if (idx == 0) return 0;
    send_dnd_event_macos(this, idx, NEUI_EVENT_DND_ENTER, x, y,
                         formats, count, suggested, buttonmap,
                         neui_data_item_none);
    return _last_accepted_action;
  }

  uint32_t Session::dispatch_dnd_move(uint32_t frame_widget_idx,
                                       int x, int y,
                                       const char* const* formats,
                                       uint32_t count,
                                       uint32_t suggested,
                                       uint32_t buttonmap)
  {
    uint32_t idx = 0;
    if (frame_widget_idx != 0 && _widgets.exists(frame_widget_idx)) {
      auto& wd = _widgets[frame_widget_idx];
      if (wd.drop_target &&
          dnd_formats_match_macos(wd.accepted_mimes, formats, count))
        idx = frame_widget_idx;
    }
    if (idx != _current_drop_target) {
      if (_current_drop_target != 0 && _current_drop_target != UINT32_MAX &&
          _widgets.exists(_current_drop_target)) {
        send_dnd_event_macos(this, _current_drop_target, NEUI_EVENT_DND_LEAVE,
                             x, y, nullptr, 0, 0, 0, neui_data_item_none);
      }
      _current_drop_target = idx;
      _last_accepted_action = 0;
      if (idx == 0) return 0;
      send_dnd_event_macos(this, idx, NEUI_EVENT_DND_ENTER, x, y,
                           formats, count, suggested, buttonmap,
                           neui_data_item_none);
      return _last_accepted_action;
    }
    if (idx == 0) return 0;
    send_dnd_event_macos(this, idx, NEUI_EVENT_DND_MOVE, x, y,
                         formats, count, suggested, buttonmap,
                         neui_data_item_none);
    return _last_accepted_action;
  }

  void Session::dispatch_dnd_leave()
  {
    if (_current_drop_target != 0 && _current_drop_target != UINT32_MAX &&
        _widgets.exists(_current_drop_target)) {
      send_dnd_event_macos(this, _current_drop_target, NEUI_EVENT_DND_LEAVE,
                           0, 0, nullptr, 0, 0, 0, neui_data_item_none);
    }
    _current_drop_target = UINT32_MAX;
    _last_accepted_action = 0;
  }

  uint32_t Session::dispatch_dnd_drop(uint32_t /*frame_widget_idx*/,
                                       int x, int y,
                                       const char* const* formats,
                                       uint32_t count,
                                       uint32_t suggested,
                                       uint32_t buttonmap,
                                       neui_detail::DataItem* drop_item)
  {
    if (_current_drop_target == 0 || _current_drop_target == UINT32_MAX ||
        !_widgets.exists(_current_drop_target)) {
      _current_drop_target = UINT32_MAX;
      _last_accepted_action = 0;
      return 0;
    }

    uint32_t item_id = 0;
    if (drop_item) {
      item_id = _data_items.allocate();
      auto* slot = _data_items.get(item_id);
      if (slot) {
        drop_item->for_each_format([&](const std::string& mime,
                                        const std::vector<uint8_t>& bytes) {
          slot->set_format(mime, bytes.data(),
                           static_cast<uint32_t>(bytes.size()));
        });
      } else {
        item_id = 0;
      }
    }

    send_dnd_event_macos(this, _current_drop_target, NEUI_EVENT_DND_DROP,
                         x, y, formats, count, suggested, buttonmap,
                         neui_data_item_t{ item_id });

    if (item_id) _data_items.release(item_id);

    uint32_t action = _last_accepted_action;
    _current_drop_target = UINT32_MAX;
    _last_accepted_action = 0;
    return action;
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
