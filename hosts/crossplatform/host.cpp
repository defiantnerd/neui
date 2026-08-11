#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <cmath>

#include "host.h"
#include "platform.h"
#include "../shared/dnd_dispatch.h"
#include "../shared/widget_paint_knob.h"
#include "../shared/widget_paint_section.h"
#include "../shared/widget_paint_tabview.h"
#include "../shared/widget_tabview_host.h"
#include "../shared/widget_paint_compound.h"
#include "../shared/widget_paint_grid.h"
#include "../shared/theme_palette.h"
#include "../shared/painter.h"
#include "../shared/widget_font.h"
#include "../shared/metrics.h"
#include "asset_manager.h"
#ifdef _WIN32
#include "../shared/win32/theme_provider_win32.h"
#include "../shared/win32/dark_menu_win32.h"
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace xpl_host
{
  // Forward decl - definition lives next to the compound paint code.
  static neui_detail::CompoundAsset* resolve_widget_compound(Session* s,
                                                              neui_asset_t a);

  // UTF-8 walking, word boundaries, and codepoint -> UTF-8 helpers used by
  // INPUTBOX / MULTILINE / GRID cell editor live in hosts/shared/text_edit.h
  // (te_utf8_char_len, te_utf8_prev_start, te_word_left, te_word_right,
  // te_word_bounds, te_encode_utf8). All call sites use those directly.

  // Render one or more underline segments below an IME composition string.
  // attrs (if non-empty) holds one CompAttr value per UTF-8 byte of comp_text;
  // contiguous runs of the same attribute become one underline segment whose
  // colour and thickness reflect the attribute kind. With no attrs, a single
  // 1px white underline spans the whole string. y_top is the top edge of the
  // underline strip (callers pass `caret_y + line_h - 2`).
  static void paint_composition_underline(neui_render_backend_t* backend,
                                           neui_render_ctx_t      ctx,
                                           float                  x_origin,
                                           float                  y_top,
                                           const std::string&     comp_text,
                                           const std::vector<uint8_t>& attrs,
                                           float                  font_size)
  {
    if (!backend->measure_text || !backend->fill_rect || comp_text.empty())
      return;
    int n = static_cast<int>(comp_text.size());

    auto colour_for_attr = [](uint8_t a) -> uint32_t {
      using neui_detail::ColorRole;
      switch (a) {
      case WidgetData::COMP_ATTR_TARGET_CONVERTED:    return neui_detail::color(ColorRole::ime_underline_target);
      case WidgetData::COMP_ATTR_TARGET_NOTCONVERTED: return neui_detail::color(ColorRole::ime_underline_target);
      case WidgetData::COMP_ATTR_CONVERTED:           return neui_detail::color(ColorRole::ime_underline_converted);
      case WidgetData::COMP_ATTR_INPUT_ERROR:         return neui_detail::color(ColorRole::ime_underline_error);
      case WidgetData::COMP_ATTR_INPUT:
      default:                                        return neui_detail::color(ColorRole::ime_underline);
      }
    };
    auto thickness_for_attr = [](uint8_t a) -> float {
      // Selected/target segments draw a 2px underline so the user can see
      // which clause they're currently operating on; everything else is 1px.
      return (a == WidgetData::COMP_ATTR_TARGET_CONVERTED ||
              a == WidgetData::COMP_ATTR_TARGET_NOTCONVERTED) ? 2.0f : 1.0f;
    };

    if (attrs.size() != static_cast<size_t>(n)) {
      // No (or malformed) attribute info - single underline in primary text colour.
      float w = backend->measure_text(ctx, comp_text.c_str(), n, font_size);
      backend->fill_rect(ctx, x_origin, y_top + 1.0f, w, 1.0f,
                          neui_detail::color(neui_detail::ColorRole::ime_underline));
      return;
    }

    int run_start = 0;
    while (run_start < n) {
      uint8_t a = attrs[static_cast<size_t>(run_start)];
      int run_end = run_start + 1;
      while (run_end < n && attrs[static_cast<size_t>(run_end)] == a) ++run_end;

      float x0 = x_origin + backend->measure_text(ctx, comp_text.c_str(),
                                                  run_start, font_size);
      float x1 = x_origin + backend->measure_text(ctx, comp_text.c_str(),
                                                  run_end, font_size);
      float thick = thickness_for_attr(a);
      backend->fill_rect(ctx, x0, y_top + (2.0f - thick),
                          x1 - x0, thick, colour_for_attr(a));
      run_start = run_end;
    }
  }

  // ---------------------------------------------------------------------------

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
  extern neui_filter_api_t    filter_api;
  extern neui_grid_api_t      grid_api;
  extern neui_dnd_api_t       dnd_api;
  extern neui_scroll_api_t    scroll_api;
  extern neui_tabs_api_t      tabs_api;
  extern neui_notify_api_t    notify_api;
  extern neui_embed_api_t     embed_api;
  extern neui_timer_api_t     timer_api;
  extern neui_pointer_api_t   pointer_api;
  extern neui_a11y_api_t      a11y_api;

  // Resolve a Session* from a 1-based session id (the upper 16 bits of a
  // widget id). Used by the iOS platform layer's NEUI_API_METRICS seam to walk
  // back to a frame's WidgetData. Returns nullptr for an unknown id.
  Session* session_by_id(uint32_t session_id)
  {
    if (session_id == 0 || session_id > sessions.size()) return nullptr;
    return sessions[session_id - 1].get();
  }

  // -------------------------------------------------------------------------
  // Session management

  static neui_session_t create_session(neui_client_t* client, void* token)
  {
    for (size_t idx = 0; idx < sessions.size(); ++idx) {
      if (!sessions[idx]) {
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

  static void* destroy(neui_session_t session)
  {
    uint32_t idx = session.session - 1;
    if (idx < sessions.size() && sessions[idx]) {
      void* token = sessions[idx]->get_token();
      sessions[idx] = nullptr;
      return token;
    }
    return nullptr;
  }

  static void* get_interface(neui_session_t /*session*/, const char* iface)
  {
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
    if (!strcmp(iface, NEUI_API_EMBED))     return &embed_api;
    if (!strcmp(iface, NEUI_API_TIMER))     return &timer_api;
    if (!strcmp(iface, NEUI_API_POINTER))   return &pointer_api;
    if (!strcmp(iface, NEUI_API_A11Y))      return &a11y_api;
    return nullptr;
  }

  static bool run_fn(neui_session_t session)
  {
    uint32_t idx = session.session - 1;
    if (idx < sessions.size() && sessions[idx])
      return sessions[idx]->run();
    return true;
  }

  static bool pump_once_fn(neui_session_t /*session*/)
  {
    // pump_once is global to the message queue, not per-session.
    return platform_pump_once();
  }

  static void endsession_fn(neui_session_t session)
  {
    uint32_t idx = (session.session & 0xffff) - 1;
    if (idx < sessions.size() && sessions[idx])
      sessions[idx]->endsession();
  }

  // Used by the platform layer to retrieve a session by ID.
  Session* get_session(neui_session_t sess)
  {
    uint32_t idx = (sess.session & 0xffff) - 1;
    if (idx < sessions.size())
      return sessions[idx].get();
    return nullptr;
  }

  // Number of session slots (including freed/null ones, so a caller iterating
  // 1..session_count() via session_by_id must null-check each). Used by the iOS
  // app-level menu-bar contribution to find the frontmost MENUBAR-bearing frame
  // across every live session.
  uint32_t session_count() { return (uint32_t)sessions.size(); }

  // -------------------------------------------------------------------------
  // Session implementation

  Session::Session(neui_client_t* client, void* token)
    : _client(client), _token(token)
  {
    _client_widget_api = static_cast<neui_widget_client_t*>(
      _client->get_interface(token, NEUI_API_WIDGETS));
    _backend = platform_get_backend();

    // Opt-in menu-item validation callback. Polled per item at popup-open.
    _menu_client = static_cast<neui_menu_client_t*>(
      _client->get_interface(token, NEUI_API_MENU_CLIENT));

    // Opt-in grid-cell-edit validation callback.
    _grid_client = static_cast<neui_grid_client_t*>(
      _client->get_interface(token, NEUI_API_GRID_CLIENT));

    // System-theme tracking. xpl host always follows the system theme;
    // the listener invalidates frames so paint pulls the current palette.
    // macOS provider is set up earlier in platform_init (which runs once at
    // host registration, before any session exists).
#ifdef _WIN32
    neui_detail::ensure_theme_provider_win32();
#endif
    _theme_listener_handle = neui_detail::register_theme_listener(
      [](void* tok) {
        auto* self = static_cast<Session*>(tok);
        if (self) self->on_theme_changed();
      },
      this);

    // Initialise this session's effective + frozen palettes. We
    // intentionally do NOT permanently install &_effective_palette as
    // the process-wide override: in a multi-session process that's a
    // "last constructed wins" race. Every code path that needs the
    // session's palette scopes it via ScopedPaletteOverride (paint_frame
    // manually, on_theme_changed + platform frame creation explicitly).
    // _frozen_palette starts as a snapshot of _effective_palette and only
    // updates on NEUI_ATTR_THEME_MODE flips, so FOLLOW=0 frames render
    // against a palette frozen at creation time.
    recompute_effective_palette();
    _frozen_palette = _effective_palette;

    // Optional client-side theme-change callback.
    _theme_client = static_cast<neui_theme_client_t*>(
      _client->get_interface(token, NEUI_API_THEME_CLIENT));
  }

  Session::~Session()
  {
    // Drop the native timer tick FIRST. Every platform's tick callback holds a
    // raw Session* and re-checks its registry before dereferencing, so a leaked
    // tick would be inert rather than a use-after-free - but leaving one armed
    // would still burn wakeups for the life of the process.
    _timers.clear();
    _timer_native_interval = 0;
    platform_timer_stop(this);

    // Leave relative pointer mode if a drag was still in flight. This un-hides
    // the pointer and re-associates it with the device; skipping it would leave
    // the whole machine with a cursor that does not follow the mouse.
    end_relative_pointer();

    // Hand the cursor back to the OS. A hidden pointer (NEUI_CURSOR_NONE)
    // outlives the session that hid it, and on macOS hide/unhide is a balanced
    // counter - so an embedded plugin editor torn down while the pointer was
    // over a cursor="none" widget would leave the DAW with NO POINTER for the
    // rest of the process's life.
    release_cursor();

    if (_theme_listener_handle != 0) {
      neui_detail::unregister_theme_listener(_theme_listener_handle);
      _theme_listener_handle = 0;
    }
    // Defensive: in case a scope guard somewhere failed to restore (or
    // an extension installed a permanent pointer), clear the override
    // when it currently points into this session.
    auto* cur = neui_detail::active_palette_override_ptr();
    if (cur == &_effective_palette || cur == &_frozen_palette)
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
      _effective_palette = sys;
    }

    // Carry over the live system accent in forced modes.
    if (mode != NEUI_THEME_MODE_AUTO) {
      _effective_palette.colors[(size_t)ColorRole::accent]
        = sys.colors[(size_t)ColorRole::accent];
      _effective_palette.colors[(size_t)ColorRole::accent_text]
        = sys.colors[(size_t)ColorRole::accent_text];
      _effective_palette.colors[(size_t)ColorRole::accent_translucent]
        = sys.colors[(size_t)ColorRole::accent_translucent];
    }

    _effective_palette.version = sys.version * 4 + (uint32_t)mode + 1;

#ifdef _WIN32
    // Tell uxtheme the app's preferred mode so popup menus follow.
    neui_detail::set_app_dark_preference(_effective_palette.is_dark);
#endif
  }

  void Session::on_theme_changed(bool from_mode_change)
  {
    // Recompute this session's effective palette from the (just-updated)
    // system palette + our NEUI_ATTR_THEME_MODE. Scope the override to
    // this session for the duration of the work below so multi-session
    // listeners that fire in sequence don't read each other's palette.
    recompute_effective_palette();
    neui_detail::ScopedPaletteOverride scope(&_effective_palette);

    // NEUI_ATTR_THEME_MODE flip: the user explicitly requested a new
    // mode, so refresh the frozen snapshot too. A system theme flip
    // leaves _frozen_palette untouched (FOLLOW=0 frames are frozen).
    if (from_mode_change)
      _frozen_palette = _effective_palette;

    // Invalidate frames that opted into theme tracking (see loop below).
    // The session-level effective palette has already been recomputed; we
    // just need to drive the side effects on opted-in frames.
#ifdef _WIN32
    // app-dark preference governs popup menus (uxtheme private API).
    // Mirrors the EFFECTIVE palette's is_dark so menus follow the
    // session's NEUI_ATTR_THEME_MODE forced light/dark, not the OS's.
    bool is_dark = neui_detail::current_palette().is_dark;
    neui_detail::set_app_dark_preference(is_dark);
#endif

    // System theme flip: only touch frames that opted into tracking via
    // NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1. Mode flip: every frame is
    // touched (the user explicitly switched modes, so even FOLLOW=0
    // frames - whose frozen palette just shifted - need to repaint).
    uint32_t idx = _widgets.child(0);
    while (idx != 0) {
      if (_widgets.exists(idx)) {
        auto& wd = _widgets[idx];
        if (wd.native_handle) {
          bool follow = wd.attrs &&
                        wd.attrs->get_int(NEUI_ATTR_FOLLOW_SYSTEM_THEME, 0) != 0;
          // DWM dark title bar + uxtheme HMENU dark stay gated by
          // FOLLOW=1 - those are active theme-tracking side effects that
          // FOLLOW=0 frames opt out of, regardless of what triggered
          // this call.
          if (follow) {
#ifdef _WIN32
            neui_detail::apply_dark_window_mode(
                static_cast<HWND>(wd.native_handle), is_dark);
            BOOL dark_attr = is_dark ? TRUE : FALSE;
            // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 on Win11 / late Win10,
            // 19 on early Win10 1809-1909. Try the new one first.
            if (FAILED(DwmSetWindowAttribute(
                    static_cast<HWND>(wd.native_handle),
                    20, &dark_attr, sizeof(dark_attr)))) {
              DwmSetWindowAttribute(
                  static_cast<HWND>(wd.native_handle),
                  19, &dark_attr, sizeof(dark_attr));
            }
#endif
          }
          // Invalidate when the frame's effective palette actually
          // shifted: FOLLOW=1 frames repaint on system theme flips,
          // FOLLOW=0 frames repaint on mode flips (their frozen
          // palette just changed).
          if (follow || from_mode_change)
            platform_invalidate(wd.native_handle);
        }
      }
      idx = _widgets.next(idx);
    }

    // Notify the client (if it opted in) so it can refresh its own
    // custom drawing on top of the framework-managed widgets.
    if (_theme_client && _theme_client->onchange)
      _theme_client->onchange(_token);
  }

  bool Session::run()
  {
    return platform_run();
  }

  void Session::endsession()
  {
    uint32_t idx = _widgets.child(0);
    while (idx != 0) {
      if (_widgets.exists(idx)) {
        auto& wd = _widgets[idx];
        if (wd.isroot && wd.native_handle) {
          platform_post_close(wd.native_handle);
          return;
        }
      }
      idx = _widgets.next(idx);
    }
  }

  // ---------------------------------------------------------------------------
  // Client timers (NEUI_API_TIMER). The deadline math lives in the portable
  // TimerTable; this layer only owns re-arming the single native tick.

  void Session::sync_timer_tick()
  {
    const uint32_t want = _timers.native_interval_ms();
    if (want == _timer_native_interval) return;   // already armed correctly
    _timer_native_interval = want;
    if (want == 0) platform_timer_stop(this);      // last timer went away
    else           platform_timer_start(this, want);
  }

  uint32_t Session::timer_add(uint32_t interval_ms)
  {
    const uint32_t id = _timers.add(interval_ms, platform_now_ms());
    if (id) sync_timer_tick();
    return id;
  }

  bool Session::timer_remove(uint32_t timer_id)
  {
    if (!_timers.remove(timer_id)) return false;
    // Safe during a tick: the table tombstones, and sync_timer_tick only
    // touches the native tick, never the walk.
    sync_timer_tick();
    return true;
  }

  bool Session::timer_set_interval(uint32_t timer_id, uint32_t interval_ms)
  {
    if (!_timers.set_interval(timer_id, interval_ms, platform_now_ms())) return false;
    sync_timer_tick();
    return true;
  }

  void Session::tick_client_timers()
  {
    // The walk itself (re-entrancy guard, local due list, liveness recheck)
    // lives in the portable TimerTable so it is Tier-1 testable - a
    // use-after-free hid in this loop when it was written out here.
    _timers.tick_and_dispatch(platform_now_ms(),
      [this](uint32_t id, uint32_t interval_ms) {
        neui_event_t ev = {};
        ev.type                   = NEUI_EVENT_TIMER;
        ev.data.timer.timer_id    = id;
        ev.data.timer.interval_ms = interval_ms;
        dispatch_event(&ev);
      });
    // A handler may have added or removed timers; re-arm if the shortest
    // interval moved.
    sync_timer_tick();
  }

  // Which user-driven change events an accessibility provider has to hear about,
  // and as what. <neui/d/a11y.h> promises "the framework raises these itself for
  // every built-in widget", and this is where it keeps that promise: one place
  // that sees every built-in change, rather than a notify call bolted onto each
  // of the value / check / selection paths, which is where such a scheme rots.
  //
  // Deliberately NOT the whole event set - only the ones that change what an AT
  // would speak. Returns -1 for everything else, which is the common case and
  // costs one switch.
  static int a11y_kind_for_event(neui_event_type_t t)
  {
    switch (t) {
      case NEUI_EVENT_VALUE_CHANGED:
      case NEUI_EVENT_ATTR_CHANGED:      return a11y_notify_value;
      case NEUI_EVENT_CHECKBOX_CHANGED:  return a11y_notify_state;
      case NEUI_EVENT_ITEM_SELECTED:
      case NEUI_EVENT_TREE_ITEM_SELECTED:
      case NEUI_EVENT_GRID_ROW_SELECTED:
      case NEUI_EVENT_GRID_CELL_SELECTED:
      case NEUI_EVENT_TAB_SELECTED:      return a11y_notify_selection;
      // Scrolling changes WHICH children are on screen (and their OFFSCREEN
      // state), so it is a layout change rather than a value change.
      case NEUI_EVENT_SCROLL_CHANGED:    return a11y_notify_structure;
      default:                           return -1;
    }
  }

  // The widget a given event payload is about. Only called for the handful of
  // types above, so it only has to know those payloads.
  static uint32_t a11y_widget_of_event(const neui_event_t* ev)
  {
    switch (ev->type) {
      case NEUI_EVENT_VALUE_CHANGED:     return ev->data.value.widget.id;
      case NEUI_EVENT_ATTR_CHANGED:      return ev->data.attr.widget.id;
      case NEUI_EVENT_CHECKBOX_CHANGED:  return ev->data.checkbox.widget.id;
      case NEUI_EVENT_ITEM_SELECTED:     return ev->data.item.widget.id;
      case NEUI_EVENT_TREE_ITEM_SELECTED:return ev->data.tree.widget.id;
      case NEUI_EVENT_GRID_ROW_SELECTED: return ev->data.grid_row.widget.id;
      case NEUI_EVENT_GRID_CELL_SELECTED:return ev->data.grid_cell.widget.id;
      case NEUI_EVENT_TAB_SELECTED:      return ev->data.tab.widget.id;
      case NEUI_EVENT_SCROLL_CHANGED:    return ev->data.scroll.widget.id;
      default:                           return 0;
    }
  }

  bool Session::dispatch_event(neui_event_t* event)
  {
    // Accessibility notification for the built-in widgets. Before the client
    // callback, so a client that consumes the event does not suppress it - the
    // AT still needs to hear that the value changed. The revision bump is what
    // makes the AT's follow-up query see the NEW state: platform_invalidate only
    // schedules a paint, so the query can easily arrive before it.
    const int a11y_kind = a11y_kind_for_event(event->type);
    if (a11y_kind >= 0) {
      const uint32_t wid = a11y_widget_of_event(event);
      const uint32_t slot = wid & 0xffff;
      if (slot != 0 && _widgets.exists(slot)) {
        bump_a11y_revision();
        if (void* frame = find_parent_native_handle(slot))
          platform_a11y_notify(frame, wid, a11y_kind);
      }
    }

    if (_client_widget_api && _client_widget_api->onevent)
      return _client_widget_api->onevent(_token, event);
    return false;
  }

  bool Session::dispatch_menu_command(MenubarWidget& mb, uint32_t cmd_id)
  {
    auto it = mb.menu_cmd_map.find(cmd_id);
    if (it == mb.menu_cmd_map.end()) return false;
    uint32_t neui_id = it->second;

    // If this item is bound to a built-in command, try the focused
    // widget first. Only if no widget consumes it do we fall through
    // to the client.
    auto data_it = mb.menu_items.find(neui_id);
    if (data_it != mb.menu_items.end()) {
      uint32_t cmd = data_it->second.menu_cmd;
      if (cmd != 0 && cmd < NEUI_CMD_USER_BASE) {
        if (invoke_focused_command(cmd)) return true;
      }
    }

    neui_event_t ev = {};
    ev.type      = NEUI_EVENT_TREE_ITEM_ACTIVATED;
    ev.data.tree = { { mb.widget_id }, { neui_id } };
    dispatch_event(&ev);
    // The command WAS routed, whether or not the client consumed the event -
    // the caller uses this to decide "did this menu own the id", not "did
    // anyone handle it".
    return true;
  }

  bool Session::dispatch_menu_event(uint32_t cmd_id)
  {
    // _menubars holds menu BARS only (POPUPMENU is deliberately absent - see
    // PopupMenuWidget), so a popup's ids can never be matched here by accident.
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto* mb = dynamic_cast<MenubarWidget*>(&_widgets[mb_idx]);
      if (!mb) continue;
      if (dispatch_menu_command(*mb, cmd_id)) return true;
    }
    return false;
  }

  void* Session::find_parent_native_handle(uint32_t widget_index)
  {
    auto parents = _widgets.get_all_parents(widget_index);
    for (uint32_t p : parents) {
      if (p == 0) continue;
      if (_widgets.exists(p) && _widgets[p].native_handle)
        return _widgets[p].native_handle;
    }
    return nullptr;
  }

  int Session::frame_client_height(uint32_t widget_index)
  {
    auto parents = _widgets.get_all_parents(widget_index);
    for (uint32_t p : parents) {
      if (p == 0) continue;
      if (_widgets.exists(p) && _widgets[p].native_handle)
        return _widgets[p].height;
    }
    return 0;
  }

  WidgetData* Session::get_widget(uint32_t index)
  {
    if (_widgets.exists(index))
      return &_widgets[index];
    return nullptr;
  }

  void Session::resize_render_ctx(uint32_t widget_index, uint32_t w, uint32_t h)
  {
    auto* wd = get_widget(widget_index);
    if (wd && wd->render_ctx && _backend)
      _backend->resize(wd->render_ctx, w, h);
  }

  // -------------------------------------------------------------------------
  // Input helpers

  static uint32_t widget_at_recursive(neui_detail::Tree<WidgetData>& widgets,
                                       uint32_t parent_idx, float x, float y)
  {
    uint32_t result = 0;
    uint32_t idx = widgets.child(parent_idx);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& wd = widgets[idx];
        if (!wd.native_handle && !wd.is_menu_model() && wd.visible) {
          if (wd.hit_test(x, y)) {
            // Disabled widgets are click-transparent: they don't claim
            // the hit, but children remain hit-testable. This matches
            // Win32 EnableWindow semantics, where a disabled control
            // passes clicks through to its parent / siblings.
            // NEUI_ATTR_INPUT_TRANSPARENT requests the same click-through
            // WITHOUT the 50% dim a disabled widget gets - so a decorative
            // overlay (ruler / crosshair / HUD) paints at full opacity yet
            // never intercepts the pointer. Read live; cheap at pointer speed.
            bool input_transparent =
              wd.attrs && wd.attrs->get_int(NEUI_ATTR_INPUT_TRANSPARENT, 0) != 0;
            if (wd.emit_events && wd.enabled && !input_transparent)
              result = idx;
            // SECTION: only descend into children when the cursor is
            // inside the body rect. The body is clipped on paint, so
            // children outside the body aren't visible and shouldn't be
            // hit-testable either. Cursor over the band / scrollbar
            // keeps the SECTION as the hit, so scrollbar drag has no
            // contention from descendants.
            bool descend = true;
            if (auto* L = wd.section_layout_ptr()) {
              int local_x = static_cast<int>(x) - wd.abs_x;
              int local_y = static_cast<int>(y) - wd.abs_y;
              bool in_body = local_x >= L->body_x &&
                             local_x <  L->body_x + L->body_w &&
                             local_y >= L->body_y &&
                             local_y <  L->body_y + L->body_h;
              descend = in_body;
            }
            if (descend) {
              uint32_t deeper = widget_at_recursive(widgets, idx, x, y);
              if (deeper != 0) result = deeper;
            }
          }
        }
      }
      idx = widgets.next(idx);
    }
    return result;
  }

  uint32_t Session::widget_at(float x, float y, uint32_t parent_idx)
  {
    return widget_at_recursive(_widgets, parent_idx, x, y);
  }

  // -------------------------------------------------------------------------
  // DnD dispatch
  //
  // The drop-target search runs independently of the regular widget_at
  // walk so it can ignore the `emit_events` requirement (containers like
  // SECTION can be drop targets even though they don't emit mouse events).
  // The walker descends through every visible non-native widget whose
  // hit_test passes and returns the deepest one that opts in as a drop
  // target with a matching MIME allow-list.

  // MIME matching shared with the other hosts (dnd_dispatch.h).
  using neui_detail::dnd_formats_match;

  static uint32_t find_drop_target_descendants(neui_detail::Tree<WidgetData>& widgets,
                                                 uint32_t parent_idx,
                                                 float x, float y,
                                                 const char* const* formats,
                                                 uint32_t formats_count)
  {
    uint32_t result = 0;
    uint32_t idx = widgets.child(parent_idx);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& wd = widgets[idx];
        if (!wd.native_handle && !wd.is_menu_model() && wd.visible) {
          if (wd.hit_test(x, y)) {
            if (wd.enabled && wd.drop_target &&
                dnd_formats_match(wd.accepted_mimes, formats, formats_count)) {
              result = idx;
            }
            uint32_t deeper = find_drop_target_descendants(widgets, idx, x, y,
                                                            formats, formats_count);
            if (deeper != 0) result = deeper;
          }
        }
      }
      idx = widgets.next(idx);
    }
    return result;
  }

  // Pick the deepest drop_target widget under (x, y) within the frame
  // rooted at `frame_widget_idx`. Falls back to the frame itself if no
  // descendant matches and the frame's own drop_target/accepted_mimes
  // allow the drag. Returns 0 if nothing accepts.
  static uint32_t find_drop_target_in_frame(neui_detail::Tree<WidgetData>& widgets,
                                              uint32_t frame_widget_idx,
                                              float x, float y,
                                              const char* const* formats,
                                              uint32_t formats_count)
  {
    if (frame_widget_idx == 0 || frame_widget_idx == UINT32_MAX ||
        !widgets.exists(frame_widget_idx)) return 0;
    uint32_t deepest = find_drop_target_descendants(widgets, frame_widget_idx,
                                                     x, y,
                                                     formats, formats_count);
    if (deepest != 0) return deepest;
    auto& frame_wd = widgets[frame_widget_idx];
    if (frame_wd.visible && frame_wd.enabled && frame_wd.drop_target &&
        dnd_formats_match(frame_wd.accepted_mimes, formats, formats_count))
      return frame_widget_idx;
    return 0;
  }

  // Adapter for the shared dispatch templates: wrap the hit_test-based
  // walker above and report the matched widget's cached frame-local
  // top-left (abs_x / abs_y, maintained by the paint walk).
  uint32_t Session::dnd_find_target(uint32_t frame_widget_idx, int x, int y,
                                     const char* const* formats, uint32_t count,
                                     int& out_abs_x, int& out_abs_y)
  {
    out_abs_x = 0;
    out_abs_y = 0;
    uint32_t idx = find_drop_target_in_frame(_widgets, frame_widget_idx,
                                              static_cast<float>(x),
                                              static_cast<float>(y),
                                              formats, count);
    if (idx != 0 && _widgets.exists(idx)) {
      out_abs_x = _widgets[idx].abs_x;
      out_abs_y = _widgets[idx].abs_y;
    }
    return idx;
  }

  // Adapter for the shared dispatch templates: build + fire the
  // NEUI_EVENT_DND_* payload with widget-local coords.
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
    ev.data.dnd.x             = frame_x - abs_x;
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

  // Resolve a widget's behavior asset (slot-vector lookup). Defined further
  // down (resolve_widget_behavior); forward-declared here for the iOS
  // drag-source resolution below.
  static neui_detail::BehaviorAsset*
  resolve_widget_behavior(Session* s, neui_asset_t a);

  uint32_t Session::dnd_resolve_drag_source(uint32_t frame_widget_idx,
                                             int frame_local_x, int frame_local_y,
                                             neui_detail::DataItem* out_item,
                                             uint32_t& out_allowed_actions)
  {
    // Deepest hit under the gesture point, then walk up the parent chain for
    // the first widget carrying a DRAG_SOURCE behavior handler. (A child of a
    // drag-source CUSTOMDRAW is unlikely, but the walk-up keeps parity with
    // how the desktop hit region is anchored to the widget.)
    uint32_t hit = widget_at(static_cast<float>(frame_local_x),
                             static_cast<float>(frame_local_y),
                             frame_widget_idx);
    uint32_t cur = hit;
    while (cur != 0 && cur != UINT32_MAX && _widgets.exists(cur)) {
      auto& wd = _widgets[cur];
      auto* ba = resolve_widget_behavior(this, wd.behavior_asset_id());
      if (ba) {
        auto* H = neui_detail::behavior_find_kind_any(
            *ba, NEUI_BEHAVIOR_KIND_DRAG_SOURCE);
        if (H) {
          // Resolve the DataItem id the client stashed in the widget's attr
          // bag (drag_data_key), copy its formats into a transient store item
          // the caller turns into an NSItemProvider.
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
    auto* ba = resolve_widget_behavior(this, wd.behavior_asset_id());
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
    void* frame = find_parent_native_handle(widget_idx);
    if (frame) platform_invalidate(frame);
  }

  // ENTER / re-target / MOVE / LEAVE / DROP state machine shared with the
  // native hosts (hosts/shared/dnd_dispatch.h); only the hit-test walker
  // and the event-send plumbing above stay xpl-local.

  uint32_t Session::dispatch_dnd_enter(uint32_t frame_widget_idx,
                                        int x, int y,
                                        const char* const* formats, uint32_t count,
                                        uint32_t suggested, uint32_t buttonmap)
  {
    return neui_detail::dnd_dispatch_enter(this, frame_widget_idx, x, y,
                                            formats, count,
                                            suggested, buttonmap);
  }

  uint32_t Session::dispatch_dnd_move(uint32_t frame_widget_idx,
                                       int x, int y,
                                       const char* const* formats, uint32_t count,
                                       uint32_t suggested, uint32_t buttonmap)
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
                                       const char* const* formats, uint32_t count,
                                       uint32_t suggested, uint32_t buttonmap,
                                       neui_detail::DataItem* drop_item)
  {
    return neui_detail::dnd_dispatch_drop(this, x, y, formats, count,
                                           suggested, buttonmap, drop_item);
  }

  void Session::widget_set_owner(neui_widget_t dialog, neui_widget_t owner)
  {
    uint32_t didx = (dialog.id == UINT32_MAX) ? 0 : (dialog.id & 0xFFFF);
    if (didx == 0 || !_widgets.exists(didx)) return;
    auto& dwd = _widgets[didx];
    if (!dwd.is_dialog()) return;  // owner only meaningful for dialogs

    if (owner.id == widget_none.id || owner.id == 0) {
      dwd.owner_index = 0;
      return;
    }
    uint32_t oidx = owner.id & 0xFFFF;
    if (!_widgets.exists(oidx)) return;
    if (!_widgets[oidx].is_frame()) return;
    dwd.owner_index = oidx;
  }

  void Session::set_focus(uint32_t new_idx)
  {
    if (new_idx == _focused_widget) return;

    // If the open combo is losing focus, close its overlay.
    if (_open_combo != 0 && _open_combo == _focused_widget && new_idx != _focused_widget)
      close_combo();

    if (_focused_widget != 0 && _widgets.exists(_focused_widget)) {
      auto& wd = _widgets[_focused_widget];
      wd.on_focus_change(false);
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_WIDGET_FOCUS;
        ev.data.focus.widget  = { wd.widget_id };
        ev.data.focus.focused = false;
        dispatch_event(&ev);
      }
    }

    _focused_widget = new_idx;

    if (new_idx != 0 && _widgets.exists(new_idx)) {
      auto& wd = _widgets[new_idx];
      wd.on_focus_change(true);
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_WIDGET_FOCUS;
        ev.data.focus.widget  = { wd.widget_id };
        ev.data.focus.focused = true;
        dispatch_event(&ev);
      }
      // Auto-scroll any enclosing scrolling SECTION ancestor so a Tab
      // into an off-screen child brings it into view.
      ensure_widget_visible(new_idx);
    }

    uint32_t ref = (new_idx != 0) ? new_idx : _focused_widget;
    void* frame = find_parent_native_handle(ref);
    if (frame) platform_invalidate(frame);

    // Tell an attached AT where focus went. Bump first: the provider resolves
    // the widget against a freshly built tree, and the FOCUSED bit it needs to
    // report lives in that tree. Focus gets an explicit bump because it is one
    // of the few changes a repaint does not necessarily express - the frame can
    // repaint pixel-identically (focus decorations are suppressed while the
    // frame lacks OS focus) and the newly focused control can even be scrolled
    // out of view.
    bump_a11y_revision();
    // Nothing is posted when focus is CLEARED (new_idx == 0): there is no
    // element to name as the new focus, and NSAccessibility / UIA both expect a
    // focused-element-changed notification to carry one. An AT re-reads focus
    // from the provider on its next query, which then answers "none".
    if (frame && new_idx != 0 && _widgets.exists(new_idx))
      platform_a11y_notify(frame, _widgets[new_idx].widget_id,
                           a11y_notify_focus);
  }

  void Session::set_hovered(uint32_t new_idx)
  {
    if (new_idx == _hovered_widget) return;

    uint32_t old_idx = _hovered_widget;
    if (old_idx != 0 && _widgets.exists(old_idx)) {
      auto& wd = _widgets[old_idx];
      wd.hovered = false;
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_MOUSE_LEAVE;
        ev.data.mouse.widget = { wd.widget_id };
        dispatch_event(&ev);
      }
    }

    _hovered_widget = new_idx;

    if (new_idx != 0 && _widgets.exists(new_idx)) {
      auto& wd = _widgets[new_idx];
      wd.hovered = true;
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_MOUSE_ENTER;
        ev.data.mouse.widget = { wd.widget_id };
        dispatch_event(&ev);
      }
    }

    // The hovered widget decides the cursor (NEUI_ATTR_CURSOR), so resolve it
    // on every hover transition. Must run AFTER _hovered_widget is updated, and
    // the stale-override drop must run BEFORE the resolve or the old override
    // would win one more time.
    drop_cursor_override_on_hover_change(new_idx);
    refresh_cursor();

    // Repaint the frame so widgets whose paint reacts to .hovered (BUTTON, ...)
    // swap visuals. Hover transitions happen at human pointer speed - cheap.
    uint32_t ref = (new_idx != 0) ? new_idx : old_idx;
    if (ref != 0) {
      if (void* frame = find_parent_native_handle(ref))
        platform_invalidate(frame);
    }
  }

  // ---------------------------------------------------------------------------
  // Mouse cursor (NEUI_ATTR_CURSOR).

  int Session::resolve_cursor_for(uint32_t widget_idx) const
  {
    // Nearest ancestor with an explicit cursor wins. A widget whose attr is
    // unset - or set to an unrecognised name, which parses to DEFAULT - is
    // transparent to the walk, which is what makes "inherit" work: a client
    // sets "ibeam" once on a SECTION and every interactive child inside it gets
    // an I-beam.
    //
    // Note the START of the walk is the HOVERED widget, and widget_at only ever
    // hands back widgets with emit_events && enabled && !input_transparent. So a
    // cursor set on a non-hit-testing widget (LABEL, IMAGE, a non-scrolling
    // SECTION's bare background) never becomes the starting point and is inert -
    // correctly, since the pointer is logically interacting with whatever is
    // beneath it. Such a widget is still walked THROUGH as an ancestor.
    // Documented in <neui/d/attrs.h> and docs/attributes.md.
    //
    // The guard bounds a malformed tree (a parent cycle would otherwise spin
    // here on every mouse move). get_parent returns 0 at the root sentinel.
    for (int guard = 0; widget_idx != 0 && guard < 256; ++guard) {
      if (!_widgets.exists(widget_idx)) break;
      const auto& wd = _widgets[widget_idx];
      if (wd.attrs) {
        if (const char* name = wd.attrs->get_string(NEUI_ATTR_CURSOR)) {
          int kind = neui_detail::cursor_kind_from_name(name);
          if (kind != NEUI_CURSOR_DEFAULT) return kind;
        }
      }
      widget_idx = _widgets.get_parent(widget_idx);
    }
    return NEUI_CURSOR_DEFAULT;
  }

  void Session::refresh_cursor()
  {
    // An internal override (GRID column-resize band) outranks the widget attr:
    // it is positional feedback about what a drag right here would do, which is
    // more specific than "this widget generally shows a hand".
    int kind = (_cursor_override != NEUI_CURSOR_DEFAULT)
                 ? _cursor_override
                 : resolve_cursor_for(_hovered_widget);

    if (kind == _cursor_applied) return;
    _cursor_applied = kind;
    platform_set_cursor(kind);
  }

  void Session::set_cursor_override(uint32_t owner_idx, int kind)
  {
    // Clearing: forget the owner too, so a later hover change has nothing to
    // compare against and cannot resurrect a stale override.
    if (kind == NEUI_CURSOR_DEFAULT) {
      if (_cursor_override == NEUI_CURSOR_DEFAULT) return;
      _cursor_override       = NEUI_CURSOR_DEFAULT;
      _cursor_override_owner = 0;
      refresh_cursor();
      return;
    }

    if (kind == _cursor_override && owner_idx == _cursor_override_owner) return;
    _cursor_override       = kind;
    _cursor_override_owner = owner_idx;
    refresh_cursor();
  }

  void Session::forget_dead_hover()
  {
    bool changed = false;

    if (_hovered_widget != 0 && !_widgets.exists(_hovered_widget)) {
      // Deliberately NOT set_hovered(0): the old widget is already gone, so
      // firing MOUSE_LEAVE at it would hand the client a dangling handle (and
      // set_hovered's own exists() check would skip the notification anyway).
      _hovered_widget = 0;
      changed = true;
    }
    if (_pressed_widget != 0 && !_widgets.exists(_pressed_widget)) {
      _pressed_widget = 0;
      changed = true;
    }
    if (_cursor_override_owner != 0 && !_widgets.exists(_cursor_override_owner)) {
      _cursor_override       = NEUI_CURSOR_DEFAULT;
      _cursor_override_owner = 0;
      changed = true;
    }

    if (changed) refresh_cursor();
  }

  // ---------------------------------------------------------------------------
  // Relative (unbounded) pointer mode (NEUI_API_POINTER).

  bool Session::begin_relative_pointer(uint32_t widget_idx)
  {
    if (_relative.active)                 return false;   // not re-entrant
    if (!platform_supports_relative_pointer()) return false;
    if (widget_idx == 0 || !_widgets.exists(widget_idx)) return false;

    void* native = find_parent_native_handle(widget_idx);
    if (!native) return false;   // not realized yet: nothing to pin against

    int ax = 0, ay = 0;
    if (!platform_begin_relative_pointer(native, &ax, &ay)) return false;

    // Seed the virtual position from where the pointer actually is in
    // widget-local space, so the first MOUSE_MOVE is continuous with the press
    // that started the drag. Falling back to the widget centre would put a
    // visible jump at the start of every drag.
    auto& wd = _widgets[widget_idx];
    float start_x = (float)wd.width  * 0.5f;
    float start_y = (float)wd.height * 0.5f;
    if (_last_mouse_valid) {
      // _last_mouse_frame_* is FRAME-local; the virtual position is WIDGET-local
      // (that is what the event payload carries), hence the abs_* subtraction.
      start_x = _last_mouse_frame_x - (float)wd.abs_x;
      start_y = _last_mouse_frame_y - (float)wd.abs_y;
    }

    _relative_anchor_x = ax;
    _relative_anchor_y = ay;
    _relative_native   = native;
    _relative.begin(widget_idx, start_x, start_y);
    return true;
  }

  void Session::end_relative_pointer()
  {
    if (!_relative.active) return;   // safe to call unconditionally from an UP

    // Only hand the platform a handle that is still LIVE. X11 dereferences it to
    // reach the Display, so a frame destroyed between begin and end would be a
    // use-after-free. end_relative_pointer_if_within closes that window for the
    // paths we know about; this re-check covers any teardown route that doesn't
    // go through widget destroy, and every platform tolerates nullptr.
    void* native = _relative_native;
    if (native != nullptr) {
      bool live = false;
      for (uint32_t i = 0; i < _widgets.slot_count(); ++i) {
        if (_widgets.exists(i) && _widgets[i].native_handle == native) {
          live = true;
          break;
        }
      }
      if (!live) native = nullptr;
    }

    _relative.end();
    _relative_native = nullptr;
    platform_end_relative_pointer(native, _relative_anchor_x, _relative_anchor_y);
  }

  void Session::end_relative_pointer_if_within(uint32_t subtree_root)
  {
    if (!_relative.active || subtree_root == 0) return;

    // Walk up from the owner: if we reach subtree_root, the owner is inside the
    // doomed subtree. Guarded like resolve_cursor_for against a malformed tree.
    uint32_t idx = _relative.widget;
    bool inside = false;
    for (int guard = 0; idx != 0 && guard < 256; ++guard) {
      if (idx == subtree_root) { inside = true; break; }
      if (!_widgets.exists(idx)) break;
      idx = _widgets.get_parent(idx);
    }

    // Also end it when the doomed subtree owns the FRAME whose native handle we
    // are holding - destroying a frame frees that handle even when the owner
    // widget itself is elsewhere in the tree.
    if (!inside && _relative_native != nullptr &&
        _widgets.exists(subtree_root) &&
        _widgets[subtree_root].native_handle == _relative_native)
      inside = true;

    if (inside) end_relative_pointer();
  }

  void Session::dispatch_relative_motion(float dx, float dy, uint32_t buttonmap)
  {
    if (!_relative.active) return;

    // The owner can be destroyed mid-drag (a client rebuilding its UI from a
    // parameter change). Bail out of the whole mode rather than dispatching at a
    // freed slot - and go through end_relative_pointer so the cursor is unhidden
    // and warped back, which forget_dead_hover alone would not do.
    if (!_widgets.exists(_relative.widget)) { end_relative_pointer(); return; }

    _relative.accumulate(dx, dy);

    auto& wd = _widgets[_relative.widget];
    neui_event_t ev = {};
    ev.type                 = NEUI_EVENT_MOUSE_MOVE;
    ev.data.mouse.widget    = { wd.widget_id };
    // dispatch_mouse_event takes FRAME-local coordinates and converts to
    // widget-local for the client itself, so the widget-local virtual position
    // has to be shifted back up by abs_* here. Handing it widget-local
    // coordinates would subtract abs_* a second time.
    ev.data.mouse.x         = _relative.report_x() + wd.abs_x;
    ev.data.mouse.y         = _relative.report_y() + wd.abs_y;
    ev.data.mouse.buttonmap = buttonmap;

    // Straight to the owner: hit-testing would be meaningless here, since the
    // virtual position deliberately leaves the widget (that is the entire point)
    // and the real pointer never moves at all.
    dispatch_mouse_event(_relative.widget, &ev);
  }

  void Session::release_cursor()
  {
    // Only touch the platform if this session actually changed the cursor.
    // ~Session can run during static teardown, where reaching into AppKit / X11
    // for no reason is worth avoiding.
    if (_cursor_applied == -1 || _cursor_applied == NEUI_CURSOR_DEFAULT) return;
    _cursor_override       = NEUI_CURSOR_DEFAULT;
    _cursor_override_owner = 0;
    _hovered_widget        = 0;
    _cursor_applied        = NEUI_CURSOR_DEFAULT;
    platform_set_cursor(NEUI_CURSOR_DEFAULT);
  }

  void Session::drop_cursor_override_on_hover_change(uint32_t new_idx)
  {
    if (_cursor_override == NEUI_CURSOR_DEFAULT) return;
    if (_cursor_override_owner == new_idx)       return;   // still inside it
    // A drag owns the pointer: a column-resize drag continues past the GRID's
    // edge and must keep its EW cursor until the button comes up. _pressed_widget
    // is the capture holder, so this is exactly "the owner is mid-drag".
    if (_cursor_override_owner != 0 &&
        _cursor_override_owner == _pressed_widget) return;
    _cursor_override       = NEUI_CURSOR_DEFAULT;
    _cursor_override_owner = 0;
  }

  void Session::set_pressed(uint32_t new_idx)
  {
    if (new_idx == _pressed_widget) return;

    uint32_t old_idx = _pressed_widget;
    if (old_idx != 0 && _widgets.exists(old_idx))
      _widgets[old_idx].pressed = false;

    _pressed_widget = new_idx;

    if (new_idx != 0 && _widgets.exists(new_idx))
      _widgets[new_idx].pressed = true;

    // Same rationale as set_hovered: frame repaint so .pressed-aware widgets
    // (BUTTON) flip to their pressed visual immediately.
    uint32_t ref = (new_idx != 0) ? new_idx : old_idx;
    if (ref != 0) {
      if (void* frame = find_parent_native_handle(ref))
        platform_invalidate(frame);
    }
  }

  void Session::on_dpi_changed(uint32_t widget_index, uint32_t new_dpi)
  {
    auto* wd = get_widget(widget_index);
    if (!wd) return;
    wd->dpi = new_dpi;
    if (wd->render_ctx && _backend)
      _backend->update_dpi(wd->render_ctx, new_dpi);
  }

  // -------------------------------------------------------------------------
  // Widget paint

  // Base paint: draw text over the parent's painted background, then the
  // focus outline. Used by LabelWidget. Mirrors the native Win32 STATIC,
  // which gets its background from the parent's WM_CTLCOLORSTATIC brush
  // (frame's panel_bg or the enclosing SECTION's body colour).
  void WidgetData::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                          bool is_focused)
  {
    using neui_detail::ColorRole;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);
    if (!text.empty() && backend->draw_text) {
      auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
      neui_detail::push_widget_font(backend, ctx, ef);
      backend->draw_text(ctx, fx + 4.0f, fy, fw - 8.0f, fh, text.c_str(), ef.size,
                         neui_detail::color(ColorRole::text_primary));
      neui_detail::pop_widget_font(backend, ctx, ef);
    }
    if (is_focused)
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.5f,
                         neui_detail::color(ColorRole::focus_ring));
  }

  // Widget-local header band height for a section. Mirrors paint_section's
  // band logic - 0 when text is empty / align "none", else SECTION_HEADER_H
  // clamped to fit the section.
  static int section_band_h(const std::string& text, int height, const char* align)
  {
    if (text.empty() || neui_detail::section_align_is_none(align)) return 0;
    int bh = static_cast<int>(neui_detail::section_header_h());
    if (bh > height) bh = height;
    return bh;
  }

  // Scan `parent_idx`'s direct widget-tree children for the auto-bounding
  // content extent: max(child.x + child.width) and max(child.y + child.height).
  static void compute_section_auto_extent(neui_detail::Tree<WidgetData>& widgets,
                                           uint32_t parent_idx,
                                           int& out_w, int& out_h)
  {
    out_w = 0;
    out_h = 0;
    uint32_t idx = widgets.child(parent_idx);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& cw = widgets[idx];
        if (cw.visible) {
          int rx = cw.x + cw.width;
          int by = cw.y + cw.height;
          if (rx > out_w) out_w = rx;
          if (by > out_h) out_h = by;
        }
      }
      idx = widgets.next(idx);
    }
  }

  void SectionWidget::refresh_scroll_state()
  {
    const char* mode = attrs ? attrs->get_string(NEUI_ATTR_SCROLL_MODE) : nullptr;
    auto axis = neui_detail::parse_section_scroll_mode(mode);
    if (axis == neui_detail::SectionScrollAxis::None) {
      // Drop state entirely - non-scrolling sections pay nothing.
      scroll_state.reset();
      emit_events = false;
      return;
    }
    if (!scroll_state)
      scroll_state = std::make_unique<neui_detail::SectionScrollState>();
    scroll_state->axis = axis;
    // Scrollable sections must receive mouse events for wheel + scrollbar drag.
    emit_events = true;
  }

  // SECTION - visual container. Body filled with the section colour
  // (NEUI_ATTR_BACKGROUND override, else a theme-derived shade lighter
  // than frame_bg so it reads as a raised panel). Optional `text` is
  // drawn as a header in a top band; "none" alignment hides the band
  // entirely. When NEUI_ATTR_SCROLL_MODE != "none", scrollbars paint over
  // the body edges and last_layout is cached for hit-testing.
  void SectionWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                             bool /*is_focused*/)
  {
    using neui_detail::ColorRole;
    uint32_t bg = neui_detail::shade(
                    neui_detail::color(ColorRole::frame_bg),
                    neui_detail::SECTION_BG_LIFT);
    if (attrs && attrs->has(NEUI_ATTR_BACKGROUND))
      bg = static_cast<uint32_t>(attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
    std::string hdr   = section_header_text();
    const char* align = section_header_align();
    neui_detail::paint_section(backend, ctx,
                                static_cast<float>(x),
                                static_cast<float>(y),
                                static_cast<float>(width),
                                static_cast<float>(height),
                                hdr.c_str(), bg, align,
                                neui_detail::color(ColorRole::text_primary),
                                attrs.get());

    // Late-refresh in case set_string(SCROLL_MODE) ran without the explicit
    // refresh hook firing - cheap when state already matches.
    refresh_scroll_state();

    // Children are positioned relative to the BODY's top-left (chip
    // "none" auto-expands the children area into the former band).
    // Auto content extent = max(child.x + child.width) / .y + .height
    // across direct children, already in body-local coords.
    int auto_w = 0, auto_h = 0;
    if (session)
      compute_section_auto_extent(session->_widgets, index, auto_w, auto_h);
    int band_h = section_band_h(hdr, height, align);
    int initial_body_w = width;
    int initial_body_h = height - band_h;
    if (initial_body_h < 0) initial_body_h = 0;

    int content_w = 0, content_h = 0;
    auto autofn = [&](int& w, int& h){ w = auto_w; h = auto_h; };
    neui_detail::resolve_section_content_extent(attrs.get(), autofn,
                                                  initial_body_w, initial_body_h,
                                                  content_w, content_h);

    auto axis = scroll_state ? scroll_state->axis
                              : neui_detail::SectionScrollAxis::None;
    last_layout = neui_detail::compute_section_layout(width, height, band_h,
                                                       content_w, content_h,
                                                       axis);
    if (scroll_state) {
      scroll_state->content_w = content_w;
      scroll_state->content_h = content_h;
      // Kinetics-aware clamp: an axis mid rubber-band stretch / spring-back
      // keeps its intentional overshoot; everything else (content shrunk,
      // body resized) snaps back into range.
      neui_detail::clamp_section_scroll_idle(*scroll_state, content_w, content_h,
                                              last_layout.body_w, last_layout.body_h);
    }

    // Scrollbars overlay the body's right + bottom edges (scrolling only).
    if (scroll_state &&
        (last_layout.vert_sb_shown || last_layout.horz_sb_shown)) {
      uint32_t sep   = neui_detail::color(ColorRole::scrollbar_separator);
      uint32_t track = neui_detail::color(ColorRole::scrollbar_track);
      uint32_t thumb = neui_detail::color(ColorRole::scrollbar_thumb);
      backend->push_transform(ctx);
      backend->translate(ctx, static_cast<float>(x), static_cast<float>(y));
      neui_detail::paint_section_scrollbars(backend, ctx, last_layout,
                                             *scroll_state, sep, track, thumb);
      backend->pop_transform(ctx);
    }
  }

  // Fire NEUI_EVENT_SCROLL_CHANGED if the section's scroll position has
  // moved since the last notification. See SectionScrollState.last_notified_*.
  void SectionWidget::notify_scroll_changed()
  {
    if (!session || !scroll_state) return;
    auto& st = *scroll_state;
    if (st.scroll_x == st.last_notified_x &&
        st.scroll_y == st.last_notified_y) return;
    st.last_notified_x = st.scroll_x;
    st.last_notified_y = st.scroll_y;
    neui_event_t ev{};
    ev.type                  = NEUI_EVENT_SCROLL_CHANGED;
    ev.data.scroll.widget.id = widget_id;
    ev.data.scroll.scroll_x  = st.scroll_x;
    ev.data.scroll.scroll_y  = st.scroll_y;
    session->dispatch_event(&ev);
  }

  void Session::ensure_widget_visible(uint32_t widget_idx)
  {
    if (!_widgets.exists(widget_idx)) return;
    auto& wd0 = _widgets[widget_idx];
    int rect_x = wd0.x, rect_y = wd0.y;
    uint32_t cur = _widgets.get_parent(widget_idx);
    SectionWidget* sec = nullptr;
    while (cur != 0 && cur != neui_detail::knone.id && _widgets.exists(cur)) {
      auto& cw = _widgets[cur];
      if (auto* s = dynamic_cast<SectionWidget*>(&cw); s && s->scroll_state) {
        sec = s; break;
      }
      rect_x += cw.x;
      rect_y += cw.y;
      cur = _widgets.get_parent(cur);
    }
    if (!sec) return;
    auto& st = *sec->scroll_state;
    auto& L  = sec->last_layout;
    int nx, ny;
    neui_detail::compute_ensure_visible(rect_x, rect_y, wd0.width, wd0.height,
                                          L.body_w, L.body_h,
                                          st.content_w, st.content_h,
                                          st.scroll_x, st.scroll_y,
                                          nx, ny);
    sec->external_commit(nx, ny);
  }

  void SectionWidget::external_commit(int nx, int ny)
  {
    if (!scroll_state) return;
    auto& st = *scroll_state;
    int max_x = st.content_w - last_layout.body_w; if (max_x < 0) max_x = 0;
    int max_y = st.content_h - last_layout.body_h; if (max_y < 0) max_y = 0;
    if (nx < 0)     nx = 0;
    if (nx > max_x) nx = max_x;
    if (ny < 0)     ny = 0;
    if (ny > max_y) ny = max_y;
    if (st.scroll_x == nx && st.scroll_y == ny) return;
    st.scroll_x = nx;
    st.scroll_y = ny;
    st.kin_v.raw_px            = (double)ny;
    st.kin_v.last_commit_px    = ny;
    st.kin_v.suppress_momentum = true;
    st.kin_h.raw_px            = (double)nx;
    st.kin_h.last_commit_px    = nx;
    st.kin_h.suppress_momentum = true;
    st.kinetic_over_v = false;
    st.kinetic_over_h = false;
    repaint();
    notify_scroll_changed();
  }

  bool SectionWidget::on_mouse_event(neui_event_t* event)
  {
    if (!scroll_state) return false;
    auto& st = *scroll_state;

    // Widget-local mouse coords (frame -> section).
    int local_x = event->data.mouse.x - abs_x;
    int local_y = event->data.mouse.y - abs_y;

    switch (event->type) {
    case NEUI_EVENT_MOUSE_WHEEL: {
      int delta = event->data.wheel.delta;
      // Route the event by the platform-supplied axis (trackpad
      // horizontal / Shift+wheel) when the section's axis allows it;
      // otherwise fall back to the only enabled axis. Section "both"
      // handles both axes - WM_MOUSEHWHEEL drives horizontal, WM_MOUSEWHEEL
      // drives vertical, so the user sees both work natively.
      bool axis_h;
      if (event->data.wheel.is_horizontal &&
          neui_detail::section_axis_has_h(st.axis))
        axis_h = true;
      else if (!event->data.wheel.is_horizontal &&
               neui_detail::section_axis_has_v(st.axis))
        axis_h = false;
      else
        axis_h = (st.axis == neui_detail::SectionScrollAxis::Horizontal);
      // Line-delta fallback path (null platform / hosts without kinetics
      // plumbing): wheel lines scale to px via SECTION_WHEEL_LINE_PX so the
      // scroll speed matches the kinetic path's classic-wheel feel. On
      // macOS + Win32 the platform layer intercepts scrolling-SECTION
      // wheels before dispatch and feeds the kinetics instead, so this
      // only runs when no kinetics are wired.
      bool changed = neui_detail::section_apply_wheel(
                       st, last_layout,
                       (double)(-delta) * neui_detail::SECTION_WHEEL_LINE_PX,
                       axis_h);
      if (changed) { repaint(); notify_scroll_changed(); }
      return changed;
    }
    case NEUI_EVENT_MOUSE_BUTTON_DOWN: {
      int hit = neui_detail::section_scrollbar_hit(last_layout, local_x, local_y);
      if (hit == 1) {
        st.vert_drag.active           = true;
        st.vert_drag.start_axis_coord = local_y;
        st.vert_drag.start_position   = st.scroll_y;
        return true;
      }
      if (hit == 2) {
        st.horz_drag.active           = true;
        st.horz_drag.start_axis_coord = local_x;
        st.horz_drag.start_position   = st.scroll_x;
        return true;
      }
      return false;
    }
    case NEUI_EVENT_MOUSE_MOVE: {
      // End drag if the framework swallowed our BUTTON_UP (e.g. release
      // happened outside the section while pressed).
      if ((st.vert_drag.active || st.horz_drag.active) &&
          !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON)) {
        st.vert_drag.active = false;
        st.horz_drag.active = false;
        return true;
      }
      // Drag math passes gutter_other=0 to match the paint geometry:
      // the track runs the full body extent and the corner dead square
      // overlays the tail when both scrollbars are visible.
      if (st.vert_drag.active) {
        auto geom = neui_detail::compute_scrollbar(
                       last_layout.body_h, 0,
                       st.content_h, last_layout.body_h, st.vert_drag.start_position);
        int new_y = neui_detail::scrollbar_drag_apply(
                       st.vert_drag, local_y, geom,
                       st.content_h, last_layout.body_h);
        if (new_y != st.scroll_y) {
          st.scroll_y = new_y;
          repaint();
          notify_scroll_changed();
        }
        return true;
      }
      if (st.horz_drag.active) {
        auto geom = neui_detail::compute_scrollbar(
                       last_layout.body_w, 0,
                       st.content_w, last_layout.body_w, st.horz_drag.start_position);
        int new_x = neui_detail::scrollbar_drag_apply(
                       st.horz_drag, local_x, geom,
                       st.content_w, last_layout.body_w);
        if (new_x != st.scroll_x) {
          st.scroll_x = new_x;
          repaint();
          notify_scroll_changed();
        }
        return true;
      }
      return false;
    }
    case NEUI_EVENT_MOUSE_BUTTON_UP: {
      if (st.vert_drag.active || st.horz_drag.active) {
        st.vert_drag.active = false;
        st.horz_drag.active = false;
        return true;
      }
      return false;
    }
    default:
      return false;
    }
  }

  // -------------------------------------------------------------------------
  // TABVIEW

  void TabViewWidget::collect_pages(std::vector<uint32_t>& out) const
  {
    neui_detail::tabview_collect_pages(session, index, out);
  }

  void TabViewWidget::apply_page_geometry()
  {
    if (!session) return;
    std::vector<uint32_t> pages;
    collect_pages(pages);
    int count = static_cast<int>(pages.size());
    if (count == 0) return;
    if (selected < 0)        selected = 0;
    if (selected >= count)   selected = count - 1;
    // Body rect from the last paint. Before the first paint it is zero - fall
    // back to a no-label-measurement layout so a programmatic set_selected made
    // before the first frame still sizes pages sensibly (the first paint then
    // recomputes the exact body rect, incl. the vertical auto-strip, and
    // re-applies). Mirrors the win32 + macOS hosts.
    int body_w = last_layout.body_w;
    int body_h = last_layout.body_h;
    neui_detail::TabEdge edge_used = edge;
    if (body_w <= 0 && body_h <= 0) {
      const char* pos = attrs ? attrs->get_string(NEUI_ATTR_TAB_POSITION) : nullptr;
      auto tp = neui_detail::parse_tab_position(pos);
      edge_used = tp.edge;
      float strip = attrs ? static_cast<float>(attrs->get_int(NEUI_ATTR_TAB_STRIP_SIZE, 0)) : 0.0f;
      neui_detail::TabViewLayout tl = neui_detail::compute_tabview_layout(
          static_cast<float>(width), static_cast<float>(height), tp.edge, strip);
      body_w = static_cast<int>(tl.body_w);
      body_h = static_cast<int>(tl.body_h);
    }
    // Carve the baseline / content-border insets out of the body so the page
    // does not paint over the strip painter's lines - identical math to the
    // win32 + macOS hosts (shared tabview_page_insets), so the page's usable
    // client area is the same on every platform.
    bool has_border = attrs && attrs->has(NEUI_ATTR_TAB_BORDER_COLOR) &&
                      attrs->get_int(NEUI_ATTR_TAB_BORDER_COLOR, 0) != 0;
    float bw = attrs ? static_cast<float>(attrs->get_int(NEUI_ATTR_TAB_BORDER_WIDTH, 0)) : 0.0f;
    int it = 0, il = 0, ib = 0, ir = 0;
    neui_detail::tabview_page_insets(edge_used, has_border, bw, it, il, ib, ir);
    int pw_w = body_w - il - ir; if (pw_w < 0) pw_w = 0;
    int pw_h = body_h - it - ib; if (pw_h < 0) pw_h = 0;
    for (int i = 0; i < count; ++i) {
      auto& pw = session->_widgets[pages[i]];
      // Pages sit at the content-body origin (the paint walk adds body_x/y
      // from section_layout_ptr), offset by the insets; clipped to the body.
      pw.x = il; pw.y = it;
      pw.width  = pw_w;
      pw.height = pw_h;
      const bool was_visible = pw.visible;
      pw.visible = (i == selected);
      // A page going off screen must not keep the focus. Before this, switching
      // tabs left the caret on a control the user could no longer see, and every
      // subsequent keystroke went to it.
      if (was_visible && !pw.visible && session)
        session->focus_leave_subtree(pages[i], true);
    }
  }

  void TabViewWidget::select_tab(int ni)
  {
    // Clamp + fire TAB_DESELECTED / TAB_SELECTED + re-resolve the selection,
    // all in the shared helper; only swap page geometry + repaint when the
    // selection actually changed.
    if (neui_detail::tabview_commit_selection(session, widget_id, index, selected, ni)) {
      apply_page_geometry();
      repaint();
    }
  }

  void TabViewWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                            bool /*is_focused*/)
  {
    using neui_detail::ColorRole;
    if (!session) return;

    const char* pos = attrs ? attrs->get_string(NEUI_ATTR_TAB_POSITION) : nullptr;
    auto tp = neui_detail::parse_tab_position(pos);
    edge = tp.edge;

    std::vector<uint32_t> pages;
    collect_pages(pages);
    int count = static_cast<int>(pages.size());
    if (selected >= count) selected = count > 0 ? count - 1 : 0;
    if (selected < 0)      selected = 0;

    // Collect labels + per-chip colours (cheap); the chip font drives the
    // label measurement below.
    neui_detail::EffectiveFont ef =
      neui_detail::read_widget_font(attrs.get(), neui_detail::TAB_CHIP_FONT);
    std::vector<const char*> labels(count, "");
    std::vector<uint32_t>    chip_bg(count, 0), chip_text(count, 0);
    for (int i = 0; i < count; ++i) {
      auto& pw = session->_widgets[pages[i]];
      labels[i] = pw.text.c_str();
      if (pw.attrs) {
        chip_bg[i]   = static_cast<uint32_t>(pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_BG_COLOR, 0));
        chip_text[i] = static_cast<uint32_t>(pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_TEXT_COLOR, 0));
      }
    }

    // Measure chip labels (for chip widths + the auto vertical strip) only when
    // the label set or font changed - measure_text is comparatively expensive
    // and this paints every frame. Cached widths are keyed by the shared
    // tab_labels_signature.
    uint64_t sig = neui_detail::tab_labels_signature(labels.data(), count,
                       ef.family.c_str(), ef.weight, ef.size);
    if (sig != label_sig || static_cast<int>(label_widths.size()) != count) {
      label_widths.assign(count, 0.0f);
      if (backend->measure_text) {
        neui_detail::push_widget_font(backend, ctx, ef);
        for (int i = 0; i < count; ++i)
          label_widths[i] = backend->measure_text(ctx, labels[i], -1, ef.size);
        neui_detail::pop_widget_font(backend, ctx, ef);
      }
      label_sig = sig;
    }
    const float* widths = label_widths.data();

    // Strip size: explicit attr wins; otherwise auto-fit vertical strips to
    // the widest label so left / right chip text is fully readable.
    float explicit_strip = attrs ? static_cast<float>(attrs->get_int(NEUI_ATTR_TAB_STRIP_SIZE, 0)) : 0.0f;
    float strip = neui_detail::tab_resolve_strip_size(edge, explicit_strip, widths, count);
    neui_detail::TabViewLayout L =
      neui_detail::compute_tabview_layout(static_cast<float>(width),
                                          static_cast<float>(height), edge, strip);

    chips.assign(count, neui_detail::TabChip{});
    if (count > 0 && edge != neui_detail::TabEdge::None)
      neui_detail::layout_tab_chips(L, edge, tp.align, widths, count, chips.data());

    // Resolve the tabview chrome colours (shared with win32 / macOS). The
    // active page's NEUI_ATTR_BACKGROUND drives body_bg so the active chip
    // reads as connected to its page.
    const neui_detail::AttrBag* active_attrs =
      (count > 0) ? session->_widgets[pages[selected]].attrs.get() : nullptr;
    neui_detail::TabPaintColors tc =
      neui_detail::resolve_tab_paint_colors(attrs.get(), active_attrs);

    neui_detail::paint_tabview(backend, ctx,
                               static_cast<float>(x), static_cast<float>(y),
                               static_cast<float>(width), static_cast<float>(height),
                               L, edge, chips.data(), count, selected,
                               labels.data(), chip_bg.data(), chip_text.data(),
                               tc.body_bg, tc.default_text, tc.inactive_chip_bg,
                               tc.sep_color, tc.border_w, tc.strip_bg, tc.content_border,
                               tc.chip_radius, attrs.get());

    // Cache the content rect so the paint walk offsets + clips the active
    // page to the body, and size the pages to it.
    last_layout = neui_detail::SectionLayout{};
    last_layout.body_x = static_cast<int>(L.body_x);
    last_layout.body_y = static_cast<int>(L.body_y);
    last_layout.body_w = static_cast<int>(L.body_w);
    last_layout.body_h = static_cast<int>(L.body_h);
    apply_page_geometry();
  }

  bool TabViewWidget::on_mouse_event(neui_event_t* event)
  {
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN ||
        event->type == NEUI_EVENT_MOUSE_BUTTON_CLICK) {
      float lx = static_cast<float>(event->data.mouse.x - abs_x);
      float ly = static_cast<float>(event->data.mouse.y - abs_y);
      int hit = neui_detail::tabview_chip_hit(chips.data(),
                                              static_cast<int>(chips.size()), lx, ly);
      if (hit >= 0) { select_tab(hit); return true; }
    }
    return false;
  }

  bool TabViewWidget::on_keydown(uint32_t keycode, uint32_t /*modifiers*/)
  {
    if (keycode == NEUI_KEY_LEFT || keycode == NEUI_KEY_UP) {
      select_tab(selected - 1); return true;
    }
    if (keycode == NEUI_KEY_RIGHT || keycode == NEUI_KEY_DOWN) {
      select_tab(selected + 1); return true;
    }
    return false;
  }

  void ButtonWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                            bool is_focused)
  {
    using neui_detail::ColorRole;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);
    // Match the native win32 BS_PUSHBUTTON visual ladder: hover lifts the
    // panel a touch, press darkens it. pressed wins over hovered if both.
    uint32_t base = neui_detail::color(ColorRole::panel_bg);
    uint32_t fill = base;
    if (pressed)      fill = neui_detail::shade(base, -16);
    else if (hovered) fill = neui_detail::shade(base, +16);
    backend->fill_rect(ctx, fx, fy, fw, fh, fill);
    if (backend->draw_rect)
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.0f,
                         neui_detail::color(ColorRole::border));
    if (!text.empty() && backend->draw_text) {
      auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
      neui_detail::push_widget_font(backend, ctx, ef);
      float text_x = fx;
      if (backend->measure_text) {
        float tw = backend->measure_text(ctx, text.c_str(), -1, ef.size);
        text_x = fx + (fw - tw) * 0.5f;
        if (text_x < fx) text_x = fx;
      }
      backend->draw_text(ctx, text_x, fy, fw - (text_x - fx), fh,
                         text.c_str(), ef.size,
                         neui_detail::color(ColorRole::text_primary));
      neui_detail::pop_widget_font(backend, ctx, ef);
    }
    if (is_focused)
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.5f,
                         neui_detail::color(ColorRole::focus_ring));
  }

  // Keyboard activation parity with native win32 BS_PUSHBUTTON: Space and
  // Enter on a focused button fire MOUSE_BUTTON_CLICK, same shape as the
  // mouse-driven CLICK in platform_win32.cpp. (0,0) coords match the
  // synthetic CLICK emitted by hosts/win32/window.cpp:550-552 on BN_CLICKED.
  bool ButtonWidget::on_keydown(uint32_t keycode, uint32_t /*modifiers*/)
  {
    if (keycode == NEUI_KEY_SPACE || keycode == NEUI_KEY_RETURN) {
      if (session) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
        ev.data.mouse = { { widget_id }, 0, 0, 0 };
        session->dispatch_event(&ev);
      }
      return true;
    }
    return false;
  }

  void WidgetData::repaint()
  {
    if (session) {
      void* frame = session->find_parent_native_handle(index);
      if (frame) platform_invalidate(frame);
    }
  }

  // InputBoxWidget paint: text-field surface + text + border, then
  // selection highlight + cursor when focused.
  void InputBoxWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                              bool is_focused)
  {
    using neui_detail::ColorRole;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);
    const float pad     = 4.0f;
    const float cline   = fy + 2.0f;
    const float cheight = fh - 4.0f;

    // Text-field surface (matches MULTILINE / LISTBOX / TREEVIEW).
    backend->fill_rect(ctx, fx, fy, fw, fh,
                        neui_detail::color(ColorRole::control_bg));

    auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
    neui_detail::push_widget_font(backend, ctx, ef);

    if (!text.empty() && backend->draw_text)
      backend->draw_text(ctx, fx + pad, fy, fw - 2 * pad, fh,
                         text.c_str(), ef.size,
                         neui_detail::color(ColorRole::text_primary));
    if (backend->draw_rect) {
      uint32_t border_color = neui_detail::color(
          is_focused ? ColorRole::border_focused : ColorRole::border);
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.0f, border_color);
    }

    if (!is_focused || !backend->measure_text) {
      neui_detail::pop_widget_font(backend, ctx, ef);
      return;
    }

    // Selection highlight
    if (neui_detail::te_has_selection(cursor_pos, sel_anchor)) {
      int lo = neui_detail::te_sel_lo(cursor_pos, sel_anchor);
      int hi = neui_detail::te_sel_hi(cursor_pos, sel_anchor);
      float sel_x0 = fx + pad + backend->measure_text(ctx, text.c_str(), lo, ef.size);
      float sel_x1 = fx + pad + backend->measure_text(ctx, text.c_str(), hi, ef.size);
      backend->fill_rect(ctx, sel_x0, cline, sel_x1 - sel_x0, cheight,
                          neui_detail::color(neui_detail::ColorRole::accent_translucent));
    }

    float cursor_x = fx + pad + backend->measure_text(ctx, text.c_str(), cursor_pos, ef.size);

    // Composition overlay: render the in-progress IME string at the caret with
    // an underline. Suppress the regular caret - the IME caret position is
    // implied by the candidate-window rect we set via ImmSetCompositionWindow.
    if (composing && !composition_text.empty() && backend->draw_text) {
      float comp_w = backend->measure_text(ctx, composition_text.c_str(),
                                           static_cast<int>(composition_text.size()),
                                           ef.size);
      backend->draw_text(ctx, cursor_x, cline, comp_w, cheight,
                         composition_text.c_str(), ef.size,
                         neui_detail::color(neui_detail::ColorRole::text_primary));
      // Per-clause underlines based on composition_attrs (one byte per
      // UTF-8 byte). Falls back to a single 1px underline if attrs absent.
      paint_composition_underline(backend, ctx, cursor_x, cline + cheight - 2.0f,
                                   composition_text, composition_attrs, ef.size);
      neui_detail::pop_widget_font(backend, ctx, ef);
      return;
    }

    // Cursor
    if (!overwrite_mode) {
      backend->fill_rect(ctx, cursor_x, cline, 1.5f, cheight,
                          neui_detail::color(neui_detail::ColorRole::text_primary));
    } else {
      float char_w = 8.0f;
      if (cursor_pos < static_cast<int>(text.size())) {
        int char_end = cursor_pos + neui_detail::te_utf8_char_len(text, cursor_pos);
        float w_to_next = backend->measure_text(ctx, text.c_str(), char_end, ef.size);
        char_w = w_to_next - (cursor_x - fx - pad);
      }
      // Overwrite caret: 50%-alpha block over the next character.
      backend->fill_rect(ctx, cursor_x, cline, std::max(char_w, 2.0f), cheight,
                          neui_detail::with_alpha(
                            neui_detail::color(neui_detail::ColorRole::text_primary), 0x80));
    }

    neui_detail::pop_widget_font(backend, ctx, ef);
  }

  // Caret rect in widget-local logical pixels. Used by the platform layer to
  // position the IME candidate window. Mirrors the math in paint() above.
  bool InputBoxWidget::caret_rect_local(neui_render_backend_t* backend,
                                        neui_render_ctx_t ctx,
                                        float* out_x, float* out_y, float* out_h)
  {
    if (!backend || !backend->measure_text || !ctx) return false;
    const float pad = 4.0f;
    auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
    neui_detail::push_widget_font(backend, ctx, ef);
    float caret_x = pad + backend->measure_text(ctx, text.c_str(), cursor_pos, ef.size);
    if (composing && !composition_text.empty()) {
      caret_x += backend->measure_text(ctx, composition_text.c_str(),
                                       composition_caret, ef.size);
    }
    neui_detail::pop_widget_font(backend, ctx, ef);
    if (out_x) *out_x = caret_x;
    if (out_y) *out_y = 2.0f;
    if (out_h) *out_h = static_cast<float>(height) - 4.0f;
    return true;
  }

  // IME composition state machine for the single-line input box.
  bool InputBoxWidget::on_composition(int kind, const char* utf8,
                                      int byte_len, int caret_byte,
                                      const uint8_t* per_byte_attrs)
  {
    switch (kind) {
    case COMP_START:
      composition_pre_state = neui_detail::EditState{ text, cursor_pos, sel_anchor };
      composing             = true;
      composition_text.clear();
      composition_caret     = 0;
      composition_attrs.clear();
      repaint();
      return true;

    case COMP_UPDATE:
      composition_text.assign(utf8 ? utf8 : "", static_cast<size_t>(byte_len > 0 ? byte_len : 0));
      composition_caret = caret_byte;
      if (composition_caret < 0) composition_caret = 0;
      if (composition_caret > static_cast<int>(composition_text.size()))
        composition_caret = static_cast<int>(composition_text.size());
      if (per_byte_attrs && byte_len > 0) {
        composition_attrs.assign(per_byte_attrs, per_byte_attrs + byte_len);
      } else {
        composition_attrs.clear();
      }
      repaint();
      return true;

    case COMP_RESULT: {
      // Commit: replace selection (if any) and insert the result string,
      // pushing one undo entry against the pre-composition snapshot. The
      // composition overlay is left visible until COMP_END so that an IME
      // which chains a fresh composition after a result keeps painting.
      bool has_sel = neui_detail::te_has_selection(cursor_pos, sel_anchor);
      // History entry uses the pre-composition snapshot so undo restores the
      // state from before the user even started composing.
      history.mark(composition_pre_state,
                   neui_detail::EditHistory::Typing, has_sel);
      neui_detail::te_erase_selection(text, cursor_pos, sel_anchor);
      if (utf8 && byte_len > 0) {
        text.insert(static_cast<size_t>(cursor_pos), utf8, static_cast<size_t>(byte_len));
        cursor_pos += byte_len;
        sel_anchor  = cursor_pos;
      }
      // After commit, the next composition step (if any) starts from this
      // post-commit state - refresh the snapshot so a chained composition's
      // own commit pushes a separate undo entry.
      composition_pre_state = neui_detail::EditState{ text, cursor_pos, sel_anchor };
      composition_text.clear();
      composition_caret = 0;
      composition_attrs.clear();
      repaint();
      return true;
    }

    case COMP_END:
      composing = false;
      composition_text.clear();
      composition_caret = 0;
      composition_attrs.clear();
      repaint();
      return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // InputBoxWidget key handling

  // Defined below (just before on_mouse_event); used by the key handler too.
  static void publish_primary_selection(const std::string& text, int cursor, int anchor);

  bool InputBoxWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    using namespace neui_detail;
    const bool shift = (modifiers & NEUI_KMOD_SHIFT) != 0;
    const bool ctrl  = (modifiers & NEUI_KMOD_CTRL)  != 0;

    switch (keycode) {
    case NEUI_KEY_LEFT:
      te_move_left (text, cursor_pos, sel_anchor, ctrl, shift, &history);
      repaint(); return true;
    case NEUI_KEY_RIGHT:
      te_move_right(text, cursor_pos, sel_anchor, ctrl, shift, &history);
      repaint(); return true;
    case NEUI_KEY_HOME:
      te_move_home (text, cursor_pos, sel_anchor, shift, &history);
      repaint(); return true;
    case NEUI_KEY_END:
      te_move_end  (text, cursor_pos, sel_anchor, shift, &history);
      repaint(); return true;
    case NEUI_KEY_INSERT:
      history.reset_action();
      overwrite_mode = !overwrite_mode;
      repaint(); return true;
    case NEUI_KEY_BACK:
      te_backspace     (text, cursor_pos, sel_anchor, ctrl, &history);
      repaint(); return true;
    case NEUI_KEY_DELETE:
      te_delete_forward(text, cursor_pos, sel_anchor, ctrl, &history);
      repaint(); return true;
    case NEUI_KEY_A:
      if (ctrl) {
        te_select_all(text, cursor_pos, sel_anchor, &history);
        publish_primary_selection(text, cursor_pos, sel_anchor);
        repaint();
        return true;
      }
      break;
    case NEUI_KEY_C:
      if (ctrl) {
        std::string sel = te_selected_text(text, cursor_pos, sel_anchor);
        if (!sel.empty()) {
          platform_clipboard_set_text(sel.c_str(), (uint32_t)sel.size());
          platform_clipboard_set_primary(sel.c_str(), (uint32_t)sel.size());
        }
        return true;
      }
      break;
    case NEUI_KEY_X:
      if (ctrl) {
        bool readonly = attrs && attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
        std::string sel = te_selected_text(text, cursor_pos, sel_anchor);
        if (!sel.empty()) {
          platform_clipboard_set_text(sel.c_str(), (uint32_t)sel.size());
          if (!readonly) {
            history.mark(EditState{ text, cursor_pos, sel_anchor },
                         EditHistory::None, true);
            te_erase_selection(text, cursor_pos, sel_anchor);
            repaint();
          }
        }
        return true;
      }
      break;
    case NEUI_KEY_V:
      if (ctrl) {
        bool readonly = attrs && attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
        if (readonly) return true;
        int n = platform_clipboard_get_text(nullptr, 0);
        if (n > 0) {
          std::vector<char> buf((size_t)n);
          platform_clipboard_get_text(buf.data(), n);
          std::string paste(buf.data(), (size_t)(n > 0 ? n - 1 : 0));
          te_paste(text, cursor_pos, sel_anchor, paste,
                   /*strip_newlines=*/true, &history);
          repaint();
        }
        return true;
      }
      break;
    case NEUI_KEY_Z:
      if (ctrl) {
        if (shift) te_redo(text, cursor_pos, sel_anchor, history);
        else       te_undo(text, cursor_pos, sel_anchor, history);
        repaint();
        return true;
      }
      break;
    case NEUI_KEY_Y:
      if (ctrl) {
        te_redo(text, cursor_pos, sel_anchor, history);
        repaint();
        return true;
      }
      break;
    default:
      break;
    }
    return false;
  }

  // Built-in command dispatch - reuses on_keydown's existing branches by
  // synthesising the corresponding Ctrl+letter keystroke (modifiers bit 1
  // = Ctrl). NEUI_CMD_DELETE has no modifier.
  // Shared "is this a command a text widget knows how to handle" check.
  // Used by both InputBoxWidget and MultilineWidget can_perform_command.
  static bool text_widget_handles_cmd(uint32_t cmd)
  {
    switch (cmd) {
    case NEUI_CMD_UNDO:
    case NEUI_CMD_REDO:
    case NEUI_CMD_CUT:
    case NEUI_CMD_COPY:
    case NEUI_CMD_PASTE:
    case NEUI_CMD_SELECT_ALL:
    case NEUI_CMD_DELETE:
      return true;
    }
    return false;
  }

  bool InputBoxWidget::perform_command(uint32_t cmd)
  {
    constexpr uint32_t CTRL = 2;
    switch (cmd) {
    case NEUI_CMD_UNDO:       return on_keydown(NEUI_KEY_Z, CTRL);
    case NEUI_CMD_REDO:       return on_keydown(NEUI_KEY_Y, CTRL);
    case NEUI_CMD_CUT:        return on_keydown(NEUI_KEY_X, CTRL);
    case NEUI_CMD_COPY:       return on_keydown(NEUI_KEY_C, CTRL);
    case NEUI_CMD_PASTE:      return on_keydown(NEUI_KEY_V, CTRL);
    case NEUI_CMD_SELECT_ALL: return on_keydown(NEUI_KEY_A, CTRL);
    case NEUI_CMD_DELETE:     return on_keydown(NEUI_KEY_DELETE, 0);
    }
    return false;
  }

  bool InputBoxWidget::can_perform_command(uint32_t cmd) const
  {
    return text_widget_handles_cmd(cmd);
  }

  bool InputBoxWidget::on_keychar(uint32_t codepoint, uint32_t /*modifiers*/)
  {
    using namespace neui_detail;
    if (codepoint < 0x20 || codepoint == 0x7F) return false;
    char buf[4];
    int  n = te_encode_utf8(codepoint, buf);
    te_insert_utf8(text, cursor_pos, sel_anchor, overwrite_mode, buf, n,
                   &history);
    if (session) {
      void* frame = session->find_parent_native_handle(index);
      if (frame) platform_invalidate(frame);
    }
    return true;
  }

  // Publish a text widget's current selection to the X11 PRIMARY selection so
  // it can be middle-click-pasted (into another app or our own widgets). No-op
  // with no selection, and on Win32 / macOS (the seam is a no-op there).
  static void publish_primary_selection(const std::string& text, int cursor, int anchor)
  {
    if (!neui_detail::te_has_selection(cursor, anchor)) return;
    std::string sel = neui_detail::te_selected_text(text, cursor, anchor);
    if (!sel.empty())
      platform_clipboard_set_primary(sel.c_str(), static_cast<uint32_t>(sel.size()));
  }

  bool InputBoxWidget::insert_text(const std::string& utf8)
  {
    bool readonly = attrs && attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
    if (readonly || utf8.empty()) return false;
    neui_detail::te_paste(text, cursor_pos, sel_anchor, utf8,
                          /*strip_newlines=*/true, &history);
    repaint();
    return true;
  }

  bool InputBoxWidget::on_mouse_event(neui_event_t* event)
  {
    // Drag-select ends on button-up: publish the selection to PRIMARY.
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP) {
      publish_primary_selection(text, cursor_pos, sel_anchor);
      return false;
    }
    bool is_down    = (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN);
    bool is_dblclk  = (event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK);
    bool is_drag    = (event->type == NEUI_EVENT_MOUSE_MOVE) &&
                      (event->data.mouse.buttonmap & NEUI_MK_LBUTTON);
    if (!is_down && !is_drag && !is_dblclk) return false;

    // Any mouse interaction breaks the typing/deleting run for undo
    // grouping - the next typed character starts a fresh undo step.
    if (is_down || is_dblclk) history.reset_action();

    // Obtain the render backend and the parent frame's render context.
    neui_render_backend_t* backend = session ? session->_backend : nullptr;
    neui_render_ctx_t ctx = nullptr;
    if (session) {
      auto parents = session->_widgets.get_all_parents(index);
      for (uint32_t p : parents) {
        if (p == 0) continue;
        if (session->_widgets.exists(p)) {
          ctx = session->_widgets[p].render_ctx;
          if (ctx) break;
        }
      }
    }

    // Click position relative to the start of the text (matches paint()'s pad=4).
    // event.mouse.x is frame-local; abs_x is the widget's frame-local
    // origin, so the difference is widget-local.
    const float pad = 4.0f;
    float click_x = static_cast<float>(event->data.mouse.x)
                    - static_cast<float>(abs_x) - pad;

    // Find the byte offset of the character boundary closest to click_x.
    // Uses midpoint snapping: a click past the midpoint of a character maps
    // to the position after that character.
    int new_pos = static_cast<int>(text.size());   // default: end of string
    if (backend && backend->measure_text && ctx) {
      auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
      neui_detail::push_widget_font(backend, ctx, ef);
      int pos      = 0;
      float prev_w = 0.0f;  // measure_text for 0 bytes is always 0
      int len = static_cast<int>(text.size());
      while (pos < len) {
        int char_end = pos + neui_detail::te_utf8_char_len(text, pos);
        float end_w  = backend->measure_text(ctx, text.c_str(), char_end, ef.size);
        if (click_x < (prev_w + end_w) * 0.5f) {
          new_pos = pos;
          break;
        }
        prev_w = end_w;
        pos    = char_end;
      }
      neui_detail::pop_widget_font(backend, ctx, ef);
    }

    // Double-click selects the word at the click position.
    if (is_dblclk) {
      int ws, we;
      neui_detail::te_word_bounds(text, new_pos, ws, we);
      sel_anchor = ws;
      cursor_pos = we;
      publish_primary_selection(text, cursor_pos, sel_anchor);
      repaint();
      return true;
    }

    cursor_pos = new_pos;
    if (is_down) {
      bool shift = (event->data.mouse.buttonmap & NEUI_MK_SHIFT) != 0;
      if (!shift) sel_anchor = new_pos;
    }
    // For drag (is_drag): sel_anchor stays at the click origin, cursor_pos moves.

    repaint();
    return true;
  }

  // -------------------------------------------------------------------------
  // ImageWidget paint

  void ImageWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                           bool /*is_focused*/)
  {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);

    using neui_detail::ColorRole;
    if (!backend->create_bitmap || !session) {
      backend->fill_rect(ctx, fx, fy, fw, fh,
                          neui_detail::color(ColorRole::control_bg_inactive));
      return;
    }

    float scale = backend->get_scale_factor ? backend->get_scale_factor(ctx) : 1.0f;
    void* bmp = nullptr;
    float img_w = 0.0f, img_h = 0.0f;

    if (asset.id != asset_none.id) {
      // Asset branch: handle-keyed source. Mirrors xpl_painter_draw_asset
      // _thunk - resolve the slot, lazy upload with device-loss
      // generation check, pull intrinsic dimensions from the entry
      // directly (no separate get_logical_size lookup needed).
      uint32_t asset_sess = (asset.id >> 16) & 0xffff;
      if (asset_sess == (session->get_session_id() & 0xffff)) {
        uint32_t slot = asset.id & 0xffff;
        auto* entry = session->_asset_manager.get_slot(slot);
        if (entry) {
          const uint32_t gen = backend->get_context_generation
            ? backend->get_context_generation(ctx) : 0u;
          auto it = entry->bitmaps.find(ctx);
          if (it != entry->bitmaps.end() && it->second.generation != gen) {
            if (backend->destroy_bitmap && it->second.bmp)
              backend->destroy_bitmap(ctx, it->second.bmp);
            entry->bitmaps.erase(it);
            it = entry->bitmaps.end();
          }
          if (it == entry->bitmaps.end()) {
            void* new_bmp = backend->create_bitmap(ctx,
                                                    entry->width_px, entry->height_px,
                                                    entry->pixels.data(),
                                                    entry->scale);
            if (new_bmp) {
              it = entry->bitmaps.emplace(ctx,
                                           neui_detail::CtxBitmap{ new_bmp, gen }).first;
            }
          }
          if (it != entry->bitmaps.end()) {
            bmp = it->second.bmp;
            if (entry->scale > 0.0f) {
              img_w = static_cast<float>(entry->width_px)  / entry->scale;
              img_h = static_cast<float>(entry->height_px) / entry->scale;
            }
          }
        }
        // entry == nullptr means the client destroyed the asset while
        // this widget still references it - paint empty, no draw.
      }
    } else if (!text.empty()) {
      // Legacy path branch.
      bmp = session->_asset_manager.get_bitmap(text, scale, backend, ctx);
      if (bmp)
        session->_asset_manager.get_logical_size(text, scale, &img_w, &img_h);
    } else {
      // No source bound - paint inactive fill, same as before.
      backend->fill_rect(ctx, fx, fy, fw, fh,
                          neui_detail::color(ColorRole::control_bg_inactive));
      return;
    }

    // Compute the destination rectangle that preserves the bitmap's
    // intrinsic aspect ratio inside the widget bounds (uniform "fit",
    // also known as letterbox / pillarbox), and centre it.
    float dst_x = fx, dst_y = fy, dst_w = fw, dst_h = fh;
    if (bmp && img_w > 0.0f && img_h > 0.0f) {
      float widget_aspect = fw / fh;
      float image_aspect  = img_w / img_h;
      if (image_aspect > widget_aspect) {
        // Image is wider than the widget - fit width, letterbox top/bottom.
        dst_w = fw;
        dst_h = fw / image_aspect;
        dst_x = fx;
        dst_y = fy + (fh - dst_h) * 0.5f;
      } else {
        // Image is taller - fit height, pillarbox left/right.
        dst_h = fh;
        dst_w = fh * image_aspect;
        dst_y = fy;
        dst_x = fx + (fw - dst_w) * 0.5f;
      }
    }

    // Optional rotation around the image centre. NEUI_ATTR_ROTATION is in
    // radians; positive = clockwise on screen (Y axis is screen-down).
    // Rotation pivots around the *destination* centre so the image keeps
    // its visible centre during rotation regardless of letterboxing.
    float rot = (attrs ? attrs->get_float(NEUI_ATTR_ROTATION, 0.0f) : 0.0f);
    bool rotated = (rot != 0.0f) && backend->push_transform != nullptr;
    if (rotated) {
      float cx = dst_x + dst_w * 0.5f;
      float cy = dst_y + dst_h * 0.5f;
      backend->push_transform(ctx);
      backend->translate(ctx,  cx,  cy);
      backend->rotate   (ctx, rot);
      backend->translate(ctx, -cx, -cy);
    }

    if (bmp) {
      backend->draw_bitmap(ctx, bmp, 0.0f, 0.0f, 0.0f, 0.0f,
                           dst_x, dst_y, dst_w, dst_h,
                           0xFFFFFFFFu);
    } else {
      backend->fill_rect(ctx, fx, fy, fw, fh,
                          neui_detail::color(ColorRole::control_bg_inactive));
      auto ef = neui_detail::read_widget_font(attrs.get(), 10.0f);
      neui_detail::push_widget_font(backend, ctx, ef);
      backend->draw_text(ctx, fx + 2.0f, fy, fw - 4.0f, fh,
                          text.c_str(), ef.size,
                          neui_detail::color(ColorRole::text_disabled));
      neui_detail::pop_widget_font(backend, ctx, ef);
    }

    if (rotated) backend->pop_transform(ctx);
  }

  // -------------------------------------------------------------------------
  // MenubarWidget cleanup

  void MenubarWidget::on_destroy(Session* s)
  {
#ifdef _WIN32
    if (native_accel) {
      DestroyAcceleratorTable(static_cast<HACCEL>(native_accel));
      native_accel = nullptr;
    }
#else
    // macOS: NSMenuItem.keyEquivalent is owned by the NSMenuItem itself,
    // freed when the NSMenu is released. native_accel is unused.
    native_accel = nullptr;
#endif
    if (hmenu) {
      // Release ONLY the root. A submenu is owned by the menu it is attached to
      // on both platforms: win32 DestroyMenu on the root destroys every attached
      // submenu with it, and on macOS platform_menubar_add_popup hands back a
      // NON-owning (__bridge void*) while the parent NSMenuItem holds the only
      // strong reference. Releasing them individually first was therefore a
      // double-free on win32, and on macOS an over-release that raised
      // NSInternalInconsistencyException from -[NSMenu dealloc] ("A submenu is
      // being released before being detached from its parent menu") - an
      // UNCAUGHT ObjC exception, i.e. a hard abort when destroying any menubar
      // that had a popup in it. Nothing leaks: t_remove detaches-and-destroys,
      // so no orphan submenu is reachable from menu_items.
      platform_menubar_destroy(hmenu);
      hmenu = nullptr;
      s->_menubars.erase(
        std::remove(s->_menubars.begin(), s->_menubars.end(), index),
        s->_menubars.end());
    }
  }

  // -------------------------------------------------------------------------
  // Painting

  // Walk the widget tree, painting children of `parent_index` and
  // recursing into each child. Each child's `wd.x` / `wd.y` is interpreted
  // as relative to its parent (the win32 host's natural HWND-relative
  // semantics); the absolute frame position is accumulated through
  // (parent_abs_x, parent_abs_y) and cached on each widget as
  // (abs_x, abs_y) for hit-test and event-coord conversion. Painting
  // happens with the renderer transform set so the child paints at its
  // own (x, y) which maps to the correct absolute position - so widget
  // paint code can stay coordinate-agnostic.
  // Where a widget's CHILDREN start - in both the parent-relative space the
  // renderer's translate uses and the frame-local absolute space cached on each
  // widget. Extracted so the painting walk and the non-painting
  // refresh_abs_positions walk share ONE definition of the offset rules and
  // cannot drift apart.
  //
  // Note the direction of the dependency: this READS the layout a SECTION /
  // TABVIEW cached during its own paint (section_layout_ptr) instead of
  // recomputing it. That is deliberate - TabViewWidget's body rect depends on
  // backend->measure_text for the chip widths (host.cpp:1665-1685), so it is not
  // reproducible without a live render context. Layout computation therefore
  // stays single-sourced in the paint path; only the offset arithmetic is
  // shared. Before a frame's first paint these caches are empty, which is what
  // WidgetData::painted_once / Session::ensure_abs_positions deal with.
  struct ChildOrigin { int rel_x, rel_y, abs_x, abs_y; };

  static ChildOrigin child_origin_of(WidgetData& wd)
  {
    ChildOrigin o{ wd.x, wd.y, wd.abs_x, wd.abs_y };
    // Children of a SECTION / TABVIEW are positioned relative to the BODY's
    // top-left, and a scrolling section additionally shifts by (-scroll).
    const auto* slay = wd.section_layout_ptr();
    if (slay) {
      auto* sst = wd.scroll_state_ptr();
      const int sx = sst ? sst->scroll_x : 0;
      const int sy = sst ? sst->scroll_y : 0;
      o.rel_x = wd.x     + slay->body_x - sx;
      o.rel_y = wd.y     + slay->body_y - sy;
      o.abs_x = wd.abs_x + slay->body_x - sx;
      o.abs_y = wd.abs_y + slay->body_y - sy;
    }
    return o;
  }

  static void paint_widgets_recursive(neui_render_backend_t* backend,
                                      neui_render_ctx_t ctx,
                                      neui_detail::Tree<WidgetData>& widgets,
                                      uint32_t parent_index,
                                      int parent_abs_x, int parent_abs_y,
                                      uint32_t focused_widget)
  {
    uint32_t idx = widgets.child(parent_index);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& wd = widgets[idx];
        wd.abs_x = parent_abs_x + wd.x;
        wd.abs_y = parent_abs_y + wd.y;
        if (!wd.native_handle && !wd.is_menu_model() && wd.visible && wd.width > 0 && wd.height > 0) {
          // Fire WIDGET_PREUPDATE to opted-in widgets so the client can
          // refresh attribute-driven state (e.g. NEUI_PARAM_VALUE) before
          // we read it during paint.
          if (wd.emit_events && wd.session) {
            neui_event_t pre{};
            pre.type = NEUI_EVENT_WIDGET_PREUPDATE;
            pre.data.preupdate.widget.id = idx;
            wd.session->dispatch_event(&pre);
          }
          // Dim disabled widgets uniformly via the backend's alpha stack.
          // Per-widget dim (not subtree); a parent's disabled flag does
          // not propagate to children in v1.
          bool dim = !wd.enabled && backend->push_alpha && backend->pop_alpha;
          if (dim) backend->push_alpha(ctx, 0.5f);
          wd.paint(backend, ctx, idx == focused_widget);
          if (dim) backend->pop_alpha(ctx);
        }
        // SECTION descent: children's stored (x, y) is relative to the
        // BODY's top-left, so we add (body_x, body_y) into the translate.
        // Chip "none" zeroes body_y so children fill from the section's
        // top edge; chip present shifts children below the band.
        // Scrolling sections additionally apply (-scroll_x, -scroll_y).
        // The body clip is pushed unconditionally for any SECTION so
        // children that overflow the body stay inside the container's
        // bounds (matching the visual expectation of "section as a
        // bounded region").
        // Descend into children only when this widget is visible - an
        // invisible container hides its whole subtree (e.g. a TABVIEW's
        // non-selected TABPAGE children). Without this gate the children of
        // a hidden parent would still paint.
        if (wd.visible) {
        const auto* slay = wd.section_layout_ptr();
        const ChildOrigin org = child_origin_of(wd);
        const int child_origin_x = org.rel_x;
        const int child_origin_y = org.rel_y;
        const int abs_origin_x   = org.abs_x;
        const int abs_origin_y   = org.abs_y;
        bool pushed_clip = false;
        if (slay) {
          if (backend->push_clip && slay->body_w > 0 && slay->body_h > 0) {
            backend->push_clip(ctx,
                                static_cast<float>(wd.x + slay->body_x),
                                static_cast<float>(wd.y + slay->body_y),
                                static_cast<float>(slay->body_w),
                                static_cast<float>(slay->body_h));
            pushed_clip = true;
          }
        }
        // Translate so the child's descendants - which store coords
        // relative to the child - draw at the correct absolute position.
        if (backend->push_transform) backend->push_transform(ctx);
        if (backend->translate)
          backend->translate(ctx, static_cast<float>(child_origin_x),
                                  static_cast<float>(child_origin_y));
        paint_widgets_recursive(backend, ctx, widgets, idx,
                                abs_origin_x, abs_origin_y, focused_widget);
        // After-children hook: widget-local coords are active here (we
        // translated by (wd.x, wd.y) above and have not popped yet).
        // Used by CUSTOMDRAW + compound to paint z>=0 layers above the
        // child-widget pass; default implementation is a no-op.
        if (!wd.native_handle && !wd.is_menu_model() && wd.visible && wd.width > 0 && wd.height > 0) {
          wd.paint_after_children(backend, ctx, idx == focused_widget);
        }
        if (backend->pop_transform) backend->pop_transform(ctx);
        if (pushed_clip && backend->pop_clip) backend->pop_clip(ctx);
        } // wd.visible descent gate
      }
      idx = widgets.next(idx);
    }
  }

  uint32_t Session::frame_clear_color(uint32_t parent_index)
  {
    // Mirror paint_frame's palette selection + clear-colour read, but
    // self-contained (restore the override on return) since this runs outside
    // a paint (WM_ERASEBKGND). Keep in sync with paint_frame below.
    const neui_detail::Palette* prev_override =
      neui_detail::active_palette_override_ptr();
    bool follow = false;
    if (parent_index < UINT32_MAX) {
      WidgetData* fw = get_widget(parent_index);
      if (fw && fw->attrs &&
          fw->attrs->get_int(NEUI_ATTR_FOLLOW_SYSTEM_THEME, 0) != 0)
        follow = true;
    }
    neui_detail::set_active_palette_override(
      follow ? &_effective_palette : &_frozen_palette);
    uint32_t clear = neui_detail::color(neui_detail::ColorRole::frame_bg);
    if (parent_index < UINT32_MAX) {
      WidgetData* fw = get_widget(parent_index);
      if (fw && fw->attrs && fw->attrs->has(NEUI_ATTR_BACKGROUND))
        clear = static_cast<uint32_t>(fw->attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
    }
    neui_detail::set_active_palette_override(prev_override);
    return clear;
  }

  void Session::paint_frame(neui_render_ctx_t ctx, uint32_t parent_index)
  {
    if (!_backend || !ctx) return;
    // In-paint guard + accessibility cache key, both for the WHOLE paint
    // including the client callbacks it makes (PREUPDATE / WIDGET_PAINT). A
    // client can call into the framework from those, and an accessibility query
    // that forced a second paint on this same render context would corrupt the
    // frame - see Session::ensure_abs_positions and in_paint().
    //
    // The bump is here rather than at each mutation site because a paint is the
    // one thing every visible change has in common; see a11y_revision().
    struct PaintScope
    {
      int& depth;
      explicit PaintScope(int& d) : depth(d) { ++depth; }
      ~PaintScope() { --depth; }
    } paint_scope(_in_paint);
    bump_a11y_revision();
    // Choose the palette for this frame BEFORE reading any colour: the
    // begin_frame clear, the widget paints, the combo / popup overlays,
    // and the focus outline all flow through current_palette(). Frames
    // with NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1 use the live tracking
    // palette; the rest use the snapshot taken at session creation /
    // last NEUI_ATTR_THEME_MODE flip, which is invariant to OS theme.
    const neui_detail::Palette* prev_override =
      neui_detail::active_palette_override_ptr();
    bool follow = false;
    if (parent_index < UINT32_MAX) {
      WidgetData* fw = get_widget(parent_index);
      if (fw && fw->attrs &&
          fw->attrs->get_int(NEUI_ATTR_FOLLOW_SYSTEM_THEME, 0) != 0)
        follow = true;
    }
    neui_detail::set_active_palette_override(
      follow ? &_effective_palette : &_frozen_palette);

    // Clear to the (now-selected) frame colour. NEUI_ATTR_BACKGROUND on
    // the frame widget overrides if set (cheap per-paint lookup; the
    // AttrBag is null on most frames so the branch is a single pointer
    // test).
    uint32_t clear = neui_detail::color(neui_detail::ColorRole::frame_bg);
    // Also the source of the frame's zoom below, so it is looked up once here
    // rather than per-consumer.
    WidgetData* fw = (parent_index < UINT32_MAX) ? get_widget(parent_index)
                                                 : nullptr;
    if (fw && fw->attrs && fw->attrs->has(NEUI_ATTR_BACKGROUND))
      clear = static_cast<uint32_t>(fw->attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
    _backend->begin_frame(ctx, clear);

    // User zoom (NEUI_ATTR_UI_SCALE). One CTM scale wrapping the entire frame
    // paint - the widget walk AND every overlay below - so all of it scales
    // together and nothing else in the host has to know about zoom. Held on
    // the Session for the duration of the paint so the painters handed to
    // client CUSTOMDRAW code can report the true device scale (and undo this
    // transform for a device-pixel widget).
    //
    // This is the only rendering seam that works identically on all three
    // backends: D2D and Cairo would also accept a dpi*Z lie, but a CG window
    // context applies no CTM scale of its own at all (AppKit hands it a
    // point-based context), so update_dpi would not zoom macOS.
    //
    // Deliberately a transform and not a change to any logical number:
    // measure_text is transform-independent on every backend, so every rect
    // derived from text (combo drop width, menubar columns, popup, toast)
    // stays in the same logical space as the cached abs_x/abs_y and the
    // logical mouse coordinates - which is what keeps paint and hit-test
    // agreeing for free. Font caches key on (family, weight, size) with no
    // DPI, so zoom needs no cache invalidation either.
    const float zoom = (fw ? fw->ui_scale() : 1.0f);
    _paint_zoom = zoom;
    const bool zooming = (zoom != 1.0f) && _backend->push_transform
                         && _backend->scale && _backend->pop_transform;
    if (zooming) {
      _backend->push_transform(ctx);
      _backend->scale(ctx, zoom, zoom);
    }

    // While the frame doesn't own OS keyboard focus, suppress focus
    // decorations (caret, focus outline) by reporting "no focused widget" to
    // the painters. The logical focus is preserved so input routing snaps
    // back when the frame regains OS focus.
    uint32_t focus_for_paint = _os_focused ? _focused_widget : 0;
    // Frame is the root of the absolute coord space - start the walk at
    // (0, 0). The frame's own widget rect isn't painted by this walk
    // (the begin_frame above did the clear); recursion enters its
    // children directly. When this platform draws the menubar in-frame and
    // the frame has one, reserve a top band: offset the whole child walk down
    // by `inset` (so cached abs_x/abs_y stay screen-accurate for hit-test) and
    // paint the band over the cleared strip afterwards. inset is 0 on Win32 /
    // macOS (native menu) and for frames without a menubar.
    // Mark the frame as having painted at least once. ensure_abs_positions
    // reads this to decide whether the SECTION / TABVIEW body-layout caches it
    // depends on have been populated yet.
    if (parent_index != 0 && _widgets.exists(parent_index)) {
      _widgets[parent_index].painted_once = true;
      _widgets[parent_index].layout_dirty = false;
    }
    int inset = frame_top_inset(parent_index);
    if (inset > 0 && _backend->push_transform && _backend->translate) {
      _backend->push_transform(ctx);
      _backend->translate(ctx, 0.0f, (float)inset);
      paint_widgets_recursive(_backend, ctx, _widgets, parent_index, 0, inset, focus_for_paint);
      _backend->pop_transform(ctx);
    } else {
      paint_widgets_recursive(_backend, ctx, _widgets, parent_index, 0, 0, focus_for_paint);
    }
    // Draw the open combo overlay on top of all other widgets.
    if (_open_combo != 0 && _widgets.exists(_open_combo)) {
      // Only paint if the combo belongs to this frame.
      auto parents = _widgets.get_all_parents(_open_combo);
      bool in_frame = false;
      for (uint32_t p : parents) if (p == parent_index) { in_frame = true; break; }
      if (in_frame) {
        auto* cb = dynamic_cast<ComboBoxWidget*>(&_widgets[_open_combo]);
        if (cb) cb->paint_overlay(_backend, ctx);
      }
    }
    // In-frame menubar band (+ open cascading dropdowns) over the reserved
    // top strip. No-op when the frame has no menubar / native-menu platforms.
    paint_menubar(ctx, parent_index);
    // Popup-menu overlay sits on top of the combo overlay.
    paint_popup_menu(ctx);
    // Standalone tree popup (widgets->popup_tree_menu) - above the menubar's own
    // cascade, since it is the thing the user just opened.
    paint_tree_popup(ctx, parent_index);
    // Toast overlay sits on top of every other overlay.
    paint_toast(ctx, parent_index);

    if (zooming) _backend->pop_transform(ctx);
    _paint_zoom = 1.0f;
    _backend->end_frame(ctx);

    // Restore the previous override so non-paint callers (event
    // handlers, theme provider, other frames painted in the same pump
    // iteration) see the session's default tracking palette again.
    neui_detail::set_active_palette_override(prev_override);
  }

  // -------------------------------------------------------------------------
  // Absolute-position refresh without painting
  //
  // abs_x/abs_y are normally a by-product of paint_widgets_recursive, which is
  // fine for hit-testing (input cannot arrive before the first paint) but not
  // for anything that must answer a positional question out-of-band - an
  // accessibility provider being the motivating case, since a screen reader can
  // probe a window that has not painted yet. This walk reproduces exactly the
  // origin arithmetic the paint walk uses (via the shared child_origin_of) and
  // nothing else: no PREUPDATE dispatch, no drawing, no layout recomputation.
  static void refresh_abs_positions_recursive(neui_detail::Tree<WidgetData>& widgets,
                                              uint32_t parent_index,
                                              int parent_abs_x, int parent_abs_y)
  {
    uint32_t idx = widgets.child(parent_index);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& wd = widgets[idx];
        wd.abs_x = parent_abs_x + wd.x;
        wd.abs_y = parent_abs_y + wd.y;
        // Descend only into visible containers, matching the paint walk - an
        // invisible parent hides its whole subtree, so its descendants' cached
        // positions are meaningless either way.
        if (wd.visible) {
          const ChildOrigin org = child_origin_of(wd);
          refresh_abs_positions_recursive(widgets, idx, org.abs_x, org.abs_y);
        }
      }
      idx = widgets.next(idx);
    }
  }

  void Session::refresh_abs_positions(uint32_t frame_index)
  {
    if (frame_index == 0 || !_widgets.exists(frame_index)) return;
    // Same root offset the paint walk starts from: the in-frame menubar band on
    // hosts that draw it (0 on win32 / macOS and for frames without a menubar).
    refresh_abs_positions_recursive(_widgets, frame_index, 0,
                                    frame_top_inset(frame_index));
  }

  bool Session::ensure_abs_positions(uint32_t frame_index)
  {
    if (frame_index == 0 || !_widgets.exists(frame_index)) return false;
    // Never from inside a paint. The force-paint below would re-enter
    // paint_frame on a render context that is mid-frame, and the reachable path
    // is real: an accessibility query arriving while a client WIDGET_PREUPDATE
    // or WIDGET_PAINT handler runs. Refusing costs one unanswered query; the
    // alternative is a corrupted frame or a crash inside a backend.
    if (in_paint()) return false;
    auto& fw = _widgets[frame_index];
    // The layout state child_origin_of reads (SECTION / TABVIEW body rects) is
    // produced only by a paint. Two cases need one forcing:
    //   - the frame has never painted, so every cache is empty;
    //   - the widget tree changed since the last paint (post-show dynamic
    //     creation is a supported pattern), so a NEW section's cache is empty
    //     even though the frame has painted before.
    // Forcing a real paint beats duplicating the layout code: TabViewWidget's
    // body rect needs backend->measure_text, so a non-painting recomputation
    // could not match it anyway. Cheap - Wave 5 measured a full 100-widget frame
    // at ~0.9 ms, and this is once per change, not per query.
    if ((!fw.painted_once || fw.layout_dirty) && fw.native_handle)
      platform_force_paint(fw.native_handle);
    // If the paint did not happen (hidden / unmapped window, or not yet
    // realized), the caches are still empty and walking now would OVERWRITE the
    // cached geometry with band-less, partially-wrong values. The documented
    // contract is that geometry goes stale rather than wrong, so leave it alone
    // and report failure; the caller decides what to do with an unanswerable
    // positional query.
    if (!fw.painted_once) return false;
    refresh_abs_positions(frame_index);
    return true;
  }

  // Mark the frame owning `widget_index` as needing a paint before its cached
  // layout can be trusted. Called from the structural mutations (create /
  // destroy / geometry / show / hide); cleared by paint_frame.
  void Session::mark_layout_dirty(uint32_t widget_index)
  {
    uint32_t frame = frame_of(widget_index);
    if (frame != 0 && _widgets.exists(frame))
      _widgets[frame].layout_dirty = true;
    // A structural change must invalidate the accessibility cache WITHOUT
    // waiting for a paint. Paint is the invalidation signal for everything a
    // repaint expresses (see a11y_revision), but a destroy is exactly the case
    // where waiting is wrong: not every mutation path repaints promptly, and a
    // provider answering from a tree that still contains a destroyed widget
    // would hand an AT its name, role and rectangle - a wrong answer, not a
    // stale one. Cheap: an increment, and the rebuild only happens if something
    // actually queries.
    bump_a11y_revision();
  }

  bool Session::is_in_subtree(uint32_t widget_index, uint32_t root_index) const
  {
    if (widget_index == 0 || root_index == 0) return false;
    if (widget_index == root_index) return true;
    if (!_widgets.exists(widget_index)) return false;
    for (uint32_t p : _widgets.get_all_parents(widget_index))
      if (p == root_index) return true;
    return false;
  }

  void Session::focus_leave_subtree(uint32_t root_index, bool try_next)
  {
    if (_focused_widget == 0) return;
    if (!is_in_subtree(_focused_widget, root_index)) return;

    if (try_next) {
      // Frame-scoped, so this cannot move focus into another window. Resolve the
      // frame BEFORE moving focus: frame_of walks up from the widget, and the
      // widget we are moving away from is the only reliable route to it.
      const uint32_t frame = frame_of(root_index);
      focus_next(true, frame);
      // focus_next cannot land back inside an invisible subtree (collect_tab_stops
      // does not descend into one), but it CAN come back to the same widget when
      // the frame has no other tab stop - in which case clearing is the honest
      // answer rather than leaving focus where it was.
      if (_focused_widget != 0 && !is_in_subtree(_focused_widget, root_index))
        return;
    }
    set_focus(0);
  }

  // The tree slot of the FRAME owning `widget_index` (a root child), or 0.
  uint32_t Session::frame_of(uint32_t widget_index) const
  {
    if (widget_index == 0 || !_widgets.exists(widget_index)) return 0;
    if (_widgets.get_parent(widget_index) == 0) return widget_index;
    auto parents = _widgets.get_all_parents(widget_index);
    // get_all_parents walks upward and STOPS at knone, so its last entry is the
    // root SENTINEL (slot 0), not the root child - scan from the end for the
    // first entry whose own parent is the sentinel. The sentinel itself fails
    // that test (get_parent(0) == knone), so it is skipped rather than returned.
    for (size_t i = parents.size(); i-- > 0; )
      if (_widgets.exists(parents[i]) && _widgets.get_parent(parents[i]) == 0)
        return parents[i];
    return 0;
  }

  // -------------------------------------------------------------------------
  // Tab stop / focus cycling

  static void collect_tab_stops(neui_detail::Tree<WidgetData>& widgets,
                                  uint32_t parent_index,
                                  std::vector<uint32_t>& out)
  {
    uint32_t idx = widgets.child(parent_index);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& wd = widgets[idx];
        if (!wd.native_handle && !wd.is_menu_model() && wd.visible && wd.tab_stop && wd.enabled)
          out.push_back(idx);
        // Do NOT descend into an invisible container - matching the paint walk,
        // which gates descent on visibility too. This used to recurse
        // unconditionally, so Tab could land on a visible child of a hidden
        // parent: most sharply on a TABVIEW, whose unselected pages are
        // `visible = false` while their controls are not, which let Tab move
        // focus onto a control on a page that is not on screen.
        if (wd.visible)
          collect_tab_stops(widgets, idx, out);
      }
      idx = widgets.next(idx);
    }
  }

  void Session::focus_next(bool forward, uint32_t frame_hint)
  {
    // Traversal is PER FRAME. Collecting across every root child would let TAB
    // walk out of one window and into a widget in another - each frame owns its
    // own native surface and its own OS keyboard focus, so moving the logical
    // focus there would leave the two disagreeing (and the user pressing keys at
    // a window that doesn't hold focus).
    //
    // `frame_hint` is the frame whose native surface DELIVERED the Tab, which the
    // platform layer always knows. Prefer it over the focused widget's frame: it
    // is the only source that is right when nothing is focused yet, and it is
    // what makes "Tab at a freshly-opened second window" focus that window's
    // first control instead of some other frame's.
    uint32_t frame = 0;
    if (frame_hint != 0 && _widgets.exists(frame_hint) &&
        _widgets[frame_hint].is_frame())
      frame = frame_hint;
    if (frame == 0) frame = frame_of(_focused_widget);
    if (frame == 0) {
      // Last resort (no hint, nothing focused): the first root child that is
      // actually a REALIZED frame. Testing `visible` here would be useless -
      // every widget is created visible=true (widgets.cpp:191) and hide() on a
      // realized frame deliberately leaves the flag alone (widgets.cpp:403-406),
      // so `visible` does not distinguish shown frames from unshown ones.
      // native_handle does, and it also skips a menu-model root (POPUPMENU is
      // isroot but is not a window and holds no tab stops).
      uint32_t rc = _widgets.child(0);
      while (rc != 0) {
        if (_widgets.exists(rc)) {
          auto& cand = _widgets[rc];
          if (cand.is_frame() && cand.native_handle) { frame = rc; break; }
        }
        rc = _widgets.next(rc);
      }
    }
    if (frame == 0 || !_widgets.exists(frame)) return;

    std::vector<uint32_t> stops;
    collect_tab_stops(_widgets, frame, stops);

    if (stops.empty()) return;

    int cur = -1;
    for (int i = 0; i < static_cast<int>(stops.size()); ++i) {
      if (stops[static_cast<size_t>(i)] == _focused_widget) { cur = i; break; }
    }

    int next;
    if (cur < 0) {
      next = forward ? 0 : static_cast<int>(stops.size()) - 1;
    } else {
      int delta = forward ? 1 : -1;
      next = (cur + delta + static_cast<int>(stops.size()))
             % static_cast<int>(stops.size());
    }

    set_focus(stops[static_cast<size_t>(next)]);
  }

  // -------------------------------------------------------------------------
  // Mouse dispatch

  void Session::dispatch_mouse_event(uint32_t widget_idx, neui_event_t* ev)
  {
    if (widget_idx == 0 || !_widgets.exists(widget_idx)) return;
    auto& w = _widgets[widget_idx];
    if (!w.emit_events) return;
    if (!w.enabled) return;  // disabled widgets don't receive mouse events
    // The platform layers fill ev->data.mouse.x/y in FRAME-local logical px
    // (one shared HWND/NSView per frame). The client-facing contract is
    // WIDGET-local - matching the native win32 / macOS hosts (per-widget
    // HWND/NSView) and the DnD payload (see neui_event_dnd_t). So translate
    // by the target's frame-local origin (abs_x/abs_y) just for the client
    // callback, then restore frame-local before the internal on_mouse_event
    // handler, which subtracts abs_x/abs_y itself.
    const int frame_x = ev->data.mouse.x;
    const int frame_y = ev->data.mouse.y;

    // Remember where the pointer was, in FRAME-local px, so
    // begin_relative_pointer can seed its virtual position from the actual press
    // point. It is called from a client's MOUSE_BUTTON_DOWN handler, i.e. from
    // inside this very dispatch, so the value is always fresh at that moment.
    // Skipped while relative mode is active: those coordinates are virtual and
    // unbounded, and feeding them back would corrupt the next drag's seed.
    if (!_relative.active) {
      _last_mouse_frame_x = (float)frame_x;
      _last_mouse_frame_y = (float)frame_y;
      _last_mouse_valid   = true;
    }

    ev->data.mouse.x = frame_x - w.abs_x;
    ev->data.mouse.y = frame_y - w.abs_y;
    const bool handled = dispatch_event(ev);
    ev->data.mouse.x = frame_x;
    ev->data.mouse.y = frame_y;
    if (!handled)
      w.on_mouse_event(ev);
  }

  // Wheel events bubble up the widget-tree parent chain so a scrolling
  // SECTION ancestor consumes the wheel when the inner widget under the
  // cursor (e.g. a LABEL inside the section) doesn't handle it.
  bool Session::dispatch_wheel_event(uint32_t widget_idx, neui_event_t* ev,
                                      uint32_t stop_before)
  {
    auto try_widget = [&](uint32_t i) -> bool {
      if (i == 0 || !_widgets.exists(i)) return false;
      auto& w = _widgets[i];
      if (!w.emit_events || !w.enabled) return false;
      // Stamp the bubbling target onto the event so the client (and any
      // widget code reading ev->data.wheel.widget) sees the actual recipient.
      ev->data.wheel.widget = { w.widget_id };
      // Frame-local -> widget-local for the client callback (each bubble
      // recipient sees coords local to itself), restored to frame-local
      // before the internal handler. See dispatch_mouse_event for rationale.
      const int frame_x = ev->data.wheel.x;
      const int frame_y = ev->data.wheel.y;
      ev->data.wheel.x = frame_x - w.abs_x;
      ev->data.wheel.y = frame_y - w.abs_y;
      const bool handled = dispatch_event(ev);
      ev->data.wheel.x = frame_x;
      ev->data.wheel.y = frame_y;
      if (handled) return true;
      return w.on_mouse_event(ev);
    };
    if (widget_idx == stop_before) return false;
    if (try_widget(widget_idx)) return true;
    auto parents = _widgets.get_all_parents(widget_idx);
    for (uint32_t p : parents) {
      if (stop_before != 0 && p == stop_before) return false;
      if (try_widget(p)) return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Key dispatch (thin: delegates to focused widget's virtual methods)

  bool Session::handle_input_key(neui_event_type_t type, uint32_t keycode,
                                   uint32_t modifiers)
  {
    if (_focused_widget == 0 || !_widgets.exists(_focused_widget)) return false;
    auto& w = _widgets[_focused_widget];
    if (type == NEUI_EVENT_KEYDOWN) return w.on_keydown(keycode, modifiers);
    if (type == NEUI_EVENT_KEYCHAR) return w.on_keychar(keycode, modifiers);
    return false;
  }

  void CheckboxWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx, bool is_focused)
  {
    using neui_detail::ColorRole;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);
    uint32_t mark_col   = neui_detail::color(ColorRole::text_primary);
    uint32_t indet_col  = neui_detail::color(ColorRole::text_secondary);
    // Hover/pressed background inside the checkbox glyph - same shade ladder
    // as ButtonWidget. Pressed wins over hovered.
    if (pressed || hovered) {
      uint32_t base = neui_detail::color(ColorRole::panel_bg);
      uint32_t fill = pressed ? neui_detail::shade(base, -16)
                              : neui_detail::shade(base, +16);
      backend->fill_rect(ctx, fx + 4.0f, fy + 4.0f, 14.0f, 14.0f, fill);
    }
    if (backend->draw_rect)
      backend->draw_rect(ctx, fx + 4.0f, fy + 4.0f, 14.0f, 14.0f, 1.5f, mark_col);
    if (check_state == 1 && backend->draw_text)
      backend->fill_rect(ctx, fx + 6.0f, fy + 6.0f, 10.0f, 10.0f, mark_col);
    if (check_state == 2 && backend->draw_text)
      backend->fill_rect(ctx, fx + 6.0f, fy + 6.0f, 10.0f, 10.0f, indet_col);

    if (!text.empty() && backend->draw_text) {
      auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
      neui_detail::push_widget_font(backend, ctx, ef);
      backend->draw_text(ctx, fx + 4.0f + 20.0f, fy, fw - 8.0f, fh, text.c_str(), ef.size,
                         neui_detail::color(ColorRole::text_primary));
      neui_detail::pop_widget_font(backend, ctx, ef);
    }
    if (is_focused)
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.5f,
                         neui_detail::color(ColorRole::focus_ring));
  }

  // Tri-state mode is driven by the NEUI_ATTR_TRISTATE attribute, which is
  // set implicitly when the widget is created as NEUI_W_CHECKBOX3 and can
  // also be set explicitly on a plain CHECKBOX.
  static bool checkbox_is_tristate(const WidgetData& wd)
  {
    return wd.attrs && wd.attrs->get_int(NEUI_ATTR_TRISTATE, 0) != 0;
  }

  // Fire NEUI_EVENT_CHECKBOX_CHANGED for user-driven state changes.
  // Matches the native-host contract (win32 / macOS already emit this)
  // so example code can drive other widgets from a checkbox toggle.
  static void checkbox_dispatch_changed(CheckboxWidget& wd)
  {
    if (!wd.session || !wd.emit_events) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_CHECKBOX_CHANGED;
    ev.data.checkbox.widget.id = wd.widget_id;
    ev.data.checkbox.state     = static_cast<neui_check_state_t>(wd.check_state);
    wd.session->dispatch_event(&ev);
  }

  bool CheckboxWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    if (keycode == NEUI_KEY_SPACE) {
      int mod = checkbox_is_tristate(*this) ? 3 : 2;
      check_state = (check_state + 1) % mod;
      repaint();
      checkbox_dispatch_changed(*this);
      return true;
    }
    return false;
  }

  bool CheckboxWidget::on_mouse_event(neui_event_t* event)
  {
    // Toggle on every press, including the second click of a rapid pair.
    // Windows reports the second click as WM_LBUTTONDBLCLK (because the
    // window class has CS_DBLCLKS for tree/inputbox use) - without this
    // branch every other click on a checkbox would be silently eaten and
    // feel like an unresponsive control.
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN ||
        event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK)
    {
      int mod = checkbox_is_tristate(*this) ? 3 : 2;
      check_state = (check_state + 1) % mod;
      repaint();
      checkbox_dispatch_changed(*this);
      return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // SliderWidget / KnobWidget - value-bearing controls.
  //
  // Both store their current value in the NEUI_PARAM_VALUE float attribute
  // (range [0..1]). Programmatic set via attrs->set_float is silent;
  // user-driven changes (drag/click/wheel/key) fire NEUI_EVENT_VALUE_CHANGED.

  static constexpr int SLIDER_TRACK_THICK = 4;
  static constexpr int SLIDER_THUMB_W     = 10;   // along travel axis
  static constexpr int SLIDER_THUMB_H     = 16;   // perpendicular to travel

  static float clamp01(float v)
  {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
  }

  // Snap v to one of N evenly-spaced positions on [0..1]. steps < 2 -> no snap.
  static float snap_to_steps(float v, int steps)
  {
    if (steps < 2) return v;
    int idx = static_cast<int>(v * static_cast<float>(steps - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= steps) idx = steps - 1;
    return static_cast<float>(idx) / static_cast<float>(steps - 1);
  }

  static int widget_get_steps(const WidgetData& wd)
  {
    return wd.attrs ? wd.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
  }

  // Compute a discrete-or-continuous nudge delta. In stepped mode each
  // press advances by `step_count` steps (typically 1 for arrows / wheel,
  // larger for PageUp/Down); in continuous mode it falls back to the
  // hand-tuned `continuous_amount` (0.05 for arrows / wheel, 0.10 for pages).
  static float nudge_delta(const WidgetData& wd,
                            int   step_count,
                            float continuous_amount)
  {
    int steps = widget_get_steps(wd);
    if (steps >= 2) {
      return static_cast<float>(step_count) / static_cast<float>(steps - 1);
    }
    return continuous_amount;
  }

  static float widget_get_value(const WidgetData& wd)
  {
    if (!wd.attrs) return 0.0f;
    return clamp01(wd.attrs->get_float(NEUI_PARAM_VALUE, 0.0f));
  }

  // Write value and fire NEUI_EVENT_VALUE_CHANGED if it actually changed.
  static void widget_set_value_user(WidgetData& wd, float new_v)
  {
    new_v = snap_to_steps(clamp01(new_v), widget_get_steps(wd));
    float old_v = widget_get_value(wd);
    if (new_v == old_v) return;
    neui_detail::ensure_attrs(wd.attrs).set_float(NEUI_PARAM_VALUE, new_v);
    if (wd.session && wd.emit_events) {
      neui_event_t ev{};
      ev.type = NEUI_EVENT_VALUE_CHANGED;
      // Use the full widget_id (session<<16 | slot) so client-side
      // comparisons against the value returned by widgets->create() match
      // - the win32 host already does this; the xpl host previously sent
      // just the slot index, breaking the cross-host contract.
      ev.data.value.widget.id = wd.widget_id;
      ev.data.value.value     = new_v;
      wd.session->dispatch_event(&ev);
    }
  }

  // NEUI_EVENT_GESTURE_BEGIN / _END for the built-in KNOB / SLIDER (their
  // gesture always edits NEUI_PARAM_VALUE). Same gating as VALUE_CHANGED.
  static void widget_emit_gesture(WidgetData& wd, bool begin)
  {
    if (!wd.session || !wd.emit_events) return;
    neui_event_t ev{};
    ev.type = begin ? NEUI_EVENT_GESTURE_BEGIN : NEUI_EVENT_GESTURE_END;
    ev.data.gesture.widget.id = wd.widget_id;
    ev.data.gesture.attr_key  = NEUI_PARAM_VALUE;
    ev.data.gesture.value     = widget_get_value(wd);
    wd.session->dispatch_event(&ev);
  }

  // One-shot user change (wheel tick / key nudge / reset) wrapped in an
  // implicit GESTURE_BEGIN / _END pair. The pair only fires when the snapped
  // value actually moves - checked BEFORE the begin so the VALUE_CHANGED of
  // the write lands between the two. Drags call widget_emit_gesture at
  // grab / release instead and route their samples through
  // widget_set_value_user directly.
  static void widget_set_value_user_gesture(WidgetData& wd, float new_v)
  {
    float snapped = snap_to_steps(clamp01(new_v), widget_get_steps(wd));
    if (snapped == widget_get_value(wd)) return;
    widget_emit_gesture(wd, true);
    widget_set_value_user(wd, new_v);
    widget_emit_gesture(wd, false);
  }

  // One accessibility increment / decrement, honouring the real-world step a
  // client declared with NEUI_API_A11Y::set_value_range.
  //
  // Returns false when there is no declared step to honour, which is the signal
  // for the caller to fall back to the ordinary arrow-key path (10 % of the
  // range, or one NEUI_ATTR_STEPS detent) - the right answer when the client
  // named no step, since then keyboard and AT should move by the same amount.
  //
  // Lives here rather than in the provider because it must produce the SAME
  // events a keypress does, and widget_set_value_user_gesture (the thing that
  // brackets the change in GESTURE_BEGIN / _END) is file-local to host.cpp.
  bool Session::a11y_step_value(uint32_t slot, bool up)
  {
    if (slot == 0 || !_widgets.exists(slot)) return false;
    WidgetData& wd = _widgets[slot];
    if (!wd.attrs) return false;
    // Both ends are needed to turn a real-world step into a normalized one - the
    // same all-or-nothing rule the adapter applies to the range itself.
    if (!wd.attrs->has(NEUI_ATTR_A11Y_RANGE_MIN) ||
        !wd.attrs->has(NEUI_ATTR_A11Y_RANGE_MAX)) return false;
    const float step = wd.attrs->get_float(NEUI_ATTR_A11Y_RANGE_STEP, 0.0f);
    if (!(step > 0.0f)) return false;             // 0 = continuous, and catches NaN
    float lo = wd.attrs->get_float(NEUI_ATTR_A11Y_RANGE_MIN, 0.0f);
    float hi = wd.attrs->get_float(NEUI_ATTR_A11Y_RANGE_MAX, 1.0f);
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    const float span = hi - lo;
    if (!(span > 0.0f)) return false;
    const float d = step / span;                  // normalized increment
    widget_set_value_user_gesture(wd, widget_get_value(wd) + (up ? d : -d));
    return true;
  }

  // ---- SliderWidget --------------------------------------------------------

  // Read NEUI_ATTR_ORIENTATION ("horizontal" / "vertical") into `is_vertical`.
  // Cheap (one hashmap lookup); cached on the widget so live changes via
  // attrs->set_string take effect on the next paint.
  static void slider_resolve_orientation(SliderWidget& s)
  {
    if (!s.attrs) { s.is_vertical = false; return; }
    const char* o = s.attrs->get_string(NEUI_ATTR_ORIENTATION);
    s.is_vertical = (o && !strcmp(o, "vertical"));
  }

  void SliderWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                            bool is_focused)
  {
    using neui_detail::ColorRole;
    slider_resolve_orientation(*this);

    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);

    // Transparent background - only the track and thumb paint; whatever the
    // parent frame drew behind shows through everywhere else.

    const float v = widget_get_value(*this);

    const int steps = widget_get_steps(*this);

    // Slider track: a slightly recessed shade of control_bg. Thumb is the
    // primary text colour shaded down a touch so it reads as a knob, not a
    // text glyph. Tick marks use the secondary text colour.
    uint32_t track_col = neui_detail::shade(neui_detail::color(ColorRole::control_bg), -16);
    uint32_t thumb_col = neui_detail::shade(neui_detail::color(ColorRole::text_primary), -16);
    uint32_t tick_col  = neui_detail::color(ColorRole::text_secondary);

    if (!is_vertical) {
      // Horizontal: track centred vertically, thumb travels along x.
      float track_y = fy + (fh - SLIDER_TRACK_THICK) * 0.5f;
      backend->fill_rect(ctx, fx + 4.0f, track_y,
                          fw - 8.0f, static_cast<float>(SLIDER_TRACK_THICK),
                          track_col);
      float travel = fw - 8.0f - SLIDER_THUMB_W;
      if (travel < 0.0f) travel = 0.0f;
      // Tick marks above the track when steps is set.
      if (steps >= 2 && travel > 0.0f) {
        float tick_top    = fy;
        float tick_bottom = track_y - 1.0f;
        if (tick_bottom > tick_top) {
          for (int i = 0; i < steps; ++i) {
            float t  = static_cast<float>(i) / static_cast<float>(steps - 1);
            float tx = fx + 4.0f + SLIDER_THUMB_W * 0.5f + travel * t;
            backend->fill_rect(ctx, tx - 0.5f, tick_top,
                                1.0f, tick_bottom - tick_top, tick_col);
          }
        }
      }
      float thumb_x = fx + 4.0f + travel * v;
      float thumb_y = fy + (fh - SLIDER_THUMB_H) * 0.5f;
      backend->fill_rect(ctx, thumb_x, thumb_y,
                          static_cast<float>(SLIDER_THUMB_W),
                          static_cast<float>(SLIDER_THUMB_H),
                          thumb_col);
    } else {
      // Vertical: track centred horizontally; value 1 = top (fader convention).
      float track_x = fx + (fw - SLIDER_TRACK_THICK) * 0.5f;
      backend->fill_rect(ctx, track_x, fy + 4.0f,
                          static_cast<float>(SLIDER_TRACK_THICK),
                          fh - 8.0f, track_col);
      float travel = fh - 8.0f - SLIDER_THUMB_W;
      if (travel < 0.0f) travel = 0.0f;
      // Tick marks to the right of the track when steps is set.
      if (steps >= 2 && travel > 0.0f) {
        float tick_left  = track_x + SLIDER_TRACK_THICK + 1.0f;
        float tick_right = fx + fw;
        if (tick_right > tick_left) {
          for (int i = 0; i < steps; ++i) {
            float t  = static_cast<float>(i) / static_cast<float>(steps - 1);
            // value 1 = top, so step 0 is at the bottom.
            float ty = fy + 4.0f + SLIDER_THUMB_W * 0.5f + travel * (1.0f - t);
            backend->fill_rect(ctx, tick_left, ty - 0.5f,
                                tick_right - tick_left, 1.0f, tick_col);
          }
        }
      }
      float thumb_y = fy + 4.0f + travel * (1.0f - v);
      float thumb_x = fx + (fw - SLIDER_THUMB_H) * 0.5f;
      backend->fill_rect(ctx, thumb_x, thumb_y,
                          static_cast<float>(SLIDER_THUMB_H),
                          static_cast<float>(SLIDER_THUMB_W),
                          thumb_col);
    }

    if (is_focused && backend->draw_rect)
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.5f,
                         neui_detail::color(ColorRole::focus_ring));
  }

  // Compute value [0..1] from a mouse coord on the slider.
  static float slider_value_from_pos(const SliderWidget& s, int mx, int my)
  {
    // mx, my are frame-local (event.mouse coords); subtract the widget's
    // frame-local origin (abs_x / abs_y) to get widget-local.
    if (!s.is_vertical) {
      float lx = static_cast<float>(mx - s.abs_x) - 4.0f - SLIDER_THUMB_W * 0.5f;
      float travel = static_cast<float>(s.width) - 8.0f - SLIDER_THUMB_W;
      if (travel <= 0.0f) return 0.0f;
      return clamp01(lx / travel);
    } else {
      float ly = static_cast<float>(my - s.abs_y) - 4.0f - SLIDER_THUMB_W * 0.5f;
      float travel = static_cast<float>(s.height) - 8.0f - SLIDER_THUMB_W;
      if (travel <= 0.0f) return 0.0f;
      return clamp01(1.0f - (ly / travel));
    }
  }

  bool SliderWidget::on_keydown(uint32_t keycode, uint32_t /*modifiers*/)
  {
    float v    = widget_get_value(*this);
    // Continuous mode: 10% of the range per arrow press (10 presses end-to-
    // end). Stepped mode: exactly one step.
    float step = nudge_delta(*this, 1, 0.10f);
    bool handled = true;
    switch (keycode) {
      case NEUI_KEY_LEFT:
      case NEUI_KEY_DOWN: v -= step; break;
      case NEUI_KEY_RIGHT:
      case NEUI_KEY_UP:   v += step; break;
      case NEUI_KEY_HOME: v = 0.0f; break;
      case NEUI_KEY_END:  v = 1.0f; break;
      default: handled = false; break;
    }
    if (handled) {
      widget_set_value_user_gesture(*this, v);
      repaint();
    }
    return handled;
  }

  bool SliderWidget::on_mouse_event(neui_event_t* event)
  {
    slider_resolve_orientation(*this);

    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK) {
      // Reset to NEUI_PARAM_DEFAULT (or 0 if unset). When a drag is still in
      // flight (the preceding MOUSE_BUTTON_DOWN opened a gesture and snapped
      // to the click position) the reset lands inside that gesture and
      // cancelling the drag closes it; otherwise (platforms that deliver
      // DOWN/UP before the DBLCLICK) it is its own implicit pair.
      float def = 0.0f;
      if (attrs) def = clamp01(attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
      if (dragging) {
        widget_set_value_user(*this, def);
        dragging = false;
        widget_emit_gesture(*this, false);
      } else {
        widget_set_value_user_gesture(*this, def);
      }
      repaint();
      return true;
    }

    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      int delta = event->data.wheel.delta;
      // data.WHEEL.buttonmap, not data.mouse: the two structs overlap in the
      // event union and mouse.buttonmap sits at the SAME offset (12) as
      // wheel.delta, so this used to test the scroll delta as a modifier mask -
      // (delta & NEUI_MK_SHIFT). Wheel-down (negative -> 0xFFFFFFFF...) always
      // read as "fine" and wheel-up never did. The wheel payload carries a real
      // buttonmap now, so read that.
      bool fine = (event->data.wheel.buttonmap & NEUI_MK_SHIFT) != 0;
      float step = nudge_delta(*this, 1, fine ? 0.01f : 0.05f) *
                   (delta > 0 ? 1.0f : -1.0f);
      widget_set_value_user_gesture(*this, widget_get_value(*this) + step);
      repaint();
      return true;
    }

    if (dragging) {
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
            !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        dragging = false;
        widget_emit_gesture(*this, false);
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        float v = slider_value_from_pos(*this, event->data.mouse.x, event->data.mouse.y);
        widget_set_value_user(*this, v);
        repaint();
        return true;
      }
      return false;
    }

    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      // Always jump to clicked position, then start drag. The gesture opens
      // BEFORE the jump so its VALUE_CHANGED lands inside the pair.
      widget_emit_gesture(*this, true);
      float v = slider_value_from_pos(*this, event->data.mouse.x, event->data.mouse.y);
      widget_set_value_user(*this, v);
      dragging       = true;
      drag_start_pos = is_vertical ? event->data.mouse.y : event->data.mouse.x;
      drag_start_val = v;
      repaint();
      return true;
    }
    return false;
  }

  // ---- KnobWidget ----------------------------------------------------------

  // Thunk: resolves a neui_asset_t against the session's AssetManager and
  // draws it via backend->draw_bitmap. Wired into neui_painter::draw_asset
  // _thunk so the curated painter API can dispatch asset draws without
  // exposing the raw bitmap pointer to clients. Also called from
  // widgets.cpp's as_paint_surface so nested draw_asset inside a surface
  // paint walks the same resolution path - hence non-static.
  void NEUI_ABI xpl_painter_draw_asset_thunk(
      void* host_token,
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      neui_asset_t asset,
      float x, float y, float w, float h,
      uint32_t frame,
      uint32_t tint)
  {
    auto* s = static_cast<Session*>(host_token);
    if (!s || !backend || !ctx) return;
    if (asset.id == asset_none.id) return;
    // Reject cross-session handles.
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return;
    uint32_t slot = asset.id & 0xffff;
    auto* entry = s->_asset_manager.get_slot(slot);
    if (!entry) return;
    // Cache-walk + lazy GPU upload + draw shared with the native hosts
    // (hosts/shared/painter.h). The dispatch helper owns the whole-vs-cell
    // rule (k_draw_asset_whole draws the whole bitmap; a frame index samples
    // one filmstrip cell).
    neui_detail::painter_draw_entry_dispatch(backend, ctx, entry, frame,
                                             x, y, w, h, tint);
  }

  // CUSTOMDRAW - hands the curated painter API to the client and lets
  // them draw. We translate so (0, 0) is the widget's top-left (the
  // recursive paint walk only pushes a translate when descending INTO
  // this widget's children, so our own paint runs in parent space) and
  // clip to the widget's bounds so a client that overdraws can't corrupt
  // sibling widgets. State changes the client leaves on the stack are
  // unwound by the matching pops below.
  // Resolve a CustomDrawWidget's compound asset to its CompoundAsset
  // storage. Returns nullptr if the slot isn't a compound or has been
  // released; caller falls back to WIDGET_PAINT in that case.
  static neui_detail::CompoundAsset* resolve_widget_compound(Session* s,
                                                                neui_asset_t a)
  {
    if (!s) return nullptr;
    if (a.id == asset_none.id) return nullptr;
    if (((a.id >> 16) & 0xffff) != (s->get_session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(a.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    return e->compound.get();
  }

  void CustomDrawWidget::paint(neui_render_backend_t* backend,
                                neui_render_ctx_t ctx, bool is_focused)
  {
    if (!session) return;

    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);

    // Device scale actually in effect: the frame's zoom (already pushed as a
    // CTM scale by paint_frame) times the monitor's DPI ratio (applied by the
    // backend). This is what the client is told in neui_event_paint_t::scale.
    const float zoom = session->paint_zoom();
    float dev_scale = zoom;
    if (backend->get_scale_factor) dev_scale *= backend->get_scale_factor(ctx);

    // NEUI_ATTR_PAINT_DEVICE_PIXELS: hand the client DEVICE pixels instead of
    // logical ones. We can only undo the part of the scale WE pushed (the
    // zoom); the backend's own DPI scale isn't ours to unwind, so "device
    // pixels" here means logical x zoom, and `scale` still reports the full
    // factor so a client can reason about the rest.
    const bool device_px =
      !!neui_detail::attrs_readonly(attrs)
      && neui_detail::attr_as_float(neui_detail::attrs_readonly(attrs),
                                     NEUI_ATTR_PAINT_DEVICE_PIXELS, 0.0f) != 0.0f;
    const bool unzoom = device_px && zoom != 1.0f
                        && backend->scale && backend->push_transform;

    if (backend->push_transform) backend->push_transform(ctx);
    if (backend->translate)
      backend->translate(ctx, static_cast<float>(x), static_cast<float>(y));
    // Clip BEFORE unzooming, so the clip rect stays in the widget's logical
    // box however the callback's coordinate space is set up.
    if (backend->push_clip) backend->push_clip(ctx, 0.0f, 0.0f, fw, fh);

    // Widget-local size in whatever space the callback will draw in.
    float cb_w = fw, cb_h = fh;
    if (unzoom) {
      // Undo the frame zoom for the callback only: one unit under the
      // callback is now one zoomed (device-side) pixel, and the widget covers
      // fw*zoom x fh*zoom of them - the same physical area as before.
      backend->scale(ctx, 1.0f / zoom, 1.0f / zoom);
      cb_w = fw * zoom;
      cb_h = fh * zoom;
    }

    neui_painter painter{};
    painter.backend          = backend;
    painter.ctx              = ctx;
    painter.host_token       = session;
    painter.draw_asset_thunk = &xpl_painter_draw_asset_thunk;
    // Under a device-pixel callback the zoom is no longer in the CTM, so the
    // backend's own get_scale_factor is already the whole truth.
    painter.extra_scale      = unzoom ? 1.0f : zoom;

    if (auto* ca = resolve_widget_compound(session, compound_asset)) {
      // Compound mode: paint z<0 layers here; z>=0 layers come from
      // paint_after_children below. WIDGET_PAINT is suppressed.
      bool selected = neui_detail::attr_as_float(
                        neui_detail::attrs_readonly(attrs), NEUI_ATTR_SELECTED, 0.0f) != 0.0f;
      uint32_t state_mask = neui_detail::compose_widget_state(enabled, hovered, pressed, selected);
      neui_detail::paint_compound_below(&painter, *ca, cb_w, cb_h,
                                          neui_detail::attrs_readonly(attrs),
                                          state_mask);
    } else {
      neui_event_t ev{};
      ev.type = NEUI_EVENT_WIDGET_PAINT;
      ev.data.paint.widget.id  = widget_id;
      ev.data.paint.painter_api = &neui_detail::k_painter_api;
      ev.data.paint.p           = &painter;
      ev.data.paint.width       = cb_w;
      ev.data.paint.height      = cb_h;
      ev.data.paint.focused     = is_focused;
      ev.data.paint.scale       = dev_scale;
      session->dispatch_event(&ev);
    }

    if (backend->pop_clip) backend->pop_clip(ctx);
    if (backend->pop_transform) backend->pop_transform(ctx);
  }

  // Called from paint_widgets_recursive after the child-widget descent.
  // We're in widget-local coords (the recursive walk pushed
  // translate(wd.x, wd.y)). For compound widgets, paint the z>=0 layers
  // here so they sit above any child widgets.
  void CustomDrawWidget::paint_after_children(neui_render_backend_t* backend,
                                                neui_render_ctx_t ctx,
                                                bool /*is_focused*/)
  {
    if (!session) return;
    auto* ca = resolve_widget_compound(session, compound_asset);
    if (!ca) return;

    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);

    if (backend->push_clip) backend->push_clip(ctx, 0.0f, 0.0f, fw, fh);

    neui_painter painter{};
    painter.backend          = backend;
    painter.ctx              = ctx;
    painter.host_token       = session;
    painter.draw_asset_thunk = &xpl_painter_draw_asset_thunk;
    painter.extra_scale      = session->paint_zoom();

    bool selected = neui_detail::attr_as_float(
                      neui_detail::attrs_readonly(attrs), NEUI_ATTR_SELECTED, 0.0f) != 0.0f;
    uint32_t state_mask = neui_detail::compose_widget_state(enabled, hovered, pressed, selected);
    neui_detail::paint_compound_above(&painter, *ca, fw, fh,
                                        neui_detail::attrs_readonly(attrs),
                                        state_mask);

    if (backend->pop_clip) backend->pop_clip(ctx);
  }

  // ---- Behavior plumbing for CUSTOMDRAW -----------------------------------

  // Resolve a behavior asset attached to a CUSTOMDRAW widget. Returns
  // nullptr if no asset, the asset was released, or the kind doesn't match.
  static neui_detail::BehaviorAsset*
  resolve_widget_behavior(Session* s, neui_asset_t a)
  {
    if (!s) return nullptr;
    if (a.id == asset_none.id) return nullptr;
    if (((a.id >> 16) & 0xffff) != (s->get_session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(a.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    return e->behavior.get();
  }

  static void xpl_behavior_invalidate(void* host_data)
  {
    auto* wd = static_cast<CustomDrawWidget*>(host_data);
    if (!wd || !wd->session) return;
    // Mirrors WidgetData::repaint() but that method is protected, so
    // duplicate its body (single line) rather than friend the dispatch.
    void* frame = wd->session->find_parent_native_handle(wd->index);
    if (frame) platform_invalidate(frame);
  }

  static void xpl_behavior_emit_attr_changed(void* host_data,
                                              const char* attr_key, float value)
  {
    auto* wd = static_cast<CustomDrawWidget*>(host_data);
    if (!wd || !wd->session || !wd->emit_events) return;
    neui_event_t ev{};
    ev.type                 = NEUI_EVENT_ATTR_CHANGED;
    ev.data.attr.widget.id  = wd->widget_id;
    ev.data.attr.attr_key   = attr_key;
    ev.data.attr.value      = value;
    wd->session->dispatch_event(&ev);
  }

  static void xpl_behavior_emit_gesture(void* host_data,
                                          const char* attr_key, float value,
                                          bool begin)
  {
    auto* wd = static_cast<CustomDrawWidget*>(host_data);
    if (!wd || !wd->session || !wd->emit_events) return;
    neui_event_t ev{};
    ev.type = begin ? NEUI_EVENT_GESTURE_BEGIN : NEUI_EVENT_GESTURE_END;
    ev.data.gesture.widget.id = wd->widget_id;
    ev.data.gesture.attr_key  = attr_key;
    ev.data.gesture.value     = value;
    wd->session->dispatch_event(&ev);
  }

  static int xpl_behavior_popup_menu(void* host_data, int local_x, int local_y,
                                       const char* const* items)
  {
    auto* wd = static_cast<CustomDrawWidget*>(host_data);
    if (!wd || !wd->session || !items) return 0;
    std::vector<std::string> v;
    for (int i = 0; items[i] != nullptr; ++i) v.emplace_back(items[i]);
    return wd->session->open_popup_menu(wd->index, local_x, local_y, v);
  }

  // Defined in widgets.cpp where dnd_begin_drag_with_preview is in scope.
  uint32_t xpl_behavior_begin_drag(void* host_data,
                                     neui_data_item_t item,
                                     uint32_t allowed_actions,
                                     uint32_t preview_image,
                                     int hot_x, int hot_y);

  // Build a dispatch ctx for this widget, ensuring its AttrBag exists.
  static neui_detail::BehaviorDispatchCtx make_behavior_ctx(CustomDrawWidget& wd)
  {
    neui_detail::BehaviorDispatchCtx ctx{};
    ctx.bag      = &neui_detail::ensure_attrs(wd.attrs);
    ctx.widget_w = static_cast<float>(wd.width);
    ctx.widget_h = static_cast<float>(wd.height);
    ctx.host_data         = &wd;
    ctx.invalidate        = &xpl_behavior_invalidate;
    ctx.emit_attr_changed = &xpl_behavior_emit_attr_changed;
    ctx.emit_gesture      = &xpl_behavior_emit_gesture;
    ctx.popup_menu        = &xpl_behavior_popup_menu;
    ctx.begin_drag        = &xpl_behavior_begin_drag;
    return ctx;
  }

  bool CustomDrawWidget::on_mouse_event(neui_event_t* event)
  {
    if (!event || !session) return false;
    auto* ba = resolve_widget_behavior(session, behavior_asset);
    if (!ba) return false;
    if (!behavior_rt) behavior_rt = std::make_unique<neui_detail::BehaviorRuntime>();
    auto ctx = make_behavior_ctx(*this);
    // event coords are frame-local; convert to widget-local.
    float local_x = static_cast<float>(event->data.mouse.x - abs_x);
    float local_y = static_cast<float>(event->data.mouse.y - abs_y);
    return neui_detail::behavior_dispatch_mouse(*ba, *behavior_rt, ctx,
                                                 event, local_x, local_y);
  }

  bool CustomDrawWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    if (!session) return false;
    auto* ba = resolve_widget_behavior(session, behavior_asset);
    if (!ba) return false;
    if (!behavior_rt) behavior_rt = std::make_unique<neui_detail::BehaviorRuntime>();
    auto ctx = make_behavior_ctx(*this);
    return neui_detail::behavior_dispatch_key(*ba, *behavior_rt, ctx,
                                                keycode, modifiers);
  }

  void KnobWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                          bool is_focused)
  {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);

    auto polarity = neui_detail::KNOB_POLARITY_MIN;
    const char* value_text = nullptr;
    if (attrs) {
      polarity   = neui_detail::parse_knob_polarity(attrs->get_string(NEUI_ATTR_POLARITY));
      value_text = attrs->get_string(NEUI_ATTR_VALUE_TEXT);
    }
    int steps = widget_get_steps(*this);

    // Transparent - only the knob disc + indicator paint; the corners of the
    // bounding rect (outside the inscribed circle) keep whatever the parent drew.
    neui_detail::paint_knob(backend, ctx, fx, fy, fw, fh,
                             widget_get_value(*this), is_focused,
                             polarity, steps, value_text, attrs.get());
  }

  bool KnobWidget::on_keydown(uint32_t keycode, uint32_t /*modifiers*/)
  {
    float v    = widget_get_value(*this);
    // Continuous mode: 10% of the range per arrow press. Stepped mode:
    // exactly one step.
    float step = nudge_delta(*this, 1, 0.10f);
    bool handled = true;
    switch (keycode) {
      case NEUI_KEY_LEFT:
      case NEUI_KEY_DOWN: v -= step; break;
      case NEUI_KEY_RIGHT:
      case NEUI_KEY_UP:   v += step; break;
      case NEUI_KEY_HOME: v = 0.0f; break;
      case NEUI_KEY_END:  v = 1.0f; break;
      default: handled = false; break;
    }
    if (handled) {
      widget_set_value_user_gesture(*this, v);
      repaint();
    }
    return handled;
  }

  // Angular-drag constants (shared between xpl and win32 knob handlers).
  // Total sweep matches the visual sweep in paint_knob (-135deg..+135deg).
  static constexpr float KNOB_SWEEP_RAD     = 4.71238898f; // 1.5 * PI (270deg)
  static constexpr float KNOB_DEAD_ZONE_R   = 4.0f;        // logical px
  static constexpr float KNOB_FINE_SCALE    = 0.2f;        // Shift = 1/5 sensitivity
  // Slider modes (vertical / horizontal NEUI_ATTR_KNOB_MODE): pixels of
  // drag to span the full [0..1] range. ~200 logical px is a common
  // DAW-host feel.
  static constexpr float KNOB_SLIDER_SWEEP_PX = 200.0f;

  // Wrap a delta angle into [-PI, +PI] so frame-by-frame tracking is robust
  // around the atan2 wraparound at +/-pi (e.g. cursor crossing 9 o'clock).
  static float wrap_pi(float d)
  {
    const float TWO_PI = 6.28318530717958647692f;
    while (d >  3.14159265358979323846f) d -= TWO_PI;
    while (d < -3.14159265358979323846f) d += TWO_PI;
    return d;
  }

  bool KnobWidget::on_mouse_event(neui_event_t* event)
  {
    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      int delta = event->data.wheel.delta;
      // data.WHEEL.buttonmap, not data.mouse: the two structs overlap in the
      // event union and mouse.buttonmap sits at the SAME offset (12) as
      // wheel.delta, so this used to test the scroll delta as a modifier mask -
      // (delta & NEUI_MK_SHIFT). Wheel-down (negative -> 0xFFFFFFFF...) always
      // read as "fine" and wheel-up never did. The wheel payload carries a real
      // buttonmap now, so read that.
      bool fine = (event->data.wheel.buttonmap & NEUI_MK_SHIFT) != 0;
      // Wheel up INCREASES knob value, wheel down DECREASES - matching the
      // SLIDER and the natural scroll direction (delta > 0 == scroll up on
      // every platform).
      float step = nudge_delta(*this, 1, fine ? 0.01f : 0.05f) *
                   (delta > 0 ? 1.0f : -1.0f);
      widget_set_value_user_gesture(*this, widget_get_value(*this) + step);
      repaint();
      return true;
    }

    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK) {
      float def = 0.0f;
      if (attrs) def = clamp01(attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
      // Normally the platform delivered DOWN / UP first, so no drag gesture
      // is open and the reset is its own implicit pair; if a drag IS still in
      // flight the reset lands inside it (the UP that follows closes it).
      if (dragging) widget_set_value_user(*this, def);
      else          widget_set_value_user_gesture(*this, def);
      repaint();
      return true;
    }

    if (event->type == NEUI_EVENT_MOUSE_RBUTTON_DOWN && session) {
      // Right-click context menu. Position the popup at the cursor in the
      // knob's local coordinate system (the popup overlay handles the
      // anchor -> frame-absolute conversion). event.mouse coords are
      // frame-local; subtract the widget's frame-local origin to get local.
      int local_x = event->data.mouse.x - abs_x;
      int local_y = event->data.mouse.y - abs_y;
      static const std::vector<std::string> k_items = {
        "Reset to default",
      };
      int picked = session->open_popup_menu(index, local_x, local_y, k_items);
      if (picked == 1) {
        float def = 0.0f;
        if (attrs) def = clamp01(attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
        widget_set_value_user_gesture(*this, def);
        repaint();
      }
      return true;
    }

    // Centre of the knob in frame-local logical pixels (matches the space
    // of event.mouse.x / event.mouse.y).
    float cx = static_cast<float>(abs_x) + static_cast<float>(width)  * 0.5f;
    float cy = static_cast<float>(abs_y) + static_cast<float>(height) * 0.5f;

    if (dragging) {
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
            !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        dragging = false;
        widget_emit_gesture(*this, false);
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        int   mx   = event->data.mouse.x;
        int   my   = event->data.mouse.y;
        bool  fine = (event->data.mouse.buttonmap & NEUI_MK_SHIFT) != 0;
        float fine_mul = fine ? KNOB_FINE_SCALE : 1.0f;
        float delta_v  = 0.0f;
        if (drag_mode == NEUI_KNOB_MODE_VERTICAL) {
          // Up = increase: negative pixel delta -> positive value delta.
          delta_v = -static_cast<float>(my - drag_prev_y) *
                    (fine_mul / KNOB_SLIDER_SWEEP_PX);
          drag_prev_y = my;
        } else if (drag_mode == NEUI_KNOB_MODE_HORIZONTAL) {
          delta_v = static_cast<float>(mx - drag_prev_x) *
                    (fine_mul / KNOB_SLIDER_SWEEP_PX);
          drag_prev_x = mx;
        } else {
          float dx = static_cast<float>(mx) - cx;
          float dy = static_cast<float>(my) - cy;
          float r2 = dx*dx + dy*dy;
          if (r2 < KNOB_DEAD_ZONE_R * KNOB_DEAD_ZONE_R) {
            // Inside dead zone - angle is unstable. Drop this sample, but
            // don't end the drag; the user may slip back out.
            return true;
          }
          float cur_angle = std::atan2(dy, dx);
          delta_v = wrap_pi(cur_angle - drag_prev_angle) *
                    (fine_mul / KNOB_SWEEP_RAD);
          drag_prev_angle = cur_angle;
        }
        // Accumulate continuously so small per-frame deltas survive across
        // step snapping. The external attribute only changes when the
        // continuous value crosses into the next step.
        drag_continuous += delta_v;
        drag_continuous = clamp01(drag_continuous);
        widget_set_value_user(*this, drag_continuous);
        repaint();
        return true;
      }
      return false;
    }

    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      // Cache the drag mode for the duration of this drag so the per-frame
      // mouse-move path doesn't pay attribute-lookup cost.
      drag_mode = attrs ? attrs->get_int(NEUI_ATTR_KNOB_MODE,
                                          NEUI_KNOB_MODE_ROTATIONAL)
                        : NEUI_KNOB_MODE_ROTATIONAL;
      int mx = event->data.mouse.x;
      int my = event->data.mouse.y;
      if (drag_mode == NEUI_KNOB_MODE_ROTATIONAL) {
        float dx = static_cast<float>(mx) - cx;
        float dy = static_cast<float>(my) - cy;
        float r2 = dx*dx + dy*dy;
        if (r2 < KNOB_DEAD_ZONE_R * KNOB_DEAD_ZONE_R) {
          // Click bang in the centre: don't start a drag we can't track.
          return true;
        }
        drag_prev_angle = std::atan2(dy, dx);
      } else {
        drag_prev_x = mx;
        drag_prev_y = my;
      }
      dragging = true;
      // Seed the continuous accumulator with the current snapped value so
      // the first delta moves us off it (rather than starting from 0).
      drag_continuous = widget_get_value(*this);
      widget_emit_gesture(*this, true);
      return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // ListItemsWidget (LISTBOX) and ComboBoxWidget (COMBOBOX)

  // Row / bar heights scale with painted_ui_scale() (identity on desktop,
  // enlarged on iOS so the scaled default font fits). Accessors rather than
  // constexpr so paint + hit-test always read the same scaled value. An
  // explicit client font/size attr doesn't change these defaults - they are
  // the painted layout grid these widgets always use.
  static int LIST_ITEM_H()       { return neui_detail::scaled_painted_metric(18); }
  static int COMBO_COLLAPSED_H() { return neui_detail::scaled_painted_metric(22); }   // collapsed combo bar
  static constexpr int SCROLLBAR_W       = 10;   // total width: 1px separator + 9px track

  // Scrollbar thumb geometry, computed identically by paint() and on_mouse_event().
  struct SbGeom {
    float track_h;   // usable track height (widget_h - 2px top/bottom margin)
    float thumb_h;   // thumb height
    float thumb_top; // thumb top offset within the track (not including the 1px margin)
  };

  static SbGeom compute_sb(int widget_h, int full_vis, uint32_t n, uint32_t scroll_offset)
  {
    float track_h = static_cast<float>(widget_h) - 2.0f;
    float thumb_h = std::max(8.0f, track_h * static_cast<float>(full_vis)
                                             / static_cast<float>(n));
    uint32_t scroll_range = n - static_cast<uint32_t>(full_vis);
    float thumb_top = (scroll_range > 0)
                      ? (track_h - thumb_h) * static_cast<float>(scroll_offset)
                        / static_cast<float>(scroll_range)
                      : 0.0f;
    return { track_h, thumb_h, thumb_top };
  }

  int ListItemsWidget::visible_rows() const
  {
    return height / LIST_ITEM_H();
  }

  // Shared rendering helper for any scrollable item list.
  // fx/fy/fw/fh define the total rect (including scrollbar column when shown).
  // full_vis: floor-visible rows - used only for scrollbar sizing; pass
  //           (int_height / LIST_ITEM_H()) for listbox, max_drop_visible() for overlay.
  // scroll_offset is read-only here; callers own the value.
  // hover_row UINT32_MAX = no row hover; otherwise the unselected row gets a
  // subtle shaded background to indicate the mouse position.
  // border_color 0 = no outer border drawn.
  static void paint_scrollable_list(
      neui_render_backend_t* backend, neui_render_ctx_t ctx,
      float fx, float fy, float fw, float fh,
      const std::vector<ListItemsWidget::Item>& items,
      uint32_t selected_item, uint32_t scroll_offset,
      int full_vis, int int_h,
      uint32_t bg_color, uint32_t border_color,
      float    text_size = 12.0f,
      uint32_t hover_row = UINT32_MAX)
  {
    uint32_t n       = static_cast<uint32_t>(items.size());
    bool     show_sb = n > static_cast<uint32_t>(full_vis);
    float    content_w = show_sb ? fw - static_cast<float>(SCROLLBAR_W) : fw;

    backend->fill_rect(ctx, fx, fy, content_w, fh, bg_color);

    // Ceiling division: draw a partial row at the bottom if it fits.
    int max_visible = (int_h + LIST_ITEM_H() - 1) / LIST_ITEM_H();
    if (max_visible < 1) max_visible = 1;

    using neui_detail::ColorRole;
    uint32_t accent_col      = neui_detail::color(ColorRole::accent);
    uint32_t hover_col       = neui_detail::shade(bg_color, +14);
    uint32_t selected_text   = neui_detail::color(ColorRole::accent_text);
    uint32_t normal_text     = neui_detail::color(ColorRole::text_primary);

    if (backend->push_clip) backend->push_clip(ctx, fx, fy, content_w, fh);
    for (int i = 0; i < max_visible; ++i) {
      uint32_t item_idx = scroll_offset + static_cast<uint32_t>(i);
      if (item_idx >= n) break;
      float row_y = fy + static_cast<float>(i * LIST_ITEM_H());
      bool sel  = (item_idx == selected_item);
      bool hov  = (item_idx == hover_row) && !sel;
      if (sel)
        backend->fill_rect(ctx, fx + 1.0f, row_y, content_w - 2.0f,
                           static_cast<float>(LIST_ITEM_H()), accent_col);
      else if (hov)
        backend->fill_rect(ctx, fx + 1.0f, row_y, content_w - 2.0f,
                           static_cast<float>(LIST_ITEM_H()), hover_col);
      if (backend->draw_text)
        backend->draw_text(ctx, fx + 4.0f, row_y, content_w - 8.0f,
                           static_cast<float>(LIST_ITEM_H()),
                           items[item_idx].text.c_str(), text_size,
                           sel ? selected_text : normal_text);
    }
    if (backend->push_clip) backend->pop_clip(ctx);

    // Scrollbar - only when items overflow.
    if (show_sb) {
      float sx = fx + content_w;
      backend->fill_rect(ctx, sx, fy, 1.0f, fh,
                          neui_detail::color(ColorRole::scrollbar_separator));
      float tx = sx + 1.0f;
      float tw = static_cast<float>(SCROLLBAR_W) - 1.0f;
      backend->fill_rect(ctx, tx, fy, tw, fh,
                          neui_detail::color(ColorRole::scrollbar_track));
      SbGeom sb = compute_sb(int_h, full_vis, n, scroll_offset);
      backend->fill_rect(ctx, tx + 1.0f, fy + 1.0f + sb.thumb_top,
                         tw - 2.0f, sb.thumb_h,
                         neui_detail::color(ColorRole::scrollbar_thumb));
    }

    if (border_color)
      backend->draw_rect(ctx, fx, fy, fw, fh, 1.0f, border_color);
  }

  void ListItemsWidget::paint(neui_render_backend_t* backend,
                               neui_render_ctx_t ctx, bool is_focused)
  {
    // Clamp scroll_offset before painting.
    uint32_t n = static_cast<uint32_t>(items.size());
    if (n == 0) scroll_offset = 0;
    else if (scroll_offset >= n) scroll_offset = n - 1;

    using neui_detail::ColorRole;
    uint32_t border_color = neui_detail::color(
        is_focused ? ColorRole::border_focused : ColorRole::border);
    auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
    neui_detail::push_widget_font(backend, ctx, ef);
    paint_scrollable_list(backend, ctx,
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height),
        items, selected_item, scroll_offset,
        height / LIST_ITEM_H(), height,
        neui_detail::color(ColorRole::control_bg), border_color,
        ef.size,
        hovered ? hover_row : UINT32_MAX);
    neui_detail::pop_widget_font(backend, ctx, ef);
  }

  bool ListItemsWidget::on_keydown(uint32_t keycode, uint32_t /*modifiers*/)
  {
    if (items.empty()) return false;
    uint32_t n    = static_cast<uint32_t>(items.size());
    uint32_t prev = selected_item;

    if (keycode == NEUI_KEY_UP) {
      selected_item = (selected_item == 0 || selected_item == UINT32_MAX)
                      ? 0 : selected_item - 1;
    } else if (keycode == NEUI_KEY_DOWN) {
      selected_item = (selected_item == UINT32_MAX)
                      ? 0 : std::min(selected_item + 1, n - 1);
    } else if (keycode == NEUI_KEY_HOME) {
      selected_item = 0;
      scroll_offset = 0;
    } else if (keycode == NEUI_KEY_END) {
      selected_item = n - 1;
    } else {
      return false;
    }

    // Keep selected_item visible.
    int max_visible = std::max(1, visible_rows());
    if (selected_item < scroll_offset)
      scroll_offset = selected_item;
    else if (selected_item >= scroll_offset + static_cast<uint32_t>(max_visible))
      scroll_offset = selected_item - static_cast<uint32_t>(max_visible) + 1;

    if (selected_item != prev) {
      text = items[selected_item].text;
      if (session) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_ITEM_SELECTED;
        ev.data.item.widget = { widget_id };
        ev.data.item.index  = selected_item;
        session->dispatch_event(&ev);
      }
    }
    repaint();
    return true;
  }

  bool ListItemsWidget::on_mouse_event(neui_event_t* event)
  {
    uint32_t n        = static_cast<uint32_t>(items.size());
    int      full_vis = std::max(1, visible_rows());
    bool     show_sb  = n > static_cast<uint32_t>(full_vis);

    // ---- Scroll wheel -------------------------------------------------------
    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      if (n == 0 || !show_sb) return false;
      int delta = event->data.wheel.delta;
      if (delta < 0) {
        // Scroll down: increase offset, clamped to scroll_range.
        uint32_t scroll_range = n - static_cast<uint32_t>(full_vis);
        uint32_t step = static_cast<uint32_t>(-delta);
        scroll_offset = std::min(scroll_offset + step, scroll_range);
      } else if (delta > 0) {
        // Scroll up: decrease offset, clamped to 0.
        uint32_t step = static_cast<uint32_t>(delta);
        scroll_offset = (scroll_offset >= step) ? scroll_offset - step : 0;
      }
      repaint();
      return true;
    }

    // ---- Scrollbar drag in progress ----------------------------------------
    if (sb_dragging) {
      // End drag if button was released (covers mouse-up outside widget bounds).
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
           !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        sb_dragging = false;
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        SbGeom sb = compute_sb(height, full_vis, n, sb_drag_start_offset);
        float movable = sb.track_h - sb.thumb_h;
        if (movable > 0.0f) {
          uint32_t scroll_range = n - static_cast<uint32_t>(full_vis);
          int delta_y = event->data.mouse.y - sb_drag_start_y;
          int new_off = static_cast<int>(sb_drag_start_offset)
                      + static_cast<int>(static_cast<float>(delta_y) * static_cast<float>(scroll_range)
                                         / movable + 0.5f);
          if (new_off < 0) new_off = 0;
          if (static_cast<uint32_t>(new_off) > scroll_range)
            new_off = static_cast<int>(scroll_range);
          scroll_offset = static_cast<uint32_t>(new_off);
        }
        repaint();
        return true;
      }
      return false;
    }

    // ---- Hover tracking (MOUSE_MOVE outside any drag) ---------------------
    if (event->type == NEUI_EVENT_MOUSE_MOVE) {
      uint32_t new_hover = UINT32_MAX;
      int rel_x = event->data.mouse.x - abs_x;
      int rel_y = event->data.mouse.y - abs_y;
      bool in_content = rel_y >= 0
                     && rel_x >= 0
                     && (!show_sb || rel_x < width - SCROLLBAR_W);
      if (in_content && n > 0) {
        uint32_t row = scroll_offset + static_cast<uint32_t>(rel_y / LIST_ITEM_H());
        if (row < n) new_hover = row;
      }
      if (new_hover != hover_row) {
        hover_row = new_hover;
        repaint();
      }
      return true;
    }

    // ---- Scrollbar click (button down, x in scrollbar column) ---------------
    if (show_sb && event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      int sb_left = abs_x + width - SCROLLBAR_W;   // separator x in frame coords
      if (event->data.mouse.x >= sb_left) {
        SbGeom   sb           = compute_sb(height, full_vis, n, scroll_offset);
        uint32_t scroll_range = n - static_cast<uint32_t>(full_vis);
        // Local y relative to the track (1px top margin).
        float local_y = static_cast<float>(event->data.mouse.y - abs_y) - 1.0f;

        if (local_y >= sb.thumb_top && local_y < sb.thumb_top + sb.thumb_h) {
          // Hit the thumb - start drag.
          sb_dragging          = true;
          sb_drag_start_y      = event->data.mouse.y;
          sb_drag_start_offset = scroll_offset;
          hover_row            = UINT32_MAX;  // clear hover during drag
        } else if (local_y < sb.thumb_top) {
          // Above thumb - page up.
          uint32_t step = static_cast<uint32_t>(full_vis);
          scroll_offset = (scroll_offset >= step) ? scroll_offset - step : 0;
          repaint();
        } else {
          // Below thumb - page down.
          uint32_t step = static_cast<uint32_t>(full_vis);
          scroll_offset = std::min(scroll_offset + step, scroll_range);
          repaint();
        }
        return true;   // never select an item via scrollbar
      }
    }

    // ---- Content-area item click --------------------------------------------
    if (event->type != NEUI_EVENT_MOUSE_BUTTON_DOWN) return false;
    if (items.empty()) return true;

    int rel_y = event->data.mouse.y - abs_y;
    if (rel_y < 0) return true;
    uint32_t clicked = scroll_offset + static_cast<uint32_t>(rel_y / LIST_ITEM_H());
    if (clicked >= n) return true;
    if (clicked == selected_item) return true;

    selected_item = clicked;
    text = items[selected_item].text;

    // Ensure the selected item is fully visible (partial rows may be clickable).
    if (selected_item < scroll_offset)
      scroll_offset = selected_item;
    else if (selected_item >= scroll_offset + static_cast<uint32_t>(full_vis))
      scroll_offset = selected_item - static_cast<uint32_t>(full_vis) + 1;

    if (session) {
      neui_event_t ev = {};
      ev.type = NEUI_EVENT_ITEM_SELECTED;
      ev.data.item.widget = { widget_id };
      ev.data.item.index  = selected_item;
      session->dispatch_event(&ev);
    }
    repaint();
    return true;
  }

  // -------------------------------------------------------------------------
  // ComboBoxWidget - collapsed single-line control with popup overlay

  bool ComboBoxWidget::hit_test(float px, float py) const
  {
    // Only the collapsed bar is interactive (the client lays out just the
    // collapsed control; the drop list is an overlay it never sized for).
    // Bounds are in frame-local coords (same space as px/py).
    return px >= static_cast<float>(abs_x) && px < static_cast<float>(abs_x + width) &&
           py >= static_cast<float>(abs_y) && py < static_cast<float>(abs_y) + static_cast<float>(collapsed_h());
  }

  int ComboBoxWidget::collapsed_h() const
  {
    return height > 0 ? height : COMBO_COLLAPSED_H();
  }

  int ComboBoxWidget::max_drop_visible() const
  {
    int n = static_cast<int>(items.size());
    if (n <= 0) return 0;
    int cap = attrs ? attrs->get_int(NEUI_ATTR_COMBO_MAX_VISIBLE, 10) : 10;
    if (cap < 1) cap = 1;
    return std::min(n, cap);
  }

  int ComboBoxWidget::drop_width(neui_render_backend_t* backend) const
  {
    int collapsed_w = width;
    int override_w  = attrs ? attrs->get_int(NEUI_ATTR_COMBO_DROP_WIDTH, 0) : 0;
    if (override_w > 0)
      return std::max(override_w, collapsed_w);

    // Auto: widest entry + text padding (4px each side), plus the scrollbar
    // column when the list overflows. Measured at the widget's font size
    // (family/weight not pushed - matches the popup-menu width heuristic and
    // keeps this callable from the non-painting hit-test handlers).
    float ef_size = neui_detail::read_widget_font(attrs.get(), 12.0f).size;
    float maxw = 0.0f;
    if (backend && backend->measure_text) {
      for (auto& it : items) {
        float w = backend->measure_text(nullptr, it.text.c_str(), -1, ef_size);
        if (w > maxw) maxw = w;
      }
    }
    bool overflow = static_cast<int>(items.size()) > max_drop_visible();
    int w = static_cast<int>(maxw + 0.5f) + 8 + (overflow ? SCROLLBAR_W : 0);
    return std::max(w, collapsed_w);
  }

  ComboBoxWidget::OverlayRect
  ComboBoxWidget::overlay_rect(neui_render_backend_t* backend) const
  {
    OverlayRect g;
    int mdv = std::max(1, max_drop_visible());
    g.w = static_cast<float>(drop_width(backend));
    g.h = static_cast<float>(mdv * LIST_ITEM_H());
    g.x = static_cast<float>(abs_x);

    float below_y = static_cast<float>(abs_y + collapsed_h());
    int   frame_h = session ? session->frame_client_height(index) : 0;
    if (frame_h <= 0) {
      // Frame size unknown - keep the historical downward behaviour.
      g.y = below_y;
      return g;
    }

    // Prefer opening below the collapsed bar. Flip above only when the list
    // overflows the frame's bottom edge AND fits in the gap above the bar;
    // if it fits neither, open toward whichever side has more room (it then
    // clips to the frame). space_above is the gap from the frame top down to
    // the bar; space_below is from just under the bar to the frame bottom.
    float space_below = static_cast<float>(frame_h) - below_y;
    float space_above = static_cast<float>(abs_y);
    if (g.h <= space_below)
      g.y = below_y;
    else if (g.h <= space_above)
      g.y = static_cast<float>(abs_y) - g.h;
    else
      g.y = (space_above > space_below) ? static_cast<float>(abs_y) - g.h
                                        : below_y;
    return g;
  }

  int ComboBoxWidget::visible_rows() const
  {
    return max_drop_visible();
  }

  void ComboBoxWidget::paint(neui_render_backend_t* backend,
                              neui_render_ctx_t ctx, bool is_focused)
  {
    using neui_detail::ColorRole;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(collapsed_h());   // only the collapsed bar
    const float arrow_w = 18.0f;

    // Hover/pressed background on the collapsed bar; pressed wins. Same
    // shade ladder as ButtonWidget. The open overlay below has its own
    // hover handling via hover_item, independent of widget-level hover.
    uint32_t base_bg = neui_detail::color(ColorRole::control_bg);
    uint32_t bar_bg  = base_bg;
    if (pressed)      bar_bg = neui_detail::shade(base_bg, -16);
    else if (hovered) bar_bg = neui_detail::shade(base_bg, +16);
    backend->fill_rect(ctx, fx, fy, fw, fh, bar_bg);

    auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
    neui_detail::push_widget_font(backend, ctx, ef);

    // Selected item text
    if (backend->draw_text) {
      const char* sel_text = (selected_item != UINT32_MAX && selected_item < items.size())
                             ? items[selected_item].text.c_str() : "";
      backend->draw_text(ctx, fx + 4.0f, fy, fw - arrow_w - 4.0f, fh,
                         sel_text, ef.size,
                         neui_detail::color(ColorRole::text_primary));
    }

    // Divider between text area and arrow button
    backend->fill_rect(ctx, fx + fw - arrow_w, fy + 2.0f, 1.0f, fh - 4.0f,
                        neui_detail::color(ColorRole::border));

    // Down-arrow symbol, centered horizontally inside the button column.
    // Keep the arrow at the fixed 12px since it's a glyph indicator, not
    // text content - resizing it would look out of place inside the
    // fixed-width arrow column.
    if (backend->draw_text) {
      const char* arrow = "\xe2\x96\xbe";  // U+25BE
      float btn_x = fx + fw - arrow_w;
      float tx    = btn_x;
      if (backend->measure_text) {
        float tw = backend->measure_text(ctx, arrow, -1, 12.0f);
        tx = btn_x + (arrow_w - tw) * 0.5f;
        if (tx < btn_x) tx = btn_x;
      }
      backend->draw_text(ctx, tx, fy, arrow_w - (tx - btn_x), fh,
                         arrow, 12.0f,
                         neui_detail::color(ColorRole::text_secondary));
    }

    neui_detail::pop_widget_font(backend, ctx, ef);

    uint32_t border_color = neui_detail::color(
        is_focused ? ColorRole::border_focused : ColorRole::border);
    backend->draw_rect(ctx, fx, fy, fw, fh, 1.0f, border_color);
  }

  void ComboBoxWidget::paint_overlay(neui_render_backend_t* backend,
                                      neui_render_ctx_t ctx)
  {
    int mdv = max_drop_visible();
    if (mdv <= 0) return;

    // Clamp scroll_offset.
    uint32_t n = static_cast<uint32_t>(items.size());
    if (n == 0) return;
    if (scroll_offset >= n) scroll_offset = n - 1;

    // The overlay is painted by Session::paint_frame OUTSIDE the
    // paint_widgets_recursive walk, so the renderer transform is at the
    // frame's identity here - we must use absolute coords, not local x/y.
    OverlayRect g = overlay_rect(backend);
    float ox = g.x, oy = g.y, ow = g.w, oh = g.h;

    // Highlight follows hover; falls back to the committed selection so the
    // overlay is never blank when first opened.
    uint32_t highlight = (hover_item != UINT32_MAX) ? hover_item : selected_item;

    auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
    neui_detail::push_widget_font(backend, ctx, ef);
    paint_scrollable_list(backend, ctx, ox, oy, ow, oh,
        items, highlight, scroll_offset,
        mdv, mdv * LIST_ITEM_H(),
        neui_detail::color(neui_detail::ColorRole::control_bg_alt),
        neui_detail::color(neui_detail::ColorRole::border),
        ef.size);
    neui_detail::pop_widget_font(backend, ctx, ef);
  }

  bool ComboBoxWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    bool is_open = session && session->_open_combo == index;

    if (is_open) {
      if (keycode == NEUI_KEY_ESCAPE) {
        // Dismiss without committing. selected_item was never modified while open.
        session->close_combo();
        return true;
      }
      if (keycode == NEUI_KEY_RETURN || keycode == NEUI_KEY_SPACE) {
        // Commit the hovered/highlighted row. Emit ITEM_SELECTED only if it
        // actually changes the previously-selected item.
        if (hover_item != UINT32_MAX && hover_item < items.size() &&
            hover_item != selected_item) {
          selected_item = hover_item;
          text = items[selected_item].text;
          neui_event_t ev = {};
          ev.type = NEUI_EVENT_ITEM_SELECTED;
          ev.data.item.widget = { widget_id };
          ev.data.item.index  = selected_item;
          session->dispatch_event(&ev);
        }
        session->close_combo();
        return true;
      }
      // UP/DOWN/HOME/END move the highlight (hover_item) only.
      uint32_t n = static_cast<uint32_t>(items.size());
      if (n == 0) return false;
      uint32_t hov = (hover_item != UINT32_MAX) ? hover_item : selected_item;
      if (keycode == NEUI_KEY_UP) {
        hov = (hov == 0 || hov == UINT32_MAX) ? 0 : hov - 1;
      } else if (keycode == NEUI_KEY_DOWN) {
        hov = (hov == UINT32_MAX) ? 0 : std::min(hov + 1, n - 1);
      } else if (keycode == NEUI_KEY_HOME) {
        hov = 0; scroll_offset = 0;
      } else if (keycode == NEUI_KEY_END) {
        hov = n - 1;
      } else {
        return false;
      }
      hover_item = hov;
      int max_vis = std::max(1, max_drop_visible());
      if (hover_item < scroll_offset)
        scroll_offset = hover_item;
      else if (hover_item >= scroll_offset + static_cast<uint32_t>(max_vis))
        scroll_offset = hover_item - static_cast<uint32_t>(max_vis) + 1;
      repaint();
      return true;
    }

    // Closed: UP/DOWN/HOME/END cycle selection directly without opening the overlay.
    if (keycode == NEUI_KEY_UP   || keycode == NEUI_KEY_DOWN ||
        keycode == NEUI_KEY_HOME || keycode == NEUI_KEY_END)
      return ListItemsWidget::on_keydown(keycode, modifiers);

    // SPACE toggles the overlay.
    if (keycode == NEUI_KEY_SPACE) {
      if (session) session->open_combo(index);
      return true;
    }
    return false;
  }

  bool ComboBoxWidget::on_mouse_event(neui_event_t* event)
  {
    if (event->type != NEUI_EVENT_MOUSE_BUTTON_DOWN) return false;
    if (!session) return false;
    if (session->_open_combo == index)
      session->close_combo();
    else
      session->open_combo(index);
    return true;
  }

  // -------------------------------------------------------------------------
  // Session combo overlay management

  // ---- Popup menu overlay --------------------------------------------------

  // Layout constants for the popup menu overlay. Item height + label font
  // scale with painted_ui_scale() so the menu text matches the rest of the
  // (scaled) painted UI on iOS; identity on desktop.
  static int POPUP_ITEM_H()  { return neui_detail::scaled_painted_metric(22); }
  static int POPUP_FONT_PX() { return neui_detail::scaled_painted_metric(13); }
  static constexpr int POPUP_PAD_X    = 12;
  static constexpr int POPUP_PAD_Y    = 4;
  static constexpr int POPUP_SEP_H    = 7;
  static constexpr int POPUP_MIN_W    = 140;

  static bool popup_item_is_separator(const std::string& s)
  {
    return s.size() == 1 && s[0] == '-';
  }

  static int popup_item_height(const std::string& s)
  {
    return popup_item_is_separator(s) ? POPUP_SEP_H : POPUP_ITEM_H();
  }

  static int popup_total_height(const std::vector<std::string>& items)
  {
    int h = POPUP_PAD_Y * 2;
    for (auto& s : items) h += popup_item_height(s);
    return h;
  }

  static int popup_total_width(neui_render_backend_t* backend,
                                neui_render_ctx_t ctx,
                                const std::vector<std::string>& items)
  {
    float maxw = 0.0f;
    if (backend && backend->measure_text) {
      for (auto& s : items) {
        if (popup_item_is_separator(s)) continue;
        float w = backend->measure_text(ctx, s.c_str(), -1, (float)POPUP_FONT_PX());
        if (w > maxw) maxw = w;
      }
    }
    int w = (int)maxw + POPUP_PAD_X * 2;
    return w < POPUP_MIN_W ? POPUP_MIN_W : w;
  }

  // Handle a click on the popup at frame-local (lx, ly). Updates
  // _popup_picked / _popup_running so the nested pump in
  // open_popup_menu returns. Returns true unconditionally - when a
  // popup is up, all clicks are popup-owned (clicks outside dismiss).
  bool Session::handle_popup_click(float lx, float ly)
  {
    if (!_popup_active) return false;
    int width = popup_total_width(_backend, nullptr, _popup_items);
    int height = popup_total_height(_popup_items);
    if (lx < static_cast<float>(_popup_x_abs) || lx >= static_cast<float>(_popup_x_abs + width) ||
        ly < static_cast<float>(_popup_y_abs) || ly >= static_cast<float>(_popup_y_abs + height)) {
      // click outside -> dismiss
      _popup_picked = 0;
      _popup_active = false;
      _popup_running = false;
      return true;
    }
    int rel_y = (int)ly - _popup_y_abs - POPUP_PAD_Y;
    int idx = 0;       // 1-based pick
    int run_y = 0;
    for (auto& s : _popup_items) {
      ++idx;
      int h = popup_item_height(s);
      if (rel_y >= run_y && rel_y < run_y + h) {
        if (popup_item_is_separator(s)) {
          // ignore separator click; stay open
          return true;
        }
        _popup_picked = idx;
        _popup_active = false;
        _popup_running = false;
        return true;
      }
      run_y += h;
    }
    return true;
  }

  bool Session::handle_popup_hover(float lx, float ly)
  {
    if (!_popup_active) return false;
    int width  = popup_total_width(_backend, nullptr, _popup_items);
    int height = popup_total_height(_popup_items);
    if (lx < static_cast<float>(_popup_x_abs) || lx >= static_cast<float>(_popup_x_abs + width) ||
        ly < static_cast<float>(_popup_y_abs) || ly >= static_cast<float>(_popup_y_abs + height)) {
      if (_popup_hover != -1) {
        _popup_hover = -1;
        void* frame = nullptr;
        uint32_t f = _widgets.child(0);
        while (f != 0 && _widgets.exists(f)) {
          if (_widgets[f].native_handle) { frame = _widgets[f].native_handle; break; }
          f = _widgets.next(f);
        }
        if (frame) platform_invalidate(frame);
      }
      return false;
    }
    int rel_y = (int)ly - _popup_y_abs - POPUP_PAD_Y;
    int idx = 0;
    int run_y = 0;
    int new_hover = -1;
    for (auto& s : _popup_items) {
      int h = popup_item_height(s);
      if (rel_y >= run_y && rel_y < run_y + h) {
        if (!popup_item_is_separator(s)) new_hover = idx;
        break;
      }
      run_y += h;
      ++idx;
    }
    if (new_hover != _popup_hover) {
      _popup_hover = new_hover;
      uint32_t f = _widgets.child(0);
      while (f != 0 && _widgets.exists(f)) {
        if (_widgets[f].native_handle) {
          platform_invalidate(_widgets[f].native_handle);
          break;
        }
        f = _widgets.next(f);
      }
    }
    return true;
  }

  bool Session::handle_popup_key(uint32_t keycode)
  {
    if (!_popup_active) return false;
    if (keycode == NEUI_KEY_ESCAPE) {
      _popup_picked = 0;
      _popup_active = false;
      _popup_running = false;
      return true;
    }
    if (keycode == NEUI_KEY_RETURN || keycode == NEUI_KEY_SPACE) {
      if (_popup_hover > 0) {
        _popup_picked = _popup_hover;
      } else {
        _popup_picked = 0;
      }
      _popup_active = false;
      _popup_running = false;
      return true;
    }
    if (keycode == NEUI_KEY_DOWN || keycode == NEUI_KEY_UP) {
      int n = (int)_popup_items.size();
      int dir = (keycode == NEUI_KEY_DOWN) ? 1 : -1;
      int next = (_popup_hover < 1) ? (dir > 0 ? 1 : n) : _popup_hover + dir;
      // Skip separators.
      for (int safety = 0; safety < n; ++safety) {
        if (next < 1) next = n;
        if (next > n) next = 1;
        if (!popup_item_is_separator(_popup_items[static_cast<size_t>(next - 1)])) break;
        next += dir;
      }
      _popup_hover = next;
      uint32_t f = _widgets.child(0);
      while (f != 0 && _widgets.exists(f)) {
        if (_widgets[f].native_handle) {
          platform_invalidate(_widgets[f].native_handle);
          break;
        }
        f = _widgets.next(f);
      }
      return true;
    }
    return false;
  }

  void Session::paint_popup_menu(neui_render_ctx_t ctx)
  {
    if (!_popup_active || !_backend) return;
    using neui_detail::ColorRole;
    int w = popup_total_width(_backend, ctx, _popup_items);
    int h = popup_total_height(_popup_items);
    float fx = (float)_popup_x_abs;
    float fy = (float)_popup_y_abs;

    _backend->fill_rect(ctx, fx, fy, (float)w, (float)h,
                        neui_detail::color(ColorRole::control_bg_alt));
    _backend->draw_rect(ctx, fx, fy, (float)w, (float)h, 1.0f,
                        neui_detail::color(ColorRole::border));

    int run_y = POPUP_PAD_Y;
    int idx = 0;
    for (auto& s : _popup_items) {
      ++idx;
      int ih = popup_item_height(s);
      if (popup_item_is_separator(s)) {
        float sy = fy + (float)run_y + (float)POPUP_SEP_H * 0.5f;
        _backend->fill_rect(ctx, fx + 4.0f, sy,
                             (float)w - 8.0f, 1.0f,
                             neui_detail::color(ColorRole::scrollbar_separator));
        run_y += ih;
        continue;
      }
      bool hovered = (idx == _popup_hover);
      if (hovered) {
        _backend->fill_rect(ctx, fx + 1.0f, fy + (float)run_y,
                             (float)w - 2.0f, (float)ih,
                             neui_detail::color(ColorRole::accent));
      }
      _backend->draw_text(ctx,
        fx + (float)POPUP_PAD_X, fy + (float)run_y,
        (float)w - (float)POPUP_PAD_X * 2.0f, (float)ih,
        s.c_str(), (float)POPUP_FONT_PX(),
        hovered ? neui_detail::color(ColorRole::accent_text)
                : neui_detail::color(ColorRole::text_primary));
      run_y += ih;
    }
  }

  int Session::open_popup_menu(uint32_t anchor_idx, int lx, int ly,
                                const std::vector<std::string>& items)
  {
    if (items.empty() || !_widgets.exists(anchor_idx)) return 0;
    if (_popup_active) return 0;

    auto& anchor = _widgets[anchor_idx];

    // Convert anchor-local logical to absolute frame-local logical by
    // walking up the tree summing x/y offsets. Frames have native_handle
    // set; their x/y is screen-relative so we stop there.
    int abs_x = lx;
    int abs_y = ly;
    auto parents = _widgets.get_all_parents(anchor_idx);
    abs_x += anchor.x;
    abs_y += anchor.y;
    for (auto p : parents) {
      if (p == 0 || !_widgets.exists(p)) continue;
      auto& pw = _widgets[p];
      if (pw.native_handle) break;  // stop at the frame
      abs_x += pw.x;
      abs_y += pw.y;
    }

    _popup_items     = items;
    _popup_x_abs     = abs_x;
    _popup_y_abs     = abs_y;
    _popup_hover     = -1;
    _popup_picked    = 0;
    _popup_active    = true;
    _popup_running   = true;

    // Find the owning frame and invalidate so the overlay paints on the
    // very next pump iteration (before the user moves the mouse).
    void* frame = find_parent_native_handle(anchor_idx);
    if (frame) platform_invalidate(frame);

    // Nested message loop until the user picks or dismisses.
    platform_run_modal_until(&_popup_running);

    int picked = _popup_picked;
    _popup_active = false;
    _popup_items.clear();
    _popup_hover  = -1;
    _popup_picked = 0;

    if (frame) platform_invalidate(frame);
    return picked;
  }

  // -------------------------------------------------------------------------
  // In-frame menubar (Linux / any platform_menubar_in_frame() host)
  //
  // The host draws the menubar itself: a horizontal band at the top of the
  // frame's client area plus cascading dropdown columns. The menu *model*
  // lives in MenubarWidget (menu_items keyed by neui item id, linked by
  // parent_item_id); this code reconstructs the tree from those links at paint
  // / hit-test time, so it needs no native HMENU. Geometry is recomputed on
  // demand from (_menu_open, _menu_path) using the frame's own render context
  // for text measurement - no cached rects, so it stays correct across resize
  // and multiple frames. Win32 (HMENU) and macOS (NSMenu) use the OS menu and
  // never enter here (frame_top_inset returns 0; the platform input layer
  // never calls the handlers).

  static constexpr int MENUBAR_BAND_H     = 24;   // reserved band height (logical px)
  static constexpr int MENUBAR_LABEL_PAD  = 10;   // padding each side of a top-level label
  static constexpr int MENUBAR_FONT_PX    = 13;
  static constexpr int MENU_SHORTCUT_GAP  = 24;   // min gap text -> shortcut in a dropdown
  static constexpr int MENU_ARROW_W       = 16;   // submenu arrow column width
  static constexpr int MENU_CHECK_W       = 16;   // checkmark gutter width (left of text)

  namespace {

    // A laid-out dropdown row. y/h are relative to the column's top-left.
    struct MenuRowL {
      uint32_t    item_id   = 0;
      std::string text;
      std::string shortcut;
      bool        separator = false;
      bool        submenu   = false;
      bool        enabled   = true;
      bool        checked   = false;
      int         y         = 0;
      int         h         = 0;
    };
    struct MenuColL {
      uint32_t              parent_item = 0;  // the item whose children this column shows
      int                   x = 0, y = 0, w = 0, h = 0;
      bool                  check_gutter = false;  // reserve a left checkmark column
      std::vector<MenuRowL> rows;
    };
    struct MenuBandItem {
      uint32_t    item_id = 0;
      int         x = 0, w = 0;
      std::string text;
    };

    // Direct children of `parent_item_id`, in insertion order.
    void menu_children(const MenubarWidget& mb, uint32_t parent_item_id,
                       std::vector<uint32_t>& out)
    {
      for (uint32_t id : mb.menu_item_ids_ordered) {
        auto it = mb.menu_items.find(id);
        if (it != mb.menu_items.end() && it->second.parent_item_id == parent_item_id)
          out.push_back(id);
      }
    }

    bool menu_item_has_children(const MenubarWidget& mb, uint32_t item_id)
    {
      for (uint32_t id : mb.menu_item_ids_ordered) {
        auto it = mb.menu_items.find(id);
        if (it != mb.menu_items.end() && it->second.parent_item_id == item_id)
          return true;
      }
      return false;
    }

    int menu_measure(neui_render_backend_t* be, neui_render_ctx_t ctx,
                     const std::string& s)
    {
      if (!be || !be->measure_text || s.empty()) return 0;
      return static_cast<int>(be->measure_text(ctx, s.c_str(), -1,
                                               static_cast<float>(MENUBAR_FONT_PX)));
    }

  } // namespace

  // Forward decl: defined below, used by paint_menubar.
  static uint32_t frame_menubar_index(neui_detail::Tree<WidgetData>& widgets,
                                      uint32_t frame_index);

  // The visible MENUBAR child of `frame_index`, or nullptr.
  MenubarWidget* Session::frame_menubar(uint32_t frame_index)
  {
    if (!_widgets.exists(frame_index)) return nullptr;
    uint32_t idx = _widgets.child(frame_index);
    while (idx != 0) {
      if (_widgets.exists(idx)) {
        auto& wd = _widgets[idx];
        if (wd.is_menubar() && wd.visible)
          return dynamic_cast<MenubarWidget*>(&wd);
      }
      idx = _widgets.next(idx);
    }
    return nullptr;
  }

  // Effective enabled state of a menu item, mirroring the other hosts'
  // popup-open auto-disable: the item's own flag AND (for a bound built-in
  // command) a focused widget that can perform it AND (if a menu client is
  // registered) its validate() verdict.
  bool Session::menu_item_enabled(const MenubarWidget& mb, uint32_t item_id)
  {
    auto it = mb.menu_items.find(item_id);
    if (it == mb.menu_items.end()) return false;
    const auto& mi = it->second;
    if (mi.is_separator) return false;
    if (!mi.enabled) return false;
    if (mi.menu_cmd != 0 && mi.menu_cmd < NEUI_CMD_USER_BASE &&
        !can_focused_perform_command(mi.menu_cmd))
      return false;
    if (_menu_client && _menu_client->validate &&
        !_menu_client->validate(_token, { mb.widget_id }, { item_id }, mi.menu_cmd))
      return false;
    return true;
  }

  // Lay out the top-level band labels (left to right). Free function (uses
  // anonymous-namespace layout types, so it can't be a header-declared member).
  static void mb_build_band(Session* s, neui_render_ctx_t ctx,
                            const MenubarWidget& mb, std::vector<MenuBandItem>& out)
  {
    int x = 0;
    std::vector<uint32_t> tops;
    menu_children(mb, 0, tops);
    for (uint32_t id : tops) {
      auto it = mb.menu_items.find(id);
      if (it == mb.menu_items.end() || it->second.is_separator) continue;
      MenuBandItem bi;
      bi.item_id = id;
      bi.text    = it->second.text;
      bi.x       = x;
      bi.w       = menu_measure(s->_backend, ctx, bi.text) + MENUBAR_LABEL_PAD * 2;
      out.push_back(bi);
      x += bi.w;
    }
  }

  // Build the chain of open dropdown columns from `path`, positioning each
  // below/beside its parent and clamping to the frame so cascades stay visible.
  //
  // Serves BOTH the in-frame menubar and the standalone tree popup
  // (popup_tree_menu). The two differ in exactly one thing - where level 0 goes -
  // so an empty `band` means "standalone popup: put level 0 at (root_x, root_y)"
  // and a non-empty `band` means "menubar: put level 0 under its band item".
  // Everything else (row sizing, arbitrary-depth cascade placement, right/bottom
  // clamping, the submenu left-flip, the checkmark gutter, shortcut columns, and
  // validate-driven enabling) is already origin-agnostic and shared verbatim.
  static void mb_build_columns(Session* s, neui_render_ctx_t ctx,
                               const MenubarWidget& mb, int frame_w, int frame_h,
                               const std::vector<MenuBandItem>& band,
                               const std::vector<uint32_t>& path,
                               std::vector<MenuColL>& out,
                               int root_x = 0, int root_y = 0)
  {
    for (size_t level = 0; level < path.size(); ++level) {
      uint32_t parent_id = path[level];
      MenuColL col;
      col.parent_item = parent_id;

      std::vector<uint32_t> kids;
      menu_children(mb, parent_id, kids);

      int max_text = 0, max_short = 0;
      bool any_sub = false, any_check = false;
      for (uint32_t kid : kids) {
        auto it = mb.menu_items.find(kid);
        if (it == mb.menu_items.end()) continue;
        const auto& mi = it->second;
        MenuRowL r;
        r.item_id   = kid;
        r.text      = mi.text;
        r.shortcut  = mi.shortcut;
        r.separator = mi.is_separator;
        r.submenu   = menu_item_has_children(mb, kid);
        r.checked   = !r.separator && !r.submenu && mi.checked;
        r.enabled   = r.separator ? false
                    : (r.submenu ? mi.enabled : s->menu_item_enabled(mb, kid));
        if (!r.separator) {
          max_text = std::max(max_text, menu_measure(s->_backend, ctx, r.text));
          if (!r.shortcut.empty())
            max_short = std::max(max_short, menu_measure(s->_backend, ctx, r.shortcut));
          if (r.submenu) any_sub = true;
          if (r.checked) any_check = true;
        }
        col.rows.push_back(std::move(r));
      }

      int w = POPUP_PAD_X * 2 + max_text;
      if (any_check)     w += MENU_CHECK_W;
      if (max_short > 0) w += MENU_SHORTCUT_GAP + max_short;
      if (any_sub)       w += MENU_ARROW_W;
      if (w < POPUP_MIN_W) w = POPUP_MIN_W;

      int run = POPUP_PAD_Y;
      for (auto& r : col.rows) {
        r.y = run;
        r.h = r.separator ? POPUP_SEP_H : POPUP_ITEM_H();
        run += r.h;
      }
      col.w = w;
      col.h = run + POPUP_PAD_Y;
      col.check_gutter = any_check;

      if (level == 0) {
        if (band.empty()) {
          // Standalone popup: level 0 opens AT the anchor point.
          col.x = root_x;
          col.y = root_y;
        } else {
          int bx = 0;
          for (auto& b : band) if (b.item_id == parent_id) { bx = b.x; break; }
          col.x = bx;
          col.y = MENUBAR_BAND_H;
        }
      } else {
        const MenuColL& prev = out[level - 1];
        int py = prev.y;
        for (auto& pr : prev.rows)
          if (pr.item_id == parent_id) { py = prev.y + pr.y; break; }
        col.x = prev.x + prev.w;
        col.y = py;
      }

      // Horizontal clamp: top-level shifts left; submenus flip to the left of
      // their parent column if they'd overflow the right edge.
      if (frame_w > 0 && col.x + col.w > frame_w) {
        if (level == 0) {
          col.x = std::max(0, frame_w - col.w);
        } else {
          int leftx = out[level - 1].x - col.w;
          col.x = (leftx >= 0) ? leftx : std::max(0, frame_w - col.w);
        }
      }
      // Vertical clamp.
      if (frame_h > 0 && col.y + col.h > frame_h)
        col.y = std::max(0, frame_h - col.h);

      out.push_back(std::move(col));
    }
  }

  int Session::frame_top_inset(uint32_t frame_index)
  {
    bool has_mb = frame_menubar(frame_index) != nullptr;
    // In-frame band painter platforms (Linux/X11) reserve the band height for
    // the host-drawn cascading menubar. Win32 / macOS / null return false here,
    // so this contributes 0.
    int inset = 0;
    if (platform_menubar_in_frame())
      inset = has_mb ? MENUBAR_BAND_H : 0;
    // Additive per-platform extra inset. iOS reserves the status-bar/notch safe
    // area (plus a hamburger band when the frame has a menubar) WITHOUT enabling
    // the Linux band painter - paint_menubar stays gated on
    // platform_menubar_in_frame() (false on iOS). 0 on every desktop platform,
    // so Linux/Win32/macOS/null behaviour is unchanged.
    void* nh = _widgets.exists(frame_index) ? _widgets[frame_index].native_handle
                                            : nullptr;
    inset += platform_frame_extra_top_inset(nh, has_mb);
    return inset;
  }

  // The usable content area of a widget, in the widget's own coordinate space
  // (logical px). For a frame this excludes any in-frame menubar band: the
  // returned origin (0, top_inset) is where child (0,0) lands in window space,
  // and (w, h) is the content size below the band - the Win32 GetClientRect
  // analogue. On native-menu platforms / menubar-less frames / non-frame
  // widgets the inset is 0, so this is just (0, 0, width, height). Internal
  // callers (toast anchoring) and the public widgets->get_client_rect share it.
  void Session::widget_client_rect(uint32_t widget_index,
                                   int* x, int* y, int* w, int* h)
  {
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (_widgets.exists(widget_index)) {
      auto& wd = _widgets[widget_index];
      int inset = frame_top_inset(widget_index);
      rw = wd.width;
      ry = inset;
      rh = wd.height - inset;
      if (rh < 0) rh = 0;
    }
    if (x) *x = rx;
    if (y) *y = ry;
    if (w) *w = rw;
    if (h) *h = rh;
  }

  void Session::close_menubar_menu()
  {
    if (!_menu_open && _menu_path.empty()) return;
    uint32_t frame = 0;
    if (_widgets.exists(_menu_bar)) {
      auto parents = _widgets.get_all_parents(_menu_bar);
      for (uint32_t p : parents)
        if (p != 0 && _widgets.exists(p) && _widgets[p].native_handle) { frame = p; break; }
    }
    _menu_open = false;
    _menu_path.clear();
    _menu_hover_item = 0;
    _menu_bar = 0;
    if (frame && _widgets.exists(frame) && _widgets[frame].native_handle)
      platform_invalidate(_widgets[frame].native_handle);
  }

  // Paint an open cascade of dropdown columns. Shared verbatim by the in-frame
  // menubar and by the standalone tree popup - the columns are already
  // positioned by mb_build_columns, so nothing here knows or cares which one it
  // is drawing.
  static void paint_menu_columns(Session* s, neui_render_ctx_t ctx,
                                 const std::vector<MenuColL>& cols,
                                 const std::vector<uint32_t>& path,
                                 uint32_t hover_item)
  {
    auto* _backend = s->_backend;
    using neui_detail::ColorRole;
    auto C = [](ColorRole r) { return neui_detail::color(r); };
    for (auto& col : cols) {
      _backend->fill_rect(ctx, (float)col.x, (float)col.y, (float)col.w, (float)col.h,
                          C(ColorRole::control_bg_alt));
      _backend->draw_rect(ctx, (float)col.x, (float)col.y, (float)col.w, (float)col.h, 1.0f,
                          C(ColorRole::border));
      for (auto& r : col.rows) {
        float ry = (float)(col.y + r.y);
        if (r.separator) {
          _backend->fill_rect(ctx, (float)col.x + 4.0f, ry + (float)POPUP_SEP_H * 0.5f,
                              (float)col.w - 8.0f, 1.0f, C(ColorRole::scrollbar_separator));
          continue;
        }
        // Highlight the hovered row, or the row whose submenu is currently open.
        bool open_sub = false;
        for (uint32_t pid : path) if (pid == r.item_id) { open_sub = true; break; }
        bool hl = (r.item_id == hover_item) || open_sub;
        if (hl)
          _backend->fill_rect(ctx, (float)col.x + 1.0f, ry, (float)col.w - 2.0f, (float)r.h,
                              C(ColorRole::accent));
        uint32_t tcol = !r.enabled ? C(ColorRole::text_disabled)
                      : hl          ? C(ColorRole::accent_text)
                                    : C(ColorRole::text_primary);
        // When the column reserves a checkmark gutter, text is indented past it
        // and a check glyph is drawn in the gutter for checked rows.
        int gutter = col.check_gutter ? MENU_CHECK_W : 0;
        if (r.checked) {
          _backend->draw_text(ctx, (float)(col.x + POPUP_PAD_X), ry,
                              (float)MENU_CHECK_W, (float)r.h,
                              "\xE2\x9C\x93" /* U+2713 check mark */,
                              (float)MENUBAR_FONT_PX, tcol);
        }
        _backend->draw_text(ctx, (float)(col.x + POPUP_PAD_X + gutter), ry,
                            (float)(col.w - POPUP_PAD_X * 2 - gutter), (float)r.h,
                            r.text.c_str(), (float)MENUBAR_FONT_PX, tcol);
        if (!r.shortcut.empty()) {
          int sw = menu_measure(_backend, ctx, r.shortcut);
          int sx = col.x + col.w - POPUP_PAD_X - (r.submenu ? MENU_ARROW_W : 0) - sw;
          uint32_t scol = !r.enabled ? C(ColorRole::text_disabled)
                        : hl          ? C(ColorRole::accent_text)
                                      : C(ColorRole::text_secondary);
          _backend->draw_text(ctx, (float)sx, ry, (float)sw, (float)r.h,
                              r.shortcut.c_str(), (float)MENUBAR_FONT_PX, scol);
        }
        if (r.submenu) {
          uint32_t acol = !r.enabled ? C(ColorRole::text_disabled)
                        : hl          ? C(ColorRole::accent_text)
                                      : C(ColorRole::text_secondary);
          _backend->draw_text(ctx, (float)(col.x + col.w - MENU_ARROW_W), ry,
                              (float)MENU_ARROW_W, (float)r.h,
                              "\xE2\x96\xB8" /* U+25B8 small right triangle */,
                              (float)MENUBAR_FONT_PX, acol);
        }
      }
    }
  }

  void Session::paint_menubar(neui_render_ctx_t ctx, uint32_t frame_index)
  {
    if (!_backend) return;
    // Only the in-frame platforms draw the band; Win32 (HMENU) / macOS (NSMenu)
    // xpl hosts have a MenubarWidget but render it through the OS menu.
    if (!platform_menubar_in_frame()) return;
    MenubarWidget* mbp = frame_menubar(frame_index);
    if (!mbp) return;
    const MenubarWidget& mb = *mbp;
    using neui_detail::ColorRole;
    auto C = [](ColorRole r) { return neui_detail::color(r); };

    int fw = _widgets.exists(frame_index) ? _widgets[frame_index].width  : 0;
    int fh = _widgets.exists(frame_index) ? _widgets[frame_index].height : 0;

    std::vector<MenuBandItem> band;
    mb_build_band(this, ctx, mb, band);

    // Band background + bottom separator line.
    _backend->fill_rect(ctx, 0.0f, 0.0f, (float)fw, (float)MENUBAR_BAND_H, C(ColorRole::panel_bg));
    _backend->fill_rect(ctx, 0.0f, (float)(MENUBAR_BAND_H - 1), (float)fw, 1.0f, C(ColorRole::border));

    // _menu_bar holds the open menu's menubar widget index; only this frame's
    // own menubar shows its band highlight + dropdowns.
    uint32_t this_mb_index = frame_menubar_index(_widgets, frame_index);
    bool mine = _menu_open && _menu_bar == this_mb_index;

    for (auto& b : band) {
      bool open_top = mine && !_menu_path.empty() && _menu_path[0] == b.item_id;
      // Open: highlight the hovered/open top-level. Closed: highlight the band
      // label under the cursor so it's an obvious mouse target.
      bool hovered  = mine ? (_menu_hover_item == b.item_id)
                           : (_menu_band_hover == b.item_id);
      if (open_top || hovered)
        _backend->fill_rect(ctx, (float)b.x, 0.0f, (float)b.w, (float)MENUBAR_BAND_H, C(ColorRole::accent));
      _backend->draw_text(ctx, (float)(b.x + MENUBAR_LABEL_PAD), 0.0f,
                          (float)(b.w - MENUBAR_LABEL_PAD * 2), (float)MENUBAR_BAND_H,
                          b.text.c_str(), (float)MENUBAR_FONT_PX,
                          (open_top || hovered) ? C(ColorRole::accent_text)
                                                : C(ColorRole::text_primary));
    }

    if (!mine) return;

    std::vector<MenuColL> cols;
    mb_build_columns(this, ctx, mb, fw, fh, band, _menu_path, cols);
    paint_menu_columns(this, ctx, cols, _menu_path, _menu_hover_item);
  }

  // ---------------------------------------------------------------------------
  // Standalone tree popup (widgets->popup_tree_menu).
  //
  // Deliberately thin: every hard part - cascade layout, edge clamping, submenu
  // flipping, checkmark gutters, shortcut columns, per-item enable via
  // NEUI_API_MENU_CLIENT::validate, and painting - is the menubar's, reused
  // unchanged. Only two things differ: level 0 opens at an anchor point instead
  // of under a band item (a mb_build_columns parameter), and a pick reports as
  // NEUI_EVENT_ITEM_SELECTED instead of driving a menu bar.
  //
  // Unlike the menubar cascade - which only ever runs where the host draws its
  // own menu band (Linux) - this runs on ALL THREE platforms, so it owns its
  // input plumbing rather than borrowing the menubar's: every platform layer
  // routes press / move / release / Esc through the handlers below.

  bool Session::show_tree_popup(uint32_t anchor_idx, int x, int y, uint32_t menu_idx)
  {
    // Refuse where the platform layer does not route input to the popup (iOS,
    // null). paint_frame would still PAINT the cascade - it is
    // platform-independent - so without this the API would put up a menu that
    // cannot be picked or dismissed and whose taps fall through to the widgets
    // underneath. Failing honestly is strictly better.
    if (!platform_supports_tree_popup()) return false;

    if (!_widgets.exists(menu_idx)) return false;
    auto* pm = dynamic_cast<PopupMenuWidget*>(&_widgets[menu_idx]);
    if (!pm) return false;                       // not a POPUPMENU

    // An empty menu must not open: a 1-row-high empty box that swallows the next
    // click is worse than doing nothing.
    std::vector<uint32_t> roots;
    menu_children(*pm, 0, roots);
    if (roots.empty()) return false;

    // Resolve the anchor to frame-local logical px. The anchor's own frame owns
    // the overlay, since that is the surface the popup paints on.
    uint32_t frame_idx = anchor_idx;
    while (frame_idx != 0 && _widgets.exists(frame_idx) &&
           !_widgets[frame_idx].is_frame())
      frame_idx = _widgets.get_parent(frame_idx);
    if (frame_idx == 0 || !_widgets.exists(frame_idx)) return false;

    int ax = x, ay = y;
    if (anchor_idx != frame_idx && _widgets.exists(anchor_idx)) {
      ax += _widgets[anchor_idx].abs_x;
      ay += _widgets[anchor_idx].abs_y;
    }

    // Close whatever menu cascade is open first: only one at a time, matching
    // the OS menus this mirrors. (Both share _menu_path / _menu_hover_item.)
    close_menubar_menu();
    close_tree_popup();

    _tree_popup_active = true;
    _tree_popup_menu   = menu_idx;
    _tree_popup_frame  = frame_idx;
    _tree_popup_x      = ax;
    _tree_popup_y      = ay;
    // Level 0's "parent" is the tree root, so the popup's rows are the items
    // directly under tree_item_root - exactly what a client added there.
    _menu_path         = { 0 };
    _menu_hover_item   = 0;

    if (void* native = _widgets[frame_idx].native_handle)
      platform_invalidate(native);
    return true;
  }

  void Session::close_tree_popup()
  {
    if (!_tree_popup_active) return;
    uint32_t frame_idx = _tree_popup_frame;
    _tree_popup_active = false;
    _tree_popup_menu   = 0;
    _tree_popup_frame  = 0;
    _menu_path.clear();
    _menu_hover_item   = 0;
    if (frame_idx != 0 && _widgets.exists(frame_idx)) {
      if (void* native = _widgets[frame_idx].native_handle)
        platform_invalidate(native);
    }
  }

  void Session::close_tree_popup_if_within(uint32_t subtree_root)
  {
    if (!_tree_popup_active || subtree_root == 0) return;
    if (subtree_root == _tree_popup_menu || subtree_root == _tree_popup_frame) {
      close_tree_popup();
      return;
    }
    // Also catch an ancestor going away (destroying the frame takes the popup
    // widget with it, and destroying a SECTION can take the menu widget).
    for (uint32_t probe : { _tree_popup_menu, _tree_popup_frame }) {
      if (probe == 0 || !_widgets.exists(probe)) continue;
      for (uint32_t p : _widgets.get_all_parents(probe)) {
        if (p == subtree_root) { close_tree_popup(); return; }
      }
    }
  }

  void Session::refresh_open_tree_popup(uint32_t menu_idx)
  {
    if (!_tree_popup_active || _tree_popup_menu != menu_idx) return;
    if (!_widgets.exists(_tree_popup_frame)) return;
    if (void* native = _widgets[_tree_popup_frame].native_handle)
      platform_invalidate(native);
  }

  bool Session::tree_popup_take_release()
  {
    if (!_tree_popup_swallow_release) return false;
    _tree_popup_swallow_release = false;
    return true;
  }

  // Build the open cascade for the active popup. Returns false when there is
  // nothing to show, so paint / hit-test share one guard.
  static bool tp_build(Session* s, neui_render_ctx_t ctx, uint32_t frame_index,
                       std::vector<MenuColL>& cols)
  {
    if (!s->_tree_popup_active || frame_index != s->_tree_popup_frame) return false;
    if (!s->_widgets.exists(s->_tree_popup_menu)) return false;
    auto* pm = dynamic_cast<PopupMenuWidget*>(&s->_widgets[s->_tree_popup_menu]);
    if (!pm) return false;
    const auto& fw = s->_widgets[frame_index];
    // Empty band == standalone popup: mb_build_columns then puts level 0 at
    // (root_x, root_y) rather than under a band item.
    static const std::vector<MenuBandItem> kNoBand;
    mb_build_columns(s, ctx, *pm, fw.width, fw.height, kNoBand, s->_menu_path,
                     cols, s->_tree_popup_x, s->_tree_popup_y);
    return !cols.empty();
  }

  void Session::paint_tree_popup(neui_render_ctx_t ctx, uint32_t frame_index)
  {
    if (!_backend) return;
    std::vector<MenuColL> cols;
    if (!tp_build(this, ctx, frame_index, cols)) return;
    paint_menu_columns(this, ctx, cols, _menu_path, _menu_hover_item);
  }

  // Shared front half of the click / hover handlers: decides whether this input
  // belongs to the popup at all, and self-heals the two states that would
  // otherwise leave an invisible modal grab swallowing input forever.
  //
  // Returns 0 = not ours (caller returns false and the input proceeds normally),
  // 1 = ours, `cols` is built.
  static int tp_claim(Session* s, uint32_t frame_index, std::vector<MenuColL>& cols)
  {
    if (!s->_tree_popup_active || !s->_backend) return 0;
    // Input on a DIFFERENT frame: the popup loses its grab, exactly like an OS
    // menu when another window is clicked. Dismiss, but do NOT consume - the
    // click belongs to that other frame.
    if (frame_index != s->_tree_popup_frame) { s->close_tree_popup(); return 0; }
    if (!s->_widgets.exists(frame_index))    { s->close_tree_popup(); return 0; }
    if (!tp_build(s, s->_widgets[frame_index].render_ctx, frame_index, cols)) {
      // The menu widget is gone (destroyed, or no longer a POPUPMENU). Close so
      // the popup cannot stay "active" with nothing to hit. Note this does NOT
      // catch an EMPTIED menu: mb_build_columns still pushes one zero-row column
      // for the root, so tp_build returns true. tree->clear closes the popup
      // itself (t_clear in widgets.cpp) - that is where the empty case is
      // handled, not here.
      s->close_tree_popup();
      return 0;
    }
    return 1;
  }

  bool Session::handle_tree_popup_click(uint32_t frame_index, float lx, float ly)
  {
    std::vector<MenuColL> cols;
    if (!tp_claim(this, frame_index, cols)) return false;

    // Any click the popup consumes also owns the matching release, so the widget
    // under the dismissed popup can't see an UP (and synthesise a CLICK).
    _tree_popup_swallow_release = true;

    for (size_t level = 0; level < cols.size(); ++level) {
      const MenuColL& col = cols[level];
      if (lx < col.x || lx >= col.x + col.w || ly < col.y || ly >= col.y + col.h)
        continue;
      for (const auto& r : col.rows) {
        float ry = (float)(col.y + r.y);
        if (ly < ry || ly >= ry + (float)r.h) continue;
        if (r.separator || !r.enabled) return true;   // consumed, no action
        if (r.submenu) {
          // Open the submenu: truncate the path to this level and descend.
          _menu_path.resize(level + 1);
          _menu_path.push_back(r.item_id);
          _menu_hover_item = r.item_id;
          if (void* native = _widgets[frame_index].native_handle)
            platform_invalidate(native);
          return true;
        }
        // A leaf: report it, then close. The item id is captured BEFORE closing
        // because close_tree_popup clears the state the id came from.
        const uint32_t menu_idx = _tree_popup_menu;
        const uint32_t item_id  = r.item_id;
        uint32_t menu_cmd = 0;
        if (_widgets.exists(menu_idx)) {
          if (auto* pm = dynamic_cast<PopupMenuWidget*>(&_widgets[menu_idx])) {
            auto it = pm->menu_items.find(item_id);
            if (it != pm->menu_items.end()) menu_cmd = it->second.menu_cmd;
          }
        }
        close_tree_popup();

        // Built-in command routing first (set_menu_cmd with NEUI_CMD_COPY and
        // friends): offer it to the focused widget, exactly as the menu bar does.
        //
        // Deliberately NOT dispatch_menu_event / dispatch_menu_command: cmd ids
        // restart at 0x8000 per menu widget, so the _menubars scan would hand a
        // popup's pick to whichever menu BAR happens to hold the same id. And
        // going through dispatch_menu_command even with the right widget would
        // fire TREE_ITEM_ACTIVATED for EVERY row (every leaf carries a cmd_id,
        // bound or not), making ITEM_SELECTED a redundant second event per pick.
        // One pick, one client-facing event.
        if (menu_cmd != 0 && menu_cmd < NEUI_CMD_USER_BASE)
          invoke_focused_command(menu_cmd);
        if (_widgets.exists(menu_idx)) {
          neui_event_t ev = {};
          ev.type              = NEUI_EVENT_ITEM_SELECTED;
          ev.data.item.widget  = { _widgets[menu_idx].widget_id };
          ev.data.item.index   = item_id;
          dispatch_event(&ev);
        }
        return true;
      }
      return true;   // inside the column but between rows: swallow
    }

    // Outside every column: dismiss without picking. The click is consumed, as
    // it is on both native platforms - dismissing a menu does not also actuate
    // whatever was under the pointer.
    close_tree_popup();
    return true;
  }

  bool Session::handle_tree_popup_hover(uint32_t frame_index, float lx, float ly)
  {
    std::vector<MenuColL> cols;
    if (!tp_claim(this, frame_index, cols)) return false;

    for (size_t level = 0; level < cols.size(); ++level) {
      const MenuColL& col = cols[level];
      if (lx < col.x || lx >= col.x + col.w || ly < col.y || ly >= col.y + col.h)
        continue;
      for (const auto& r : col.rows) {
        float ry = (float)(col.y + r.y);
        if (ly < ry || ly >= ry + (float)r.h) continue;
        if (r.separator) return true;
        // Hover-to-open submenus, and hover-to-collapse a deeper cascade, which
        // is what makes a cascading menu feel native.
        std::vector<uint32_t> want(_menu_path.begin(),
                                   _menu_path.begin() + (long)level + 1);
        if (r.submenu && r.enabled) want.push_back(r.item_id);
        if (want != _menu_path || _menu_hover_item != r.item_id) {
          _menu_path       = want;
          _menu_hover_item = r.item_id;
          if (void* native = _widgets[frame_index].native_handle)
            platform_invalidate(native);
        }
        return true;
      }
      return true;
    }
    return true;   // still inside the popup's modal-ish grab: swallow hover
  }

  bool Session::handle_tree_popup_key(uint32_t keycode)
  {
    if (!_tree_popup_active) return false;
    if (keycode == NEUI_KEY_ESCAPE) { close_tree_popup(); return true; }
    return false;
  }

  // Find the index of the visible menubar child of `frame_index` (0 if none).
  static uint32_t frame_menubar_index(neui_detail::Tree<WidgetData>& widgets,
                                      uint32_t frame_index)
  {
    if (!widgets.exists(frame_index)) return 0;
    uint32_t idx = widgets.child(frame_index);
    while (idx != 0) {
      if (widgets.exists(idx) && widgets[idx].is_menubar() && widgets[idx].visible)
        return idx;
      idx = widgets.next(idx);
    }
    return 0;
  }

  bool Session::handle_menubar_click(uint32_t frame_index, float lx, float ly)
  {
    MenubarWidget* mbp = frame_menubar(frame_index);
    if (!mbp) {
      if (_menu_open) { close_menubar_menu(); return true; }
      return false;
    }
    const MenubarWidget& mb = *mbp;
    uint32_t this_mb_index = frame_menubar_index(_widgets, frame_index);
    neui_render_ctx_t ctx = _widgets[frame_index].render_ctx;
    int fw = _widgets[frame_index].width, fh = _widgets[frame_index].height;
    bool mine = _menu_open && _menu_bar == this_mb_index;

    std::vector<MenuBandItem> band;
    mb_build_band(this, ctx, mb, band);

    bool in_band = ly >= 0.0f && ly < (float)MENUBAR_BAND_H;

    if (in_band) {
      for (auto& b : band) {
        if (lx >= (float)b.x && lx < (float)(b.x + b.w)) {
          if (mine && !_menu_path.empty() && _menu_path[0] == b.item_id) {
            close_menubar_menu();           // toggle the open top-level closed
          } else {
            _menu_open       = true;
            _menu_bar        = this_mb_index;
            _menu_path       = { b.item_id };
            _menu_hover_item = b.item_id;
            platform_invalidate(_widgets[frame_index].native_handle);
          }
          return true;
        }
      }
      // Click on empty band area: dismiss if open, else consume (band is ours).
      if (mine) close_menubar_menu();
      return true;
    }

    if (!mine) return false;  // closed + click below the band -> normal dispatch

    std::vector<MenuColL> cols;
    mb_build_columns(this, ctx, mb, fw, fh, band, _menu_path, cols);
    for (size_t level = 0; level < cols.size(); ++level) {
      const MenuColL& col = cols[level];
      if (lx < (float)col.x || lx >= (float)(col.x + col.w) ||
          ly < (float)col.y || ly >= (float)(col.y + col.h))
        continue;
      for (auto& r : col.rows) {
        float ry = (float)(col.y + r.y);
        if (ly < ry || ly >= ry + (float)r.h) continue;
        if (r.separator) return true;
        if (r.submenu) {
          if (!r.enabled) return true;
          _menu_path.resize(level + 1);
          _menu_path.push_back(r.item_id);
          _menu_hover_item = r.item_id;
          platform_invalidate(_widgets[frame_index].native_handle);
          return true;
        }
        if (r.enabled) {
          uint32_t cmd_id = mb.menu_items.at(r.item_id).cmd_id;
          close_menubar_menu();
          dispatch_menu_event(cmd_id);
        }
        return true;  // disabled leaf: consume, no action
      }
      return true;     // inside the column padding: consume
    }

    // Click outside band and all columns while open: dismiss.
    close_menubar_menu();
    return true;
  }

  bool Session::handle_menubar_hover(uint32_t frame_index, float lx, float ly)
  {
    if (!_menu_open) return false;
    MenubarWidget* mbp = frame_menubar(frame_index);
    uint32_t this_mb_index = frame_menubar_index(_widgets, frame_index);
    if (!mbp || _menu_bar != this_mb_index) return false;
    const MenubarWidget& mb = *mbp;
    neui_render_ctx_t ctx = _widgets[frame_index].render_ctx;
    int fw = _widgets[frame_index].width, fh = _widgets[frame_index].height;

    std::vector<MenuBandItem> band;
    mb_build_band(this, ctx, mb, band);

    // Hover over the band switches the open top-level menu.
    if (ly >= 0.0f && ly < (float)MENUBAR_BAND_H) {
      for (auto& b : band) {
        if (lx >= (float)b.x && lx < (float)(b.x + b.w)) {
          if (_menu_path.empty() || _menu_path[0] != b.item_id) {
            _menu_path       = { b.item_id };
            _menu_hover_item = b.item_id;
            platform_invalidate(_widgets[frame_index].native_handle);
          } else if (_menu_hover_item != b.item_id) {
            _menu_hover_item = b.item_id;
            platform_invalidate(_widgets[frame_index].native_handle);
          }
          return true;
        }
      }
      return true;  // over the band but not on a label: keep capture, no change
    }

    std::vector<MenuColL> cols;
    mb_build_columns(this, ctx, mb, fw, fh, band, _menu_path, cols);
    for (size_t level = 0; level < cols.size(); ++level) {
      const MenuColL& col = cols[level];
      if (lx < (float)col.x || lx >= (float)(col.x + col.w) ||
          ly < (float)col.y || ly >= (float)(col.y + col.h))
        continue;
      for (auto& r : col.rows) {
        float ry = (float)(col.y + r.y);
        if (ly < ry || ly >= ry + (float)r.h) continue;
        if (r.separator) return true;
        std::vector<uint32_t> newpath(_menu_path.begin(),
                                      _menu_path.begin() + static_cast<std::ptrdiff_t>(level + 1));
        if (r.submenu && r.enabled) newpath.push_back(r.item_id);
        bool changed = (newpath != _menu_path) || (_menu_hover_item != r.item_id);
        _menu_path       = std::move(newpath);
        _menu_hover_item = r.item_id;
        if (changed) platform_invalidate(_widgets[frame_index].native_handle);
        return true;
      }
      return true;
    }
    return true;  // inside an open menu region but over no row: keep capture
  }

  bool Session::handle_menubar_band_hover(uint32_t frame_index, float lx, float ly)
  {
    MenubarWidget* mbp = frame_menubar(frame_index);
    if (!mbp) {
      if (_menu_band_hover != 0) {
        _menu_band_hover = 0;
        platform_invalidate(_widgets[frame_index].native_handle);
      }
      return false;
    }
    // Below the band: clear any highlight and let normal widget hover proceed.
    if (ly < 0.0f || ly >= (float)MENUBAR_BAND_H) {
      if (_menu_band_hover != 0) {
        _menu_band_hover = 0;
        platform_invalidate(_widgets[frame_index].native_handle);
      }
      return false;
    }
    // Over the band: highlight the label under the cursor (0 = gap between
    // labels). The band reserves the top inset, so no widgets live here.
    std::vector<MenuBandItem> band;
    mb_build_band(this, _widgets[frame_index].render_ctx, *mbp, band);
    uint32_t hov = 0;
    for (auto& b : band)
      if (lx >= (float)b.x && lx < (float)(b.x + b.w)) { hov = b.item_id; break; }
    if (hov != _menu_band_hover) {
      _menu_band_hover = hov;
      platform_invalidate(_widgets[frame_index].native_handle);
    }
    return true;
  }

  bool Session::handle_menubar_key(uint32_t keycode, uint32_t /*modifiers*/)
  {
    if (!_menu_open) return false;
    MenubarWidget* mbp = _widgets.exists(_menu_bar)
                       ? dynamic_cast<MenubarWidget*>(&_widgets[_menu_bar]) : nullptr;
    if (!mbp) { close_menubar_menu(); return true; }
    const MenubarWidget& mb = *mbp;

    // Resolve the owning frame for invalidation.
    uint32_t frame = 0;
    {
      auto parents = _widgets.get_all_parents(_menu_bar);
      for (uint32_t p : parents)
        if (p != 0 && _widgets.exists(p) && _widgets[p].native_handle) { frame = p; break; }
    }
    auto invalidate = [&]() {
      if (frame && _widgets.exists(frame) && _widgets[frame].native_handle)
        platform_invalidate(_widgets[frame].native_handle);
    };

    if (keycode == NEUI_KEY_ESCAPE) {
      if (_menu_path.size() > 1) { _menu_path.pop_back(); _menu_hover_item =
        _menu_path.empty() ? 0 : _menu_path.back(); invalidate(); }
      else close_menubar_menu();
      return true;
    }

    std::vector<uint32_t> tops; menu_children(mb, 0, tops);

    // Selectable (non-separator) children of the deepest open column.
    uint32_t cur_parent = _menu_path.empty() ? 0 : _menu_path.back();
    std::vector<uint32_t> rows;
    {
      std::vector<uint32_t> kids; menu_children(mb, cur_parent, kids);
      for (uint32_t k : kids) {
        auto it = mb.menu_items.find(k);
        if (it != mb.menu_items.end() && !it->second.is_separator) rows.push_back(k);
      }
    }

    auto move_row = [&](int dir) {
      if (rows.empty()) return;
      int cur = -1;
      for (int i = 0; i < (int)rows.size(); ++i)
        if (rows[static_cast<size_t>(i)] == _menu_hover_item) { cur = i; break; }
      int n = (int)rows.size();
      int next = (cur < 0) ? (dir > 0 ? 0 : n - 1) : (((cur + dir) % n) + n) % n;
      // skip disabled
      for (int guard = 0; guard < n; ++guard) {
        uint32_t cand = rows[static_cast<size_t>(next)];
        bool ok = menu_item_has_children(mb, cand) || menu_item_enabled(mb, cand);
        if (ok) break;
        next = ((next + dir) % n + n) % n;
      }
      _menu_hover_item = rows[static_cast<size_t>(next)];
      invalidate();
    };

    if (keycode == NEUI_KEY_DOWN) { move_row(+1); return true; }
    if (keycode == NEUI_KEY_UP)   { move_row(-1); return true; }

    if (keycode == NEUI_KEY_LEFT) {
      if (_menu_path.size() > 1) {
        _menu_hover_item = _menu_path.back();
        _menu_path.pop_back();
        invalidate();
      } else {
        // move to previous top-level menu
        int cur = -1;
        for (int i = 0; i < (int)tops.size(); ++i)
          if (!_menu_path.empty() && tops[static_cast<size_t>(i)] == _menu_path[0]) { cur = i; break; }
        if (!tops.empty()) {
          int prev = (cur <= 0) ? (int)tops.size() - 1 : cur - 1;
          _menu_path = { tops[static_cast<size_t>(prev)] }; _menu_hover_item = tops[static_cast<size_t>(prev)]; invalidate();
        }
      }
      return true;
    }

    if (keycode == NEUI_KEY_RIGHT) {
      // If the hovered row is a submenu, descend; else move to the next top-level.
      if (_menu_hover_item != 0 && menu_item_has_children(mb, _menu_hover_item) &&
          (_menu_path.empty() || _menu_path.back() != _menu_hover_item)) {
        _menu_path.push_back(_menu_hover_item);
        invalidate();
      } else {
        int cur = -1;
        for (int i = 0; i < (int)tops.size(); ++i)
          if (!_menu_path.empty() && tops[static_cast<size_t>(i)] == _menu_path[0]) { cur = i; break; }
        if (!tops.empty()) {
          int nxt = (cur < 0) ? 0 : (cur + 1) % (int)tops.size();
          _menu_path = { tops[static_cast<size_t>(nxt)] }; _menu_hover_item = tops[static_cast<size_t>(nxt)]; invalidate();
        }
      }
      return true;
    }

    if (keycode == NEUI_KEY_RETURN) {
      if (_menu_hover_item != 0) {
        if (menu_item_has_children(mb, _menu_hover_item)) {
          if (_menu_path.empty() || _menu_path.back() != _menu_hover_item) {
            _menu_path.push_back(_menu_hover_item);
            invalidate();
          }
        } else if (menu_item_enabled(mb, _menu_hover_item)) {
          uint32_t cmd_id = mb.menu_items.at(_menu_hover_item).cmd_id;
          close_menubar_menu();
          dispatch_menu_event(cmd_id);
        }
      }
      return true;
    }

    return true;  // capture all keys while a menu is open
  }

  bool Session::try_menubar_accel(uint32_t keycode, uint32_t modifiers)
  {
    if (keycode == 0 || keycode == NEUI_KEY_NONE) return false;
    const uint32_t mask = NEUI_KMOD_CTRL | NEUI_KMOD_SHIFT |
                          NEUI_KMOD_ALT  | NEUI_KMOD_META;
    uint32_t mods = modifiers & mask;
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto* mbp = dynamic_cast<MenubarWidget*>(&_widgets[mb_idx]);
      if (!mbp) continue;
      for (uint32_t id : mbp->menu_item_ids_ordered) {
        auto it = mbp->menu_items.find(id);
        if (it == mbp->menu_items.end()) continue;
        const auto& mi = it->second;
        if (mi.is_separator || mi.shortcut_key == NEUI_KEY_NONE) continue;
        // A disabled item's accelerator must not fire (matches the menu-item
        // gray-out and the native try_menubar_accel_ios `enabled` guard). On iOS
        // the system menu bar now routes shortcut picks through here, so a
        // disabled UIKeyCommand element would otherwise still dispatch.
        if (!mi.enabled) continue;
        if (mi.shortcut_key == keycode && (mi.shortcut_mods & mask) == mods) {
          dispatch_menu_event(mi.cmd_id);
          return true;  // accelerator fired (consumed regardless of handler)
        }
      }
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Toast overlay

  // Toast geometry / animation constants (logical px / ms).
  static constexpr int      TOAST_PAD_X    = 18;
  static constexpr int      TOAST_PAD_Y    = 12;
  static constexpr int      TOAST_LINE_GAP = 4;
  static constexpr float    TOAST_FONT_PX  = 14.0f;
  // Width clamp: never wider than this fraction of the frame's client area.
  static constexpr float    TOAST_MAX_FRAME_FRAC = 0.7f;

  // Slice a UTF-8 string on '\n' into display lines. Empty input becomes a
  // single empty line so callers can still compute geometry.
  static void toast_split_lines(const std::string& s,
                                 std::vector<std::string>& out)
  {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
      if (i == s.size() || s[i] == '\n') {
        out.emplace_back(s.data() + start, i - start);
        start = i + 1;
      }
    }
    if (out.empty()) out.emplace_back();
  }

  void Session::toast_show(uint32_t parent_window_idx, const char* text)
  {
    if (!_widgets.exists(parent_window_idx)) return;
    auto* fw = dynamic_cast<FrameWidget*>(&_widgets[parent_window_idx]);
    if (!fw || !fw->native_handle) return;

    ToastState& ts = fw->toast;
    ts.text   = text ? text : "";
    ts.active = true;
    ts.start_ms = platform_now_ms();

    // A toast is host-drawn chrome that is not a widget and not in the a11y
    // tree, so a screen reader would otherwise never know it appeared - the
    // window would flash a message that a blind user simply misses.
    // <neui/d/a11y.h> promises the framework announces its own toasts; this is
    // that. Not assertive: a toast is informational by definition, and
    // interrupting whatever the user is listening to would be worse than
    // waiting for a gap.
    if (!ts.text.empty())
      platform_a11y_announce(fw->native_handle, ts.text.c_str(), false);

    // Measure lines with the default text format. The xpl backend has no
    // measure-text-with-explicit-font seam beyond push_font, so we use
    // the default font that the toast paints with. Width = widest line +
    // 2*PAD_X (clamped to frame fraction). Height = lines*line_h +
    // (lines-1)*LINE_GAP + 2*PAD_Y. line_h derived from font size so it
    // matches the rendered glyph height.
    std::vector<std::string> lines;
    toast_split_lines(ts.text, lines);

    float line_h = std::round(TOAST_FONT_PX * 1.35f);
    float max_w  = 0.0f;
    if (_backend && _backend->measure_text && fw->render_ctx) {
      for (auto& ln : lines) {
        float w = _backend->measure_text(fw->render_ctx, ln.c_str(),
                                          static_cast<int>(ln.size()),
                                          TOAST_FONT_PX);
        if (w > max_w) max_w = w;
      }
    } else {
      // Fallback estimate: 7 logical px per character.
      for (auto& ln : lines) {
        float w = static_cast<float>(ln.size()) * 7.0f;
        if (w > max_w) max_w = w;
      }
    }

    int max_box_w = static_cast<int>(static_cast<float>(fw->width) *
                                      TOAST_MAX_FRAME_FRAC);
    if (max_box_w < 80) max_box_w = 80;
    int w_px = static_cast<int>(std::ceil(max_w)) + 2 * TOAST_PAD_X;
    if (w_px > max_box_w) w_px = max_box_w;
    if (w_px < 80) w_px = 80;

    int n      = static_cast<int>(lines.size());
    int h_px   = static_cast<int>(line_h) * n
               + TOAST_LINE_GAP * (n - 1)
               + 2 * TOAST_PAD_Y;

    ts.width   = w_px;
    ts.height  = h_px;
    ts.line_h  = static_cast<int>(line_h);
    ts.fade_in_ms  = 200;
    ts.hold_ms     = 2000;
    ts.fade_out_ms = 400;

    platform_start_toast_animation(fw->native_handle);
    platform_invalidate(fw->native_handle);
  }

  // Compute the toast rect (in frame-local logical px) for a frame's
  // current ToastState. Returns false if no toast is active.
  static bool toast_current_rect(FrameWidget* fw, int base_y,
                                  int& out_x, int& out_y,
                                  int& out_w, int& out_h)
  {
    if (!fw || !fw->toast.active) return false;
    const ToastState& ts = fw->toast;
    uint64_t now     = platform_now_ms();
    uint64_t elapsed = now >= ts.start_ms ? (now - ts.start_ms) : 0;
    uint32_t total   = ts.fade_in_ms + ts.hold_ms + ts.fade_out_ms;
    if (elapsed >= total) return false;

    float slide_t = 1.0f;
    if (elapsed < ts.fade_in_ms) {
      float p  = static_cast<float>(elapsed) / static_cast<float>(ts.fade_in_ms);
      float ip = 1.0f - p;
      slide_t  = 1.0f - ip * ip * ip;
    } else if (elapsed >= ts.fade_in_ms + ts.hold_ms) {
      uint64_t oe = elapsed - ts.fade_in_ms - ts.hold_ms;
      float p     = static_cast<float>(oe) / static_cast<float>(ts.fade_out_ms);
      float e     = p * p * p;
      slide_t     = 1.0f - e;
    }
    int rest_y  = base_y + ts.line_h;
    int start_y = base_y - ts.height;
    out_y = static_cast<int>(static_cast<float>(start_y) +
            (static_cast<float>(rest_y) - static_cast<float>(start_y)) *
            slide_t);
    out_x = (fw->width - ts.width) / 2;
    if (out_x < 0) out_x = 0;
    out_w = ts.width;
    out_h = ts.height;
    return true;
  }

  bool Session::handle_toast_click(uint32_t frame_widget_idx,
                                     float lx, float ly)
  {
    auto* fw = dynamic_cast<FrameWidget*>(get_widget(frame_widget_idx));
    if (!fw || !fw->toast.active) return false;
    int rx, ry, rw, rh;
    if (!toast_current_rect(fw, frame_top_inset(frame_widget_idx), rx, ry, rw, rh))
      return false;
    if (lx < static_cast<float>(rx) || lx >= static_cast<float>(rx + rw) ||
        ly < static_cast<float>(ry) || ly >= static_cast<float>(ry + rh))
      return false;
    // Jump to the start of the fade-out phase so the click produces an
    // immediate visual dismiss. We do this by reprojecting start_ms so
    // `elapsed == fade_in_ms + hold_ms` right now.
    ToastState& ts = fw->toast;
    uint64_t now = platform_now_ms();
    uint64_t hold_end_offset = ts.fade_in_ms + ts.hold_ms;
    ts.start_ms = (now >= hold_end_offset) ? (now - hold_end_offset) : 0;
    if (fw->native_handle) platform_invalidate(fw->native_handle);
    return true;
  }

  void Session::paint_toast(neui_render_ctx_t ctx, uint32_t frame_widget_idx)
  {
    if (!_backend || !ctx) return;
    auto* fw = dynamic_cast<FrameWidget*>(get_widget(frame_widget_idx));
    if (!fw || !fw->toast.active) return;
    ToastState& ts = fw->toast;

    uint64_t now    = platform_now_ms();
    uint64_t elapsed = now >= ts.start_ms ? (now - ts.start_ms) : 0;
    uint32_t total  = ts.fade_in_ms + ts.hold_ms + ts.fade_out_ms;
    if (elapsed >= total) {
      ts.active = false;
      ts.text.clear();
      if (fw->native_handle) platform_stop_toast_animation(fw->native_handle);
      return;
    }

    // Phase math. progress is 0..1 across the current phase. eased uses a
    // cubic ease-out for the fly-in / fly-out so motion settles smoothly.
    float alpha     = 1.0f;
    float slide_t   = 1.0f;  // 0 = fully off-screen above, 1 = at rest
    if (elapsed < ts.fade_in_ms) {
      float p = static_cast<float>(elapsed) / static_cast<float>(ts.fade_in_ms);
      // Ease-out cubic: 1 - (1-p)^3
      float ip = 1.0f - p;
      float e  = 1.0f - ip * ip * ip;
      alpha   = e;
      slide_t = e;
    } else if (elapsed < ts.fade_in_ms + ts.hold_ms) {
      alpha   = 1.0f;
      slide_t = 1.0f;
    } else {
      uint64_t out_elapsed = elapsed - ts.fade_in_ms - ts.hold_ms;
      float p = static_cast<float>(out_elapsed) /
                static_cast<float>(ts.fade_out_ms);
      // Ease-in cubic for out: p^3 - reverse the alpha + slide.
      float e = p * p * p;
      alpha   = 1.0f - e;
      slide_t = 1.0f - e;
    }

    // Anchor to the client area (below an in-frame menubar band, if any), so
    // the toast slides down from the content top rather than over the band.
    // base_y is 0 on native-menu platforms / menubar-less frames.
    int cx, cy, cw, ch;
    widget_client_rect(frame_widget_idx, &cx, &cy, &cw, &ch);
    int base_y   = cy;

    // Resting position: top edge sits `line_h` below the client-area top
    // (one-line gap above the toast). Slide_t blends between fully hidden
    // above the client top and the resting position.
    int rest_y   = base_y + ts.line_h;         // top edge gap = one line
    int start_y  = base_y - ts.height;         // fully out of view above
    int top_y    = static_cast<int>(static_cast<float>(start_y) +
                    (static_cast<float>(rest_y) - static_cast<float>(start_y)) *
                    slide_t);
    int left_x   = (fw->width - ts.width) / 2;
    if (left_x < 0) left_x = 0;

    if (!_backend->push_alpha || !_backend->pop_alpha) {
      // Alpha stack is required for the cross-fade; without it skip painting
      // rather than dump a fully opaque toast on top of widgets.
      return;
    }
    // Clip to the client area so the fly-in never paints over the menubar band
    // (the toast is painted after the band). No-op-equivalent full-window clip
    // when there's no band.
    bool clipped = _backend->push_clip != nullptr && _backend->pop_clip != nullptr;
    if (clipped)
      _backend->push_clip(ctx, static_cast<float>(cx), static_cast<float>(cy),
                          static_cast<float>(cw), static_cast<float>(ch));
    _backend->push_alpha(ctx, alpha);

    using neui_detail::ColorRole;
    // Drop shadow: simple offset rect underneath the toast for depth.
    uint32_t shadow_argb = 0x60000000;  // semi-transparent black
    _backend->fill_rect(ctx,
                        static_cast<float>(left_x + 2),
                        static_cast<float>(top_y + 3),
                        static_cast<float>(ts.width),
                        static_cast<float>(ts.height),
                        shadow_argb);

    uint32_t bg     = neui_detail::color(ColorRole::control_bg_alt);
    uint32_t border = neui_detail::color(ColorRole::border);
    _backend->fill_rect(ctx,
                        static_cast<float>(left_x),
                        static_cast<float>(top_y),
                        static_cast<float>(ts.width),
                        static_cast<float>(ts.height),
                        bg);
    _backend->draw_rect(ctx,
                        static_cast<float>(left_x),
                        static_cast<float>(top_y),
                        static_cast<float>(ts.width),
                        static_cast<float>(ts.height),
                        1.0f,
                        border);

    // Text. One draw_text per line; cell rect is the line strip with
    // PAD_X horizontal inset. The backend's draw_text vertically centres
    // text within the cell, so each cell is exactly line_h tall.
    std::vector<std::string> lines;
    toast_split_lines(ts.text, lines);
    uint32_t fg = neui_detail::color(ColorRole::text_primary);
    int run_y = top_y + TOAST_PAD_Y;
    for (auto& ln : lines) {
      _backend->draw_text(ctx,
                          static_cast<float>(left_x + TOAST_PAD_X),
                          static_cast<float>(run_y),
                          static_cast<float>(ts.width - 2 * TOAST_PAD_X),
                          static_cast<float>(ts.line_h),
                          ln.c_str(),
                          TOAST_FONT_PX,
                          fg);
      run_y += ts.line_h + TOAST_LINE_GAP;
    }

    _backend->pop_alpha(ctx);
    if (clipped) _backend->pop_clip(ctx);
  }

  void Session::open_combo(uint32_t idx)
  {
    if (!_widgets.exists(idx)) return;
    auto* cb = dynamic_cast<ComboBoxWidget*>(&_widgets[idx]);
    if (cb) {
      // Initialise the hover highlight at the currently-selected item so the
      // overlay's first frame already shows the previous selection.
      cb->hover_item = cb->selected_item;
      // Adjust scroll so the selected item is visible.
      if (cb->selected_item != UINT32_MAX && cb->selected_item < cb->items.size()) {
        uint32_t sel   = cb->selected_item;
        uint32_t max_v = static_cast<uint32_t>(cb->max_drop_visible());
        if (max_v > 0) {
          if (sel < cb->scroll_offset)
            cb->scroll_offset = sel;
          else if (sel >= cb->scroll_offset + max_v)
            cb->scroll_offset = sel - max_v + 1;
        }
      }
    }
    _open_combo = idx;
    void* frame = find_parent_native_handle(idx);
    if (frame) platform_invalidate(frame);
  }

  void Session::close_combo()
  {
    uint32_t idx = _open_combo;
    _open_combo = 0;
    if (idx != 0 && _widgets.exists(idx)) {
      auto* cb = dynamic_cast<ComboBoxWidget*>(&_widgets[idx]);
      if (cb) cb->hover_item = UINT32_MAX;   // clear hover when overlay closes
      void* frame = find_parent_native_handle(idx);
      if (frame) platform_invalidate(frame);
    }
  }

  bool Session::handle_combo_click(float lx, float ly)
  {
    if (_open_combo == 0 || !_widgets.exists(_open_combo)) return false;
    auto* cb = dynamic_cast<ComboBoxWidget*>(&_widgets[_open_combo]);
    if (!cb) { _open_combo = 0; return false; }

    uint32_t n       = static_cast<uint32_t>(cb->items.size());
    int      full_vis = std::max(1, cb->max_drop_visible());
    ComboBoxWidget::OverlayRect g = cb->overlay_rect(_backend);
    float ox = g.x, oy = g.y, ow = g.w, oh = g.h;

    bool in_overlay = (lx >= ox && lx < ox + ow && ly >= oy && ly < oy + oh);

    if (in_overlay && n > 0) {
      // Check scrollbar column first.
      if (lx >= ox + ow - static_cast<float>(SCROLLBAR_W) &&
          n > static_cast<uint32_t>(full_vis)) {
        SbGeom   sb    = compute_sb(full_vis * LIST_ITEM_H(), full_vis, n, cb->scroll_offset);
        uint32_t range = n - static_cast<uint32_t>(full_vis);
        float    local_y = ly - oy - 1.0f;

        if (local_y >= sb.thumb_top && local_y < sb.thumb_top + sb.thumb_h) {
          // Thumb hit - start drag (platform will SetCapture after this returns).
          _combo_sb_dragging       = true;
          _combo_sb_drag_start_y   = static_cast<int>(ly);
          _combo_sb_drag_start_off = cb->scroll_offset;
        } else if (local_y < sb.thumb_top) {
          // Above thumb - page up.
          uint32_t step = static_cast<uint32_t>(full_vis);
          cb->scroll_offset = (cb->scroll_offset >= step) ? cb->scroll_offset - step : 0;
          platform_invalidate(find_parent_native_handle(_open_combo));
        } else {
          // Below thumb - page down.
          uint32_t step = static_cast<uint32_t>(full_vis);
          cb->scroll_offset = std::min(cb->scroll_offset + step, range);
          platform_invalidate(find_parent_native_handle(_open_combo));
        }
        return true;   // consumed; overlay stays open
      }

      // Item area click - select and close.
      int rel_y = static_cast<int>(ly - oy);
      uint32_t vis_scroll = cb->scroll_offset;
      if (vis_scroll >= n) vis_scroll = n - 1;
      uint32_t clicked = vis_scroll + static_cast<uint32_t>(rel_y / LIST_ITEM_H());
      if (clicked < n && clicked != cb->selected_item) {
        cb->selected_item = clicked;
        cb->text = cb->items[clicked].text;
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_ITEM_SELECTED;
        ev.data.item.widget = { cb->widget_id };
        ev.data.item.index  = clicked;
        dispatch_event(&ev);
      }
    }
    close_combo();
    return true;  // always consumed when combo is open
  }

  bool Session::handle_combo_wheel(float lx, float ly, int delta)
  {
    if (_open_combo == 0 || !_widgets.exists(_open_combo)) return false;
    auto* cb = dynamic_cast<ComboBoxWidget*>(&_widgets[_open_combo]);
    if (!cb) return false;

    int   full_vis = std::max(1, cb->max_drop_visible());
    ComboBoxWidget::OverlayRect g = cb->overlay_rect(_backend);
    float ox = g.x, oy = g.y, ow = g.w, oh = g.h;

    // Only consume if cursor is over the overlay rect.
    if (lx < ox || lx >= ox + ow || ly < oy || ly >= oy + oh) return false;

    uint32_t n = static_cast<uint32_t>(cb->items.size());
    if (n <= static_cast<uint32_t>(full_vis)) return true;  // nothing to scroll
    uint32_t range = n - static_cast<uint32_t>(full_vis);

    if (delta < 0)
      cb->scroll_offset = std::min(cb->scroll_offset + static_cast<uint32_t>(-delta), range);
    else if (delta > 0)
      cb->scroll_offset = (cb->scroll_offset >= static_cast<uint32_t>(delta))
                          ? cb->scroll_offset - static_cast<uint32_t>(delta) : 0;

    void* frame = find_parent_native_handle(_open_combo);
    if (frame) platform_invalidate(frame);
    return true;
  }

  bool Session::handle_combo_scroll_drag(float ly)
  {
    if (!_combo_sb_dragging) return false;
    if (_open_combo == 0 || !_widgets.exists(_open_combo)) {
      _combo_sb_dragging = false;
      return false;
    }
    auto* cb = dynamic_cast<ComboBoxWidget*>(&_widgets[_open_combo]);
    if (!cb) { _combo_sb_dragging = false; return false; }

    int      full_vis = std::max(1, cb->max_drop_visible());
    uint32_t n        = static_cast<uint32_t>(cb->items.size());
    SbGeom   sb       = compute_sb(full_vis * LIST_ITEM_H(), full_vis, n,
                                   _combo_sb_drag_start_off);
    float movable = sb.track_h - sb.thumb_h;
    if (movable > 0.0f) {
      uint32_t range  = n - static_cast<uint32_t>(full_vis);
      int delta_y     = static_cast<int>(ly) - _combo_sb_drag_start_y;
      int new_off     = static_cast<int>(_combo_sb_drag_start_off)
                      + static_cast<int>(static_cast<float>(delta_y) * static_cast<float>(range)
                                         / movable + 0.5f);
      if (new_off < 0) new_off = 0;
      if (static_cast<uint32_t>(new_off) > range) new_off = static_cast<int>(range);
      cb->scroll_offset = static_cast<uint32_t>(new_off);
    }
    void* frame = find_parent_native_handle(_open_combo);
    if (frame) platform_invalidate(frame);
    return true;
  }

  bool Session::handle_combo_hover(float lx, float ly)
  {
    if (_open_combo == 0 || !_widgets.exists(_open_combo)) return false;
    auto* cb = dynamic_cast<ComboBoxWidget*>(&_widgets[_open_combo]);
    if (!cb) return false;

    int   full_vis = std::max(1, cb->max_drop_visible());
    ComboBoxWidget::OverlayRect g = cb->overlay_rect(_backend);
    float ox = g.x, oy = g.y, ow = g.w, oh = g.h;

    bool in_overlay = (lx >= ox && lx < ox + ow && ly >= oy && ly < oy + oh);
    if (!in_overlay) return false;

    uint32_t n = static_cast<uint32_t>(cb->items.size());
    if (n == 0) return true;

    // Don't change selection while the cursor is over the scrollbar column.
    bool show_sb = n > static_cast<uint32_t>(full_vis);
    if (show_sb && lx >= ox + ow - static_cast<float>(SCROLLBAR_W))
      return true;

    int rel_y = static_cast<int>(ly - oy);
    if (rel_y < 0) return true;
    uint32_t row = cb->scroll_offset + static_cast<uint32_t>(rel_y / LIST_ITEM_H());
    if (row >= n) return true;

    if (cb->hover_item != row) {
      cb->hover_item = row;
      void* frame = find_parent_native_handle(_open_combo);
      if (frame) platform_invalidate(frame);
    }
    return true;
  }

  // -------------------------------------------------------------------------
  // MultilineWidget - text editor with explicit newlines and vertical scroll

  // Visual line height scales with painted_ui_scale() so the (scaled) default
  // font fits; identity on desktop. Accessor so paint + hit-test agree.
  static int ML_LINE_H()  { return neui_detail::scaled_painted_metric(18); }
  static constexpr int ML_PAD_X   = 4;
  static constexpr int ML_PAD_Y   = 2;
  static constexpr float ML_FONT_SIZE = 12.0f;

  // Byte offsets of each line's first character. Always includes 0; the list
  // ends with one entry past the last newline (or the empty line at the end).
  // Example: "ab\nc"  -> [0, 3]
  //          "ab\n"   -> [0, 3]
  //          ""       -> [0]
  static std::vector<int> ml_line_starts(const std::string& s)
  {
    std::vector<int> starts;
    starts.push_back(0);
    for (int i = 0; i < static_cast<int>(s.size()); ++i) {
      if (s[static_cast<size_t>(i)] == '\n') starts.push_back(i + 1);
    }
    return starts;
  }

  // Returns the 0-based line index containing byte offset `pos`.
  static int ml_line_from_pos(const std::vector<int>& starts, int pos)
  {
    // Binary search for the largest start <= pos.
    int lo = 0, hi = static_cast<int>(starts.size()) - 1;
    while (lo < hi) {
      int mid = (lo + hi + 1) >> 1;
      if (starts[static_cast<size_t>(mid)] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
  }

  // End byte offset of the visual line `line` (exclusive). `starts` may hold
  // hard-break starts (preceded by a consumed '\n') and, when word-wrap is on,
  // soft-break starts (no separator char). A hard break ends one byte before
  // the next start (the '\n'); a soft break ends exactly at the next start.
  // The last line ends at text.size(). Identical to the old logic when every
  // break is a '\n' (the no-wrap case), so non-wrap behaviour is unchanged.
  static int ml_line_end(const std::string& s, const std::vector<int>& starts,
                          int line)
  {
    if (line + 1 < static_cast<int>(starts.size())) {
      int ns = starts[static_cast<size_t>(line + 1)];
      return (ns > 0 && s[static_cast<size_t>(ns) - 1] == '\n') ? ns - 1 : ns;
    }
    return static_cast<int>(s.size());
  }

  static bool ml_readonly(const WidgetData& wd)
  {
    return wd.attrs && wd.attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
  }

  // How many full lines fit in the visible area.
  static int ml_visible_lines(int widget_h)
  {
    int avail = widget_h - 2 * ML_PAD_Y;
    int n = avail / ML_LINE_H();
    return n > 0 ? n : 1;
  }

  static void ml_scroll_to_cursor(MultilineWidget& ml,
                                   const std::vector<int>& starts)
  {
    int line = ml_line_from_pos(starts, ml.cursor_pos);
    int vis  = ml_visible_lines(ml.height);
    if (line < static_cast<int>(ml.scroll_offset))
      ml.scroll_offset = static_cast<uint32_t>(line);
    else if (line >= static_cast<int>(ml.scroll_offset) + vis)
      ml.scroll_offset = static_cast<uint32_t>(line - vis + 1);
  }

  // ---- MultilineWidget paint --------------------------------------------

  void MultilineWidget::paint(neui_render_backend_t* backend,
                               neui_render_ctx_t ctx, bool is_focused)
  {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);

    const std::vector<int>& starts = cached_line_starts();
    uint32_t n_lines = static_cast<uint32_t>(starts.size());
    int    vis     = ml_visible_lines(height);
    bool   show_sb = n_lines > static_cast<uint32_t>(vis);
    // When wrapping, the gutter is always reserved (the wrap layout was
    // computed against that narrower width via ml_wrap_avail_w), so content_w
    // here must match even when the scrollbar isn't painted - otherwise the
    // wrapped rows and the paint width would disagree.
    float  content_w = (show_sb || wrap_enabled())
                         ? fw - static_cast<float>(SCROLLBAR_W) : fw;

    uint32_t scroll_range = (n_lines > static_cast<uint32_t>(vis))
                              ? n_lines - static_cast<uint32_t>(vis) : 0;
    if (scroll_offset > scroll_range) scroll_offset = scroll_range;

    // Background.
    using neui_detail::ColorRole;
    backend->fill_rect(ctx, fx, fy, content_w, fh,
                        neui_detail::color(ColorRole::control_bg));

    auto ef = neui_detail::read_widget_font(attrs.get(), ML_FONT_SIZE);
    neui_detail::push_widget_font(backend, ctx, ef);

    // Selection range (always normalized lo..hi).
    int sel_lo = neui_detail::te_sel_lo(cursor_pos, sel_anchor);
    int sel_hi = neui_detail::te_sel_hi(cursor_pos, sel_anchor);
    bool has_sel = neui_detail::te_has_selection(cursor_pos, sel_anchor);

    int max_visible = (static_cast<int>(fh) - 2 * ML_PAD_Y + ML_LINE_H() - 1) / ML_LINE_H();
    if (max_visible < 1) max_visible = 1;

    const float base_x  = fx + static_cast<float>(ML_PAD_X);
    const float avail_w = content_w - static_cast<float>(2 * ML_PAD_X);

    // Without horizontal scrolling, glyphs past the right edge are clipped, so
    // there's no point shaping/drawing them - a long line otherwise costs
    // O(line length) per paint. Cap how many leading characters of each line
    // we touch, using a conservative lower bound on glyph advance (~0.12 em)
    // plus a margin so a visible glyph is never dropped. For normal-length
    // lines the cap exceeds the line, so nothing changes.
    const int vis_char_cap =
        static_cast<int>(content_w / std::max(1.0f, ef.size * 0.12f)) + 16;
    auto vis_end_of = [&](int ls, int le) -> int {
      int p = ls, c = 0;
      while (p < le && c < vis_char_cap) {
        p += neui_detail::te_utf8_char_len(text, p);
        ++c;
      }
      return p;
    };

    if (backend->push_clip) backend->push_clip(ctx, fx, fy, content_w, fh);

    for (int i = 0; i < max_visible; ++i) {
      int line = static_cast<int>(scroll_offset) + i;
      if (line >= static_cast<int>(n_lines)) break;
      int ls = starts[static_cast<size_t>(line)];
      int le = ml_line_end(text, starts, line);
      int ve = vis_end_of(ls, le);   // last char we shape/draw on this line

      float row_y   = fy + static_cast<float>(ML_PAD_Y + i * ML_LINE_H());
      float row_h   = static_cast<float>(ML_LINE_H());

      // Selection highlight for this line. Measure only within the visible
      // span; if the selection runs off the right edge fill to that edge
      // (the rect is clipped to content_w anyway).
      if (has_sel && sel_lo <= le && sel_hi >= ls && backend->measure_text) {
        int lo = std::min(std::max(sel_lo, ls), ve);
        int hi = std::min(sel_hi, le);
        float x0 = base_x + backend->measure_text(ctx, text.c_str() + ls,
                                                   lo - ls, ef.size);
        float x1;
        if (hi > ve) {
          x1 = base_x + avail_w;   // selection end is off-screen
        } else {
          x1 = base_x + backend->measure_text(ctx, text.c_str() + ls,
                                               hi - ls, ef.size);
          // Selection extending past the end-of-line newline: small trailing
          // strip so the newline itself reads as selected.
          if (sel_hi > le) x1 += 6.0f;
        }
        if (x1 > x0)
          backend->fill_rect(ctx, x0, row_y, x1 - x0, row_h,
                              neui_detail::color(ColorRole::accent_translucent));
      }

      // Line text (capped to the visible span; the remainder is clipped).
      if (ve > ls && backend->draw_text) {
        _paint_scratch.assign(text, static_cast<size_t>(ls),
                              static_cast<size_t>(ve - ls));
        backend->draw_text(ctx, base_x, row_y, avail_w, row_h,
                           _paint_scratch.c_str(), ef.size,
                           neui_detail::color(ColorRole::text_primary));
      }
    }

    // Caret + IME composition overlay.
    if (is_focused && backend->measure_text) {
      int line = ml_line_from_pos(starts, cursor_pos);
      int cl_le = (line < static_cast<int>(n_lines))
                    ? ml_line_end(text, starts, line) : cursor_pos;
      if (line >= static_cast<int>(scroll_offset) &&
          line <  static_cast<int>(scroll_offset) + vis &&
          cursor_pos <= vis_end_of(starts[static_cast<size_t>(line)], cl_le)) {
        int   ls   = starts[static_cast<size_t>(line)];
        float col  = backend->measure_text(ctx, text.c_str() + ls,
                                            cursor_pos - ls, ef.size);
        float cx   = fx + static_cast<float>(ML_PAD_X) + col;
        float cy   = fy + static_cast<float>(ML_PAD_Y
                     + (line - static_cast<int>(scroll_offset)) * ML_LINE_H());
        if (composing && !composition_text.empty() && backend->draw_text) {
          // Draw composition string at the caret position with per-clause
          // underlines. Suppress the regular caret while composing - the
          // IME's candidate window provides the visible caret position.
          float comp_w = backend->measure_text(ctx, composition_text.c_str(),
                                               static_cast<int>(composition_text.size()),
                                               ef.size);
          backend->draw_text(ctx, cx, cy, comp_w,
                             static_cast<float>(ML_LINE_H()),
                             composition_text.c_str(),
                             ef.size,
                             neui_detail::color(ColorRole::text_primary));
          paint_composition_underline(backend, ctx, cx,
                                       cy + static_cast<float>(ML_LINE_H()) - 2.0f,
                                       composition_text, composition_attrs, ef.size);
        } else {
          backend->fill_rect(ctx, cx, cy, 1.5f, static_cast<float>(ML_LINE_H()),
                             neui_detail::color(ColorRole::text_primary));
        }
      }
    }

    if (backend->push_clip) backend->pop_clip(ctx);
    neui_detail::pop_widget_font(backend, ctx, ef);

    // Vertical scrollbar.
    if (show_sb) {
      float sx = fx + content_w;
      backend->fill_rect(ctx, sx, fy, 1.0f, fh,
                          neui_detail::color(ColorRole::scrollbar_separator));
      float tx = sx + 1.0f;
      float tw = static_cast<float>(SCROLLBAR_W) - 1.0f;
      backend->fill_rect(ctx, tx, fy, tw, fh,
                          neui_detail::color(ColorRole::scrollbar_track));
      SbGeom sb = compute_sb(height, vis, n_lines, scroll_offset);
      backend->fill_rect(ctx, tx + 1.0f, fy + 1.0f + sb.thumb_top,
                         tw - 2.0f, sb.thumb_h,
                         neui_detail::color(ColorRole::scrollbar_thumb));
    }

    // Border.
    uint32_t border_color = neui_detail::color(
        is_focused ? ColorRole::border_focused : ColorRole::border);
    backend->draw_rect(ctx, fx, fy, fw, fh, 1.0f, border_color);
  }

  // ---- MultilineWidget keyboard ----------------------------------------

  // Column (x pixels) of `pos` within its line. 0 if the line is empty or the
  // backend lacks measure_text.
  static float ml_col_px(MultilineWidget& ml,
                          const std::vector<int>& starts, int pos)
  {
    if (!ml.session || !ml.session->_backend ||
        !ml.session->_backend->measure_text)
      return 0.0f;
    int line = ml_line_from_pos(starts, pos);
    int ls   = starts[static_cast<size_t>(line)];
    // Any render context from an ancestor frame works.
    neui_render_ctx_t ctx = nullptr;
    for (uint32_t p : ml.session->_widgets.get_all_parents(ml.index)) {
      if (p == 0) continue;
      if (ml.session->_widgets.exists(p)) {
        ctx = ml.session->_widgets[p].render_ctx;
        if (ctx) break;
      }
    }
    if (!ctx) return 0.0f;
    auto ef = neui_detail::read_widget_font(ml.attrs.get(), ML_FONT_SIZE);
    neui_detail::push_widget_font(ml.session->_backend, ctx, ef);
    float w = ml.session->_backend->measure_text(ctx, ml.text.c_str() + ls,
                                                  pos - ls, ef.size);
    neui_detail::pop_widget_font(ml.session->_backend, ctx, ef);
    return w;
  }

  // Snap col_px (pixels from line start) to the closest character boundary
  // on the given line using midpoint snapping.
  static int ml_pos_from_col(MultilineWidget& ml,
                              const std::vector<int>& starts,
                              int line, float col_px)
  {
    if (!ml.session || !ml.session->_backend ||
        !ml.session->_backend->measure_text)
      return starts[static_cast<size_t>(line)];
    neui_render_ctx_t ctx = nullptr;
    for (uint32_t p : ml.session->_widgets.get_all_parents(ml.index)) {
      if (p == 0) continue;
      if (ml.session->_widgets.exists(p)) {
        ctx = ml.session->_widgets[p].render_ctx;
        if (ctx) break;
      }
    }
    if (!ctx) return starts[static_cast<size_t>(line)];

    auto ef = neui_detail::read_widget_font(ml.attrs.get(), ML_FONT_SIZE);
    neui_detail::push_widget_font(ml.session->_backend, ctx, ef);
    int ls  = starts[static_cast<size_t>(line)];
    int le  = ml_line_end(ml.text, starts, line);

    // Midpoint-snap col_px to the nearest character boundary on [ls, le].
    // Boundary widths are monotonic in the boundary index, so binary-search
    // the boundaries instead of measuring every prefix linearly: O(log K)
    // measure_text calls (each creates a DirectWrite layout) instead of O(K),
    // and O(K log K) shaping instead of O(K^2). K = chars on the line. The
    // byte-offset walks (boundary()) are allocation-free and cheap relative
    // to the shaping the linear version did per character.
    int result = le;
    if (col_px <= 0.0f) {
      result = ls;
    } else {
      // Number of characters on the line.
      int K = 0;
      for (int p = ls; p < le; p += neui_detail::te_utf8_char_len(ml.text, p)) ++K;
      // Byte offset of the i-th character start (0 <= i <= K; boundary(K) == le).
      auto boundary = [&](int i) -> int {
        int p = ls;
        for (int k = 0; k < i; ++k) p += neui_detail::te_utf8_char_len(ml.text, p);
        return p;
      };
      auto width_at = [&](int i) -> float {
        if (i <= 0) return 0.0f;
        int b = boundary(i);
        return ml.session->_backend->measure_text(
          ctx, ml.text.c_str() + ls, b - ls, ef.size);
      };
      // Largest index lo with width_at(lo) <= col_px (lo == 0 always qualifies
      // since width_at(0) == 0 < col_px here).
      int lo = 0, hi = K;
      while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (width_at(mid) <= col_px) lo = mid; else hi = mid - 1;
      }
      if (lo >= K) {
        result = le;
      } else {
        // Snap to whichever of boundary(lo) / boundary(lo+1) is nearer the
        // midpoint - identical tie-breaking to the old linear walk.
        float wl = width_at(lo);
        float wh = width_at(lo + 1);
        result = (col_px < (wl + wh) * 0.5f) ? boundary(lo) : boundary(lo + 1);
      }
    }
    neui_detail::pop_widget_font(ml.session->_backend, ctx, ef);
    return result;
  }

  // ---- Word-wrap line model ---------------------------------------------

  // First render context found walking up from the widget (the frame's ctx).
  // Needed to measure text outside paint (wrap layout, hit-test). null before
  // the frame is shown.
  static neui_render_ctx_t ml_find_ctx(MultilineWidget& ml)
  {
    if (!ml.session) return nullptr;
    for (uint32_t p : ml.session->_widgets.get_all_parents(ml.index)) {
      if (p == 0) continue;
      if (ml.session->_widgets.exists(p)) {
        neui_render_ctx_t ctx = ml.session->_widgets[p].render_ctx;
        if (ctx) return ctx;
      }
    }
    return nullptr;
  }

  // Target content width (logical px) a wrapped line must fit within. The
  // scrollbar gutter is always reserved when wrapping so the layout doesn't
  // reflow when the vertical scrollbar appears/disappears (which would be a
  // circular dependency: line count -> scrollbar -> width -> line count).
  static float ml_wrap_avail_w(const MultilineWidget& ml)
  {
    float w = static_cast<float>(ml.width) - 2.0f * ML_PAD_X
            - static_cast<float>(SCROLLBAR_W);
    return w > 1.0f ? w : 1.0f;
  }

  // Largest character boundary in [vs, le] whose prefix width fits avail_w
  // (at least one char, so an over-wide glyph still makes progress). Uses an
  // exponential probe then binary search so each measure_text shapes at most
  // ~2x the fitting length - O(fit chars) shaping, not O(line length).
  static int ml_fit_chars_end(MultilineWidget& ml, neui_render_ctx_t ctx,
                              const neui_detail::EffectiveFont& ef,
                              int vs, int le, float avail_w)
  {
    const std::string& s = ml.text;
    auto* be = ml.session->_backend;
    auto char_end = [&](int count) -> int {
      int p = vs;
      for (int k = 0; k < count && p < le; ++k)
        p += neui_detail::te_utf8_char_len(s, p);
      return p;
    };
    auto width_n = [&](int count) -> float {
      int b = char_end(count);
      if (b <= vs) return 0.0f;
      return be->measure_text(ctx, s.c_str() + vs, b - vs, ef.size);
    };
    // Exponential search for a char count whose width overflows (or hits le).
    int hi = 1;
    while (char_end(hi) < le && width_n(hi) <= avail_w) hi *= 2;
    if (char_end(hi) >= le && width_n(hi) <= avail_w)
      return le;                       // whole remainder fits on this row
    int lo = hi / 2;                   // width_n(lo) <= avail_w (or lo == 0)
    while (lo < hi) {
      int mid = (lo + hi + 1) / 2;
      if (width_n(mid) <= avail_w) lo = mid; else hi = mid - 1;
    }
    if (lo < 1) lo = 1;                // force progress on an over-wide glyph
    return char_end(lo);
  }

  // Build visual-line start offsets with greedy word-wrap to avail_w. Hard
  // '\n' always starts a new row; within a logical line, each row takes as
  // many chars as fit, breaking after the last space when there is one (else
  // a hard char break for an over-long word). Falls back to logical lines
  // when there is no context to measure with.
  static std::vector<int> ml_build_wrapped_starts(MultilineWidget& ml,
      neui_render_ctx_t ctx, const neui_detail::EffectiveFont& ef, float avail_w)
  {
    const std::string& s = ml.text;
    int n = static_cast<int>(s.size());
    std::vector<int> starts;
    starts.push_back(0);
    if (!ctx || !ml.session || !ml.session->_backend ||
        !ml.session->_backend->measure_text || avail_w <= 1.0f) {
      for (int i = 0; i < n; ++i) if (s[static_cast<size_t>(i)] == '\n') starts.push_back(i + 1);
      return starts;
    }
    neui_detail::push_widget_font(ml.session->_backend, ctx, ef);
    int ls = 0;
    while (true) {
      int le = ls;
      while (le < n && s[static_cast<size_t>(le)] != '\n') ++le;
      int vs = ls;
      while (vs < le) {
        int b = ml_fit_chars_end(ml, ctx, ef, vs, le, avail_w);
        if (b >= le) break;                       // remainder fits this row
        int brk = b;
        for (int p = b; p > vs; --p) {            // prefer a word boundary
          if (s[static_cast<size_t>(p) - 1] == ' ') { brk = p; break; }
        }
        if (brk <= vs)
          brk = vs + neui_detail::te_utf8_char_len(s, vs);   // ensure progress
        if (brk >= le) break;
        starts.push_back(brk);
        vs = brk;
      }
      if (le >= n) break;
      ls = le + 1;
      starts.push_back(ls);                        // hard-break row start
    }
    neui_detail::pop_widget_font(ml.session->_backend, ctx, ef);
    return starts;
  }

  // Visual line starts, cached. No-wrap: logical lines (cheap O(N) scan).
  // Wrap: word-wrapped rows (measure-based, hence the cache key on width /
  // font / wrap). paint never mutates text mid-call, so the returned
  // reference stays valid for a paint.
  const std::vector<int>& MultilineWidget::cached_line_starts()
  {
    bool  wrap    = wrap_enabled();
    float avail_w = wrap ? ml_wrap_avail_w(*this) : 0.0f;
    auto  ef      = neui_detail::read_widget_font(attrs.get(), ML_FONT_SIZE);
    if (!_ls_dirty && wrap == _cache_wrap &&
        avail_w == _cache_w && ef.size == _cache_font)
      return _ls_cache;

    bool built_ok = true;
    if (!wrap) {
      _ls_cache = ml_line_starts(text);
    } else {
      neui_render_ctx_t ctx = ml_find_ctx(*this);
      if (!ctx) built_ok = false;     // pre-show: retry once a ctx exists
      _ls_cache = ml_build_wrapped_starts(*this, ctx, ef, avail_w);
    }
    _ls_dirty   = !built_ok;
    _cache_wrap = wrap;
    _cache_w    = avail_w;
    _cache_font = ef.size;
    return _ls_cache;
  }

  bool MultilineWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    using namespace neui_detail;
    const bool shift    = (modifiers & NEUI_KMOD_SHIFT) != 0;
    const bool ctrl     = (modifiers & NEUI_KMOD_CTRL)  != 0;
    const bool readonly = ml_readonly(*this);

    auto starts   = cached_line_starts();   // copy: stable across edits below
    int  text_len = (int)text.size();
    // scroll() runs after every mutating branch (BACK / DELETE / RETURN / cut /
    // paste / undo / redo); nav branches call ml_scroll_to_cursor directly. So
    // invalidating the visual-line cache here covers all in-place edits; the
    // following cached_line_starts() then rebuilds the post-edit layout.
    auto scroll   = [&]{ mark_lines_dirty(); ml_scroll_to_cursor(*this, cached_line_starts()); };

    switch (keycode) {
    case NEUI_KEY_LEFT:
      te_move_left (text, cursor_pos, sel_anchor, ctrl, shift, &history);
      ml_scroll_to_cursor(*this, starts); repaint(); return true;

    case NEUI_KEY_RIGHT:
      te_move_right(text, cursor_pos, sel_anchor, ctrl, shift, &history);
      ml_scroll_to_cursor(*this, starts); repaint(); return true;

    // Vertical nav is layout-driven, so it stays local. The column-tracking
    // up/down picks the nearest byte offset on the target visual line.
    case NEUI_KEY_UP: {
      history.reset_action();
      int line = ml_line_from_pos(starts, cursor_pos);
      if (line > 0) {
        float col = ml_col_px(*this, starts, cursor_pos);
        cursor_pos = ml_pos_from_col(*this, starts, line - 1, col);
      } else {
        cursor_pos = 0;
      }
      if (!shift) sel_anchor = cursor_pos;
      ml_scroll_to_cursor(*this, starts); repaint(); return true;
    }
    case NEUI_KEY_DOWN: {
      history.reset_action();
      int line = ml_line_from_pos(starts, cursor_pos);
      if (line + 1 < (int)starts.size()) {
        float col = ml_col_px(*this, starts, cursor_pos);
        cursor_pos = ml_pos_from_col(*this, starts, line + 1, col);
      } else {
        cursor_pos = text_len;
      }
      if (!shift) sel_anchor = cursor_pos;
      ml_scroll_to_cursor(*this, starts); repaint(); return true;
    }

    // Home / End: per-line by default; Ctrl jumps to whole-text start / end.
    case NEUI_KEY_HOME: {
      history.reset_action();
      if (ctrl) cursor_pos = 0;
      else      cursor_pos = starts[static_cast<size_t>(ml_line_from_pos(starts, cursor_pos))];
      if (!shift) sel_anchor = cursor_pos;
      ml_scroll_to_cursor(*this, starts); repaint(); return true;
    }
    case NEUI_KEY_END: {
      history.reset_action();
      if (ctrl) cursor_pos = text_len;
      else      cursor_pos = ml_line_end(text, starts, ml_line_from_pos(starts, cursor_pos));
      if (!shift) sel_anchor = cursor_pos;
      ml_scroll_to_cursor(*this, starts); repaint(); return true;
    }

    case NEUI_KEY_BACK:
      if (readonly) return true;
      te_backspace     (text, cursor_pos, sel_anchor, ctrl, &history);
      scroll(); repaint(); return true;

    case NEUI_KEY_DELETE:
      if (readonly) return true;
      te_delete_forward(text, cursor_pos, sel_anchor, ctrl, &history);
      scroll(); repaint(); return true;

    case NEUI_KEY_RETURN: {
      if (readonly) return true;
      // Newline is its own undo group - never coalesces with surrounding typing.
      bool has_sel = te_has_selection(cursor_pos, sel_anchor);
      history.mark(EditState{ text, cursor_pos, sel_anchor },
                   EditHistory::None, has_sel);
      te_erase_selection(text, cursor_pos, sel_anchor);
      text.insert((size_t)cursor_pos, 1, '\n');
      cursor_pos += 1;
      sel_anchor  = cursor_pos;
      scroll(); repaint(); return true;
    }

    case NEUI_KEY_A:
      if (ctrl) {
        te_select_all(text, cursor_pos, sel_anchor, &history);
        publish_primary_selection(text, cursor_pos, sel_anchor);
        repaint();
        return true;
      }
      break;

    case NEUI_KEY_C:
      if (ctrl) {
        std::string sel = te_selected_text(text, cursor_pos, sel_anchor);
        if (!sel.empty()) {
          platform_clipboard_set_text(sel.c_str(), (uint32_t)sel.size());
          platform_clipboard_set_primary(sel.c_str(), (uint32_t)sel.size());
        }
        return true;
      }
      break;

    case NEUI_KEY_X:
      if (ctrl) {
        std::string sel = te_selected_text(text, cursor_pos, sel_anchor);
        if (!sel.empty()) {
          platform_clipboard_set_text(sel.c_str(), (uint32_t)sel.size());
          if (!readonly) {
            history.mark(EditState{ text, cursor_pos, sel_anchor },
                         EditHistory::None, true);
            te_erase_selection(text, cursor_pos, sel_anchor);
            scroll(); repaint();
          }
        }
        return true;
      }
      break;

    case NEUI_KEY_V:
      if (ctrl) {
        if (readonly) return true;
        int n = platform_clipboard_get_text(nullptr, 0);
        if (n > 0) {
          std::vector<char> buf((size_t)n);
          platform_clipboard_get_text(buf.data(), n);
          // Multiline keeps newlines but strips CR (normalise CRLF -> LF).
          std::string paste;
          paste.reserve((size_t)n);
          for (int i = 0; i + 1 < n; ++i) {
            char c = buf[(size_t)i];
            if (c == '\r') continue;
            paste.push_back(c);
          }
          te_paste(text, cursor_pos, sel_anchor, paste,
                   /*strip_newlines=*/false, &history);
          scroll(); repaint();
        }
        return true;
      }
      break;

    case NEUI_KEY_Z:
      if (ctrl) {
        if (shift) te_redo(text, cursor_pos, sel_anchor, history);
        else       te_undo(text, cursor_pos, sel_anchor, history);
        scroll(); repaint();
        return true;
      }
      break;

    case NEUI_KEY_Y:
      if (ctrl) {
        te_redo(text, cursor_pos, sel_anchor, history);
        scroll(); repaint();
        return true;
      }
      break;

    default:
      break;
    }
    return false;
  }

  bool MultilineWidget::perform_command(uint32_t cmd)
  {
    constexpr uint32_t CTRL = 2;
    switch (cmd) {
    case NEUI_CMD_UNDO:       return on_keydown(NEUI_KEY_Z, CTRL);
    case NEUI_CMD_REDO:       return on_keydown(NEUI_KEY_Y, CTRL);
    case NEUI_CMD_CUT:        return on_keydown(NEUI_KEY_X, CTRL);
    case NEUI_CMD_COPY:       return on_keydown(NEUI_KEY_C, CTRL);
    case NEUI_CMD_PASTE:      return on_keydown(NEUI_KEY_V, CTRL);
    case NEUI_CMD_SELECT_ALL: return on_keydown(NEUI_KEY_A, CTRL);
    case NEUI_CMD_DELETE:     return on_keydown(NEUI_KEY_DELETE, 0);
    }
    return false;
  }

  bool MultilineWidget::can_perform_command(uint32_t cmd) const
  {
    return text_widget_handles_cmd(cmd);
  }

  bool MultilineWidget::on_keychar(uint32_t codepoint, uint32_t /*modifiers*/)
  {
    using namespace neui_detail;
    if (ml_readonly(*this)) return false;
    if (codepoint < 0x20 || codepoint == 0x7F) return false;
    char buf[4];
    int  n = te_encode_utf8(codepoint, buf);
    te_insert_utf8(text, cursor_pos, sel_anchor, /*overwrite=*/false,
                   buf, n, &history);
    mark_lines_dirty();
    ml_scroll_to_cursor(*this, cached_line_starts());
    repaint();
    return true;
  }

  // Caret rect in widget-local logical pixels - mirrors the math in paint().
  // Returns false if the caret line is scrolled out of view.
  bool MultilineWidget::caret_rect_local(neui_render_backend_t* backend,
                                          neui_render_ctx_t ctx,
                                          float* out_x, float* out_y, float* out_h)
  {
    if (!backend || !backend->measure_text || !ctx) return false;
    auto starts = cached_line_starts();
    int  line   = ml_line_from_pos(starts, cursor_pos);
    int  vis    = ml_visible_lines(height);
    if (line < static_cast<int>(scroll_offset) ||
        line >= static_cast<int>(scroll_offset) + vis)
      return false;
    int   ls  = starts[static_cast<size_t>(line)];
    auto ef = neui_detail::read_widget_font(attrs.get(), ML_FONT_SIZE);
    neui_detail::push_widget_font(backend, ctx, ef);
    float col = backend->measure_text(ctx, text.c_str() + ls,
                                       cursor_pos - ls, ef.size);
    if (composing && !composition_text.empty()) {
      col += backend->measure_text(ctx, composition_text.c_str(),
                                    composition_caret, ef.size);
    }
    neui_detail::pop_widget_font(backend, ctx, ef);
    if (out_x) *out_x = static_cast<float>(ML_PAD_X) + col;
    if (out_y) *out_y = static_cast<float>(ML_PAD_Y
                          + (line - static_cast<int>(scroll_offset)) * ML_LINE_H());
    if (out_h) *out_h = static_cast<float>(ML_LINE_H());
    return true;
  }

  // IME composition state machine. Mirrors InputBoxWidget; the differences are
  // that committing must also re-flow scroll-to-cursor and that read-only
  // multilines refuse composition entirely.
  bool MultilineWidget::on_composition(int kind, const char* utf8,
                                        int byte_len, int caret_byte,
                                        const uint8_t* per_byte_attrs)
  {
    if (ml_readonly(*this)) return false;

    switch (kind) {
    case COMP_START:
      composition_pre_state = neui_detail::EditState{ text, cursor_pos, sel_anchor };
      composing             = true;
      composition_text.clear();
      composition_caret     = 0;
      composition_attrs.clear();
      repaint();
      return true;

    case COMP_UPDATE:
      composition_text.assign(utf8 ? utf8 : "", static_cast<size_t>(byte_len > 0 ? byte_len : 0));
      composition_caret = caret_byte;
      if (composition_caret < 0) composition_caret = 0;
      if (composition_caret > static_cast<int>(composition_text.size()))
        composition_caret = static_cast<int>(composition_text.size());
      if (per_byte_attrs && byte_len > 0) {
        composition_attrs.assign(per_byte_attrs, per_byte_attrs + byte_len);
      } else {
        composition_attrs.clear();
      }
      repaint();
      return true;

    case COMP_RESULT: {
      bool has_sel = neui_detail::te_has_selection(cursor_pos, sel_anchor);
      history.mark(composition_pre_state,
                   neui_detail::EditHistory::Typing, has_sel);
      neui_detail::te_erase_selection(text, cursor_pos, sel_anchor);
      if (utf8 && byte_len > 0) {
        text.insert(static_cast<size_t>(cursor_pos), utf8, static_cast<size_t>(byte_len));
        cursor_pos += byte_len;
        sel_anchor  = cursor_pos;
      }
      composition_pre_state = neui_detail::EditState{ text, cursor_pos, sel_anchor };
      composition_text.clear();
      composition_caret = 0;
      composition_attrs.clear();

      mark_lines_dirty();
      auto starts = cached_line_starts();
      ml_scroll_to_cursor(*this, starts);
      repaint();
      return true;
    }

    case COMP_END:
      composing = false;
      composition_text.clear();
      composition_caret = 0;
      composition_attrs.clear();
      repaint();
      return true;
    }
    return false;
  }

  // ---- MultilineWidget mouse --------------------------------------------

  bool MultilineWidget::insert_text(const std::string& utf8)
  {
    bool readonly = attrs && attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
    if (readonly || utf8.empty()) return false;
    // Keep newlines (multiline), normalise CRLF -> LF.
    std::string ins;
    ins.reserve(utf8.size());
    for (char c : utf8) if (c != '\r') ins.push_back(c);
    neui_detail::te_paste(text, cursor_pos, sel_anchor, ins,
                          /*strip_newlines=*/false, &history);
    mark_lines_dirty();
    ml_scroll_to_cursor(*this, cached_line_starts());
    repaint();
    return true;
  }

  bool MultilineWidget::on_mouse_event(neui_event_t* event)
  {
    // Drag-select ends on button-up (body, not scrollbar): publish to PRIMARY.
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP && !sb_dragging) {
      publish_primary_selection(text, cursor_pos, sel_anchor);
      return false;
    }
    auto starts = cached_line_starts();
    uint32_t n_lines = static_cast<uint32_t>(starts.size());
    int vis = ml_visible_lines(height);
    bool show_sb = n_lines > static_cast<uint32_t>(vis);

    // ---- wheel ----
    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      if (!show_sb) return false;
      int delta = event->data.wheel.delta;
      uint32_t range = n_lines - static_cast<uint32_t>(vis);
      if (delta < 0) {
        uint32_t step = static_cast<uint32_t>(-delta);
        scroll_offset = std::min(scroll_offset + step, range);
      } else if (delta > 0) {
        uint32_t step = static_cast<uint32_t>(delta);
        scroll_offset = (scroll_offset >= step) ? scroll_offset - step : 0;
      }
      repaint();
      return true;
    }

    // ---- scrollbar drag ----
    if (sb_dragging) {
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
           !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        sb_dragging = false;
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        SbGeom sb = compute_sb(height, vis, n_lines, sb_drag_start_offset);
        float movable = sb.track_h - sb.thumb_h;
        if (movable > 0.0f) {
          uint32_t range = n_lines - static_cast<uint32_t>(vis);
          int delta_y = event->data.mouse.y - sb_drag_start_y;
          int new_off = static_cast<int>(sb_drag_start_offset)
                      + static_cast<int>(static_cast<float>(delta_y) * static_cast<float>(range)
                                         / movable + 0.5f);
          if (new_off < 0) new_off = 0;
          if (static_cast<uint32_t>(new_off) > range) new_off = static_cast<int>(range);
          scroll_offset = static_cast<uint32_t>(new_off);
        }
        repaint();
        return true;
      }
      return false;
    }

    // ---- scrollbar click ----
    if (show_sb && event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      int sb_left = abs_x + width - SCROLLBAR_W;
      if (event->data.mouse.x >= sb_left) {
        SbGeom sb = compute_sb(height, vis, n_lines, scroll_offset);
        uint32_t range = n_lines - static_cast<uint32_t>(vis);
        float local_y = static_cast<float>(event->data.mouse.y - abs_y) - 1.0f;
        if (local_y >= sb.thumb_top && local_y < sb.thumb_top + sb.thumb_h) {
          sb_dragging = true;
          sb_drag_start_y      = event->data.mouse.y;
          sb_drag_start_offset = scroll_offset;
        } else if (local_y < sb.thumb_top) {
          uint32_t step = static_cast<uint32_t>(vis);
          scroll_offset = (scroll_offset >= step) ? scroll_offset - step : 0;
          repaint();
        } else {
          uint32_t step = static_cast<uint32_t>(vis);
          scroll_offset = std::min(scroll_offset + step, range);
          repaint();
        }
        return true;
      }
    }

    // ---- body click / drag: position cursor ----
    bool is_down = (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN);
    bool is_drag = (event->type == NEUI_EVENT_MOUSE_MOVE) &&
                   (event->data.mouse.buttonmap & NEUI_MK_LBUTTON);
    if (!is_down && !is_drag) return false;

    // Mouse interaction breaks the typing/deleting run for undo grouping.
    if (is_down) history.reset_action();

    int rel_y = event->data.mouse.y - abs_y - ML_PAD_Y;
    int row   = rel_y / ML_LINE_H();
    if (row < 0) row = 0;
    int line = static_cast<int>(scroll_offset) + row;
    if (line >= static_cast<int>(n_lines))
      line = static_cast<int>(n_lines) - 1;

    float click_x = static_cast<float>(event->data.mouse.x - abs_x - ML_PAD_X);
    if (click_x < 0.0f) click_x = 0.0f;
    int new_pos = ml_pos_from_col(*this, starts, line, click_x);

    cursor_pos = new_pos;
    if (is_down) {
      bool shift = (event->data.mouse.buttonmap & NEUI_MK_SHIFT) != 0;
      if (!shift) sel_anchor = new_pos;
    }
    repaint();
    return true;
  }

  // -------------------------------------------------------------------------
  // TreeviewWidget - hierarchical list with expand/collapse

  // Row height scales with painted_ui_scale() so the (scaled) default font
  // fits; identity on desktop. Accessor so paint + hit-test agree.
  static int TREE_ROW_H()      { return neui_detail::scaled_painted_metric(20); }
  static constexpr int TREE_INDENT     = 16;    // logical pixels per depth level
  static constexpr int TREE_CHEVRON_W  = 14;    // clickable disclosure area width
  static constexpr int TREE_LEFT_PAD   = 4;     // gutter before first level

  bool TreeviewWidget::has_children(uint32_t id) const
  {
    for (uint32_t other_id : tree_items_ordered) {
      auto it = tree_items.find(other_id);
      if (it != tree_items.end() && it->second.parent_id == id)
        return true;
    }
    return false;
  }

  std::vector<TreeviewWidget::VisRow> TreeviewWidget::flatten_visible() const
  {
    // Build children lookup preserving insertion order.
    std::unordered_map<uint32_t, std::vector<uint32_t>> kids;
    for (uint32_t id : tree_items_ordered) {
      auto it = tree_items.find(id);
      if (it == tree_items.end()) continue;
      kids[it->second.parent_id].push_back(id);
    }

    std::vector<VisRow> out;
    out.reserve(tree_items_ordered.size());

    // Recursive descent through children of root (parent_id == 0).
    struct Frame { uint32_t id; int depth; };
    std::vector<Frame> stack;
    // Push root children in reverse so the first child is popped first.
    {
      auto it = kids.find(0u);
      if (it != kids.end())
        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit)
          stack.push_back({ *rit, 0 });
    }

    while (!stack.empty()) {
      Frame f = stack.back();
      stack.pop_back();
      auto it = tree_items.find(f.id);
      if (it == tree_items.end()) continue;

      auto kit = kids.find(f.id);
      bool has_kids = (kit != kids.end() && !kit->second.empty());
      out.push_back({ f.id, f.depth, has_kids });

      if (has_kids && it->second.expanded)
        for (auto rit = kit->second.rbegin(); rit != kit->second.rend(); ++rit)
          stack.push_back({ *rit, f.depth + 1 });
    }

    return out;
  }

  // Returns index of `id` in `rows`, or -1 if not present.
  static int find_row(const std::vector<TreeviewWidget::VisRow>& rows, uint32_t id)
  {
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].id == id) return static_cast<int>(i);
    return -1;
  }

  void TreeviewWidget::paint(neui_render_backend_t* backend,
                              neui_render_ctx_t ctx, bool is_focused)
  {
    auto rows = flatten_visible();
    uint32_t n = static_cast<uint32_t>(rows.size());

    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);

    int full_vis = std::max(1, height / TREE_ROW_H());
    bool show_sb = n > static_cast<uint32_t>(full_vis);
    float content_w = show_sb ? fw - static_cast<float>(SCROLLBAR_W) : fw;

    if (n == 0) scroll_offset = 0;
    else if (scroll_offset >= n) scroll_offset = n - 1;
    uint32_t scroll_range = (n > static_cast<uint32_t>(full_vis))
                              ? n - static_cast<uint32_t>(full_vis) : 0;
    if (scroll_offset > scroll_range) scroll_offset = scroll_range;

    // Background.
    using neui_detail::ColorRole;
    backend->fill_rect(ctx, fx, fy, content_w, fh,
                        neui_detail::color(ColorRole::control_bg));

    // Visible rows (ceiling division so a partial trailing row is drawn).
    int max_visible = (height + TREE_ROW_H() - 1) / TREE_ROW_H();
    if (max_visible < 1) max_visible = 1;

    auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);

    if (backend->push_clip) backend->push_clip(ctx, fx, fy, content_w, fh);
    for (int i = 0; i < max_visible; ++i) {
      uint32_t row_idx = scroll_offset + static_cast<uint32_t>(i);
      if (row_idx >= n) break;
      const VisRow& vr = rows[row_idx];
      auto it = tree_items.find(vr.id);
      if (it == tree_items.end()) continue;
      const TreeItem& ti = it->second;

      float row_y = fy + static_cast<float>(i * TREE_ROW_H());
      float rowh  = static_cast<float>(TREE_ROW_H());
      float indent_px = static_cast<float>(TREE_LEFT_PAD + vr.depth * TREE_INDENT);

      bool sel = (vr.id == selected_tree_item);
      bool hov = hovered && (row_idx == hover_row) && !sel;
      // Selection background; otherwise unselected hover highlight.
      if (sel)
        backend->fill_rect(ctx, fx + 1.0f, row_y, content_w - 2.0f, rowh,
                           neui_detail::color(is_focused
                              ? ColorRole::accent
                              : ColorRole::control_bg_inactive));
      else if (hov)
        backend->fill_rect(ctx, fx + 1.0f, row_y, content_w - 2.0f, rowh,
                           neui_detail::shade(
                             neui_detail::color(ColorRole::control_bg), +14));

      // Disclosure chevron (only when this item has children). Keep the
      // chevron at the fixed 11px (default font) - it's a glyph indicator,
      // not the row's label text, so it sits in the host default regardless
      // of the widget's font selection.
      if (vr.has_children && backend->draw_text) {
        const char* glyph = ti.expanded
                              ? "\xe2\x96\xbe"   // v (down triangle)
                              : "\xe2\x96\xb8";  // > (right triangle)
        backend->draw_text(ctx,
          fx + indent_px, row_y,
          static_cast<float>(TREE_CHEVRON_W), rowh,
          glyph, 11.0f,
          neui_detail::color(ColorRole::text_secondary));
      }

      // Label.
      if (!ti.text.empty() && backend->draw_text) {
        uint32_t text_color = !ti.enabled
              ? neui_detail::color(ColorRole::text_disabled)
              : (sel && is_focused
                  ? neui_detail::color(ColorRole::accent_text)
                  : neui_detail::color(ColorRole::text_primary));
        neui_detail::push_widget_font(backend, ctx, ef);
        backend->draw_text(ctx,
          fx + indent_px + static_cast<float>(TREE_CHEVRON_W),
          row_y,
          content_w - indent_px - static_cast<float>(TREE_CHEVRON_W) - 4.0f,
          rowh,
          ti.text.c_str(), ef.size, text_color);
        neui_detail::pop_widget_font(backend, ctx, ef);
      }
    }
    if (backend->push_clip) backend->pop_clip(ctx);

    // Scrollbar.
    if (show_sb) {
      float sx = fx + content_w;
      backend->fill_rect(ctx, sx, fy, 1.0f, fh,
                          neui_detail::color(ColorRole::scrollbar_separator));
      float tx = sx + 1.0f;
      float tw = static_cast<float>(SCROLLBAR_W) - 1.0f;
      backend->fill_rect(ctx, tx, fy, tw, fh,
                          neui_detail::color(ColorRole::scrollbar_track));
      SbGeom sb = compute_sb(height, full_vis, n, scroll_offset);
      backend->fill_rect(ctx, tx + 1.0f, fy + 1.0f + sb.thumb_top,
                         tw - 2.0f, sb.thumb_h,
                         neui_detail::color(ColorRole::scrollbar_thumb));
    }

    // Outer border.
    uint32_t border_color = neui_detail::color(
        is_focused ? ColorRole::border_focused : ColorRole::border);
    backend->draw_rect(ctx, fx, fy, fw, fh, 1.0f, border_color);
  }

  // ---- keyboard -----------------------------------------------------------

  // Emit TREE_ITEM_SELECTED if the selection actually changed to a valid item.
  static void fire_tree_selected(Session* s, const TreeviewWidget& tv,
                                  uint32_t prev, uint32_t curr)
  {
    if (!s || prev == curr) return;
    if (curr == UINT32_MAX) return;
    if (tv.tree_items.find(curr) == tv.tree_items.end()) return;
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_TREE_ITEM_SELECTED;
    ev.data.tree.widget = { tv.widget_id };
    ev.data.tree.item   = { curr };
    s->dispatch_event(&ev);
  }

  // True if `id` exists in the tree and is enabled. Disabled items are
  // visually grayed and refuse selection / activation - same policy the
  // win32 host enforces via the TVN_SELCHANGINGW veto.
  static bool tv_item_enabled(const TreeviewWidget& tv, uint32_t id)
  {
    auto it = tv.tree_items.find(id);
    return it != tv.tree_items.end() && it->second.enabled;
  }

  // Walk `rows` from `from` in the given direction (+1 or -1) and return
  // the index of the first enabled row encountered. Returns -1 if none.
  // Used by arrow-key navigation to skip past disabled rows.
  static int tv_next_enabled_row(const TreeviewWidget& tv,
                                  const std::vector<TreeviewWidget::VisRow>& rows,
                                  int from, int direction)
  {
    int i = from + direction;
    int n = static_cast<int>(rows.size());
    while (i >= 0 && i < n) {
      if (tv_item_enabled(tv, rows[static_cast<size_t>(i)].id)) return i;
      i += direction;
    }
    return -1;
  }

  // Scroll so that `row_idx` is fully visible.
  static void tree_ensure_visible(TreeviewWidget& tv, int full_vis, uint32_t row_idx)
  {
    if (row_idx < tv.scroll_offset)
      tv.scroll_offset = row_idx;
    else if (row_idx >= tv.scroll_offset + static_cast<uint32_t>(full_vis))
      tv.scroll_offset = row_idx - static_cast<uint32_t>(full_vis) + 1;
  }

  bool TreeviewWidget::on_keydown(uint32_t keycode, uint32_t /*modifiers*/)
  {
    auto rows = flatten_visible();
    if (rows.empty()) return false;

    int full_vis = std::max(1, height / TREE_ROW_H());
    int cur      = find_row(rows, selected_tree_item);
    uint32_t prev_sel = selected_tree_item;

    switch (keycode) {
    case NEUI_KEY_UP: {
      // Find the next enabled row above the current selection.
      int target = (cur < 0) ? tv_next_enabled_row(*this, rows, -1, +1)
                              : tv_next_enabled_row(*this, rows, cur, -1);
      if (target >= 0) selected_tree_item = rows[static_cast<size_t>(target)].id;
      break;
    }
    case NEUI_KEY_DOWN: {
      int target = (cur < 0) ? tv_next_enabled_row(*this, rows, -1, +1)
                              : tv_next_enabled_row(*this, rows, cur, +1);
      if (target >= 0) selected_tree_item = rows[static_cast<size_t>(target)].id;
      break;
    }
    case NEUI_KEY_HOME: {
      int target = tv_next_enabled_row(*this, rows, -1, +1);
      if (target >= 0) selected_tree_item = rows[static_cast<size_t>(target)].id;
      break;
    }
    case NEUI_KEY_END: {
      int target = tv_next_enabled_row(*this, rows,
                                        static_cast<int>(rows.size()), -1);
      if (target >= 0) selected_tree_item = rows[static_cast<size_t>(target)].id;
      break;
    }
    case NEUI_KEY_LEFT: {
      // If selection is expanded -> collapse; otherwise jump to parent.
      if (cur < 0) return false;
      auto it = tree_items.find(selected_tree_item);
      if (it == tree_items.end()) return false;
      if (it->second.expanded) {
        it->second.expanded = false;
      } else {
        uint32_t parent_id = it->second.parent_id;
        auto pit = tree_items.find(parent_id);
        if (parent_id != 0 && pit != tree_items.end() && pit->second.enabled)
          selected_tree_item = parent_id;
      }
      break;
    }
    case NEUI_KEY_RIGHT: {
      if (cur < 0) return false;
      auto it = tree_items.find(selected_tree_item);
      if (it == tree_items.end()) return false;
      if (rows[static_cast<size_t>(cur)].has_children) {
        if (!it->second.expanded) {
          it->second.expanded = true;
        } else {
          // Already expanded -> move to first enabled child.
          if (cur + 1 < static_cast<int>(rows.size()) &&
              rows[static_cast<size_t>(cur + 1)].depth > rows[static_cast<size_t>(cur)].depth) {
            int target = tv_item_enabled(*this, rows[static_cast<size_t>(cur + 1)].id)
                           ? cur + 1
                           : tv_next_enabled_row(*this, rows, cur + 1, +1);
            // Only accept the target if it's still under our subtree.
            if (target >= 0 && target < static_cast<int>(rows.size()) &&
                rows[static_cast<size_t>(target)].depth > rows[static_cast<size_t>(cur)].depth)
              selected_tree_item = rows[static_cast<size_t>(target)].id;
          }
        }
      }
      break;
    }
    case NEUI_KEY_RETURN:
    case NEUI_KEY_SPACE: {
      if (cur < 0) return false;
      // Enter/Space on a disabled item is inert. (Arrow nav skips past
      // disabled rows so cur normally points at an enabled one, but a
      // freshly-set selected_tree_item from set_selected() could land us
      // here on a disabled row.)
      if (!tv_item_enabled(*this, selected_tree_item)) return true;
      // Enter/Space toggles expansion on items with children and always fires ACTIVATED.
      auto it = tree_items.find(selected_tree_item);
      if (it != tree_items.end() && rows[static_cast<size_t>(cur)].has_children)
        it->second.expanded = !it->second.expanded;
      if (session) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_TREE_ITEM_ACTIVATED;
        ev.data.tree.widget = { widget_id };
        ev.data.tree.item   = { selected_tree_item };
        session->dispatch_event(&ev);
      }
      repaint();
      return true;
    }
    default:
      return false;
    }

    // Recompute visible rows after possible collapse/expand and keep selection visible.
    auto new_rows = flatten_visible();
    int new_cur   = find_row(new_rows, selected_tree_item);
    if (new_cur >= 0) tree_ensure_visible(*this, full_vis, static_cast<uint32_t>(new_cur));

    fire_tree_selected(session, *this, prev_sel, selected_tree_item);
    repaint();
    return true;
  }

  // ---- mouse --------------------------------------------------------------

  bool TreeviewWidget::on_mouse_event(neui_event_t* event)
  {
    auto rows    = flatten_visible();
    uint32_t n   = static_cast<uint32_t>(rows.size());
    int full_vis = std::max(1, height / TREE_ROW_H());
    bool show_sb = n > static_cast<uint32_t>(full_vis);

    // ---- wheel ---------------------------------------------------------
    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      if (!show_sb) return false;
      int delta = event->data.wheel.delta;
      uint32_t range = n - static_cast<uint32_t>(full_vis);
      if (delta < 0) {
        uint32_t step = static_cast<uint32_t>(-delta);
        scroll_offset = std::min(scroll_offset + step, range);
      } else if (delta > 0) {
        uint32_t step = static_cast<uint32_t>(delta);
        scroll_offset = (scroll_offset >= step) ? scroll_offset - step : 0;
      }
      repaint();
      return true;
    }

    // ---- scrollbar drag in progress ------------------------------------
    if (sb_dragging) {
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
           !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        sb_dragging = false;
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        SbGeom sb = compute_sb(height, full_vis, n, sb_drag_start_offset);
        float movable = sb.track_h - sb.thumb_h;
        if (movable > 0.0f) {
          uint32_t range = n - static_cast<uint32_t>(full_vis);
          int delta_y = event->data.mouse.y - sb_drag_start_y;
          int new_off = static_cast<int>(sb_drag_start_offset)
                      + static_cast<int>(static_cast<float>(delta_y) * static_cast<float>(range)
                                         / movable + 0.5f);
          if (new_off < 0) new_off = 0;
          if (static_cast<uint32_t>(new_off) > range) new_off = static_cast<int>(range);
          scroll_offset = static_cast<uint32_t>(new_off);
        }
        repaint();
        return true;
      }
      return false;
    }

    // ---- hover tracking -------------------------------------------------
    if (event->type == NEUI_EVENT_MOUSE_MOVE) {
      uint32_t new_hover = UINT32_MAX;
      int rel_x = event->data.mouse.x - abs_x;
      int rel_y = event->data.mouse.y - abs_y;
      bool in_content = rel_y >= 0
                     && rel_x >= 0
                     && (!show_sb || rel_x < width - SCROLLBAR_W);
      if (in_content && n > 0) {
        uint32_t row = scroll_offset + static_cast<uint32_t>(rel_y / TREE_ROW_H());
        if (row < n) new_hover = row;
      }
      if (new_hover != hover_row) {
        hover_row = new_hover;
        repaint();
      }
      return true;
    }

    // ---- scrollbar click ------------------------------------------------
    if (show_sb && event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
      int sb_left = abs_x + width - SCROLLBAR_W;
      if (event->data.mouse.x >= sb_left) {
        SbGeom sb = compute_sb(height, full_vis, n, scroll_offset);
        uint32_t range = n - static_cast<uint32_t>(full_vis);
        float local_y = static_cast<float>(event->data.mouse.y - abs_y) - 1.0f;
        if (local_y >= sb.thumb_top && local_y < sb.thumb_top + sb.thumb_h) {
          sb_dragging = true;
          sb_drag_start_y = event->data.mouse.y;
          sb_drag_start_offset = scroll_offset;
          hover_row = UINT32_MAX;  // clear hover during drag
        } else if (local_y < sb.thumb_top) {
          uint32_t step = static_cast<uint32_t>(full_vis);
          scroll_offset = (scroll_offset >= step) ? scroll_offset - step : 0;
          repaint();
        } else {
          uint32_t step = static_cast<uint32_t>(full_vis);
          scroll_offset = std::min(scroll_offset + step, range);
          repaint();
        }
        return true;
      }
    }

    // ---- double-click in content area -> ACTIVATED ----------------------
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK) {
      int rel_y = event->data.mouse.y - abs_y;
      if (rel_y < 0) return false;
      uint32_t row = scroll_offset + static_cast<uint32_t>(rel_y / TREE_ROW_H());
      if (row >= n) return false;
      // Disabled rows refuse selection and activation entirely.
      if (!tv_item_enabled(*this, rows[row].id)) return true;
      selected_tree_item = rows[row].id;
      // Toggle expansion on double-click for parent nodes.
      if (rows[row].has_children) {
        auto it = tree_items.find(selected_tree_item);
        if (it != tree_items.end())
          it->second.expanded = !it->second.expanded;
      }
      if (session) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_TREE_ITEM_ACTIVATED;
        ev.data.tree.widget = { widget_id };
        ev.data.tree.item   = { selected_tree_item };
        session->dispatch_event(&ev);
      }
      repaint();
      return true;
    }

    // ---- single click in content area ----------------------------------
    if (event->type != NEUI_EVENT_MOUSE_BUTTON_DOWN) return false;
    int rel_y = event->data.mouse.y - abs_y;
    if (rel_y < 0) return true;
    uint32_t row = scroll_offset + static_cast<uint32_t>(rel_y / TREE_ROW_H());
    if (row >= n) return true;

    const VisRow& vr = rows[row];
    int indent_px  = TREE_LEFT_PAD + vr.depth * TREE_INDENT;
    int chevron_x0 = abs_x + indent_px;
    int chevron_x1 = chevron_x0 + TREE_CHEVRON_W;

    // Chevron click toggles expansion and does NOT change selection.
    if (vr.has_children &&
        event->data.mouse.x >= chevron_x0 &&
        event->data.mouse.x <  chevron_x1)
    {
      auto it = tree_items.find(vr.id);
      if (it != tree_items.end())
        it->second.expanded = !it->second.expanded;
      repaint();
      return true;
    }

    // Row body click selects - but disabled rows are inert.
    if (!tv_item_enabled(*this, vr.id)) return true;
    uint32_t prev_sel = selected_tree_item;
    selected_tree_item = vr.id;
    tree_ensure_visible(*this, full_vis, row);
    fire_tree_selected(session, *this, prev_sel, selected_tree_item);
    repaint();
    return true;
  }

  // -------------------------------------------------------------------------
  // GridWidget - scrollable table. Cells are paint-state, not widgets;
  // the model + paint helpers live in hosts/shared/grid_model.h and
  // hosts/shared/widget_paint_grid.h so the native hosts can reuse them.

  using neui_detail::GridColumn;
  using neui_detail::GridModel;
  using neui_detail::GridViewport;
  using neui_detail::GridHit;
  using neui_detail::GridHitRegion;

  static neui_detail::GridPaintConfig xpl_grid_config(const GridWidget& g)
  {
    return neui_detail::grid_read_config(g.attrs.get());
  }

  static bool xpl_grid_fire_row_selected(GridWidget& g, int row)
  {
    if (!g.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_SELECTED;
    ev.data.grid_row.widget.id = g.widget_id;
    ev.data.grid_row.row       = row;
    return g.session->dispatch_event(&ev);
  }

  static bool xpl_grid_fire_cell_selected(GridWidget& g, int row, int col)
  {
    if (!g.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_SELECTED;
    ev.data.grid_cell.widget.id = g.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    return g.session->dispatch_event(&ev);
  }

  static bool xpl_grid_fire_cell_clicked(GridWidget& g, int row, int col)
  {
    if (!g.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_CLICKED;
    ev.data.grid_cell.widget.id = g.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    return g.session->dispatch_event(&ev);
  }

  static void xpl_grid_fire_row_activated(GridWidget& g, int row)
  {
    if (!g.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_ACTIVATED;
    ev.data.grid_row.widget.id = g.widget_id;
    ev.data.grid_row.row       = row;
    g.session->dispatch_event(&ev);
  }

  static void xpl_grid_fire_column_resized(GridWidget& g, int col, int old_w, int new_w)
  {
    if (!g.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_COLUMN_RESIZED;
    ev.data.grid_column_resize.widget.id = g.widget_id;
    ev.data.grid_column_resize.col       = col;
    ev.data.grid_column_resize.old_width = old_w;
    ev.data.grid_column_resize.new_width = new_w;
    g.session->dispatch_event(&ev);
  }

  static void xpl_grid_fire_sort_changed(GridWidget& g, int col,
                                            neui_grid_sort_dir_t dir)
  {
    if (!g.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_SORT_CHANGED;
    ev.data.grid_sort.widget.id = g.widget_id;
    ev.data.grid_sort.col       = col;
    ev.data.grid_sort.dir       = (int)dir;
    g.session->dispatch_event(&ev);
  }

  // Run the dispatch ladder for a body click: ROW_SELECTED -> (cell_focus ?
  // CELL_SELECTED : skip) -> CELL_CLICKED, stopping early at the first
  // consumer. Always updates the widget's selected_row / selected_col.
  static void xpl_grid_click_ladder(GridWidget& g, int row, int col)
  {
    auto cfg = xpl_grid_config(g);
    g.model.selected_row = row;
    if (cfg.cell_focus) g.model.selected_col = col;
    if (xpl_grid_fire_row_selected(g, row)) return;
    if (cfg.cell_focus) {
      if (xpl_grid_fire_cell_selected(g, row, col)) return;
    }
    xpl_grid_fire_cell_clicked(g, row, col);
  }

  // ---- Cell-edit dispatch helpers ----------------------------------------

  static void xpl_grid_fire_cell_edit_event(GridWidget& g, neui_event_type_t t,
                                              int row, int col)
  {
    if (!g.session) return;
    neui_event_t ev{};
    ev.type = t;
    ev.data.grid_cell.widget.id = g.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    g.session->dispatch_event(&ev);
  }

  // Try to open the in-place editor at the given logical (row, col). No-op
  // when grid_cell_edit_allowed rejects, or when an editor is already open
  // somewhere. Fires NEUI_EVENT_GRID_CELL_EDIT_BEGIN on success. The caller
  // owns the repaint (repaint() is protected on WidgetData).
  static bool xpl_grid_try_begin_edit(GridWidget& g, int row, int col)
  {
    if (g.model.edit.active) return false;
    auto cfg = xpl_grid_config(g);
    if (!neui_detail::grid_cell_edit_allowed(g.model, row, col, cfg.cell_focus))
      return false;
    neui_detail::grid_begin_edit(g.model, row, col);
    xpl_grid_fire_cell_edit_event(g, NEUI_EVENT_GRID_CELL_EDIT_BEGIN, row, col);
    return true;
  }

  // Validate + commit the pending edit. On success writes the new text into
  // the cell, fires NEUI_EVENT_GRID_CELL_CHANGED, and closes the editor;
  // returns true. On reject (client validate returned false) leaves the
  // editor open with the proposed text and returns false. The caller owns
  // the repaint.
  static bool xpl_grid_commit_edit(GridWidget& g)
  {
    if (!g.model.edit.active) return false;
    int  row = g.model.edit.row;
    int  col = g.model.edit.col;
    auto* client = g.session ? g.session->_grid_client : nullptr;
    const std::string proposed = g.model.edit.te.text;
    if (client && client->validate_cell) {
      neui_widget_t w{}; w.id = g.widget_id;
      if (!client->validate_cell(g.session->get_token(), w, row, col,
                                   proposed.c_str())) {
        // Reject: stay in edit mode so the user can fix the value.
        return false;
      }
    }
    // Accept: drop the editor state THEN write the cell so the next paint
    // renders the new value (not the edit overlay).
    (void)neui_detail::grid_end_edit(g.model);
    auto& r = g.model.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = proposed;
    g.model.sort_dirty = true;
    xpl_grid_fire_cell_edit_event(g, NEUI_EVENT_GRID_CELL_CHANGED, row, col);
    return true;
  }

  // Cancel the pending edit and fire NEUI_EVENT_GRID_CELL_EDIT_CANCEL. The
  // caller owns the repaint.
  static void xpl_grid_cancel_edit(GridWidget& g)
  {
    if (!g.model.edit.active) return;
    int row = g.model.edit.row;
    int col = g.model.edit.col;
    (void)neui_detail::grid_end_edit(g.model);
    xpl_grid_fire_cell_edit_event(g, NEUI_EVENT_GRID_CELL_EDIT_CANCEL, row, col);
  }

  // Public bridges - widgets.cpp lives in the same translation unit
  // namespace but the static helpers above can't be referenced from a
  // separately-compiled .cpp. These thin wrappers expose them under
  // namespace-scope external linkage.
  bool xpl_grid_try_begin_edit_pub(GridWidget& g, int row, int col)
  { return xpl_grid_try_begin_edit(g, row, col); }
  bool xpl_grid_commit_edit_pub(GridWidget& g) { return xpl_grid_commit_edit(g); }
  void xpl_grid_cancel_edit_pub(GridWidget& g) { xpl_grid_cancel_edit(g); }

  void GridWidget::paint(neui_render_backend_t* backend,
                          neui_render_ctx_t ctx, bool is_focused)
  {
    neui_detail::paint_grid(backend, ctx,
                              (float)x, (float)y,
                              (float)width, (float)height,
                              model, attrs.get(), is_focused);
  }

  bool GridWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    using namespace neui_detail;
    auto cfg = grid_read_config(attrs.get());
    GridViewport vp = grid_compute_viewport(model, width, height,
                                              cfg.row_h, cfg.header_h);
    int n_rows = (int)model.rows.size();
    int n_cols = (int)model.columns.size();

    // --- Edit-mode keys take priority over the nav switch ----------------
    if (model.edit.active) {
      auto& te   = model.edit.te;
      auto& hist = model.edit.history;
      const bool shift = (modifiers & NEUI_KMOD_SHIFT) != 0;
      const bool ctrl  = (modifiers & NEUI_KMOD_CTRL)  != 0;
      switch (keycode) {
      case NEUI_KEY_RETURN:
        xpl_grid_commit_edit(*this);
        repaint();
        return true;
      case NEUI_KEY_ESCAPE:
        xpl_grid_cancel_edit(*this);
        repaint();
        return true;
      case NEUI_KEY_LEFT:
        te_move_left (te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
        repaint(); return true;
      case NEUI_KEY_RIGHT:
        te_move_right(te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
        repaint(); return true;
      case NEUI_KEY_HOME:
        te_move_home (te.text, te.cursor, te.sel_anchor, shift, &hist);
        repaint(); return true;
      case NEUI_KEY_END:
        te_move_end  (te.text, te.cursor, te.sel_anchor, shift, &hist);
        repaint(); return true;
      case NEUI_KEY_BACK:
        te_backspace     (te.text, te.cursor, te.sel_anchor, ctrl, &hist);
        repaint(); return true;
      case NEUI_KEY_DELETE:
        te_delete_forward(te.text, te.cursor, te.sel_anchor, ctrl, &hist);
        repaint(); return true;
      case NEUI_KEY_A:
        if (ctrl) {
          te_select_all(te.text, te.cursor, te.sel_anchor, &hist);
          repaint();
        }
        return true;
      case NEUI_KEY_C:
        if (ctrl) {
          std::string sel = te_selected_text(te.text, te.cursor, te.sel_anchor);
          if (!sel.empty())
            platform_clipboard_set_text(sel.c_str(), (uint32_t)sel.size());
        }
        return true;
      case NEUI_KEY_X:
        if (ctrl) {
          std::string sel = te_selected_text(te.text, te.cursor, te.sel_anchor);
          if (!sel.empty()) {
            platform_clipboard_set_text(sel.c_str(), (uint32_t)sel.size());
            hist.mark(EditState{ te.text, te.cursor, te.sel_anchor },
                      EditHistory::None, true);
            te_erase_selection(te.text, te.cursor, te.sel_anchor);
            repaint();
          }
        }
        return true;
      case NEUI_KEY_V:
        if (ctrl) {
          int n = platform_clipboard_get_text(nullptr, 0);
          if (n > 0) {
            std::vector<char> buf((size_t)n);
            platform_clipboard_get_text(buf.data(), n);
            // Drop trailing null; the text might have embedded \r\n which
            // te_paste strips.
            std::string paste(buf.data(), (size_t)(n > 0 ? n - 1 : 0));
            te_paste(te.text, te.cursor, te.sel_anchor, paste,
                     /*strip_newlines=*/true, &hist);
            repaint();
          }
        }
        return true;
      case NEUI_KEY_Z:
        if (ctrl) {
          if (shift) te_redo(te.text, te.cursor, te.sel_anchor, hist);
          else       te_undo(te.text, te.cursor, te.sel_anchor, hist);
          repaint();
        }
        return true;
      case NEUI_KEY_Y:
        if (ctrl) {
          te_redo(te.text, te.cursor, te.sel_anchor, hist);
          repaint();
        }
        return true;
      default:
        // Swallow keys we don't act on so they can't escape to the menubar
        // accelerator path while the user is typing.
        return true;
      }
    }

    if (n_rows == 0) return false;

    // Nav walks visual order so Up / Down etc. move the cursor through the
    // rows the user sees after sorting. grid_set_selected_visual writes the
    // corresponding logical row into selected_row.
    grid_ensure_sort_clean(model);

    int prev_row = model.selected_row;
    int prev_col = model.selected_col;
    int vis = grid_visible_rows(vp, cfg.row_h);
    if (vis < 1) vis = 1;
    bool handled = true;

    switch (keycode) {
    case NEUI_KEY_UP: {
      int v = grid_selected_visual(model);
      grid_set_selected_visual(model, (v < 0) ? 0 : (v - 1));
      break;
    }
    case NEUI_KEY_DOWN: {
      int v = grid_selected_visual(model);
      grid_set_selected_visual(model, (v < 0) ? 0 : (v + 1));
      break;
    }
    case NEUI_KEY_PAGEUP: {
      int v = grid_selected_visual(model);
      grid_set_selected_visual(model, (v < 0) ? 0 : (v - vis));
      break;
    }
    case NEUI_KEY_PAGEDOWN: {
      int v = grid_selected_visual(model);
      grid_set_selected_visual(model, (v < 0) ? vis : (v + vis));
      break;
    }
    case NEUI_KEY_HOME:
      if (cfg.cell_focus && !(modifiers & NEUI_KMOD_CTRL)) {
        model.selected_col = (n_cols > 0) ? 0 : -1;
        if (model.selected_row < 0) grid_set_selected_visual(model, 0);
      } else {
        grid_set_selected_visual(model, 0);
        if (cfg.cell_focus) model.selected_col = (n_cols > 0) ? 0 : -1;
      }
      break;
    case NEUI_KEY_END:
      if (cfg.cell_focus && !(modifiers & NEUI_KMOD_CTRL)) {
        model.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
        if (model.selected_row < 0) grid_set_selected_visual(model, n_rows - 1);
      } else {
        grid_set_selected_visual(model, n_rows - 1);
        if (cfg.cell_focus) model.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
      }
      break;
    case NEUI_KEY_LEFT:
      if (cfg.cell_focus) {
        if (model.selected_col > 0) model.selected_col--;
        else if (model.selected_col < 0 && n_cols > 0) model.selected_col = 0;
        if (model.selected_row < 0) model.selected_row = 0;
      } else {
        int step = grid_horizontal_step_px(model);
        model.scroll_offset_x -= step;
        grid_clamp_scroll(model, vp, cfg.row_h);
        repaint();
        return true;   // scrolled, no selection change
      }
      break;
    case NEUI_KEY_RIGHT:
      if (cfg.cell_focus) {
        if (model.selected_col < n_cols - 1) {
          if (model.selected_col < 0) model.selected_col = 0;
          else                         model.selected_col++;
        }
        if (model.selected_row < 0) model.selected_row = 0;
      } else {
        int step = grid_horizontal_step_px(model);
        model.scroll_offset_x += step;
        grid_clamp_scroll(model, vp, cfg.row_h);
        repaint();
        return true;
      }
      break;
    case NEUI_KEY_RETURN: {
      int r = model.selected_row;
      if (r < 0) return true;
      // Try opening the in-place editor first; falls through to the
      // legacy row-activated event when the column is not editable
      // (or cell-focus is off).
      if (cfg.cell_focus && model.selected_col >= 0 &&
          xpl_grid_try_begin_edit(*this, r, model.selected_col)) {
        repaint();
        return true;
      }
      xpl_grid_fire_row_activated(*this, r);
      return true;
    }
    default:
      handled = false;
      break;
    }

    if (!handled) return false;

    // Keyboard row nav snaps back to exact row alignment (drops any
    // smooth-scroll fine offset; also self-cancels an in-flight rubber-band).
    model.scroll_px_offset = 0;

    // Keep selection in view.
    if (cfg.cell_focus && model.selected_col >= 0)
      grid_ensure_cell_visible(model, vp, cfg.row_h,
                                 model.selected_row, model.selected_col);
    else
      grid_ensure_row_visible(model, vp, cfg.row_h, model.selected_row);

    if (model.selected_row != prev_row) {
      xpl_grid_fire_row_selected(*this, model.selected_row);
    }
    if (cfg.cell_focus && (model.selected_row != prev_row ||
                            model.selected_col != prev_col)) {
      xpl_grid_fire_cell_selected(*this, model.selected_row, model.selected_col);
    }
    repaint();
    return true;
  }

  // Focus loss commits an open in-place editor as if the user pressed
  // Enter; on validate-reject we fall back to cancel so we don't leave a
  // stale editor over a widget that no longer has the keyboard. Gaining
  // focus is the common case (nothing to do here).
  void GridWidget::on_focus_change(bool gained)
  {
    if (gained) return;
    if (!model.edit.active) return;
    if (!xpl_grid_commit_edit(*this))
      xpl_grid_cancel_edit(*this);
    repaint();
  }

  bool GridWidget::on_keychar(uint32_t codepoint, uint32_t /*modifiers*/)
  {
    if (!model.edit.active) return false;
    // Filter control characters; ENTER / ESCAPE / BACK / TAB etc. arrive as
    // both KEYDOWN (handled above) and KEYCHAR (suppressed here). Below 0x20
    // and DEL are non-printable.
    if (codepoint < 0x20 || codepoint == 0x7F) return true;
    char buf[4];
    int  n = neui_detail::te_encode_utf8(codepoint, buf);
    auto& te = model.edit.te;
    neui_detail::te_insert_utf8(te.text, te.cursor, te.sel_anchor,
                                  te.overwrite, buf, n, &model.edit.history);
    repaint();
    return true;
  }

  bool GridWidget::on_mouse_event(neui_event_t* event)
  {
    using namespace neui_detail;
    auto cfg = grid_read_config(attrs.get());
    GridViewport vp = grid_compute_viewport(model, width, height,
                                              cfg.row_h, cfg.header_h);

    int lx = event->data.mouse.x - abs_x;
    int ly = event->data.mouse.y - abs_y;

    // --- Edit-mode mouse handling ---------------------------------------
    // A click anywhere commits the edit; if the click lands on the editing
    // cell itself the editor stays open afterwards so the user can continue
    // typing. Move / wheel events fall through to the normal handlers.
    if (model.edit.active &&
        (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN ||
         event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK)) {
      grid_ensure_sort_clean(model);
      GridHit hit = grid_hit_test(model, vp, cfg.row_h,
                                    width, height, lx, ly);
      bool on_editing_cell = (hit.region == GridHitRegion::Cell &&
                              hit.row == model.edit.row &&
                              hit.col == model.edit.col);
      if (on_editing_cell) {
        // No-op (consume); keep typing. A future iteration could move the
        // caret to the click point using measure_text.
        return true;
      }
      // Commit (or reject). On reject the editor stays open and the click
      // is swallowed so the underlying grid doesn't also act on it.
      if (!xpl_grid_commit_edit(*this)) { repaint(); return true; }
      repaint();
      // Commit succeeded - editor closed. Fall through to normal click
      // handling so the click also selects the newly clicked cell.
    }

    // --- column-resize drag in progress ---
    if (model.column_resize_col >= 0) {
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        int dx = event->data.mouse.x - model.column_resize_start_x;
        int new_w = model.column_resize_start_w + dx;
        int min_w = grid_column_min_width(model, model.column_resize_col,
                                            cfg.col_min_w_def);
        if (new_w < min_w) new_w = min_w;
        if (new_w > 5000) new_w = 5000;
        model.columns[(size_t)model.column_resize_col].width = new_w;
        grid_clamp_scroll(model, vp, cfg.row_h);
        session->set_cursor_override(widget_id & 0xffff, NEUI_CURSOR_EW_RESIZE);
        repaint();
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP) {
        int new_w = model.columns[(size_t)model.column_resize_col].width;
        int col   = model.column_resize_col;
        int old_w = model.column_resize_old_w;
        model.column_resize_col     = -1;
        model.column_resize_start_x = 0;
        model.column_resize_start_w = 0;
        session->set_cursor_override(widget_id & 0xffff, NEUI_CURSOR_DEFAULT);
        if (new_w != old_w) xpl_grid_fire_column_resized(*this, col, old_w, new_w);
        repaint();
        return true;
      }
      return false;
    }

    // --- vertical scrollbar drag in progress ---
    if (model.vert_drag.active) {
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
           !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        model.vert_drag.active = false;
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        int vis = grid_visible_rows(vp, cfg.row_h);
        ScrollbarGeom g = compute_scrollbar(vp.body_h, 0,
                                              (int)model.rows.size(), vis,
                                              model.vert_drag.start_position);
        int rel = ly - vp.body_y;
        model.scroll_offset_y = scrollbar_drag_apply(model.vert_drag, rel, g,
                                                        (int)model.rows.size(), vis);
        model.scroll_px_offset = 0;   // scrollbar drag = exact row alignment
        grid_clamp_scroll(model, vp, cfg.row_h);
        repaint();
        return true;
      }
      return false;
    }

    // --- horizontal scrollbar drag in progress ---
    if (model.horz_drag.active) {
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
           !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        model.horz_drag.active = false;
        return true;
      }
      if (event->type == NEUI_EVENT_MOUSE_MOVE) {
        int content_w = grid_total_content_width(model);
        ScrollbarGeom g = compute_scrollbar(vp.body_w, 0,
                                              content_w, vp.body_w,
                                              model.horz_drag.start_position);
        int rel = lx - vp.body_x;
        model.scroll_offset_x = scrollbar_drag_apply(model.horz_drag, rel, g,
                                                       content_w, vp.body_w);
        grid_clamp_scroll(model, vp, cfg.row_h);
        repaint();
        return true;
      }
      return false;
    }

    // --- mouse move: cursor feedback when over a header divider ---
    if (event->type == NEUI_EVENT_MOUSE_MOVE) {
      GridHit hit = grid_hit_test(model, vp, cfg.row_h,
                                    width, height, lx, ly);
      // DEFAULT here clears the override rather than forcing an arrow, so a
      // client's NEUI_ATTR_CURSOR on the GRID (or an ancestor) shows through
      // everywhere except the resize band.
      session->set_cursor_override(widget_id & 0xffff,
                                     hit.region == GridHitRegion::HeaderDivider
                                       ? NEUI_CURSOR_EW_RESIZE
                                       : NEUI_CURSOR_DEFAULT);
      return false;
    }

    // --- wheel: vertical scroll by N rows (STEPPED mode) ---
    // SMOOTH mode is handled in the platform layer (platform_win32.cpp /
    // platform_macos.mm) where the raw NSEvent / WM_MOUSEWHEEL info hasn't
    // been quantized to lines yet; by the time it reaches here the only
    // option is row-stepping. Convention from LISTBOX / TREEVIEW: one wheel
    // notch == one row.
    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      int delta = event->data.wheel.delta;
      if (delta == 0) return false;
      grid_scroll_step_rows(model, vp, cfg.row_h, -delta);
      repaint();
      return true;
    }

    // --- button down: hit-test + start drag / select ---
    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN ||
        event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK)
    {
      // hit-test reads display_order, rebuild it first if dirty.
      grid_ensure_sort_clean(model);
      GridHit hit = grid_hit_test(model, vp, cfg.row_h,
                                    width, height, lx, ly);
      switch (hit.region) {
      case GridHitRegion::HeaderDivider:
        if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
          model.column_resize_col     = hit.col;
          model.column_resize_start_x = event->data.mouse.x;
          model.column_resize_start_w = model.columns[(size_t)hit.col].width;
          model.column_resize_old_w   = model.column_resize_start_w;
          session->set_cursor_override(widget_id & 0xffff, NEUI_CURSOR_EW_RESIZE);
          return true;
        }
        return true;
      case GridHitRegion::Header:
        // Sort cycle on a sortable column header. Shift+click = add /
        // cycle a secondary level; plain click replaces the stack.
        if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN &&
            grid_header_click_allowed(model, hit.col)) {
          bool shift = (event->data.mouse.buttonmap & NEUI_MK_SHIFT) != 0;
          neui_grid_sort_dir_t new_dir =
            grid_apply_header_click(model, hit.col, shift);
          repaint();
          xpl_grid_fire_sort_changed(*this, hit.col, new_dir);
        }
        return true;
      case GridHitRegion::VertScrollTrack: {
        int vis = grid_visible_rows(vp, cfg.row_h);
        ScrollbarGeom g = compute_scrollbar(vp.body_h, 0,
                                              (int)model.rows.size(), vis,
                                              model.scroll_offset_y);
        int rel = ly - vp.body_y;
        if (g.visible && rel >= g.thumb_pos && rel < g.thumb_pos + g.thumb_len) {
          model.vert_drag.active           = true;
          model.vert_drag.start_axis_coord = rel;
          model.vert_drag.start_position   = model.scroll_offset_y;
        } else if (g.visible) {
          int step = vis > 0 ? vis : 1;
          if (rel < g.thumb_pos) model.scroll_offset_y -= step;
          else                   model.scroll_offset_y += step;
          model.scroll_px_offset = 0;   // page step = exact row alignment
          grid_clamp_scroll(model, vp, cfg.row_h);
          repaint();
        }
        return true;
      }
      case GridHitRegion::HorzScrollTrack: {
        int content_w = grid_total_content_width(model);
        ScrollbarGeom g = compute_scrollbar(vp.body_w, 0,
                                              content_w, vp.body_w,
                                              model.scroll_offset_x);
        int rel = lx - vp.body_x;
        if (g.visible && rel >= g.thumb_pos && rel < g.thumb_pos + g.thumb_len) {
          model.horz_drag.active           = true;
          model.horz_drag.start_axis_coord = rel;
          model.horz_drag.start_position   = model.scroll_offset_x;
        } else if (g.visible) {
          int step = vp.body_w > 0 ? vp.body_w : 60;
          if (rel < g.thumb_pos) model.scroll_offset_x -= step;
          else                   model.scroll_offset_x += step;
          grid_clamp_scroll(model, vp, cfg.row_h);
          repaint();
        }
        return true;
      }
      case GridHitRegion::Cell: {
        if (event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK) {
          // Selection already updated by the prior DOWN. Try opening the
          // in-place editor first (mirrors ENTER behaviour); if the cell
          // isn't editable, fall back to the legacy ROW_ACTIVATED event.
          if (xpl_grid_try_begin_edit(*this, hit.row, hit.col)) {
            repaint();
          } else {
            xpl_grid_fire_row_activated(*this, hit.row);
          }
        } else {
          // Skip the cell-click ladder for disabled cells - select the
          // row but suppress CELL_CLICKED (matches per-cell enabled
          // semantics).
          const GridCellOverride* ov = grid_find_override(model, hit.row, hit.col);
          bool cell_dis = ov && ov->has_enabled && !ov->enabled;
          int prev_row = model.selected_row;
          model.selected_row = hit.row;
          if (cfg.cell_focus) model.selected_col = hit.col;
          if (cell_dis) {
            if (model.selected_row != prev_row)
              xpl_grid_fire_row_selected(*this, hit.row);
          } else {
            xpl_grid_click_ladder(*this, hit.row, hit.col);
          }
        }
        repaint();
        return true;
      }
      case GridHitRegion::BodyEmpty:
        // Click in empty area below the rows clears selection.
        if (model.selected_row != -1) {
          model.selected_row = -1;
          model.selected_col = -1;
          xpl_grid_fire_row_selected(*this, -1);
          repaint();
        }
        return true;
      case GridHitRegion::Corner:
      case GridHitRegion::VertScrollThumb:
      case GridHitRegion::HorzScrollThumb:
      case GridHitRegion::None:
      default:
        return false;
      }
    }

    return false;
  }

  // -------------------------------------------------------------------------
  // Accessibility support seams (consumed by a11y_adapter.cpp)
  //
  // Two things the adapter needs live here rather than in its own translation
  // unit: the painted row metrics (file-static, right next to the paint code
  // they belong to) and the menu cascade geometry (built by mb_build_band /
  // mb_build_columns over anonymous-namespace layout types). Both are exposed
  // as accessors so there is exactly ONE definition of each - a second copy in
  // the adapter would drift the first time either side was touched, and the
  // failure mode is an AT pointing at the wrong place on screen, which is worse
  // than not reporting a position at all.

  int list_row_height()         { return LIST_ITEM_H(); }
  int tree_row_height()         { return TREE_ROW_H(); }
  int menubar_band_height()     { return MENUBAR_BAND_H; }
  int scrollbar_gutter_width()  { return SCROLLBAR_W; }

  void Session::collect_menu_elements(uint32_t menu_idx,
                                      std::vector<MenuElementRect>& out)
  {
    if (!_backend || !_widgets.exists(menu_idx)) return;
    auto* mbp = dynamic_cast<MenubarWidget*>(&_widgets[menu_idx]);
    if (!mbp) return;
    const MenubarWidget& mb = *mbp;

    const bool is_popup = !mb.is_menubar();
    // A real menu BAR only has an on-screen presence where this host draws the
    // band itself. Where the OS owns the menu it also owns its accessibility,
    // and publishing our own copy would have the AT read every menu twice.
    if (!is_popup && !platform_menubar_in_frame()) return;
    // ...and only while it is visible, matching frame_menubar() / paint_menubar.
    // A hidden menubar reserves no band (frame_top_inset returns 0), so its
    // children occupy y 0..band_h - reporting band items there would put phantom
    // menus on top of real widgets.
    if (!is_popup && !mb.visible) return;

    // Which frame's surface is this menu drawn on, and is its cascade open?
    uint32_t frame_index = 0;
    std::vector<uint32_t> path;
    if (is_popup) {
      if (!_tree_popup_active || _tree_popup_menu != menu_idx) return;
      frame_index = _tree_popup_frame;
      path        = _menu_path;
    } else {
      frame_index = frame_of(menu_idx);
      if (_menu_open && _menu_bar == menu_idx) path = _menu_path;
    }
    if (frame_index == 0 || !_widgets.exists(frame_index)) return;

    const auto& fw = _widgets[frame_index];
    neui_render_ctx_t ctx = fw.render_ctx;
    // Text measurement needs a context. Reporting nothing beats reporting rects
    // measured as zero-width, which would collapse every item onto one point.
    if (!ctx) return;

    // Shared lookup: the geometry structs hold COPIES of the item text, but the
    // rows we emit must point at storage that outlives this call, so the text
    // comes from the item model itself.
    auto item_text = [&mb](uint32_t id, const char** text, const char** shortcut) {
      auto it = mb.menu_items.find(id);
      if (it == mb.menu_items.end()) return;
      if (text)     *text     = it->second.text.c_str();
      if (shortcut) *shortcut = it->second.shortcut.c_str();
    };

    std::vector<MenuBandItem> band;
    if (!is_popup) {
      mb_build_band(this, ctx, mb, band);
      const int band_h = MENUBAR_BAND_H;
      for (const auto& b : band) {
        MenuElementRect e;
        e.item_id     = b.item_id;
        e.parent_item = 0;
        e.x = b.x; e.y = 0; e.w = b.w; e.h = band_h;
        e.has_submenu = menu_item_has_children(mb, b.item_id);
        // Same rule mb_build_columns applies to a dropdown row: a submenu
        // holder reports its own flag (a validate() verdict is about a command,
        // and a submenu has none), a leaf goes through the full verdict.
        auto bit = mb.menu_items.find(b.item_id);
        e.enabled = e.has_submenu ? (bit != mb.menu_items.end() && bit->second.enabled)
                                  : menu_item_enabled(mb, b.item_id);
        e.expanded    = !path.empty() && path[0] == b.item_id;
        item_text(b.item_id, &e.text, &e.shortcut);
        out.push_back(e);
      }
    }

    if (path.empty()) return;

    // The open cascade. mb_build_columns is the same call the paint and
    // hit-test paths make, with the same arguments, so the reported rows are
    // the rows on screen by construction.
    std::vector<MenuColL> cols;
    if (is_popup) {
      static const std::vector<MenuBandItem> kNoBand;
      mb_build_columns(this, ctx, mb, fw.width, fw.height, kNoBand, path, cols,
                       _tree_popup_x, _tree_popup_y);
    } else {
      mb_build_columns(this, ctx, mb, fw.width, fw.height, band, path, cols);
    }

    for (size_t level = 0; level < cols.size(); ++level) {
      const MenuColL& col = cols[level];
      for (const auto& r : col.rows) {
        MenuElementRect e;
        e.item_id     = r.item_id;
        e.parent_item = col.parent_item;
        e.x = col.x; e.y = col.y + r.y; e.w = col.w; e.h = r.h;
        e.enabled     = r.enabled;
        e.checked     = r.checked;
        e.has_submenu = r.submenu;
        e.separator   = r.separator;
        // A row whose submenu is the NEXT open column is the expanded one.
        e.expanded    = (level + 1 < path.size()) && path[level + 1] == r.item_id;
        item_text(r.item_id, &e.text, &e.shortcut);
        out.push_back(e);
      }
    }
  }

} // namespace xpl_host

// -------------------------------------------------------------------------
// C API table

extern "C" {
  static neui_api_t xpl_base_api = {
    NEUI_VERSION,
    xpl_host::create_session,
    xpl_host::destroy,
    xpl_host::get_interface,
    xpl_host::run_fn,
    xpl_host::endsession_fn,
    xpl_host::pump_once_fn,
  };
}

void xpl_host::register_host()
{
  platform_init();
  neui_register(NEUI_HOST_CROSSPLATFORM, &xpl_base_api);
}

extern "C" void neui_register_xplhost()
{
  xpl_host::register_host();
}
