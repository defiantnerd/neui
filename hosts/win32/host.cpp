#include <vector>
#include <memory>
#include <cstring>

#include "host.h"
#include "window.h"
#include "../../backends/d2d/d2d_backend.h"
#include "../shared/dnd_dispatch.h"
#include "../shared/win32/theme_provider_win32.h"
#include "../shared/win32/theme_brushes_win32.h"
#include "../shared/win32/dark_menu_win32.h"

/*
*       win32 host
*
*       This file implements the session handling.
*
*/
namespace win32_host
{

  std::vector<std::unique_ptr<Session>> sessions;

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
  extern neui_scroll_api_t    scroll_api;

  neui_session_t create_session(neui_client_t* client, void* token)
  {
    // creates a new session and re-uses session slots if possible
    for (size_t idx = 0; idx < sessions.size(); ++idx) {
      if (sessions[idx] == nullptr) {
        sessions[idx] = std::make_unique<Session>(client, token);
        neui_session_t sess = { static_cast<uint32_t>(idx + 1) };
        sessions[idx]->set_session_id(sess);
        return sess;
      }
    }
    sessions.push_back(std::make_unique<Session>(client, token));
    neui_session_t sess = { static_cast<uint32_t>(sessions.size()) };
    sessions.back()->set_session_id(sess);
    return sess;
  }

  void* destroy(neui_session_t session)
  {
    uint32_t idx = session.session - 1;
    if (idx < sessions.size() && sessions[idx]) {
      void* token = sessions[idx]->get_token();
      sessions[idx] = nullptr;
      return token;
    }
    return nullptr;
  }

  void* get_interface(neui_session_t session, const char* iface)
  {
    if (!strcmp(iface, NEUI_API_WIDGETS))   return &win32_host::widgets_api;
    if (!strcmp(iface, NEUI_API_ITEMS))     return &win32_host::items_api;
    if (!strcmp(iface, NEUI_API_TREE))      return &win32_host::tree_api;
    if (!strcmp(iface, NEUI_API_ATTRS))     return &win32_host::attrs_api;
    if (!strcmp(iface, NEUI_API_CLIPBOARD)) return &win32_host::clipboard_api;
    if (!strcmp(iface, NEUI_API_COMMANDS))  return &win32_host::commands_api;
    if (!strcmp(iface, NEUI_API_ASSETS))    return &win32_host::asset_api;
    if (!strcmp(iface, NEUI_API_COMPOUND))  return &win32_host::compound_api;
    if (!strcmp(iface, NEUI_API_BEHAVIOR))  return &win32_host::behavior_api;
    if (!strcmp(iface, NEUI_API_GRID))      return &win32_host::grid_api;
    if (!strcmp(iface, NEUI_API_DND))       return &win32_host::dnd_api;
    if (!strcmp(iface, NEUI_API_SCROLL))    return &win32_host::scroll_api;
    return nullptr;
  }

  bool pump_once_fn(neui_session_t /*session*/)
  {
    // pump_once is global to the message queue, not per-session. We
    // accept the session parameter for ABI consistency but ignore it.
    return win32_host::pump_once();
  }

  bool run(neui_session_t session)
  {
    uint32_t idx = session.session - 1;
    if (idx < sessions.size()) {
      auto& s = sessions[idx];
      if (s) {
        return s->run();
      }
    }
    return true;
  }

  // Session infrastructure methods

  Session::Session(neui_client_t* client, void* token)
    : _client(client), _token(token)
  {
    _client_widget_api = static_cast<neui_widget_client_t*>(
      _client->get_interface(token, NEUI_API_WIDGETS));

    // Opt-in menu-item validation callback. Polled per item at popup-open.
    _menu_client = static_cast<neui_menu_client_t*>(
      _client->get_interface(token, NEUI_API_MENU_CLIENT));

    // Opt-in grid-cell-edit validation callback.
    _grid_client = static_cast<neui_grid_client_t*>(
      _client->get_interface(token, NEUI_API_GRID_CLIENT));

    // Bring up the system-theme provider (idempotent across sessions) and
    // subscribe so this session can re-apply DWM dark mode + invalidate
    // its frames when light/dark or accent changes.
    neui_detail::ensure_theme_provider_win32();
    _theme_listener_handle = neui_detail::register_theme_listener(
      [](void* tok) {
        auto* self = static_cast<Session*>(tok);
        if (self) self->on_theme_changed();
      },
      this);

    // Initialise this session's effective palette. We do NOT permanently
    // install &_effective_palette as the process-wide override - that
    // races "last-constructed wins" across multiple Sessions in the same
    // process. Every code path that reads current_palette() while
    // attached to this session scopes the override explicitly via
    // ScopedPaletteOverride (paint, on_theme_changed, theme-bearing
    // entry points in window.cpp).
    recompute_effective_palette();

    // Optional client-side theme-change callback.
    _theme_client = static_cast<neui_theme_client_t*>(
      _client->get_interface(token, NEUI_API_THEME_CLIENT));
  }

  Session::~Session()
  {
    // Asset manager owns CPU pixels plus a per-paint-ctx GPU upload
    // cache. By the time we get here every HWND has been destroyed and
    // PaintedWndProc::WM_DESTROY has already dropped per-ctx entries,
    // but a defensive clear() with the backend ensures any straggler
    // GPU bitmap is freed before the manager destructs.
    _asset_manager.clear(neui_d2d_backend::get_backend());

    if (_theme_listener_handle != 0) {
      neui_detail::unregister_theme_listener(_theme_listener_handle);
      _theme_listener_handle = 0;
    }
    // Defensive: in case a scope guard somewhere failed to restore (or
    // an extension installed a permanent pointer), clear the override
    // when it currently points into this session.
    if (neui_detail::active_palette_override_ptr() == &_effective_palette)
      neui_detail::set_active_palette_override(nullptr);
  }

  void Session::recompute_effective_palette()
  {
    using neui_detail::ColorRole;
    int mode = _session_attrs.get_int(NEUI_ATTR_THEME_MODE,
                                       NEUI_THEME_MODE_AUTO);
    const neui_detail::Palette& sys = neui_detail::mutable_current_palette();

    if (mode == NEUI_THEME_MODE_LIGHT) {
      _effective_palette = neui_detail::default_light_palette();
      _effective_palette.is_dark = false;
    } else if (mode == NEUI_THEME_MODE_DARK) {
      _effective_palette = neui_detail::default_dark_palette();
      _effective_palette.is_dark = true;
    } else {
      _effective_palette = sys;  // AUTO - copy system palette as-is
    }

    // For LIGHT / DARK, carry over the user's live system accent so the
    // selection / focused-row colour still reflects their preference.
    if (mode != NEUI_THEME_MODE_AUTO) {
      _effective_palette.colors[(size_t)ColorRole::accent]
        = sys.colors[(size_t)ColorRole::accent];
      _effective_palette.colors[(size_t)ColorRole::accent_text]
        = sys.colors[(size_t)ColorRole::accent_text];
      _effective_palette.colors[(size_t)ColorRole::accent_translucent]
        = sys.colors[(size_t)ColorRole::accent_translucent];
    }

    // Bump version so the brush cache invalidates and rebuilds with the
    // new colours on the next WM_CTLCOLOR* call.
    _effective_palette.version = sys.version * 4 + (uint32_t)mode + 1;

    // Tell uxtheme the app's preferred mode so popup menus (drop-downs
    // off the menu bar) get the right palette. This is per-session
    // strictly speaking, but uxtheme's app-wide preference is the only
    // hook for the popup HWND - multi-session apps will share the most
    // recently set preference, which is fine for the typical case.
    neui_detail::set_app_dark_preference(_effective_palette.is_dark);
  }

  static bool widget_is_frame(const WidgetData& wd)
  {
    return wd.type
        && (!strcmp(wd.type, NEUI_W_APPWINDOW)
         || !strcmp(wd.type, NEUI_W_PLUGWINDOW)
         || !strcmp(wd.type, NEUI_W_DIALOG));
  }

  bool Session::frame_follows_theme(WidgetData* wd)
  {
    // Walk children of root looking for the frame that owns this widget,
    // then check its follow attr. Tree<T> doesn't expose a public parent()
    // accessor, so we do it via release_order() (cheap; only called on
    // theme-change / WM_CTLCOLOR boundaries, not in hot paint loops).
    if (!wd) return false;
    auto in_subtree = [&](uint32_t root) {
      // BFS from root; return true if wd->index is found.
      std::vector<uint32_t> q;
      q.push_back(_widgets.child(root));
      while (!q.empty()) {
        uint32_t i = q.back(); q.pop_back();
        while (i != 0 && _widgets.exists(i)) {
          if (i == wd->index) return true;
          uint32_t c = _widgets.child(i);
          if (c) q.push_back(c);
          i = _widgets.next(i);
        }
      }
      return false;
    };

    // Find every frame at top level (children of root) and check whether
    // the target widget is a descendant of it.
    uint32_t f = _widgets.child(0);
    while (f != 0 && _widgets.exists(f)) {
      WidgetData& fwd = _widgets[f];
      if (widget_is_frame(fwd)) {
        if (f == wd->index || in_subtree(f)) {
          return fwd.attrs &&
                 fwd.attrs->get_int(NEUI_ATTR_FOLLOW_SYSTEM_THEME, 0) != 0;
        }
      }
      f = _widgets.next(f);
    }
    return false;
  }

  // Defined in window.cpp - re-applies DWM dark mode and any other
  // frame-level theme state. Called from on_theme_changed.
  void apply_theme_to_frame_w32(WidgetData& frame_wd);

  void Session::invalidate_widgets_with_compound(uint32_t asset_id)
  {
    // Depth-first walk - check every CUSTOMDRAW widget whose
    // compound_asset matches; InvalidateRect on its HWND triggers
    // PaintedWndProc::WM_PAINT which re-runs the compound paint pass.
    auto order = _widgets.release_order();
    for (uint32_t i : order) {
      if (i == 0 || !_widgets.exists(i)) continue;
      WidgetData& wd = _widgets[i];
      if (wd.type && strcmp(wd.type, NEUI_W_CUSTOMDRAW) == 0
          && wd.compound_asset.id == asset_id
          && wd.hwnd)
      {
        InvalidateRect(wd.hwnd, nullptr, FALSE);
      }
    }
  }

  void Session::on_theme_changed()
  {
    // Recompute this session's effective palette from the (just-updated)
    // system palette + our NEUI_ATTR_THEME_MODE. Scope the override to
    // this session for the duration of the work below so multi-session
    // listeners that fire in sequence don't read each other's palette.
    recompute_effective_palette();
    neui_detail::ScopedPaletteOverride scope(&_effective_palette);

    // Invalidate the cached HBRUSH table - it's keyed by palette version
    // and brush_for_role() will rebuild lazily on next WM_CTLCOLOR.
    neui_detail::invalidate_theme_brushes();
    // (set_app_dark_preference is called from inside
    //  recompute_effective_palette above - no need to repeat here.)

    // Walk every widget. For each frame, re-apply DWM dark mode (gated
    // inside apply_theme_to_frame_w32 by the follow attr). For child
    // native controls, re-apply the SetWindowTheme dark/light hint and
    // refresh treeview colours. Then invalidate so paint and WM_CTLCOLOR
    // repull palette.
    auto order = _widgets.release_order();
    for (uint32_t i : order) {
      if (i == 0 || !_widgets.exists(i)) continue;
      WidgetData& wd = _widgets[i];
      if (widget_is_frame(wd)) {
        apply_theme_to_frame_w32(wd);
      } else {
        apply_native_theme_w32(wd);
      }
      if (wd.hwnd) InvalidateRect(wd.hwnd, nullptr, TRUE);
    }

    // Notify the client (if it opted in) so it can refresh its own
    // custom drawing on top of the framework-managed widgets.
    if (_theme_client && _theme_client->onchange)
      _theme_client->onchange(_token);
  }

  bool Session::dispatch_event(neui_event_t* event)
  {
    if (_client_widget_api && _client_widget_api->onevent) {
      return _client_widget_api->onevent(_token, event);
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // DnD dispatch. v1 reports drops at the frame level: IDropTarget is
  // registered on the frame HWND and the framework dispatches to that
  // single widget if it has drop_target=true and accepted_mimes match.
  // Child widgets opt-ing in are not yet OS-level drop targets, so they
  // never receive a callback through this path.

  // MIME matching shared with the other hosts (dnd_dispatch.h).
  using neui_detail::dnd_formats_match;

  // Helper used by send_dnd_event_internal: subtracts the matched
  // widget's frame-local top-left to produce widget-local coords.
  void Session::send_dnd_event_internal(uint32_t widget_idx,
                                         uint32_t type_u32,
                                         int frame_x, int frame_y,
                                         int abs_x, int abs_y,
                                         const char* const* formats,
                                         uint32_t formats_count,
                                         uint32_t suggested, uint32_t buttonmap,
                                         neui_data_item_t data_item)
  {
    auto type = static_cast<neui_event_type_t>(type_u32);
    if (!_client_widget_api || !_client_widget_api->onevent) return;
    if (!_widgets.exists(widget_idx)) return;
    auto& wd = _widgets[widget_idx];
    neui_event_t ev = {};
    ev.type = type;
    ev.data.dnd.widget        = { wd.widget_id };
    ev.data.dnd.x             = frame_x - abs_x;
    ev.data.dnd.y             = frame_y - abs_y;
    ev.data.dnd.buttonmap     = buttonmap;
    ev.data.dnd.formats       = formats;
    ev.data.dnd.formats_count = formats_count;
    ev.data.dnd.data          = data_item;
    ev.data.dnd.suggested_action = suggested;

    _in_dnd_dispatch = true;
    _client_widget_api->onevent(_token, &ev);
    _in_dnd_dispatch = false;
  }

  // Recursive descendant walker. Stack-passes parent abs coords so each
  // widget's frame-local top-left is parent_abs + wd.x / wd.y. Updates
  // out_idx / out_abs_x / out_abs_y as deeper matches are found.
  static void find_drop_target_descendants_w32(neui_detail::Tree<WidgetData>& widgets,
                                                 uint32_t parent_idx,
                                                 int parent_abs_x, int parent_abs_y,
                                                 int frame_x, int frame_y,
                                                 const char* const* formats,
                                                 uint32_t formats_count,
                                                 uint32_t& out_idx,
                                                 int& out_abs_x, int& out_abs_y)
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
            find_drop_target_descendants_w32(widgets, idx,
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

  uint32_t Session::find_drop_target_in_frame_w32(uint32_t frame_widget_idx,
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
    int deepest_abs_x = 0;
    int deepest_abs_y = 0;
    find_drop_target_descendants_w32(_widgets, frame_widget_idx,
                                      0, 0,
                                      frame_x, frame_y,
                                      formats, formats_count,
                                      deepest, deepest_abs_x, deepest_abs_y);
    if (deepest != 0) {
      out_abs_x = deepest_abs_x;
      out_abs_y = deepest_abs_y;
      return deepest;
    }

    // Fallback: the frame itself.
    auto& frame_wd = _widgets[frame_widget_idx];
    if (frame_wd.drop_target &&
        dnd_formats_match(frame_wd.accepted_mimes, formats,
                                 formats_count)) {
      // Frame's client-area origin is (0, 0) in DnD-event coords.
      out_abs_x = 0;
      out_abs_y = 0;
      return frame_widget_idx;
    }
    return 0;
  }

  // ENTER / re-target / MOVE / LEAVE / DROP state machine shared with the
  // other hosts (hosts/shared/dnd_dispatch.h); only the hit-test walker
  // (find_drop_target_in_frame_w32 above) and the event-send plumbing
  // stay win32-local, reached via the dnd_find_target / dnd_send_event
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

  void Session::endsession()
  {
    // Find the root app window and post WM_CLOSE, which goes through the
    // existing NEUI_EVENT_APP_QUIT path so the client can still intervene.
    uint32_t idx = _widgets.child(0);
    while (idx != 0) {
      if (_widgets.exists(idx)) {
        auto& wd = _widgets[idx];
        if (wd.isroot && wd.hwnd) {
          PostMessage(wd.hwnd, WM_CLOSE, 0, 0);
          return;
        }
      }
      idx = _widgets.next(idx);
    }
  }

  static void endsession_fn(neui_session_t session)
  {
    uint32_t idx = (session.session & 0xffff) - 1;
    if (idx < sessions.size() && sessions[idx])
      sessions[idx]->endsession();
  }

};

#ifdef __cplusplus
extern "C" {
#endif

  static neui_api_t win_base_api = {
    NEUI_VERSION,

    win32_host::create_session,
    win32_host::destroy,
    win32_host::get_interface,
    win32_host::run,
    win32_host::endsession_fn,
    win32_host::pump_once_fn,
  };

#ifdef __cplusplus
}
#endif

void win32_host::register_host()
{
  neui_register(NEUI_HOST_WIN32, &win_base_api);
}

// Forced-symbol-reference for the linker. Mirror of neui_register_xplhost()
// in hosts/crossplatform/host.cpp. Called from neui_init()
// (src/neui.c) so clients don't have to know about it; also remains a
// publicly callable escape hatch for fine-grained registration.
extern "C" void neui_register_win32host()
{
  win32_host::register_host();
}
