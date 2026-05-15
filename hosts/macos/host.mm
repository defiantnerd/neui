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

  // -------------------------------------------------------------------------

  Session::Session(neui_client_t* client, void* token)
    : _client(client), _token(token)
  {
    if (_client && _client->get_interface) {
      _client_widget_api = static_cast<neui_widget_client_t*>(
        _client->get_interface(token, NEUI_API_WIDGETS));
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

// Forced-symbol-reference for the example's link line. Mirror of
// neui_register_xplhost() in hosts/crossplatform/host.cpp.
extern "C" void neui_register_macoshost()
{
  macos_host::register_host();
}
