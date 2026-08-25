// Native iOS host (neui.host.ios) - Session lifecycle + interface routing +
// register_host. Mirror of hosts/macos/host.mm.
//
// MILESTONE 7. The FOUNDATION-milestone registration-only stub is replaced
// with the real session bookkeeping + the full get_interface API-table
// routing. The UIKit lifecycle (UIWindow / UIViewController / painted view)
// lives in window.mm; the API tables in widgets.mm.

#include "host.h"

#import <UIKit/UIKit.h>   // [UIFont systemFontSize] for the painted-UI scale

// recompute_painted_ui_scale_ios() - the Dynamic-Type painted-UI scale helper.
// The theme provider's inline functions are ODR-safe across TUs; the seed-flag
// static lives inside ensure_theme_provider_ios (which only window.mm calls), so
// pulling this header in here just to set the scale is harmless.
#include "../shared/ios/theme_provider_ios.h"
#include "../shared/metrics.h"
#include "../shared/dnd_dispatch.h"  // dnd_formats_match + dnd_dispatch_* templates

#include <cstring>
#include <functional>

namespace ios_host
{
  // Process-wide session registry. Slot index + 1 is the public session id;
  // 0 is reserved for "invalid".
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
  extern neui_filter_api_t    filter_api;
  extern neui_grid_api_t      grid_api;
  extern neui_dnd_api_t       dnd_api;
  extern neui_scroll_api_t    scroll_api;
  extern neui_tabs_api_t      tabs_api;
  extern neui_notify_api_t    notify_api;

  // -------------------------------------------------------------------------

  Session::Session(neui_client_t* client, void* token)
    : _client(client), _token(token)
  {
    if (_client && _client->get_interface) {
      _client_widget_api = static_cast<neui_widget_client_t*>(
        _client->get_interface(token, NEUI_API_WIDGETS));
      _grid_client = static_cast<neui_grid_client_t*>(
        _client->get_interface(token, NEUI_API_GRID_CLIENT));

      // Opt-in client resource provider: asked for image / font / component /
      // sidecar bytes before this host tries the bundle or the disk.
      _resource_client = static_cast<neui_resource_client_t*>(
        _client->get_interface(token, NEUI_API_RESOURCE_CLIENT));
      if (_resource_client) {
        neui_detail::ResourceProvider provider;
        provider.client = _resource_client;
        provider.token  = token;
        _asset_manager.set_resource_provider(provider);
      }
    }
  }

  Session::~Session() = default;

  // UIApplicationMain owns the run loop on iOS; neui never owns / stops it, so
  // run() returns immediately and the client builds its UI from the scene
  // delegate, never from a blocking neui->run().
  bool Session::run() { return true; }
  void Session::endsession() {}

  bool Session::dispatch_event(neui_event_t* event)
  {
    if (_client_widget_api && _client_widget_api->onevent)
      return _client_widget_api->onevent(_token, event);
    return false;
  }

  // mark_widget_dirty_for_paint is defined in window.mm (touches UIView). The
  // compound invalidation walk stays pure C++ here.
  void mark_widget_dirty_for_paint(WidgetData& wd);

  // Defined in window.mm (UIKit): installs the iOS-real measure_text +
  // safe_area_insets seams into the shared NEUI_API_METRICS vtable.
  void install_metrics_seams_ios();

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

  // -------------------------------------------------------------------------
  // Drag & drop. The frame's content view (NEUINativeIOSContentView) owns the
  // UIDropInteraction + UIDragInteraction (window.mm); these are the pure-C++
  // hit-test + dispatch halves, mirror of hosts/macos/host.mm. The shared
  // dnd_dispatch state machine drives ENTER / re-target / MOVE / LEAVE / DROP.

  using neui_detail::dnd_formats_match;

  // Recursive descendant walker (parent-relative coord accumulation). Mirror of
  // find_drop_target_descendants_macos.
  static void find_drop_target_descendants_ios(
      neui_detail::Tree<WidgetData>& widgets, uint32_t parent_idx,
      int parent_abs_x, int parent_abs_y, int frame_x, int frame_y,
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
                dnd_formats_match(wd.accepted_mimes, formats, formats_count)) {
              out_idx = idx; out_abs_x = abs_x; out_abs_y = abs_y;
            }
            find_drop_target_descendants_ios(widgets, idx, abs_x, abs_y,
                                             frame_x, frame_y,
                                             formats, formats_count,
                                             out_idx, out_abs_x, out_abs_y);
          }
        }
      }
      idx = widgets.next(idx);
    }
  }

  uint32_t Session::find_drop_target_in_frame_ios(uint32_t frame_widget_idx,
                                                  int frame_x, int frame_y,
                                                  const char* const* formats,
                                                  uint32_t formats_count,
                                                  int& out_abs_x, int& out_abs_y)
  {
    out_abs_x = 0; out_abs_y = 0;
    if (frame_widget_idx == 0 || frame_widget_idx == UINT32_MAX) return 0;
    if (!_widgets.exists(frame_widget_idx)) return 0;

    uint32_t deepest = 0;
    int dax = 0, day = 0;
    find_drop_target_descendants_ios(_widgets, frame_widget_idx, 0, 0,
                                     frame_x, frame_y, formats, formats_count,
                                     deepest, dax, day);
    if (deepest != 0) { out_abs_x = dax; out_abs_y = day; return deepest; }

    auto& frame_wd = _widgets[frame_widget_idx];
    if (frame_wd.drop_target &&
        dnd_formats_match(frame_wd.accepted_mimes, formats, formats_count))
      return frame_widget_idx;
    return 0;
  }

  void Session::dnd_send_event(uint32_t widget_idx, uint32_t event_type,
                               int frame_x, int frame_y, int abs_x, int abs_y,
                               const char* const* formats, uint32_t count,
                               uint32_t suggested, uint32_t buttonmap,
                               neui_data_item_t data_item)
  {
    if (!_client_widget_api || !_client_widget_api->onevent) return;
    if (!_widgets.exists(widget_idx)) return;
    auto& wd = _widgets[widget_idx];
    neui_event_t ev = {};
    ev.type = static_cast<neui_event_type_t>(event_type);
    ev.data.dnd.widget        = { wd.widget_id };
    ev.data.dnd.x             = frame_x - abs_x;  // widget-local
    ev.data.dnd.y             = frame_y - abs_y;
    ev.data.dnd.buttonmap     = buttonmap;
    ev.data.dnd.formats       = formats;
    ev.data.dnd.formats_count = count;
    ev.data.dnd.data          = data_item;
    ev.data.dnd.suggested_action = suggested;

    _in_dnd_dispatch = true;
    _client_widget_api->onevent(_token, &ev);
    _in_dnd_dispatch = false;
  }

  uint32_t Session::dispatch_dnd_enter(uint32_t frame_widget_idx, int x, int y,
                                       const char* const* formats, uint32_t count,
                                       uint32_t suggested, uint32_t buttonmap)
  {
    return neui_detail::dnd_dispatch_enter(this, frame_widget_idx, x, y,
                                           formats, count, suggested, buttonmap);
  }
  uint32_t Session::dispatch_dnd_move(uint32_t frame_widget_idx, int x, int y,
                                      const char* const* formats, uint32_t count,
                                      uint32_t suggested, uint32_t buttonmap)
  {
    return neui_detail::dnd_dispatch_move(this, frame_widget_idx, x, y,
                                          formats, count, suggested, buttonmap);
  }
  void Session::dispatch_dnd_leave()
  {
    neui_detail::dnd_dispatch_leave(this);
  }
  uint32_t Session::dispatch_dnd_drop(uint32_t /*frame_widget_idx*/, int x, int y,
                                      const char* const* formats, uint32_t count,
                                      uint32_t suggested, uint32_t buttonmap,
                                      neui_detail::DataItem* drop_item)
  {
    return neui_detail::dnd_dispatch_drop(this, x, y, formats, count,
                                          suggested, buttonmap, drop_item);
  }

  // Resolve a widget's behavior asset (slot-vector lookup, mirror of the
  // widgets.mm resolve_behavior_ios body - inlined to avoid a cross-TU dep).
  static neui_detail::BehaviorAsset*
  dnd_behavior_asset_ios(Session* s, neui_asset_t a)
  {
    if (!s || a.id == asset_none.id) return nullptr;
    if (((a.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(a.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    return e->behavior.get();
  }

  uint32_t Session::dnd_resolve_drag_source(uint32_t frame_widget_idx,
                                            int frame_x, int frame_y,
                                            neui_detail::DataItem* out_item,
                                            uint32_t& out_allowed_actions)
  {
    // Deepest visible+enabled descendant under the gesture, then walk up the
    // parent chain for the first widget carrying a DRAG_SOURCE handler.
    uint32_t deepest = 0;
    // Match ANY visible+enabled widget under the point (not just drop_target),
    // so the walk picks the deepest hit; we then check it for a DRAG_SOURCE.
    std::function<void(uint32_t,int,int,int)> walk =
      [&](uint32_t parent, int ox, int oy, int depth) {
        (void)depth;
        uint32_t c = _widgets.child(parent);
        while (c != 0) {
          if (_widgets.exists(c)) {
            auto& cw = _widgets[c];
            if (cw.visible && cw.enabled) {
              int ax = ox + cw.x, ay = oy + cw.y;
              if (frame_x >= ax && frame_x < ax + cw.width &&
                  frame_y >= ay && frame_y < ay + cw.height) {
                deepest = c;
                walk(c, ax, ay, depth + 1);
              }
            }
          }
          c = _widgets.next(c);
        }
      };
    if (frame_widget_idx != 0 && frame_widget_idx != UINT32_MAX &&
        _widgets.exists(frame_widget_idx))
      walk(frame_widget_idx, 0, 0, 0);

    uint32_t cur = deepest;
    while (cur != 0 && cur != UINT32_MAX && _widgets.exists(cur)) {
      auto& wd = _widgets[cur];
      auto* ba = dnd_behavior_asset_ios(this, wd.behavior_asset);
      if (ba) {
        auto* H = neui_detail::behavior_find_kind_any(
            *ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
        if (H) {
          neui_data_item_t src = neui_data_item_none;
          if (wd.attrs && !H->drag_data_key.empty()) {
            int v = wd.attrs->get_int(H->drag_data_key, 0);
            if (v != 0) src.id = static_cast<uint32_t>(v);
          }
          if (src.id != 0 && out_item) {
            if (auto* slot = _data_items.get(src.id)) {
              slot->for_each_format([&](const std::string& mime,
                                        const std::vector<uint8_t>& bytes) {
                out_item->set_format(mime, bytes.data(),
                                     static_cast<uint32_t>(bytes.size()));
              });
            }
          }
          out_allowed_actions = H->allowed_actions ? H->allowed_actions
                                                    : (NEUI_DND_ACTION_COPY |
                                                       NEUI_DND_ACTION_MOVE);
          return cur;
        }
      }
      cur = _widgets.get_parent(cur);
    }
    return 0;
  }

  void Session::dnd_report_drag_result(uint32_t widget_idx, uint32_t action)
  {
    if (!_widgets.exists(widget_idx)) return;
    auto& wd = _widgets[widget_idx];
    auto* ba = dnd_behavior_asset_ios(this, wd.behavior_asset);
    if (!ba) return;
    auto* H = neui_detail::behavior_find_kind_any(
        *ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
    if (!H || H->result_attr.empty()) return;
    neui_detail::ensure_attrs(wd.attrs).set_int(H->result_attr,
                                                static_cast<int>(action));
    if (_client_widget_api && _client_widget_api->onevent) {
      neui_event_t ev{};
      ev.type                = NEUI_EVENT_ATTR_CHANGED;
      ev.data.attr.widget.id = wd.widget_id;
      ev.data.attr.attr_key  = H->result_attr.c_str();
      ev.data.attr.value     = static_cast<float>(action);
      _client_widget_api->onevent(_token, &ev);
    }
    mark_widget_dirty_for_paint(wd);
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
    if (!strcmp(iface, NEUI_API_FILTER))    return &filter_api;
    if (!strcmp(iface, NEUI_API_GRID))      return &grid_api;
    if (!strcmp(iface, NEUI_API_DND))       return &dnd_api;
    if (!strcmp(iface, NEUI_API_SCROLL))    return &scroll_api;
    if (!strcmp(iface, NEUI_API_TABS))      return &tabs_api;
    if (!strcmp(iface, NEUI_API_NOTIFY))    return &notify_api;
    if (!strcmp(iface, NEUI_API_METRICS))   return &neui_detail::k_metrics_api;
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
  static bool pump_once_fn(neui_session_t /*session*/) { return true; }

  void register_host()
  {
    // Scale the host-painted widgets' DEFAULT text + layout metrics to the
    // iOS Dynamic-Type body size so they match native UIKit controls AND follow
    // the user's Larger-Text / accessibility setting (live - recomputed on every
    // content-size change in -traitCollectionDidChange:). Canonical painted
    // default is 12px; at the default "Large" category body~17pt -> scale ~1.42,
    // preserving the previous look, larger categories grow it. Process-wide; the
    // xpl host on iOS sets the same value, and desktop hosts never touch it (so
    // they stay at 1.0, byte-for-byte unchanged). Client font/metric attrs still
    // override the scaled default.
    neui_detail::recompute_painted_ui_scale_ios();

    // Install the iOS-real NEUI_API_METRICS seams (UIFont measurement + the
    // frame view's safeAreaInsets) into the shared vtable. Desktop hosts leave
    // the shared desktop defaults in place.
    install_metrics_seams_ios();

    static neui_api_t base_api = {
      NEUI_VERSION,
      create_session,
      destroy,
      get_interface,
      run_fn,
      endsession_fn,
      pump_once_fn,
    };
    neui_register(NEUI_HOST_IOS, &base_api);
  }

} // namespace ios_host

// Forced-symbol-reference for the link line. Mirror of
// neui_register_macoshost() in hosts/macos/host.mm.
extern "C" void neui_register_ioshost()
{
  ios_host::register_host();
}
